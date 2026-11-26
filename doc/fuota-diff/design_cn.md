# FUOTA 设计与实现汇总（156–169）

日期：2026-11-26
适用：`feat/fuota-diff`（基线 `fuota a70713b`）
读者：需要理解或 merge 这套改动的同事
组织方式：按**运行顺序**分章，每章标注对应任务号，并固定四段：**为什么 / 接口与数据流 / 代码位置 / 坑与取舍**。

---

## 0. 一页总览

### 0.1 数据从哪来、到哪去

```text
[主机]  167 签名 ──> 165 全量包
                     169 差分包（两份签名镜像）
                                |
                                v
[空口]  LNS 按分片下发（TS004/TS005）
                                |
                                v
[设备]  收片（161/157）──> 头校验（161）
                                |
                     失败 ──> 擦目标区 + 原因（158/161）
                                |
                     通过 ──> 安装（157，用 156 解码器）
                                |
                             boot_request_upgrade(TEST) ──> 重启
                                |
                        MCUboot 验签 ──> 新镜像运行（confirm 未做，票 160）
```

### 0.2 任务号 ↔ 模块 ↔ 代码

| 任务 | 模块 | 主要代码 |
|---|---|---|
| 167 | 签名 | `tools/builds/sign_image.py` |
| 165 | 全量包 + 向量 | `tools/builds/pack_image.py`、`fuota_package.py` |
| 169 | 差分包 | `pack_image.py diff`、`build/hpatch-host` |
| 161 | 头校验 + 状态机 | `fuota_header.c`、`lorawan_fuota.c` |
| 157 | 安装 | `fuota_install.c`、`lorawan_fuota.c` |
| 156 | 差分解码 | `fuota_diff.c` + HPatchLite/tinyuz |
| 158 | 失败处理 | `lorawan_fuota.c`（`finish_window`） |

> 阅读顺序建议：本章 → 第 1 章（包）→ 第 2 章（校验）→ 第 3 章（安装）→ 第 4 章（失败）→ 第 5 章（状态）→ 第 6 章（存储）。

---

## 1. 包与工具链（167 / 165 / 169）

### 为什么

设备和 Server 之间只认一种包：**32 字节业务头 + 载荷**。载荷要么是完整签名镜像，要么是补丁。签名保证真实性，CRC 保证内容没坏，MD5 用于 Server 侧整包校验——三者职责不重叠。

### 接口与数据流

```text
zephyr.bin --167 sign--> zephyr.signed.bin
             --165 pack--> firmware.bin（头 + 整镜像）
             --169 diff--> firmware.bin（头 + 补丁）
```

- 头 magic `@RAK`，CRC 只覆盖载荷。
- 差分两侧都必须是**已签名镜像**，且用**同一把密钥**签名；否则主机验签不过。
- 工具内部顺序：hdiffi 出补丁 → hpatchi 还原 → imgtool 验签 → 与目标镜像逐字节比对 → 才写包。

### 代码位置

- `sign_image.py`：调用 `imgtool sign --pad-header`，拒绝仓库内密钥和示例密钥。
- `pack_image.py pack/vectors/verify`：统一走 `fuota_package.build_firmware()`。
- `pack_image.py diff`：在 `build_firmware()` 前插补丁、后插主机验收。

### 坑与取舍

| 点 | 说明 |
|---|---|
| 密钥 | sysbuild 默认用 `root-rsa-2048.pem`；测试签名要用同一把，否则 `No signature found` |
| 容量 | 同构建小改补丁 6,427 B；真实两版 103,500 B，**32 KB `fuota_delta` 装不下**，必须回退全量 |
| 旧格式 | `FUOT` 向量已作废，统一 `@RAK` |

---

## 2. 接收与头校验（161）

### 为什么

分片是流式到达的，必须边收边落盘；但"这包是不是给我的、类型对不对、内容有没有坏、基线对不对"要等收齐、CRC 才能算。所以**写盘与校验分离**：先按逻辑偏移落盘，收齐后在安装前统一校验。

### 接口与数据流

32 字节头规则（字段偏移只在 `fuota_header.c`）：

| 字段 | 规则 | 原因码 |
|---|---|---|
| magic | `@RAK` | 0x11 |
| header_version | 1 | 0x11 |
| header_length | 32 | 0x11 |
| product_id | byte0 == `device_code`；1–3 == 0 | 0x12 |
| target_version | **不校验**（同版本/降级都允许） | — |
| base_version | 差分 == 当前；全量 == 0 | 0x14 |
| package_kind | 与 Descriptor 一致 | 0x11 |
| diff_algo | 全量 0、差分 1 | 0x11 |
| flags | 0 | 0x11 |
| image_length | == 载荷长度 | 0x15 |
| crc32 | 仅载荷 | 0x16 |

### 代码位置

- `verify_stored()`：从头缓存读 32 B，组 `expect`，调 `fuota_hdr_verify()`。
- 载荷读取回调把逻辑偏移 `-32` 映射到目标区。
- 失败：擦目标区 → `FAILED` → `last_reason` → `notify_complete(-1013)`。

### 坑与取舍

| 点 | 说明 |
|---|---|
| 同版本 | 新设计明确允许；旧 AC 写"拒绝同版本"，需要 PM 改文案 |
| 头位置 | 设计写 `storage.header_slot`；实际放 scratch 第二扇区（storage 被 NVS 占） |
| 校验时机 | 设计希望 VERIFYING 期间保持 Class C；当前先恢复 Class A 再校验（上游结构） |
| 非 RAK | `RAK1` Host 包无 32 B 头；已加 `session_has_header`，但 RAK1 尚未启用 |

---

## 3. 安装（157 / 156）

### 为什么

