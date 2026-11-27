# Device boot

Status: implemented

RZI does not implement a bootloader. MCUBoot source remains a west-fetched
module. RZI owns how customers enable it, how each board is partitioned,
how to flash, and how FUOTA lands in a slot.

See also: [fuota.md](../api/fuota.md).

## Contract (every product board)

1. Target an RZI board or variant. For upstream RAK4631 and RAK3172,
   select `rak4631/nrf52840/rzi` or `rak3172/stm32wle5xx/rzi` so the
   RZI radio and partition configuration is included.
2. Enable `CONFIG_RZI_MCUBOOT=y`. Official samples and the product app
   already do.
3. Build with **sysbuild**. Configuration fails without it, so an
   application-only image is not flashed with no bootloader at 0x0.
4. First flash and recovery: flash the **merged image**
   (`merged_<board>.hex`) from the build directory, not `zephyr.hex`.
5. Later upgrades on dual-slot boards: write a signed MCUBoot image into
   **slot1** (`image-1`), then use `rzi_fuota_apply()` (LoRaWAN FUOTA).
   RZI uses partition names, not a board-specific absolute address.
   Single-slot boards (3372) have no `image-1`, so they do not support
   FUOTA. Reflash the merged image.

Partition tables live on the product board, not in RZI module Kconfig.
Adding a board only adds that board's DTS and flash notes.

| Board | Full name | west target | Status |
|---|---|---|---|
| `rak4631/nrf52840/rzi` | RZI RAK4631 | `rak4631/nrf52840/rzi` | MCUBoot and LoRaWAN samples wired |
| `rak3172/stm32wle5xx/rzi` | RZI RAK3372 | `rak3172/stm32wle5xx/rzi` | Single-slot MCUBoot; on-chip SUBGHZ (USP patch); no FUOTA |
| `rak3372_f` | RAK3372-F | `rak3372_f/stm32wle5xx` | MCUboot scratch swap; secondary slot on MX25L400 |
| `rak3362` | RAK3362 | `rak3362/nrf54l15/cpuapp` | Overwrite-only MCUBoot; SX1262 on SPI22; console and AT on uart20 |

nRF52840 (RAK4631) keeps the upstream Zephyr boot and slot
addresses. Native Zephyr and ArduinoCore-zephyr share this map. The
upstream 192 KB storage region holds the sketch, settings, and the
LoRa Basics Modem context:

| Partition | Address | Size | Use |
|---|---|---|---|
| `mcuboot` | `0x00000` | 64 KB | Sole bootloader |
| `image-0` | `0x10000` | 384 KB | RZI app or Arduino loader |
| `image-1` | `0x70000` | 384 KB | FUOTA / slot swap |
| `user` | `0xd0000` | 160 KB | Arduino LLEXT sketch |
| `storage` | `0xf8000` | 24 KB | Zephyr settings / NVS |
| `lora_context` | `0xfe000` | 8 KB | LoRa Basics Modem context |

The `rzi` snippet installs this map on `rak4631/nrf52840`. Changing
mode only replaces the signed image in slot0. MCUBoot, the sketch,
settings, and the modem context stay in place. Native applications
ignore the `user` partition. An image or sketch built for the previous
48 KB boot, 304 KB slots, and 336 KB sketch at `0xa4000` must be
flashed as a merged hex after a full erase. Its slot trailer now sits
inside the new application image.

STM32WLE5 (RAK3372, 256 KB) is single-slot and has no `image-1` / `user`.
Settings and the LoRa Basics Modem context do not share a partition:

| Partition | Address | Size | Use |
|---|---|---|---|
| `mcuboot` | `0x00000` | 32 KB | Sole bootloader |
| `image-0` | `0x08000` | 208 KB | RZI application |
| `storage` | `0x3c000` | 8 KB | Zephyr settings / NVS |
| `lora_context` | `0x3e000` | 8 KB | LoRa Basics Modem context |

The previous layout used a 216 KB `image-0` and one 8 KB area at `0x3e000`.
That area was documented as settings, and the modem context also fell back
to it. A current image must be flashed as a merged hex after a full erase.
The old context bytes are not a valid session, and the device has to rejoin.
Do not boot a current application with a bootloader that still expects the
216 KB slot: its trailer address now sits in `storage`.

