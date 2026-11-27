# 编译机上的两个工具

这两个文件只在电脑上运行，不编进设备固件。签名由构建系统完成（sysbuild 调用 imgtool），`pack_image.py` 负责给已签名镜像套 32 字节头。

| 文件 | 票 | 干什么 |
| --- | --- | --- |
| `pack_image.py` | 165、169 | 给已经签过名的镜像套上业务头。`pack` 是整包，`diff` 是补丁 |
| `fuota_package.py` | 共用 | 没有命令。负责拼这 32 字节头、算 CRC 和 MD5、写文件 |

头的每个字段见 `doc/fuota-diff/design_cn.md` 第 1 节。设计原文是 `03_LoRa_FUOTA详细技术设计_new.md` 第 2 节。

```text
zephyr.bin
  sysbuild / imgtool          签名，得到 zephyr.signed.bin
  pack_image.py pack         整份已签名镜像 + 32 字节头
  pack_image.py diff         旧已签名镜像与新已签名镜像的补丁 + 同一套头
```

三层检查各管一件事：

| 检查 | 谁做 | 回答的问题 |
| --- | --- | --- |
| MCUboot 签名 | 构建时由 imgtool 签名，设备启动时再查一次 | 镜像是不是我们的私钥签的，有没有被改过 |
| CRC-32 | 165 / 169 写在头里 | 这 32 字节头和后面的内容在传输中有没有坏 |
| MD5 | 写在 `firmware.md5` | 整个文件在上传或保存时有没有坏 |

签名不保护这 32 字节头。设备将来要先看头和 CRC，再丢掉这 32 字节，把剩下的内容交给 MCUboot。

## 先构建 hdiffi / hpatchi

这两个程序不在仓库里，由 `west.yml` 钉死的 HPatchLite（`6c9e469`）编译得到。

```powershell
# 1) 首次拉取模块
west update

# 2) 编译（只要 tinyuz，不编 zlib/lzma）
Push-Location ..\modules\HPatchLite
make ZLIB=0 LZMA=0
Pop-Location

# 3) 放到脚本默认查找的位置
New-Item -ItemType Directory -Force ..\build\hpatch-host | Out-Null
Copy-Item ..\modules\HPatchLite\hdiffi.exe, ..\modules\HPatchLite\hpatchi.exe ..\build\hpatch-host\
```

- Windows 没有 `make` 时，可用 `builds\vc\HPatchLite.sln`（Visual Studio）或 WSL；
- 默认 `ZLIB=1 LZMA=1` 会把 zlib/lzma 一起编进来，FUOTA 用不到，建议关掉。

## 其它依赖

| 依赖 | 获取方式 |
|---|---|
| imgtool | `west update` 后位于 `bootloader\mcuboot\scripts\imgtool.py`，无需编译 |
| 签名密钥 | 由构建系统或受控签名环境持有，不能进仓库；换钥要重编 Bootloader |
| 主机 gcc | 任意 mingw-w64 / 系统 gcc |
## 签名（由构建完成）

本仓库**不提供签名脚本**。`zephyr.bin` 由构建系统签名：sysbuild 调用 MCUboot 的 `imgtool`，得到 `zephyr.signed.bin`，再交给 `pack_image.py`。

构建配置（`prj.conf` 或 extra conf）：

```conf
CONFIG_BOOT_SIGNATURE_KEY_FILE="D:/secure/fuota-sign.pem"
CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="1.2.3+4"
```

- 私钥只在受控签名环境或构建机，不能进仓库；
- 版本四段都是 0 到 255；
- 槽参数必须和板级一致：RAK3372-F 是 208 KiB，即 `0x34000`。

如果构建机不能持有私钥，在签名环境里直接用 imgtool：

```text
python bootloader\mcuboot\scripts\imgtool.py sign ^
  --key D:\secure\fuota-sign.pem --align 8 --version 1.2.3+4 ^
  --header-size 0x200 --slot-size 0x34000 --pad-header ^
  zephyr.bin zephyr.signed.bin
```

签完可以自己验一次：

```text
python bootloader\mcuboot\scripts\imgtool.py verify ^
  --key D:\secure\fuota-sign-pub.pem zephyr.signed.bin
```

