# West patches

Status: implemented

RZI owns the temporary compatibility patches required by its pinned backend
dependencies. Applications import RZI through `west.yml` and do not copy
these files.

See also: [boot.md](boot.md), [usp-stack.md](usp-stack.md).

The patch definition is stored in:

```text
rzi/
└── zephyr/
    ├── patches.yml
    └── patches/
        ├── usp/
        │   ├── 0001-export-secure-element-session-keys.patch
        │   ├── 0002-ral-sx126x-frequency-limits.patch
        │   ├── 0003-user-channel-mask.patch
        │   ├── 0004-reuse-license-annotations.patch
        │   └── 0015-optional-certification-and-test-mode.patch
        ├── usp_zephyr/
        │   ├── 0005-stm32wl-subghz-radio.patch
        │   ├── 0006-zephyr-4.5-warning-fixes.patch
        │   ├── 0007-reuse-license-annotations.patch
        │   ├── 0012-skip-disabled-log-formatting.patch
        │   ├── 0013-gate-2g4-region-on-radio-support.patch
        │   ├── 0014-usp-idle-until-init.patch
        │   ├── 0016-optional-certification-and-test-mode.patch
        │   └── 0017-context-erase-follows-device-page.patch
        └── zephyr/
            ├── 0009-stm32wl-locked-pm-resume.patch
            ├── 0010-stm32-spi-resume-config.patch
            ├── 0011-spi-nor-dpd-resume-delay.patch
            └── 0012-stm32wl-lpuart-wakeup-drain.patch
```

