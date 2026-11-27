# FUOTA Usage Guide (build / sign / package / diff / verify / flash)

Date: 2026-11-26
Applies to: `feat/fuota-diff` (based on `fuota a70713b`)
Environment: Windows + west workspace (run the commands below from the west workspace root; the repository lives in `RZI/`)

---

## 0. One-time setup

| Tool | Location | Purpose |
|---|---|---|
| west / venv python | `.venv\Scripts\` | build and scripts |
| host gcc | `build\host-toolchain\w64devkit\bin\gcc.exe` | host tests |
| hdiffi / hpatchi | `build\hpatch-host\hdiffi.exe` / `hpatchi.exe` | diff generation and restore |
| imgtool | `bootloader\mcuboot\scripts\imgtool.py` | signing and verification |
| test keys | `build\keys\test_3172-f.pem` (private), `test_3172-f-pub.pem` (public) | test only, never release |

> The signing key is held by the build system or the controlled signing environment and must never enter the repository.
> `hdiffi`/`hpatchi` are not in the repository; see `tools/builds/README.md` for the build steps (`west update`, then `make ZLIB=0 LZMA=0` in `modules/HPatchLite`).

## 1. Build

```powershell
# Diff disabled (default)
west build --sysbuild -b rak3372_f/stm32wle5xx -d build/merge-nodiff rzi/samples/lorawan/fuota

# Diff enabled
west build --sysbuild -b rak3372_f/stm32wle5xx -d build/merge-check rzi/samples/lorawan/fuota `
  -- "-DEXTRA_CONF_FILE=$($PWD.Path)\build\diff-enable.conf"
```

> A relative `EXTRA_CONF_FILE` is resolved against the application directory, so pass an absolute path (the command above uses `$PWD`).

`build/diff-enable.conf`:

```
CONFIG_RZI_LORAWAN_FUOTA_DIFF=y
```

Outputs: `build/<dir>/fuota/zephyr/zephyr.bin` (unsigned) and `zephyr.signed.bin` (signed by sysbuild).

### 1.1 Feature switches

| Feature | Switch | Default | Notes |
|---|---|---|---|
| FUOTA component | `CONFIG_RZI_LORAWAN_FUOTA` | `y` in the sample | when off, 156/157/158/161 are not compiled |
| Delta decoder | `CONFIG_RZI_LORAWAN_FUOTA_DIFF` | `n` | links `hpatch_lite`/`tuz_dec` only when on; with it off a `RAK0` session can still be created but installation returns `-RZI_ERR_NOT_SUPPORTED` |
| Header cache fallback offset | `CONFIG_RZI_LORAWAN_FUOTA_HEADER_OFFSET` | `0` | only for boards without a `scratch` partition |
| Build signing | `CONFIG_BOOT_SIGNATURE_KEY_FILE`, `CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION` | board level | see section 2 |

157/158/161 have no separate switch; they are compiled with `CONFIG_RZI_LORAWAN_FUOTA` and take effect automatically.

## 2. Signing (done by the build)

This repository does **not** provide a signing script. Signing is done by the build system: sysbuild calls MCUboot's `imgtool` and produces `zephyr.signed.bin`.

Build configuration (`prj.conf` or an extra conf):

```conf
CONFIG_BOOT_SIGNATURE_KEY_FILE="D:/secure/fuota-sign.pem"
CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="1.2.0+3"
```

- The private key lives only in the controlled signing environment or on the build machine and must never enter the repository;
- the version has four fields, 0-255, and must match `--version` passed to `pack_image`;
- the slot size must match the board: RAK3372-F uses `0x34000`.

When the build machine cannot hold the private key, sign directly in the signing environment:

```powershell
python bootloader\mcuboot\scripts\imgtool.py sign `
  --key D:\secure\fuota-sign.pem --align 8 --version 1.2.0+3 `
  --header-size 0x200 --slot-size 0x34000 --pad-header `
  zephyr.bin zephyr.signed.bin
```

Verify with the public key that the device bootloader holds:

```powershell
python bootloader\mcuboot\scripts\imgtool.py verify `
  --key D:\secure\fuota-sign-pub.pem zephyr.signed.bin
```

