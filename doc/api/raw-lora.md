# Raw LoRa API

Status: implemented

Public C contract for P2P LoRa, P2P FSK, and radio certification tests.
The ABI is `include/rzi/lora/lora.h` (group 0.1.0).

See also: [error-codes.md](error-codes.md),
[at-command-compatibility.md](../compatibility/at-commands.md),
[lorawan-api.md](lorawan.md).

## Purpose

`rzi_lora_*` owns raw radio operation that is not a LoRaWAN session.
Applications and the AT package share this API. Public types do not expose
USP, LoRa Basics Modem, or Zephyr LoRa driver types.

A return value of zero from `rzi_lora_send()` or `rzi_lora_receive()` means
the request was accepted. Completion is `tx_done` / `rx_done`.

With the USP backend, `tx_done` fires when the packet has left the radio,
and each received packet produces one `rx_done`. Both are delivered from
`rzi_lora_poll()` in the calling thread, so a client that waits for them
must call `rzi_lora_poll()` periodically (the AT package does this from its
process loop).

`rzi_lora_test_rssi()` starts a measurement at the configured frequency;
`rzi_lora_test_get_rssi()` returns `-RZI_ERR_BUSY` until the result is
ready.

## Completion model

Raw LoRa does not use a LoRaWAN-style service worker. Completion follows the
selected backend:

- Zephyr: `lora_send()` / `lora_recv()` block the caller; callbacks publish
  from that thread after the driver returns.
- USP: RAC completes asynchronously; application callbacks are delivered from
  `rzi_lora_poll()` in the caller thread.
- Test: same-thread software publish.

A unified Raw LoRa async dispatcher would only hide Zephyr blocking behind
another thread, add queue latency on every P2P path, and blur ownership with
the LoRaWAN dispatcher. Keep backend-native completion until a product need
requires a driver-level async API.

## P2P low-power policy

Status: implemented for USP; hardware acceptance pending.

P2P LoRa and FSK follow activity-based power ownership:

- RX enabled (timed or continuous): prevent system STOP for the complete
  receive operation, including intervals between packets in continuous RX.
- TX in flight: prevent system STOP until completion, timeout, or cancellation.
- Neither RX nor TX active: permit low power when other system constraints allow.

An initialized P2P service alone is not an active receive operation. Ending
TX must not release the protection of a receive operation that remains active.
Failures, explicit stop, timeout, and deinit balance activity ownership.
USP stop/deinit can return BUSY while RAC completes cancellation; keep polling
and retry. A new receive request returns BUSY while another RX or its
cancellation is pending; stop reception first before starting another window.
Shared-radio ownership must be respected when putting the radio to sleep.

Keeping the CPU out of STOP does not dispatch USP callbacks by itself.
The current poll-based completion contract still applies. AT runs extensions
once at startup and wakes immediately when the USP backend queues a TX or RX
completion, without periodic timer polling or another serial command. Native
clients retain their explicit poll responsibility.
With `CONFIG_RZI_LORA_USP_RX_IRQ_WAKE` (enabled by default on STM32WL SUBGHZ
when `CONFIG_PM=y`), an open receive window does not hold the PM reference
while listening; TX, pending cancellation, and queued completions still do.
See [P2P power ownership](../architecture/power.md#p2p-activity-and-low-power-policy)
for the RUI3 reference and the current implementation gaps. This policy does
not require adding a dedicated raw-radio dispatcher thread.

## Lifecycle

`rzi_lora_init()` selects LoRa or FSK operation and applies the stored
configuration. Calling it while active returns `-RZI_ERR_ALREADY`.

`rzi_lora_stop()` ends receive, transmit, or certification-test activity while
retaining configuration, callbacks, and backend resources needed for a fast
restart. It is idempotent; after success, `init` may select either modulation
again. A backend without a safe stop operation returns
`-RZI_ERR_NOT_SUPPORTED` and remains active.

`rzi_lora_deinit()` first stops the service, then releases backend resources
and clears the callback table. Configuration remains available for the next
`init`. A stop or release failure preserves ownership so the caller may retry.
Callers serialize stop/deinit with send, receive, poll, and their callbacks;
the current raw-radio API does not own a dispatcher thread.

## Backends

| Kconfig | Radio |
|---|---|
| `CONFIG_RZI_LORA_BACKEND_USP` | USP radio access controller (RAC); LBM test mode for certification tests |
| `CONFIG_RZI_LORA_BACKEND_ZEPHYR` | Zephyr `<zephyr/drivers/lora.h>` |
| `CONFIG_RZI_LORA_BACKEND_TEST` | Software loopback |

With USP, P2P send and receive run as RAC transactions at
`RAC_LOW_PRIORITY` (TX) and `RAC_VERY_LOW_PRIORITY` (RX), below every LoRaWAN
radio task. They share the radio with a running LoRaWAN session: LoRaWAN
preempts them, an aborted transmission completes with `-RZI_ERR_BUSY`, and
continuous or timed receive is rescheduled after the preemption until its
deadline. Certification tests (`rzi_lora_test_*`, CW, RSSI) use LoRa Basics
Modem test mode, which suspends LoRaWAN until `rzi_lora_stop()` or the next
P2P request. The FSK format in test mode is fixed by LBM (24-bit sync word
`C1 94 C1`, 40-bit preamble, inverted CCITT CRC, whitening).

The AT package still separates the modes with `AT+NWM` (the production path
reboots). Encryption is XOR of the payload with the stored key and IV, not
RUI3 AES-CTR.

## Limits

- Spreading factor 5 through 12, TX power 5 through 22 dBm.
- Payload up to `RZI_LORA_MAX_PAYLOAD` (255 bytes).
- `timeout_ms` 0 stops receive, 65535 receives continuously.
- Radio tests that the selected backend does not implement return
  `-RZI_ERR_NOT_SUPPORTED`. Backend and driver codes are mapped at the
  boundary and never forwarded as POSIX errno.
