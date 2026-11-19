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
5. Later upgrades on dual-slot boards: USP FUOTA descriptor 0 writes the
   signed image into **slot1** (`image-1`). After the multicast window RZI
   requests a test upgrade and reboots so MCUboot can swap or overwrite.
   Descriptor `0x304B4152` (ASCII `RAK0`) writes `fuota_delta` and does not
   reboot.
   RZI uses partition names, not a board-specific absolute address.
   Single-slot boards (3372) have no `image-1`, so they do not support
   FUOTA. Reflash the merged image.

Partition tables live on the product board, not in RZI module Kconfig.
Adding a board only adds that board's DTS and flash notes.

| Board | Full name | west target | Status |
|---|---|---|---|
| `rak4631/nrf52840/rzi` | RZI RAK4631 | `rak4631/nrf52840/rzi` | MCUBoot and LoRaWAN samples wired |
| `rak3172/stm32wle5xx/rzi` | RZI RAK3372 | `rak3172/stm32wle5xx/rzi` | Single-slot MCUBoot; on-chip SUBGHZ (USP patch); no FUOTA |
| `rak3372_f` | RAK3372-F | `rak3372_f/stm32wle5xx` | Overwrite-only MCUBoot; image-1 on the on-module MX25L400; FUOTA |
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

RAK3372-F uses a larger boot partition than RAK3372, because MCUboot
must drive the MX25L400, and adds the secondary slot on that NOR:

| Partition | Device | Address | Size | Use |
|---|---|---|---|---|
| `mcuboot` | internal | `0x00000` | 48 KB | Sole bootloader (includes the SPI NOR driver) |
| `image-0` | internal | `0x0c000` | 208 KB | RZI application |
| `image-1` | MX25L400 | `0x00000` | 208 KB | FUOTA / overwrite |
| `storage` | MX25L400 | `0x34000` | 8 KB | Zephyr settings / NVS |
| `lora_context` | MX25L400 | `0x36000` | 8 KB | LoRa Basics Modem context |

### RAK3372-F partition migration

The RAK3372-F layout moved settings and LoRa Basics Modem context onto the
MX25L400 so both image slots could grow by 16 KB:

| Layout | `image-0` | `storage` | `lora_context` | `image-1` |
|---|---|---|---|---|
| Legacy (200 KB) | 200 KB at `0x0c000` | 8 KB at `0x3e000` (shared) | shared with `storage` | 200 KB |
| Prior (192 KB) | 192 KB at `0x0c000` | 8 KB at `0x3c000` (internal) | 8 KB at `0x3e000` (internal) | 192 KB |
| Current | 208 KB at `0x0c000` | 8 KB at `0x34000` (MX25) | 8 KB at `0x36000` (MX25) | 208 KB |

The MCUboot image, application image and external slot geometry must use the
same layout. Do not install a current application over an older MCUboot with
a mismatched slot size. The old bootloader will use the previous trailer
offset and can interpret settings or modem context data as swap metadata.

For a device that has ever run a previous layout:

1. Back up credentials that cannot be regenerated. Treat NVS and modem
   context as incompatible with the new layout (and now on external flash).
2. Erase the STM32 internal flash and the used region of the MX25L400.
3. Build with sysbuild and flash
   `merged_rak3372_f_stm32wle5xx.hex`.
4. Power-cycle the board so the STM32 and MX25L400 both start from a known
   hardware state.
5. Re-provision application settings and rejoin the LoRaWAN network.
6. Invalidate or erase any old image in external `image-1` before the first
   FUOTA test with the 208 KB layout.

Flash the merged image whenever the installed MCUboot version or layout is
unknown. Downgrading to a prior layout is also a destructive migration:
use a full erase and the matching merged image, and expect settings to be
lost.

Sysbuild mode comes from the board `Kconfig.sysbuild`: 4631, 3362, and
3372-F are overwrite-only; 3372 is `MCUBOOT_MODE_SINGLE_APP`. The radio is the
STM32WLE5 on-chip SUBGHZ (`st,stm32wl-subghz-radio`). The USP driver
needs `zephyr/patches/usp_zephyr/0005-stm32wl-subghz-radio.patch`.

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
