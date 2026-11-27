# FUOTA 使用指南（构建 / 签名 / 打包 / 差分 / 验证 / 上板）

日期：2026-11-26  
适用：`feat/fuota-diff`（基线 `fuota a70713b`）  
环境：Windows + west 工作区（以下命令在 west 工作区根目录执行，仓库位于 `RZI/`）

---

## 0. 一次性准备

| 工具 | 位置 | 用途 |
|---|---|---|
| west / venv python | `.venv\Scripts\` | 构建、脚本 |
| 主机 gcc | `build\host-toolchain\w64devkit\bin\gcc.exe` | 主机测试 |
| hdiffi / hpatchi | `build\hpatch-host\hdiffi.exe` / `hpatchi.exe` | 差分包生成与还原 |
| imgtool | `bootloader\mcuboot\scripts\imgtool.py` | 签名/验签 |
| 测试密钥 | `build\keys\test_3172-f.pem`（私钥）、`test_3172-f-pub.pem`（公钥） | 仅测试，禁止发布 |

> 签名密钥由构建系统或受控签名环境持有，不能进入仓库。
> `hdiffi`/`hpatchi` 不在仓库里，构建方法见 `tools/builds/README.md`（`west update` 后进 `modules/HPatchLite` 执行 `make ZLIB=0 LZMA=0`）。

## 1. 构建

```powershell
# 差分关闭（默认）
west build --sysbuild -b rak3372_f/stm32wle5xx -d build/merge-nodiff rzi/samples/lorawan/fuota

# 差分开启
west build --sysbuild -b rak3372_f/stm32wle5xx -d build/merge-check rzi/samples/lorawan/fuota `
  -- "-DEXTRA_CONF_FILE=$($PWD.Path)\build\diff-enable.conf"
```

> `EXTRA_CONF_FILE` 的相对路径会被解析到应用目录，必须给绝对路径（上面用 `$PWD`）。

`build/diff-enable.conf`：

```
CONFIG_RZI_LORAWAN_FUOTA_DIFF=y
```

产物：`build/<dir>/fuota/zephyr/zephyr.bin`（未签名）、`zephyr.signed.bin`（sysbuild 已签）。

### 1.1 功能开关

| 功能 | 开关 | 默认 | 说明 |
|---|---|---|---|
| FUOTA 组件 | `CONFIG_RZI_LORAWAN_FUOTA` | 样例 `y` | 关掉后 156/157/158/161 都不编译 |
| 差分解码 | `CONFIG_RZI_LORAWAN_FUOTA_DIFF` | `n` | 打开才链接 `hpatch_lite`/`tuz_dec`；关闭时 `RAK0` 会话仍可建立，但安装返回 `-RZI_ERR_NOT_SUPPORTED` |
| 头缓存回退偏移 | `CONFIG_RZI_LORAWAN_FUOTA_HEADER_OFFSET` | `0` | 仅没有 `scratch` 分区的板子使用 |
| 构建签名 | `CONFIG_BOOT_SIGNATURE_KEY_FILE`、`CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION` | 板级 | 见 §2 |

157/158/161 没有单独开关，随 `CONFIG_RZI_LORAWAN_FUOTA` 一起编译并自动生效。

## 2. 签名（由构建完成）

本仓库**不提供签名脚本**。签名由构建系统完成：sysbuild 调用 MCUboot 的 `imgtool`，产出 `zephyr.signed.bin`。

构建配置（`prj.conf` 或 extra conf）：

```conf
CONFIG_BOOT_SIGNATURE_KEY_FILE="D:/secure/fuota-sign.pem"
CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="1.2.0+3"
```

- 私钥只在受控签名环境或构建机，不能进仓库；
- 版本四段 0–255，必须与 `pack_image` 的 `--version` 一致；
- 槽参数与板级一致：RAK3372-F 是 `0x34000`。

构建机不能持有私钥时，在签名环境直接用 imgtool：

```powershell
python bootloader\mcuboot\scripts\imgtool.py sign `
  --key D:\secure\fuota-sign.pem --align 8 --version 1.2.0+3 `
  --header-size 0x200 --slot-size 0x34000 --pad-header `
  zephyr.bin zephyr.signed.bin
```

验签（必须用设备 Bootloader 对应的公钥）：

```powershell
python bootloader\mcuboot\scripts\imgtool.py verify `
  --key D:\secure\fuota-sign-pub.pem zephyr.signed.bin
```

差分的新旧两版必须用同一把密钥签名。

> 示例中的 `D:\secure\...` 是占位路径，按签名环境的实际目录替换。

## 3. 打包（全量）

```powershell
python RZI\tools\builds\pack_image.py pack `
  --image out\new.signed.bin --device-code 3 --version 1.2.0+3 `
  -o out\full
```