安装层只做"把载荷变成 slot1 里的可启动镜像"，不关心包从哪来、是否合法。头已经由 161 处理，所以安装接口只看 `header_length`，不重复校验魔数/CRC。

### 接口与数据流

```c
int rzi_fuota_install(uint32_t descriptor, uint16_t header_length,
                      const struct rzi_fuota_install_io *io);
```

- `read_pkg` 指向**载荷**（offset 0 = 载荷首字节），`pkg_size` 是整包长度。
- 全量：256 B 块拷入 slot1，flush。
- 差分：`read_slot0`（运行镜像）+ `read_pkg`（补丁）→ `write_slot1`（flash_img）。
- 失败返回负错误，不置 pending、不 Boot。

解码器（156）：HPatchLite + tinyuz，字典 ≤512、还原缓存 256 B、patch 缓存 192 B、解压输入 64 B；无堆分配；开关 `CONFIG_RZI_LORAWAN_FUOTA_DIFF` 默认 `n`。

### 代码位置

- `fuota_install.c`：全量拷贝 / 差分还原分支。
- `fuota_diff.c`：`rzi_fuota_diff_patch_io()` 回调版 + 内存版。
- `lorawan_fuota.c`：`finish_window()` → `install_delta_pkg()`。

### 坑与取舍

| 点 | 说明 |
|---|---|
| 头剥离 | 头在收片写入时就被拆到 scratch，slot1 从偏移 0 起就是镜像；不需要再搬 172 KB |
| 闪存占用 | 差分开启时链接 hpatch_lite + tuz；关闭时符号不存在 |
| RAM | 差分路径全部用固定缓存/栈；bss 约 57 KB / 64 KB，余量小 |

---

## 4. 失败处理（158）

### 为什么

坏包不能被启动，失败必须留痕。设计原则：**擦目标区、不置 pending、记录原因、交给上报**。

### 接口与数据流

| 场景 | 动作 | 原因码 |
|---|---|---|
| 头校验失败 | 擦 `fuota_delta`/slot1 → `FAILED` | 0x11–0x16 |
| 差分安装失败 | 擦 slot1（delta 留到下次 Setup）→ `FAILED` | 0x05 |
| 擦写失败 | 记录 | 0x07 |
| 未收齐 | 上报 | 0x08 |

### 代码位置

- `finish_window()`：校验失败分支、`install_delta_pkg()` 失败分支。
- `status.reason`：`enum rzi_fuota_reason`，数值与 FPort 101 事件码一致。

### 坑与取舍

| 点 | 说明 |
|---|---|
| 不做的事 | 不持久化结果、不重启、不置 pending（设计要"失败也重启上报"，属票 160/17） |
| 状态 | 失败停在 `FAILED`，没有 `REPORTING` |

---

## 5. 状态机（161）

### 为什么

把"配置、收片、校验、安装"分开，外部可观察，失败可定位。

### 数据流

```text
IDLE --入网--> PRECONFIG --首片--> RECEIVING --收齐--> VERIFYING
                                                        |
                                    失败 --> FAILED（擦 + reason）
                                                        |
                                    通过 --> INSTALLING --> 重启
```

8 个枚举：`IDLE/PRECONFIG/RECEIVING/VERIFYING/INSTALLING/RUNNING_NEW/REPORTING/FAILED`。

### 代码位置

- `set_state_locked()`；首片在 `fuota_store()` 置 `RECEIVING`；`finish_window()` 置 `VERIFYING`/`INSTALLING`/`FAILED`。

### 坑与取舍

| 点 | 说明 |
|---|---|
| `RUNNING_NEW`/`REPORTING` | 只定义未驱动（依赖 confirm 与完整上报） |
| `MCUBOOT` | 设计有、实现无：Bootloader 阶段应用侧不可观测 |

---

## 6. 存储与分区（跨任务）

| 分区 | 地址/大小 | 用途 |
|---|---|---|
| slot1 | 外部 `0x00000` / 208 KB | 全量镜像、差分输出 |
| scratch | `0x34000` / 8 KB | 第一扇区分片暂存；**第二扇区 `0x1000` 放 32 B 头** |
| `fuota_delta` | `0x36000` / 32 KB | 差分包载荷 |
| storage | `0x3e000` / 8 KB | NVS（AT 配置） |
| `lora_context` | `0x40000` / 8 KB | LBM 上下文 |

逻辑映射：

```text
[0,32)      -> scratch[0x1000 + off]
[32, ...)   -> slot1 / fuota_delta[off - 32]
```

坑：头本来应该进 storage，但 storage 已被 NVS 占用；改用 scratch 第二扇区属于板级偏差，需在设计中注明。

---

## 7. 与 `_new` 设计的差距

| 项 | 现状 | 归属 |
|---|---|---|
| confirm / 回滚 | 未实现 | 票 160 |
| pending 后抖动 | 未实现 | 票 160 |
| `T_status_hold` | 未实现 | 票 163 |
| 完整上报 | 只有原因码 | 任务 17（无票） |
| Host MCU | RAK1 拒绝 | 设计待定 |
| `cap_delta` | 未置位 | 任务 18 |
| FEC 落 storage | 未实现 | 可选 |

---

## 8. 代码索引与测试

| 功能 | 文件 |
|---|---|
| 头解析/校验 | `src/lorawan/services/fuota/fuota_header.c/.h` |
| 差分解码 | `fuota_diff.c/.h` |
| 安装 | `fuota_install.c/.h` |
| 服务/状态/路由 | `lorawan_fuota.c` |
| 配置 | `zephyr/Kconfig.lorawan` |
| 测试 | `tests/lorawan/fuota_header|fuota_diff|fuota_install` |
| 工具 | `tools/builds/*.py` |

验证入口：`usage_cn.md` 第 5 节（主机测试）、第 9 节（无 LNS 上板）。
