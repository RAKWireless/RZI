# USP stack

Status: implemented

How RZI sits on top of Semtech USP (`modules/lib/usp`) and its Zephyr port
(`modules/usp_zephyr`), with three call chains traced through real functions.

See also: [architecture.md](overview.md),
[lorawan-backends.md](../api/backends.md),
[lora-api.md](../api/raw-lora.md),
[at-framework.md](../api/at-framework.md).

## Repositories

| Repository | Path | Role | Zephyr-specific |
|---|---|---|---|
| `rzi` | `rzi/` | SDK module: AT adapters, services, backends | Yes |
| `usp_zephyr` | `modules/usp_zephyr` | Zephyr module: HAL implementation, radio BSP, USP thread, Kconfig/CMake glue, boards, samples | Yes |
| `usp` | `modules/lib/usp` | Portable core: RAC, radio planner, RAL/RALF, chip drivers, protocols | No |

LoRa Basics Modem (LBM) is not a separate repository. It is the protocol
library at `modules/lib/usp/protocols/lbm_lib`, and it runs on top of RAC.

`usp_zephyr/west.yml` pins `usp` (currently `v1.2.1`) at `modules/lib/usp`.
`usp` has no Zephyr build logic of its own:
`usp_zephyr/modules/usp/CMakeLists.txt` sets `RAC_LIB_DIR` and `LBM_LIB_DIR`
and picks the sources to compile from Kconfig. LBM is compiled only with
`CONFIG_USP_LORA_BASICS_MODEM=y`.

`usp` declares the HAL interface (`smtc_modem_hal.h`).
`usp_zephyr/modules/smtc_modem_hal/` implements it. Porting to another OS
means rewriting only that layer and the radio BSP.

## Layers

```text
RZI AT adapters -> public services -> private backends -> USP platform
                                         |                |
                              LoRaWAN -> LBM              | shared rac_api_mutex
                              Raw LoRa -> RAC             |
                                         \                /
                                 RAC / radio planner
                                         |
                                     RAL / RALF
                                         |
                            selected radio driver / HAL
                                         |
                     usp_zephyr: SPI, GPIO, IRQs, timers, flash
                                         |
                                Zephyr -> radio hardware

The examples below use the LR20xx/LR2021 path to illustrate BSP calls.
RAK SX1262/STM32WL builds select their own driver and board wiring.
```

LoRaWAN goes through LBM. Raw LoRa / FSK (P2P) skips LBM and submits
transactions straight to RAC. LBM and the RZI P2P backend are therefore peer
RAC users that queue in the same radio planner.

| Layer | Repository | Example functions | Owns |
|---|---|---|---|
| AT commands | rzi | `handle_psend()` | Parsing, `OK` / `ERROR` replies |
| Services | rzi | `rzi_lorawan_send()`, `rzi_lora_send()` | Validation and state; backend-agnostic (USP, Zephyr, test) |
| Backends | rzi | `usp_send()`, `submit()` | Mapping RZI semantics to Semtech APIs, locking |
| Platform | rzi | `rzi_usp_lock()`, `modem_event_callback()` | USP init, mutual exclusion with the USP thread, LBM event dispatch |
| LBM | usp | `smtc_modem_request_uplink()` → `lr1mac_core_payload_send()` | LoRaWAN MAC |
| RAC and radio planner | usp | `smtc_rac_submit_radio_transaction()`, `rp_task_enqueue()` | Multi-protocol arbitration and scheduling |
| RAL and drivers | usp | `ralf_setup_lora()` → `lr20xx_radio_lora_set_modulation_params()` | Radio parameters to chip commands |
| Zephyr port | usp_zephyr | `lr20xx_hal_write()`, USP thread, `smtc_modem_hal_*` | SPI, GPIO, timers, thread, HAL implementation |
| OS | zephyr | `spi_transceive_dt()` | Peripheral access |

## Example 1: `AT+SEND` (LoRaWAN uplink)

The AT thread only queues the uplink. The USP thread drives the radio.

```text
AT thread: rzi_lorawan_send(port, data, size, type)
  -> service validates arguments and session
  -> rzi_lorawan_backend.base.send()
  -> USP enter / rzi_usp_lock()
  -> smtc_modem_request_uplink()
  -> success: tx_pending=true
  -> USP finish / unlock / wake engine
  -> return acceptance (AT replies OK)

Later, in the USP thread:
  smtc_modem_run_engine() -> MAC task -> radio planner
  -> RAL / selected driver -> BSP HAL -> Zephyr SPI -> radio
```