设备上的 Bootloader 只保存对应公钥；换钥要重新编译 Bootloader。两版差分必须用同一把密钥签名。

> 示例中的 `D:\secure\...` 是占位路径，按实际签名环境替换。

## 165：全量包

全量包把整份 `zephyr.signed.bin` 原样放进升级包，前面加 32 字节，说明这是哪款设备、要升到哪个版本。魔数是 `@RAK`。头里 `package_kind=0`，`diff_algo=0`，`base_version=0.0.0.0`。CRC32 只覆盖头后面的 `image_length` 字节。整包另有一份 MD5。

```text
python tools/builds/pack_image.py pack ^
  --image zephyr.signed.bin --device-code 1 --version 1.2.3+4 ^
  -o out\fuota

python tools/builds/pack_image.py verify out\fuota
```

| 文件 | 内容 |
| --- | --- |
| `firmware.bin` | 32 字节头 + 整份已签名镜像 |
| `firmware.md5` | 这个文件的 MD5 |
| `manifest.json` | 给人看的机型、版本、长度、MD5 |

缺文件、版本不是四段、机型不在 0 到 255，不会写成品。写完再读回，头或 CRC 或 MD5 不对就删掉这三个文件。

没有 MCUboot 章的文件只能加 `--unsigned`。输出改名为 `firmware.unsigned.bin`，不能当发布包。

`vectors` 另外写四个故意错误的包。它们是父票 FUOTA-37、实现票 FUOTA-161 的输入：设备收齐后、安装前用这些包练习拒绝。161 的实现在设备侧，不在这个目录。合法包仍在输出目录，错误包在 `vectors\`。

```text
python tools/builds/pack_image.py vectors ^
  --image zephyr.signed.bin --device-code 1 --version 1.2.3+4 ^
  -o out\fuota
```

| 文件 | 故意错在哪里 |
| --- | --- |
| `vectors/device_mismatch.bin` | 机型字节不是这台设备 |
| `vectors/header_crc.bin` | 内容被改过，CRC 没重算 |
| `vectors/same_version.bin` | 和合法包相同。同版本允许安装，这个文件应当通过 |
| `vectors/base_mismatch.bin` | 全量包的基线版本被写成 `9.9.9.9` |

## 169：差分包

差分包不运送整份新镜像。它拿旧的已签名镜像和新的已签名镜像，算出一份小补丁，再套上同一套 32 字节头。头里 `package_kind=1`，`diff_algo=1`，`base_version` 是旧版本，`image_length` 是补丁的长度，不是新镜像的长度。

写文件之前，脚本在临时目录里自己把补丁打回去：用 `hpatchi -s-256` 还原，结果必须和新签名镜像每一个字节都相同，并且 `imgtool verify` 通过。任何一步失败都不留 `firmware.bin`。

`hdiffi.exe` 和 `hpatchi.exe` 在 `build\hpatch-host\`，由 `west.yml` 钉死的 HPatchLite `6c9e469` 在电脑上编译。它们不进固件。

```text
python tools/builds/pack_image.py diff ^
  --old old.signed.bin --new new.signed.bin ^
  --device-code 1 ^
  --base-version 1.0.0+0 --version 1.2.3+4 ^
  --key D:\secure\fuota-sign.pem ^
  --hdiffi ..\build\hpatch-host\hdiffi.exe ^
  --hpatchi ..\build\hpatch-host\hpatchi.exe ^
  --imgtool ..\bootloader\mcuboot\scripts\imgtool.py ^
  -o out\fuota-diff
```

`--version` 和 `--base-version` 必须就是签这两份镜像时用的版本。脚本不会从镜像里猜。`--key` 必须是签新镜像对应的公钥，用来检查还原结果。

## 怎么确认工具没坏

在 `RZI` 仓库根目录：

```text
python tools\tests\test_fuota_package.py
```

结束时应是 `Ran 8 tests` 和 `OK`。中间的 `error:` 是拒绝用例自己打印的原因。屏幕上出现这些字，同时最后一行是 `OK`，就是通过。

## 相关文档

- 设计：`doc/fuota-diff/design_cn.md`
- 使用：`doc/fuota-diff/usage_cn.md`
- 票据与过程记录：`doc/fuota-diff/notes/`（不提交）
