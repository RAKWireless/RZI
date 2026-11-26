# 编译机上的三个工具

这三个文件只在电脑上运行，不编进设备固件。顺序是先签名，再打包。全量包和差分包共用同一套 32 字节头。

| 文件 | 票 | 干什么 |
| --- | --- | --- |
| `sign_image.py` | 167 | 给编译出来的 `zephyr.bin` 盖章，并马上检查章是真的 |
| `pack_image.py` | 165、169 | 给已经盖过章的镜像套上业务头。`pack` 是整包，`diff` 是补丁 |
| `fuota_package.py` | 共用 | 没有命令。负责拼这 32 字节头、算 CRC 和 MD5、写文件 |

头的每个字段见 `doc/fuota-diff/cn/FUOTA-165-167-169.md`。设计原文是 `03_LoRa_FUOTA详细技术设计.md` 第 2 节。

```text
zephyr.bin
  sign_image.py              盖章，得到 zephyr.signed.bin
  pack_image.py pack         整份盖章镜像 + 32 字节头
  pack_image.py diff         旧盖章镜像与新盖章镜像的补丁 + 同一套头
```

三层检查各管一件事：

| 检查 | 谁做 | 回答的问题 |
| --- | --- | --- |
| MCUboot 签名 | 167 盖章，设备启动时再查一次 | 镜像是不是我们的私钥签的，有没有被改过 |
| CRC-32 | 165 / 169 写在头里 | 这 32 字节头和后面的内容在传输中有没有坏 |
| MD5 | 写在 `firmware.md5` | 整个文件在上传或保存时有没有坏 |

签名不保护这 32 字节头。设备将来要先看头和 CRC，再丢掉这 32 字节，把剩下的内容交给 MCUboot。

## 167：签名和验签

签名就是盖章。`zephyr.bin` 只是编译结果，谁都可以改。`sign_image.py` 调用 MCUboot 的 `imgtool`，用一把私钥给它盖章，写出 `zephyr.signed.bin`。设备上的 Bootloader 只保存对应的公钥，能查出章是假的或镜像被改过，但推不出私钥。

验签是盖完章立刻自己检查一遍。脚本用同一把私钥再跑 `imgtool verify`。检查失败就删掉刚写出的文件，避免把一个坏镜像交给后面的打包。这一步发生在编译机上，不是设备在收包。

私钥用 `--key` 或环境变量 `RZI_SIGNING_KEY`，文件必须放在本仓库外面。名字是 `root-ec-p256.pem`、`root-ed25519.pem`、`root-rsa-2048.pem` 的示例钥匙会被拒绝。设计要的是 ECDSA-P256 或 Ed25519。当前日常构建里的 `zephyr.signed.bin` 是 sysbuild 用示例钥匙 `root-rsa-2048.pem` 盖的章，那不是 167 的交付物。

```text
python tools/builds/sign_image.py ^
  --key D:\secure\fuota-sign.pem ^
  --version 1.2.3+4 ^
  --header-size 0x200 ^
  --slot-size 0x34000 ^
  --align 8 ^
  --imgtool ..\bootloader\mcuboot\scripts\imgtool.py ^
  zephyr.bin -o zephyr.signed.bin
```

`--header-size`、`--slot-size`、`--align` 必须和这块板的 MCUboot 一致。RAK3372-F 的槽是 208 KiB，即 `0x34000`。版本写成 `1.2.3+4` 或 `1.2.3.4`，四段都是 0 到 255。

自己做主机试验时，可以在仓库外生成一把钥匙：

```text
python ..\bootloader\mcuboot\scripts\imgtool.py keygen -k D:\secure\fuota-sign.pem -t ecdsa-p256
```

这把钥匙只能验证用它自己签过的镜像。已经编进当前 MCUboot 的是那把 RSA 示例公钥，换钥匙之后要重新编译 Bootloader，设备才会认。

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

`vectors` 另外写四个故意错误的包。它们是父票 FUOTA-37、实现票 FUOTA-161 的输入：设备收齐后、安装前用这些包练习拒绝。161 不在这个目录，现在也还没做。合法包仍在输出目录，错误包在 `vectors\`。

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

`--version` 和 `--base-version` 必须就是签这两份镜像时用的版本。脚本不会从镜像里猜。`--key` 必须是签新镜像的那一把，用来检查还原结果。

## 怎么确认这三个工具没坏

在 `RZI` 仓库根目录：

```text
D:\Git_code\rzi-workspace\.venv\Scripts\python.exe tools\tests\test_fuota_package.py
```

结束时应是 `Ran 12 tests` 和 `OK`。中间的 `error:` 是拒绝用例自己打印的原因。屏幕上出现这些字，同时最后一行是 `OK`，就是通过。

## 相关文档

- 验收对照：`doc/fuota-diff/cn/PM验收对照.md`
- 移植与内存：`doc/fuota-diff/cn/FUOTA-156.md`
- apply 接口：`doc/fuota-diff/cn/FUOTA-157.md`
- Patch 调用与失败处理：`doc/fuota-diff/cn/FUOTA-158.md`
- 头校验与状态机：`doc/fuota-diff/cn/FUOTA-161.md`
- 设计总结：HPatchLite 仓库 `docs/差分接入架构.md`
