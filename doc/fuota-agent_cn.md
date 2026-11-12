# FUOTA 接收 Agent 设计与验证

[English](fuota-agent.md)

适用于使用 USP v1 backend 的 RZI LoRaWAN FUOTA Service。本文描述接收 Agent 的协议处理、Class 切换、控制应答与 SDK 接口。

依终端任务分工，任务 11 负责 TS003／TS004／TS005 接入及组播期间的业务收发控制。任务 15 在缺片或断网时沿用 Agent 的关闭组播、还原原 Class 与状态回报流程。

## 接入范围

Agent 提供 `OFFLINE → READY → PRECONFIG → RECEIVING → RECOVERING → FINALIZING → READY`。
单播设置期间正常业务收发；实际进组播前 backend 先阻挡尚未发出的业务上行，SDK 阻挡新的业务
上行与对应上层下行投递。Package／MAC 控制持续处理。

COMPLETE、接收 timeout、设置 timeout 或错误皆经同一关闭／还原流程。预设每步最多三次；
关闭会停用监听并清掉本轮调度，保留组播安全上下文及 TS004 控制／数据，还原原 A 或 C。

原 C 也先关闭组播并重建原接收模式，不以同为 C 略过。原 B 不提供可接受的 FUOTA context；
McGroupSetup 使用标准 IDerror，FragSessionSetup 使用标准 index unsupported；不占 RFU 或假报 DR／频率错误。
不支援的 Class B multicast command 不建立接收调度。

原 A 还原后重新起算 keepalive 周期。原 C 离开收片阶段后立即还原并监听 RXC，从本会话 StatusAns 或先结案的 DeleteAns 传送完成时重新起算。若未收到查询，从原窗口截止与还原完成两者较晚者加上状态保留期后起算。

单播 RX1／RX2／RXC 收到 StatusReq 时，高优先序立即排入 StatusAns，无 Package 0～3 秒、
BlockAckDelay 或 TPM 额外 0.5～6 秒乱数；忙碌时仍排队，沿用正常 MAC／区域／LBT／CSMA／relay 限制。
组播 B／C 查询仍保留原 Package 调度、BlockAckDelay 与 TPM 随机等待。一般上行也保留原乱数。
依据 TS004 v1.0.0 §3.2／§3.5 对 multicast 查询的随机回覆描述；不是认证通过声明。
https://lora-alliance.org/wp-content/uploads/2020/11/fragmented_data_block_transport_v1.0.0.pdf

收到有效 FragStatus／Delete Req 后，Package 自行安排 Ans，不依赖下一笔 keepalive。
FINALIZING 以同 generation 的控制应答本机传送结果或保留期限结算。
`cleanup_uplink_done` 为相容保留栏位，在此政策维持 false；不再作为 keepalive 或结束条件。
`status_answered`／`delete_answered` 仍独立记录，`lns_confirmed` 不由本机 TXDONE 推定。
保留期限预设 120 秒，从原窗口截止与还原完成两者较晚者起算；接收 timeout 则从还原完成起算。
缺片逾时后，StatusAns 的本机发送完成只更新 status_answered，继续保留 TS004 会话。
同代次 DeleteAns 发送完成后才释放；若只收到查询，保留到原期限再释放，重复查询不延长期限。
收到过成功的控制回覆时 cleanup_error 为 0；完全没有成功回覆而期满时为 TIMEOUT。分片逾时原因独立保留。
完整数据保留到消费者释放，失败数据在收尾结算后释放；新 setup 不可覆写保留区。
keepalive=0 不会启动替代收尾上行，Class A 应用须自行提供上行；Class C 可直接接收单播查询。
周期 keepalive 发送失败沿用下一周期再尝试，不新增密集重试；实际 TX 可受 MAC 忙碌而延后。
示例共用一个周期上行计时器；单播 setup 应答优先调度，周期工作遇到待送应答时等下一个周期。相同分片设置重送会重新回覆成功，保留目前代次、已收数据与 decoder；冲突设置仍回报资源占用。