Both versions of a diff package must be signed with the same key.

> `D:\secure\...` in the examples is a placeholder; replace it with the real directory of the signing environment.

## 3. Package (full image)

```powershell
python RZI\tools\builds\pack_image.py pack `
  --image out\new.signed.bin --device-code 3 --version 1.2.0+3 `
  -o out\full
```

Outputs: `firmware.bin` (32 B header + image), `firmware.md5`, `manifest.json`.

Rejection vectors:

```powershell
python RZI\tools\builds\pack_image.py vectors `
  --image out\new.signed.bin --device-code 3 --version 1.2.0+3 `
  -o out\vectors
```

Produces four fixtures plus `expected.json` (`same_version` is expected to be **OK**).

## 4. Diff package

Both versions must be signed with the **same key**, otherwise verification fails.

```powershell
python RZI\tools\builds\pack_image.py diff `
  --old out\old.signed.bin --new out\new.signed.bin `
  --device-code 3 --base-version 1.0.0+1 --version 1.2.0+3 `
  --key build\keys\test_3172-f-pub.pem `
  --hdiffi build\hpatch-host\hdiffi.exe --hpatchi build\hpatch-host\hpatchi.exe `
  --imgtool bootloader\mcuboot\scripts\imgtool.py `
  -o out\pkg
```

Internally the tool runs hdiffi, restores with hpatchi, verifies the signature with imgtool, and compares the result byte-for-byte with `new.signed.bin` before writing the package.

**Capacity check**: `fuota_delta` is only 32 KB.

| Scenario | Baseline / change | Patch | Fits? |
|---|---|---|---|
| A | same build + 32 B | 6,427 B | yes |
| C-L1 | `76cb6ce` + 1 function | 3,942 B | yes |
| C-L2 | `76cb6ce` + 4 functions | 4,342 B | yes |
| C-L3 | `76cb6ce` + 4.8 KB of code | 5,124 B | yes |
| B | 11-17 to 11-26 cross-build | **103,500 B** | no, use full |

When the patch exceeds 32 KB the Server must fall back to the full image.

## 5. Verification

### 5.1 Package self-check

```powershell
python RZI\tools\builds\pack_image.py verify out\pkg
```

Checks `@RAK`, `image_length`, CRC32 (payload only) and MD5. Flipping any byte reports a CRC/MD5 mismatch.

### 5.2 Host tests

```powershell
$gcc = "build\host-toolchain\w64devkit\bin\gcc.exe"

# Header rules (13 checks)
& $gcc -std=c11 -I RZI\src\lorawan\services\fuota `
  RZI\tests\lorawan\fuota_header\host\main.c `
  RZI\src\lorawan\services\fuota\fuota_header.c -o build\fuota-header-host\fuota_header_test.exe
build\fuota-header-host\fuota_header_test.exe

# Vector replay (legal/same-version accepted; device/CRC/base rejected)
& $gcc -std=c11 -I RZI\src\lorawan\services\fuota `
  RZI\tests\lorawan\fuota_header\host\vectors.c `
  RZI\src\lorawan\services\fuota\fuota_header.c -o build\fuota-header-host\fuota_vectors_test.exe
build\fuota-header-host\fuota_vectors_test.exe out\vectors

# Install (full 4 B + diff 171840 B)
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

### 5.3 FEC replay

```powershell
& build\host-toolchain\w64devkit\bin\gcc.exe -std=c11 -O1 `
  -DFRAG_MAX_NB=300 -DFRAG_MAX_SIZE=64 `
  -I modules\lib\usp\protocols\lbm_lib\smtc_modem_core\lorawan_packages\fragmented_data_block_transport\v1.0.0 `
  RZI\tests\lorawan\fuota_fec\src\main.c -o build\fuota_fec_host\replay.exe
build\fuota_fec_host\replay.exe RZI\tests\lorawan\fuota_fec\replay.bin
```

Expected: `FEC_BOUNDARIES_PASS cases=7`, `FEC_RECONSTRUCTION_PASS ... mismatches=0`.

> The ASan build requires Linux; Windows mingw does not support it.

## 6. Flashing

```powershell
west flash -d build\real-new --runner jlink
```

