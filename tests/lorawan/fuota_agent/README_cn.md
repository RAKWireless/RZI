# FUOTA Agent 主机回归测试

[English](README.md)

修改 Agent 状态机、Service 与 Agent 之间的 callback，或 keepalive 恢复时机后，可先跑这套测试。测试在 Zephyr `native_sim` 上执行 RZI 的 FUOTA Service 与 Agent，由测试用 LoRaWAN backend 注入分片完成、逾时、控制回覆完成等事件。开发机即可执行，无需连接 RAK3172-F、网关或 ChirpStack。

## 执行整套测试

在已依项目完成 west 初始化及依赖同步的 Linux／WSL 构建环境执行。需要 west、Zephyr 的 Twister Python 依赖、CMake、Ninja，以及可构建 32 位 native_sim 的 GCC。从 RZI 目录进入含有 `RZI/`、`zephyr/` 和 `.west/` 的 workspace 根目录，再执行以下指令。

```sh
cd "$(west topdir)"
export ZEPHYR_BASE="$PWD/zephyr"
export ZEPHYR_TOOLCHAIN_VARIANT=host
out=build/fuota-agent-readme
python3 zephyr/scripts/twister -T RZI/tests/lorawan/fuota_agent -p native_sim -O "$out"
```

整套通过时，请核对摘要中实际执行且通过的用例数。逐案结果在 `$out/twister.json`。失败时，先依案例名称在 `handler.log` 找到第一个失败的 assertion。编译阶段失败则看 `build.log`。

```sh
find "$out" \( -name handler.log -o -name build.log \) -print
```

**本地整合工作树使用 Docker 时**，可先在 WSL 执行下列指令进入项目使用的构建映像，再从上方的 `export ZEPHYR_BASE` 开始。容器内的工作目录已是 west 根目录。这个工作树与原基线共用 Git 物件，因此同时以唯读方式挂载原基线。

```sh
cd /home/ian/workspace/rzi_fuota_task11
baseline=/home/ian/workspace/rzi_fuota
docker run --rm -it --entrypoint bash --user "$(id -u):$(id -g)" \
  -e HOME=/tmp -v "$PWD:/workdir" -v "$baseline:$baseline:ro" -w /workdir \
  ghcr.io/zephyrproject-rtos/zephyr-build:latest
```

## 单跑一个案例

完成上面的构建后，在同一个 shell 取得模拟程序。修改原始码后请先重跑 Twister，以更新执行档。

```sh
sim=$(find "$out" -type f -name zephyr.exe -print -quit)
"$sim" -test=rzi_fuota_agent::test_class_c_no_query_resumes_one_period_after_hold
```

这个案例模拟原 Class C 提前收齐分片、之后未收到状态查询。它检查首笔 keepalive 是否等到原组播窗口截止、状态保留期结束，再经过一个 keepalive 周期才提交。请同时确认指定案例为 `PASS`，且摘要为 `pass = 1, fail = 0`。其他案例显示 `SKIP` 是单个用例筛选的预期结果。要改跑其他案例，从 [src/main.c](src/main.c) 选取 `ZTEST(rzi_fuota_agent, test_...)` 的第二个参数，替换指令中 `::` 后的名称。

| 要检查的行为 | 可单跑的案例 |
|---|---|
| 收齐后还原原 Class，依 A／C 的规则恢复周期上行 | `test_success_restores_then_resumes_periodic_by_class` |
| 缺片逾时后，先处理状态回覆与删除，再送首笔 keepalive | `test_timeout_status_then_delete_before_first_keepalive` |
| PRECONFIG 中删除分片设置后，能再建立新会话 | `test_delete_fragment_only_preconfig_allows_new_generation` |
| Agent 遗失事件时，避免误报收片完成 | `test_lost_events_never_report_complete` |

## 回调顺序回归基准

部分用例通过公开的 FUOTA API 注册监听器，逐条核对回调顺序、接收方和执行线程。
在 `handler.log` 中搜索 `CB_BASELINE` 可查看场景、序号、通知、监听器、线程和结果；
`CB_TOTAL` 为实际次数。遗漏、多发或顺序变化均会导致用例失败。
观测从 PRECONFIG 前开始，到场景结束后注入的 TIME 事件为止。
该事件用于确认此前的后端事件已处理，其自身不计入次数。

`control.status+delete` 表示此时快照中的两个应答位均已置位。
Class C 自然接收超时用例注入两次状态应答完成；
独立的先查询再删除用例各注入一次。要验证在 CLOSED 中替换监听器，
运行 `test_early_complete_retention_and_repeat`。该用例还会在关闭 IMG_MANAGER
的配置下调用 apply 并在测试线程显式释放数据。

## 如何解读时间与结果

[prj.conf](prj.conf) 把状态保留期缩为 3 秒、keepalive 周期缩为 1 秒，方便快速回归。这套测试可以验证状态转移、callback 与 backend 调用顺序。要验收「ChirpStack 在 10 秒内收到 FragSessionStatusAns」或量测装置实际上行时间，仍需比对装置、LNS 及网关 log。

Agent 的设计及其他测试入口见 [FUOTA 接收 Agent 设计](../../../doc/fuota-agent_cn.md#示例与测试)。
