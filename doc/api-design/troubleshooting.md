# Troubleshooting and recovery

Status: normative

<a id="exception-reference"></a>

Distinguish rejection, known failure, and unknown outcomes. These are application recommendations, not automatic SDK retry/recovery.

<a id="failure-evidence"></a>

## 1. Troubleshooting reference

| Symptom | Check | Action |
| --- | --- | --- |
| init error | Return/state, repeated init. | Correct/retry; configuration/resources may have changed. |
| init accepted, no READY | ERROR, callback registration. | Bound the wait; check stop/deinit from an application worker before reinit. |
| Join failed | Return/final JOIN_FAILED, keys, region, mask. | Correct and retry with limits/backoff; no immediate loops. |
| send BUSY | Prior outstanding send. | Wait for its result; use unknown-outcome handling if missing. |
| Other send error | NOT_JOINED/NOT_READY/arguments. | Restore activation or correct input. This request was rejected. |
| TXDONE | Status and error. | SENT means transmitted; CONFIRMED means ACK. Retry per policy. ACK does not prove application-server processing. |
| Accepted send, no TXDONE | RESET/OVERFLOW, blocked callbacks. | Record unknown outcome after deadline. Timeout does not cancel; busy=false does not prove no transmission. Deduplicate before retry. |
| RESET/OVERFLOW | State/session, known outcomes. | Reconcile state and restore session if needed. OVERFLOW does not identify the lost event. |
| stop/deinit error | Traffic, prohibited callback context. | Retain references, retry from allowed thread. Stop may already have succeeded. |

<a id="exception-matrix"></a>

## 2. Send recovery

1. Handle negative send returns as rejection; do not wait for completion for that request.

2. After acceptance, inspect TXDONE status/error. Missing results at the application deadline mean unknown outcome; reconcile state and consider duplicate delivery before retry.

Local timeout does not establish failure or cancel the original request. Use the table for specific cases.

Bound retries by count/time. Select deadlines using region, data rate, and stack scheduling; no universal wait proves failure.

<a id="failure-flow"></a>

## 3. Diagnostic evidence

- Calls: timestamp, API, argument summary, return; send message ID, port, length, type.
- Events: time/type; TXDONE status/error; ERROR code.
- State: successful state/joined/busy queries and query failures.
- Environment: firmware, region, data rate, RESET/OVERFLOW. Never log keys.

Service contracts:
[LoRaWAN](../api/lorawan.md) · [Raw LoRa / FSK](../api/raw-lora.md) ·
[Network Mode](../api/network-mode.md) · [Storage](../api/storage.md) ·
[Power](../api/power.md) · [FUOTA](../api/fuota.md) ·
[Board Architecture](../architecture/board-design.md)

See also: [API design index](index.md).