Watch the serial log; FPort 101 carries the event codes (0x11-0x16 etc.).

Notes:

- **confirm is not implemented** (ticket 160); with swap, an unconfirmed image **rolls back** after boot. Do not mistake that for a failed upgrade.
- Before flashing a diff package, make sure `cap_delta` is set, otherwise the Server will not send diff packages.
- The header cache lives in the second scratch sector (`0x1000`); MCUboot overwrites it while swapping, which is harmless after installation.

## 7. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `No module named imgtool` | imgtool not installed in the venv | add `--imgtool bootloader\mcuboot\scripts\imgtool.py` |
| `No signature found for the given key` | the two images were signed with different keys | re-sign both with the same private key |
| `corrupt patch` / `west patch` fails | patch was converted to CRLF | make sure `.gitattributes` contains `*.patch -text` |
| Same-version package rejected | stale AC / old code | the new design allows same-version; the device must not check `target_version` |
| Diff package too large | patch > 32 KB | Server falls back to the full image, or grow `fuota_delta` |
| `FUOT` vectors | old format | regenerate with `@RAK` |

## 8. Related documents

- Design (English): `design_en.md`
- Chinese version: `usage_cn.md`
- Ticket and process notes: `notes/` (not committed)

## 9. Board verification without a full LNS

Without an LNS the **FUOTA session itself cannot be verified** (session setup, fragment reception, checks, installation, StatusAns and FPort 101 all depend on downlinks/uplinks). Only device-level sanity checks are possible.

### 9.1 What can be verified

| Item | Method | Pass criterion |
|---|---|---|
| Flash and boot | `west flash -d build\real-new --runner jlink` | board boots, AT serial responds |
| Version | read 32 B at `0x0800C000` with STM32CubeProgrammer | `magic=0x96F3B83D`, version bytes match the flashed image |
| Repeated reset | manual reset / power cycle 10 times | boots every time, version does not roll back |
| AT service | send `AT` and version commands (see AT manual) | normal response |
| Bootloader present | read 32 B at `0x08000000` | not all `0xFF` |

> Note: `prj.conf` sets `CONFIG_LOG=n` and `CONFIG_CONSOLE=n`; the UART is owned by AT, so FUOTA state logs are not visible by default. To see logs, add an overlay enabling LOG and use RTT or a second UART, otherwise it will fight with AT for the port.

### 9.2 What cannot be verified (requires LNS)

- Session setup / Class C
- Fragment completion and FEC
- The actual rejection path of the 32 B header check
- Delta patch and installation
- StatusAns wait
- FPort 101 reporting
- confirm / rollback

### 9.3 Three ways to fill the gap

| Approach | Cost | Coverage |
|---|---|---|
| Minimal LNS (ChirpStack + gateway/single-channel forwarder) | medium | full chain |
| Test mode: feed a package over AT into `fuota_delta`/scratch and trigger verify+install | about 1 day of development | most of 161/158/157/156 |
| Host tests + boot verification only | low | logic and boot, no air interface |

### 9.4 Board record template

```
Date:
commit:
Image: build/real-new/.../merged_*.hex  MD5:
Boots: 10  Failures: 0
slot0 magic: 0x96F3B83D   Version: 1.2.0+3
AT response: OK
Conclusion:
```

### 9.5 Minimal checklist for today (about 20 minutes)

```powershell
# 1) Flash a known version (1.2.0+3, diff enabled)
west flash -d build\real-new --runner jlink
#   without J-Link: west flash -d build\real-new --runner pyocd

# 2) Confirm AT responds on the serial port (baud per board configuration)
#    send: AT

# 3) Read the slot0 header (internal flash 0x0800C000)
pyocd commander -t stm32wle5jcix -c "read32 0x0800C000 8"
# Expected: first word 0x96F3B83D; bytes 20..23 are 01 02 00 03 (1.2.0.3)

# 4) Reset / power cycle 10 times, confirm AT responds each time

# 5) Record
#    commit SHA, MD5 of the merged hex, boot count, version
```

Pass criteria: flash succeeds, AT responds every time, slot0 magic and version are correct, 10 boots without rollback.