TS003 来源区分 MAC DeviceTime 与 ALCSync。PackageVersionReq 不建立传输。分片长度来自 Package 已验证的
NbFrag／FragSize／Padding；Agent 不读取 MCUboot Header 猜长度、不识别 full／delta。
FEC 及 Flash 由原模组提供；checked storage 传递错误，写失败不能成为成功 DATA_READY。
主机验证另修正 provider `IsPowerOfTwo` 的 `1U << i` 无号位移，不改 FEC 算法。

## 公开接口

使用 `<rzi/lorawan/fuota.h>`，先注册 `agent_event` 再初始化 LoRaWAN；初始化自动启动服务。
接收通知通常在专用 worker 上运行。应用显式释放数据成功时，状态回调也可同步在调用线程运行。回调应仅复制事件入应用队列，不阻塞。SDK dispatcher 也只把 backend 值事件入队。
`rzi_fuota_get_snapshot()` 查阶段、会话参数、时间来源、数据与退出结果。
`rzi_fuota_get_session_info()`／`rzi_fuota_get_data_info()` 是值复制描述，不含 USP 指标或金钥。
`rzi_fuota_read_image()` 限 ready 且已知有效长度，并检查 offset／size。
`rzi_fuota_release_data(generation)` 在还原及收尾结算后调用 provider 释放；错代次拒绝。
`set_expected_size()` 保留相容，但在 Agent 仅存为 `expected_length` 提示，不覆盖空口长度。
未启用 Agent 的 backend，新 API 可链接并回 NOT_SUPPORTED。

`rzi_fuota_apply()` 由 FUOTA service 提供。收尾期间回 BUSY，完成后沿用同事基线由 IMG_MANAGER 复制候选映像、请求 TEST 并重启的流程。
FMP callback 不触发安装。应用层须另行校验映像并决定是否允许安装；本机 FINISHED 不等于 LNS 已确认或安装成功。

## backend／provider 契约

- `get_session_info` 供事件遗失时查回本轮身份；事件队列溢出使本轮不可信，清理并拒绝假完成。
- `close_multicast`／`restore_settings` 回 0 表示已完成，非单纯排入队列；原 Class 与组播接收参数由 backend 管理。
- 相容 backend `cleanup_uplink(0)` 仅供周期 keepalive；Agent 不再以非零 generation 要求独立收尾上行。
- Package CONTROL_DONE 可在第一笔 keepalive 前结算，例如原 Class C；传送结果不代表 LNS 收件。
- `release_data` 不由 Agent 分配／擦除存储；active、保留中或错代次应拒绝。
- Package observer 传递 setup、窗口、实际开始／结束、FEC 与 control TX 结果，使用保存的 generation。
- 完成、timeout、重复或迟到 END 幂等；旧代次不覆盖新轮。
- 关闭／还原失败到达次数上限后保留错误，不宣称业务已恢复；由 leave／rejoin 恢复流程处理。
- `SessionInfo.window_end_ms`（DEADLINE 有效位）是 backend 由原 GPS 窗口结束时间转换的
  monotonic deadline，主动取消不会移除；START 时更新对应，事件队列延迟不延长窗口。
  仅有秒级 ALCSync 时允许不到一秒的转换精度；没有可用截止时间的 backend 在 START 使用
  接收开始＋timeout 作为后备，未进窗口且没有截止时间则立即收尾。
- Agent 在无事件时睡至下一个期限，不以固定短间隔轮询。
- 原窗口与收尾等待为不同期限；组播查询保留 TS004 随机应答延迟；单播采上述快速调度，不变更
  ChirpStack Profile 的状态等待时间。Server 是否及时收到 Ans 仍需独立验证。