Pins are USP v1.2.1 and Zephyr 4.5.0. v1.2.1 already contains the former
Zephyr 4.4 warning fixes, the LR-FHSS source path, removal of the duplicate
`xiao_nrf54l15` board, and the global SX1262 compile definition.
`0005` adds standalone STM32WL on-chip SUBGHZ transceiver support
(`stm32wl_subghz_hal.c` and `stm32wl_subghz_board.c`) modeled on ST's
`stm32wlxx_hal_subghz.c`, keeping `sx126x_hal.c` and `sx126x_board.c`
exclusively for external SX126x radios. `0006`
replaces the deprecated `<zephyr/sys_clock.h>` include and
`SPI_OP_MODE_MASTER` on Zephyr 4.5. `usp/0001` exports session keys
from the software secure element. The LR11xx crypto engine cannot export key
material and returns an error. `usp/0002` turns the SX126x frequency-limit
redefinition in `ral_sx126x.h` into `#if`/`#else`; upstream's redefinition is
a warning that twister's `-Werror` turns into a build failure on every SX1262
board. `usp/0003` adds `smtc_modem_set_channel_mask()` and
`smtc_modem_get_channel_mask()`. The upstream API has no way to limit the
US915, AU915 or CN470 channels before join. The patch ANDs the user mask into
every default channel mask (join, the post-join defaults, and the CN470
session plan). It also makes the US915/AU915 channel rotation ignore disabled
channels; otherwise a mask stalls the rotation. A network LinkADRReq and the
ADR back-off still replace the mask until the next join, as LoRaWAN requires.
`usp/0004` and `usp_zephyr/0007` add a `REUSE.toml` to each module so that
`west spdx` records licenses and copyrights for Semtech sources, which carry
the Clear BSD text without SPDX tags; see [sbom.md](../development/sbom.md).
`zephyr/0009` keeps STM32WL wake ISRs pending until STOP resume restores
clocks and system-managed devices (upstream
[#122293](https://github.com/zephyrproject-rtos/zephyr/pull/122293)).
`zephyr/0010` clears the STM32 SPI driver's cached configuration so the next
transfer restores CR1/CR2 after STOP
([#122292](https://github.com/zephyrproject-rtos/zephyr/pull/122292)).
`zephyr/0011` busy-waits for a SPI NOR deep-power-down delay when the caller
cannot yield, which is the idle thread during that locked resume
([#122312](https://github.com/zephyrproject-rtos/zephyr/pull/122312)).
`usp_zephyr/0014` lets the USP thread sleep until the modem is initialized
when `CONFIG_RZI_USP_IDLE_UNTIL_INIT=y`. `usp/0015` and `usp_zephyr/0016`
gate the LoRaWAN certification package and radio test mode on
`CONFIG_LORA_BASICS_MODEM_CERTIFICATION` and `CONFIG_LORA_BASICS_MODEM_TEST_MODE`
(both default `y`) so flash-constrained builds can omit them. `usp_zephyr/0017`
sizes the LoRaWAN context read-modify-write buffer to at least 4 KiB and erases
the page reported by the flash device that holds the context partition. The
internal STM32WL page is 2 KiB, which an MX25L400 rejects because its sector
is 4 KiB. `zephyr/0012` drains
the LPUART FIFO after STOP, before the PM core restores clocks. Apply it
after `0009`.
Drop each patch on its own once the pinned Zephyr revision contains it.

## STM32WL SUBGHZ design rationale

The STM32WL on-chip radio speaks the SX126x command set, so `usp` reuses
`sx126x_driver`, `ral_sx126x`, and `ralf_sx126x` unchanged. Everything below
`sx126x_hal.h` and `lora_lbm_transceiver.h`, however, differs from an external
SX1261/1262/1268 over SPI + GPIOs. `0005` follows two rules so the two targets
never interfere:

1. **Build-time file split instead of `#if` branches in upstream SX126x files.**
   When `st,stm32wl-subghz-radio` is enabled, `drivers/usp/CMakeLists.txt`
   compiles `stm32wl_subghz_hal.c` and `stm32wl_subghz_board.c` *instead of*
   `sx126x_hal.c` and `sx126x_board.c`. Upstream `sx126x_hal.c` stays untouched
   and `sx126x_board.c` carries no STM32WL code, avoiding patch conflicts when
   bumping `usp_zephyr` and preventing regressions on external SX1262 boards
   (RAK4631, RAK3162).
   - `stm32wl_subghz_hal.c` implements the synchronous radio-access contract
     (`sx126x_hal_write`, `sx126x_hal_read`, `sx126x_hal_reset`,
     `sx126x_hal_wakeup`) plus RF front-end switch GPIOs (`tx-enable-gpios`,
     `rx-enable-gpios`, `antenna-enable-gpios`).
   - `stm32wl_subghz_board.c` owns the single `DT_DRV_COMPAT` Zephyr device
     instance (putting the radio into warm-start sleep at boot so `STDBY_RC`
     does not block `STOP2`), EXTI 44 interrupt dispatch, `PM_DEVICE` clock
     gating, and the `__weak` override of `ral_sx126x_bsp_get_tx_cfg()` for
     `RFO_HP` / `RFO_LP` (on-chip SUBGHZ has no `tx-power-cfg` table in
     Devicetree).

2. **Direct `SUBGHZSPI` register access modeled on ST's `stm32wlxx_hal_subghz.c`
   instead of Zephyr's generic `spi_stm32` driver.**
   - *Why `spi_stm32` deadlocks on `SetSleep`:* `spi_stm32_complete()` spins on
     `SPI_SR_BSY` with no timeout before releasing CS. Sending `SetSleep`
     (`0x84`) puts the internal SUBGHZ block to sleep before `BSY` clears,
     hanging the caller forever.
   - *Why `SUBGHZSPI` is dedicated:* `SUBGHZSPI` is an internal point-to-point
     link to the radio with no external pins or bus sharing. Driving `CR1`/`CR2`
     and `DR` directly (`TXE` -> write -> `RXNE` -> read) with NSS controlled
     via `LL_PWR_SelectSUBGHZSPI_NSS()` matches `HAL_SUBGHZ_ExecSetCmd()` /
     `ExecGetCmd()` byte for byte, removes the `CONFIG_SPI` dependency when no
     external SPI bus is used, and automatically restores `CR1`/`CR2` when STOP
     mode clears peripheral registers.
   - *Sleep and wakeup semantics:* Matching `HAL_SUBGHZ_ExecSetCmd()` and
     `SUBGHZ_CheckDeviceReady()`, `SetSleep` releases NSS and returns without
     polling `RFBUSYS`, both `SetSleep` and `SetRxDutyCycle` mark the radio as
     sleeping, and waking from sleep asserts NSS until `LL_PWR_IsActiveFlag_RFBUSYS()`
     clears without clocking dummy `GET_STATUS` SPI bytes.
   - *Level IRQ and TCXO reset:* The internal radio IRQ on EXTI line 44 stays
     high until `ClearIrqStatus` (`0x02`), so the ISR masks the NVIC line and
     `sx126x_hal_write()` re-arms it after `ClearIrqStatus`. Because `LL_RCC_RF_EnableReset()`
     also resets the `PB0-VDD_TCXO` regulator, `sx126x_hal_reset()` parks
     `SYSCLK` on HSI whenever the CPU is clocked from that HSE TCXO, reprograms
     `SET_TCXOMODE`, and restores the HSE PLL.

## Applying the patches

Run the commands from the west workspace root after `west update`:

```sh
west patch -sm rzi clean
west patch -sm rzi apply --roll-back
```

`-sm rzi` (`--src-module rzi`) tells `west patch` to read the patch definition
from the RZI module. It does not mean that the patches target RZI. Each entry's
`module` field in `patches.yml` selects the destination module: `zephyr`,
`usp_zephyr`, or `usp`.

`apply` is not idempotent, so automated build wrappers should run `clean`
before `apply`. `--roll-back` cleans modules already modified by the current
operation if a later patch fails.

Warning: `clean` runs the configured `checkout-command`, currently
`git checkout .`, in every destination module. It discards uncommitted tracked
changes in those modules. `clean-command` removes only the untracked files
created by the patch stack (`REUSE.toml`, `stm32wl_subghz_hal.c`, and
`stm32wl_subghz_board.c`), so the next `apply` can add them again; other
untracked files are preserved.

## Maintaining the manifest

After adding or modifying a patch, regenerate the manifest from the RZI
repository root:

```sh
python3 tools/patches/generate-patch-manifest.py
```

The generator computes checksums using the same normalized-text algorithm as
Zephyr's `west patch`; this avoids CRLF-related checksum differences.

Patches are temporary compatibility measures. Remove a patch once the pinned
upstream revision contains the equivalent fix, and prefer immutable upstream
commits for RZI releases.