## Example 2: `AT+PSEND` (raw LoRa, no LBM)

```text
AT thread: rzi_lora_send(payload, size)
  -> service applies optional XOR
  -> raw backend send / submit(TX_ID)
  -> fill RAC context with modulation, frequency, power, timing
  -> smtc_rac_submit_radio_transaction()

USP thread: smtc_rac_run_engine()
  -> radio planner arbitration
  -> selected driver / HAL -> radio

Higher-priority LoRaWAN work can delay or preempt P2P traffic.
```

Because both paths share the radio planner, `on_tx_done()` in
`lora_backend_usp.c` maps `RP_STATUS_TASK_ABORTED` (preempted by LoRaWAN or
cancelled by `stop()`) to `-RZI_ERR_BUSY`.

## Example 3: TX-done interrupt back to RZI

```text
Radio IRQ -> board callback -> modem HAL -> radio planner IRQ flag
  -> wake USP thread

USP thread:
  RAC handles IRQ -> MAC completion / RX windows
  -> LBM raises TXDONE -> modem_event_callback()
  -> smtc_modem_get_event() -> backend handle_event()
  -> clear tx_pending -> publish backend result to fixed queue

RZI dispatcher:
  dequeue -> convert to public event -> subscribers / AT events
```

## Locking

Every `smtc_*` call from RZI must sit between `rzi_usp_lock()` and
`rzi_usp_unlock()`. The lock is `rac_api_mutex`, the same mutex the USP thread
holds around `smtc_modem_run_engine()` and `smtc_rac_run_engine()`, so
`usp_platform.c` enforces `BUILD_ASSERT(CONFIG_USP_THREADS_MUTEXES)`.
`rzi_usp_unlock()` also calls `smtc_modem_hal_wake_up()` so the USP thread
processes newly queued work immediately instead of waiting for its next
timeout.

`modem_event_callback()` and the handler installed with
`rzi_usp_set_event_handler()` run in the USP thread with the lock held. They
must not block on anything that waits for the USP thread.

## Relation to the usp_zephyr samples

`samples/usp/` in `usp_zephyr` is split by the layer each sample exercises:

| Directory | Uses | RZI equivalent |
|---|---|---|
| `lbm/` (for example `periodical_uplink`) | LBM `smtc_modem_*`; RAC runs underneath | `lorawan/backend/usp` |
| `rac/` (for example `multiprotocol`) | LBM and another protocol sharing the radio through RAC | LoRaWAN and P2P together |
| `sdk/` (for example `ping_pong`) | Raw modulations submitted to RAC, no LoRaWAN | `lora/backend/lora_backend_usp.c` |

## Radio BSP variants under `drivers/usp/sx126x/`

External SX1261/1262/1268 boards (such as RAK4631 and RAK3162) and STM32WL
on-chip SUBGHZ boards (RAK3172, RAK3372-F) share Semtech's portable
`sx126x_driver` and `ral_sx126x`, but select mutually exclusive BSP/HAL sources
in `drivers/usp/CMakeLists.txt` (via `usp_zephyr/0005-stm32wl-subghz-radio.patch`):

| Target | Devicetree compatible | Compiled sources | Bus, busy, reset, IRQ |
|---|---|---|---|
| External SX126x | `semtech,sx126{1,2,8}-new` | `sx126x_hal.c` + `sx126x_board.c` + `sx126x_ral_bsp.c` | Zephyr `spi_dt_spec`, GPIO `busy` / `reset` / `dio1`, `dio2-as-rf-switch` |
| STM32WL SUBGHZ | `st,stm32wl-subghz-radio` | `stm32wl_subghz_hal.c` + `stm32wl_subghz_board.c` + `sx126x_ral_bsp.c` | Direct `SUBGHZSPI` + `PWR` NSS, `PWR_SR2.RFBUSYS`, `RCC` RF reset, EXTI 44, GPIO RF switch |

STM32WL bypasses Zephyr's generic `spi_stm32` driver and follows ST's
`stm32wlxx_hal_subghz.c` directly because `SetSleep` puts the internal radio to
sleep before `SPI_SR_BSY` clears (which hangs `spi_stm32`), and waking from
sleep only requires holding internal NSS low until `RFBUSYS` clears. See
[west-patch.md](west-patch.md#stm32wl-subghz-design-rationale)
for the full design rationale.