产物：`firmware.bin`（32B 头 + 镜像）、`firmware.md5`、`manifest.json`。

拒绝向量：

```powershell
python RZI\tools\builds\pack_image.py vectors `
  --image out\new.signed.bin --device-code 3 --version 1.2.0+3 `
  -o out\vectors
```

生成 4 个 fixture + `expected.json`（`same_version` 的预期是 **OK**）。

## 4. 差分

两版镜像必须用**同一把密钥**签名，否则验签会失败。

```powershell
python RZI\tools\builds\pack_image.py diff `
  --old out\old.signed.bin --new out\new.signed.bin `
  --device-code 3 --base-version 1.0.0+1 --version 1.2.0+3 `
  --key build\keys\test_3172-f-pub.pem `
  --hdiffi build\hpatch-host\hdiffi.exe --hpatchi build\hpatch-host\hpatchi.exe `
  --imgtool bootloader\mcuboot\scripts\imgtool.py `
  -o out\pkg
```

工具内部会：hdiffi 生成补丁 → hpatchi 还原 → imgtool 验签 → 与 `new.signed.bin` 逐字节比对 → 才写出包。

**容量检查**：`fuota_delta` 只有 32 KB。

| 场景 | 基线/改动 | 补丁 | 能否装下 |
|---|---|---|---|
| A | 同一构建 + 32 B | 6,427 B | ✅ |
| C-L1 | `76cb6ce` + 1 个函数 | 3,942 B | ✅ |
| C-L2 | `76cb6ce` + 4 个函数 | 4,342 B | ✅ |
| C-L3 | `76cb6ce` + 4.8 KB 代码 | 5,124 B | ✅ |
| B | 11-17 → 11-26 跨构建 | **103,500 B** | ❌ 应走全量 |

超过 32 KB 时 Server 必须回退全量包。

## 5. 验证

### 5.1 包自检

```powershell
python RZI\tools\builds\pack_image.py verify out\pkg
```

检查 `@RAK`、`image_length`、CRC32（仅载荷）、MD5。篡改任一字节会报 CRC/MD5 不匹配。

### 5.2 主机测试

```powershell
$gcc = "build\host-toolchain\w64devkit\bin\gcc.exe"

# 头规则（13 条）
& $gcc -std=c11 -I RZI\src\lorawan\services\fuota `
  RZI\tests\lorawan\fuota_header\host\main.c `
  RZI\src\lorawan\services\fuota\fuota_header.c -o build\fuota-header-host\fuota_header_test.exe
build\fuota-header-host\fuota_header_test.exe

# 向量回放（合法/同版本接受；机型/CRC/基线拒绝）
& $gcc -std=c11 -I RZI\src\lorawan\services\fuota `
  RZI\tests\lorawan\fuota_header\host\vectors.c `
  RZI\src\lorawan\services\fuota\fuota_header.c -o build\fuota-header-host\fuota_vectors_test.exe
build\fuota-header-host\fuota_vectors_test.exe out\vectors

# 安装（全量 4B + 差分 171840B）
& $gcc -std=c11 -DRZI_FUOTA_DIFF_HOST -D_IS_USED_SHARE_hpatch_lite_types=1 `
  -I RZI\tests\lorawan\fuota_diff\host -I RZI\src\lorawan\services\fuota `
  -I modules\HPatchLite\HDiffPatch\libHDiffPatch\HPatchLite `
  -I modules\HPatchLite\tinyuz\decompress `
  RZI\tests\lorawan\fuota_install\host\main.c `
  RZI\src\lorawan\services\fuota\fuota_install.c `
  RZI\src\lorawan\services\fuota\fuota_diff.c `
  modules\HPatchLite\HDiffPatch\libHDiffPatch\HPatchLite\hpatch_lite.c `
  modules\HPatchLite\tinyuz\decompress\tuz_dec.c `
  -o build\fuota-install-host\fuota_install_test.exe
build\fuota-install-host\fuota_install_test.exe
```

### 5.3 FEC 回放

```powershell
& build\host-toolchain\w64devkit\bin\gcc.exe -std=c11 -O1 `
  -DFRAG_MAX_NB=300 -DFRAG_MAX_SIZE=64 `
  -I modules\lib\usp\protocols\lbm_lib\smtc_modem_core\lorawan_packages\fragmented_data_block_transport\v1.0.0 `
  RZI\tests\lorawan\fuota_fec\src\main.c -o build\fuota_fec_host\replay.exe
build\fuota_fec_host\replay.exe RZI\tests\lorawan\fuota_fec\replay.bin
```

预期：`FEC_BOUNDARIES_PASS cases=7`、`FEC_RECONSTRUCTION_PASS ... mismatches=0`。