提供存储模式必须是 USP 的 PROVIDED_STORAGE_IMPL；自订 USER_STORAGE_IMPL 的错误回传尚未验证。
checked FUOTA 存取使用既有 Flash 配置／位址映射，回传容量并保护边界与 modem context。
示例默认最多接收 200 个分片，每片最多 100 字节。正式映像区、分区配置与完整容量由分区写入任务处理。

启用 `CONFIG_RZI_LORAWAN_FUOTA_TRACE` 时，UART 跟踪记录以 `RZI_FUOTA_TRACE` 开头，时间戳在事件发生时采集。已配置的组播窗口启动成功但缺少分片配置时，记录 `fuota.preconfig_fail`、会话代次和有效位。本地诊断不产生 FPort 101 事件。

## GenAppKey 与 AT 设计

既有 `rzi_lorawan_set_gen_app_key(key, 16)`／get API 与 AppKey 分开。
未设置回 NO_DATA；LoRaWAN 1.0.x 入网配置沿用 AppKey，明确全零仍是独立设置。
独立值在下一次 OTAA Join 套用，OTAA AppKey 保持原值；派生由 backend 金钥服务处理。
设置／保存失败不提交新值。装置与 LNS 来源需一致；本机 SDK 测试不代表已验证空口金钥对齐。

未来 AT 层：检查输入为 16-byte 金钥的合法文字编码 → 调用同一 SDK setter → 回报保存结果 →
下一次 Join 生效。不得以 MCROOTKEY 代替 GenAppKey 写入，不输出金钥明文。
命令名称／语法留待 AT 框架整合；本接收模组仅提供 SDK 接口。

## 示例与测试

[FUOTA 示例](../samples/lorawan/fuota/README.rst) 默认保留候选数据。接收演示模式在 FINISHED 后读完整个数据块，再按代次释放。读取失败时保留数据。
Agent 主机测试的整套和单个用例运行方式、结果及 log 位置见 [FUOTA Agent 测试说明](../tests/lorawan/fuota_agent/README_cn.md)。

从 west workspace 根目录：

```sh
west build -b native_sim -d build/fuota-agent-tests RZI/tests/lorawan/fuota_agent
build/fuota-agent-tests/zephyr/zephyr.exe
west build -b native_sim -d build/fuota-service-tests RZI/tests/lorawan/fuota
build/fuota-service-tests/zephyr/zephyr.exe
west build -b native_sim -d build/lorawan-settings-tests RZI/tests/lorawan/settings
build/lorawan-settings-tests/zephyr/zephyr.exe
python3 RZI/tests/lorawan/fuota_protocol/run.py
west build --sysbuild -b rak3372_f/stm32wle5xx -d build/fuota-demo RZI/samples/lorawan/fuota -- -Dfuota_CONFIG_APP_FUOTA_RECEIVE_DEMO=y
```

protocol host 测试编译真实 TS003／TS004／TS005 及 FEC，用替身取代 modem／radio／Flash，启用 ASan／UBSan。
native_sim 验证 Agent 调用顺序、代次、双向闸门、逾时、错误及金钥来源；既有 settings 测试含保存失败交易。
两者都不构成 RF、实体 Flash 故障、LNS 远端收尾或跨板验收证据。
既有验收案例沿用原识别码 `I1-T01`～`I1-T27`，以便查回实机与 LNS 纪录。空口验收涵盖原 A／C 还原、原 B 拒绝、再次传输、
独立／预设／不一致 GenAppKey 及单播 Req／Ans 与 LNS 最终结果时间线。

依赖修改已纳入 `zephyr/patches/usp/0012-fuota-v1-agent-hooks.patch`、0014 trace、0015 prompt recovery、0016 unicast setup retry 与
`zephyr/patches/usp_zephyr/0013-fuota-checked-storage.patch`，杂凑由 patch manifest 管理。
不可在含未提交工作区直接执行 west patch clean；本轮仅于隔离的来源副本验证 patch 套用。