nRF54L15 (RAK3362, 1524 KB RRAM) uses the same dual-slot layout as the
upstream nRF54L15 boards without FLPR:

| Partition | Address | Size | Use |
|---|---|---|---|
| `mcuboot` | `0x000000` | 64 KB | Sole bootloader |
| `image-0` | `0x010000` | 712 KB | RZI application |
| `image-1` | `0x0c2000` | 712 KB | FUOTA / overwrite |
| `storage` | `0x174000` | 28 KB | Zephyr settings / NVS |
| `lora_context` | `0x17b000` | 8 KB | LoRa Basics Modem context |

### RAK3372-F flash layout

The STM32WLE5 internal flash holds MCUboot and the primary image. MCUboot
uses the on-module MX25L400 SPI NOR for the secondary image and scratch.
Offsets below are relative to the start of each flash device.

| Partition | Device | Offset | Size | Use |
|---|---|---|---|---|
| `mcuboot` | Internal flash | `0x00000` | 48 KiB | Bootloader with SPI NOR support |
| `image-0` | Internal flash | `0x0c000` | 208 KiB | Primary image |
| `image-1` | MX25L400 | `0x00000` | 208 KiB | Secondary image |
| `image-scratch` | MX25L400 | `0x34000` | 8 KiB | Swap scratch, two 4 KiB erase sectors |
| `fuota_delta` | MX25L400 | `0x36000` | 32 KiB | Reassembly for descriptor `RAK0` |
| `storage` | MX25L400 | `0x3e000` | 8 KiB | Zephyr settings / NVS |
| `lora_context` | MX25L400 | `0x40000` | 8 KiB | LoRa Basics Modem context |

For development, build with sysbuild and flash
`merged_rak3372_f_stm32wle5xx.hex` so the bootloader and application use
the same partition layout.

The RAK3372-F board defaults to `MCUBOOT_MODE_SWAP_SCRATCH`. RAK4631 and
RAK3362 default to overwrite-only, and RAK3372 defaults to
`MCUBOOT_MODE_SINGLE_APP`.

On RAK3372-F, the STM32WLE5 on-chip SUBGHZ radio
(`st,stm32wl-subghz-radio`) uses the USP driver patch
`zephyr/patches/usp_zephyr/0005-stm32wl-subghz-radio.patch`.

This is **not** compatible with RUI3 / factory Arduino boot: no `AT+BOOT`,
`nrfutil`, or UF2. Moving to RZI is a one-time SWD boot change, the same
class as RAK swapping 4631 versus 4631-R boot.

Development builds use the MCUBoot default key. Production must switch to
your own signing key (this repository does not yet wire CI signing).

## Build and flash: RAK4631

```bash
west build -p always --sysbuild \
  -b rak4631/nrf52840/rzi \
  -d build/rzi-periodical-uplink \
  rzi/samples/lorawan/periodical_uplink
```

Artifact: `build/rzi-periodical-uplink/merged_rak4631_nrf52840_rzi.hex` (boot + signed
application).

Flash this sysbuild with a configured SWD runner:

```bash
west flash -d build/rzi-periodical-uplink
```

For manual programming, use the merged HEX named above. A customer
application supplies its own build wrappers; no external app scripts are
required by the SDK workflow.

Keys go in the application overlay (`app.overlay` or
`boards/rak4631_nrf52840_rzi.overlay`). Do not edit board DTS. Radio,
partitions, and USP SX1262 compatibility are already on `rak4631/nrf52840/rzi`.

## RZI RAK3372 (single slot)

```bash
west build -p always --sysbuild \
  -b rak3172/stm32wle5xx/rzi \
  -d build/rzi-periodical-uplink-3372 \
  rzi/samples/lorawan/periodical_uplink
```

The artifact is `build/rzi-periodical-uplink-3372/merged_rak3172_stm32wle5xx_rzi.hex`.
There is no slot1, so FUOTA is not available.

## RAK3372-F (external secondary slot)

```bash
west build -p always --sysbuild \
  -b rak3372_f/stm32wle5xx \
  -d build/rzi-fuota-3372f \
  rzi/samples/lorawan/fuota
```

The artifact is `build/rzi-fuota-3372f/merged_rak3372_f_stm32wle5xx.hex`.
FUOTA writes the signed image to `image-1` on the MX25L400. MCUboot copies
it over `image-0` on the next boot.