> ASan 版需要 Linux；Windows mingw 不支持。

## 6. 上板

```powershell
west flash -d build\real-new --runner jlink
```

用串口观察日志；FPort 101 看事件码（0x11–0x16 等）。

注意：

- **confirm 未实现**（票 160），swap 模式下新镜像启动后不确认会**回滚**，不要误判为升级失败。
- 差分上板前确认 `cap_delta` 已置位，否则服务端不会下发差分。
- 头缓存位于 `scratch` 第二扇区（`0x1000`）；换槽时会被 MCUboot 覆盖，安装完成后无影响。

## 7. 常见问题

| 现象 | 原因 | 处理 |
|---|---|---|
| `No module named imgtool` | venv 未装 imgtool | 加 `--imgtool bootloader\mcuboot\scripts\imgtool.py` |
| `No signature found for the given key` | 两版镜像签名密钥不一致 | 用同一私钥重签两版 |
| `corrupt patch` / `west patch` 失败 | 补丁被 CRLF 转换 | 确认 `.gitattributes` 有 `*.patch -text` |
| 同版本包被拒绝 | 旧 AC / 旧代码 | 新设计允许同版本；设备端不应再判 `target_version` |
| 差分包装不下 | 补丁 > 32 KB | Server 回退全量，或扩 `fuota_delta` |
| `FUOT` 向量 | 旧格式作废 | 用 `@RAK` 重新生成 |

## 8. 相关文档

- 设计说明（中文）：`design_cn.md`
- 英文版：`usage_en.md`
- 票据与过程记录：`notes/`（不随代码提交）
## 9. 没有完整 LNS 时的上板验证

没有 LNS 时，**无法验证 FUOTA 会话本身**（会话建立、收片、校验、安装、StatusAns、FPort 101 都依赖下行/上行）。能做的只有"设备基础状态"验证。

### 9.1 可验证项

| 项 | 方法 | 通过标准 |
|---|---|---|
| 烧录与启动 | `west flash -d build\real-new --runner jlink` | 板子正常启动，AT 串口有响应 |
| 版本正确 | STM32CubeProgrammer 读 `0x0800C000` 起 32 B | `magic=0x96F3B83D`，版本字节 = 烧录版本 |
| 反复复位 | 手动复位 / 断电 10 次 | 每次都能启动，版本不回退（未触发回滚） |
| AT 服务 | 串口发 `AT` 及版本类命令（以 AT 手册为准） | 有正常应答 |
| Bootloader 存在 | 读 `0x08000000` 起 32 B | 非全 `0xFF` |

> 注意：`prj.conf` 里 `CONFIG_LOG=n`、`CONFIG_CONSOLE=n`，**串口被 AT 占用**，默认看不到 FUOTA 状态日志。需要日志时另加 overlay 打开 LOG，并改用 RTT 或第二路 UART，否则会和 AT 抢串口。

### 9.2 不能验证项（必须 LNS）

- 会话建立 / Class C
- 分片收齐与 FEC
- 32B 头校验的实际拒绝路径
- 差分 Patch 与安装
- StatusAns 等待
- FPort 101 上报
- confirm / 回滚

### 9.3 三种补齐方式

| 方式 | 成本 | 能覆盖 |
|---|---|---|
| 最小 LNS（ChirpStack + 网关/单通道转发） | 中 | 全链路 |
| 测试模式：AT 口灌包到 `fuota_delta`/scratch 再触发校验安装 | 开发约 1 天 | 161/158/157/156 大部分 |
| 只做主机测试 + 上板启动验证 | 低 | 逻辑与启动，不含空口 |

### 9.4 上板记录模板

```
日期：
commit：
烧录镜像：build/real-new/.../merged_*.hex  MD5：
启动次数：10  失败：0
slot0 magic：0x96F3B83D   版本：1.2.0+3
AT 响应：正常
结论：
```
### 9.5 最小验证清单

```powershell
# 1) 烧录已知版本（1.2.0+3，差分开启）
west flash -d build\real-new --runner jlink
#   没有 J-Link 时：west flash -d build\real-new --runner pyocd

# 2) 串口确认 AT 有响应（波特率按板级配置）
#    发送：AT

# 3) 读 slot0 头（内部 Flash 0x0800C000）
pyocd commander -t stm32wle5jcix -c "read32 0x0800C000 8"
# 预期：第一个字 0x96F3B83D；字节 20..23 为 01 02 00 03（1.2.0.3）

# 4) 复位/断电 10 次，每次确认 AT 有响应

# 5) 记录
#    commit SHA、merged hex 的 MD5、启动次数、版本
```

通过标准：烧录成功、AT 每次都有响应、slot0 头 magic 与版本正确、10 次启动无回滚。