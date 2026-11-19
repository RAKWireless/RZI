# 5. Error sources and reporting

Status: normative

<a id="exceptions"></a>

Failures arrive through returns or callbacks. Distinguish synchronous rejection from results after acceptance, then identify RZI, USP, or Zephyr as the source.

<a id="failure-model"></a>

## 5.1 Sources and delivery

Problems discovered during calls return directly. Later results use defined service events. The source determines conversion; delivery determines where the caller handles it.

| Source | Synchronous return | Asynchronous event |
| --- | --- | --- |
| RZI | Argument/state/resource errors. | Services with events report runtime failures, e.g. ERROR/OVERFLOW. |
| USP | rzi_usp_result converts modem codes. | Backend converts modem events to JOIN_FAILED/TXDONE etc. |
| Zephyr | Backend/foundation maps negative errno. | Workers/drivers publish public results such as TXDONE/NOT_SENT. |

Storage uses returns rather than invented events. Applications handle RZI codes/events, not vendor codes/errno. Each declaration defines its error set.

<a id="failure-side-effects"></a>

## 5.2 Direct service errors

Send validates port/pointer/type with INVALID, length with TOO_LARGE, and session with check_joined before backend entry. The dispatcher can also report ERROR/OVERFLOW at runtime.

Source: [lorawan.c](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/lorawan.c).

```text
	if ((data == NULL && size != 0U) || port == 0 || port > 223 || !message_type_valid(type)) {
		return -RZI_ERR_INVALID;
	}
	if (size > RZI_LORAWAN_MAX_PAYLOAD) {
		return -RZI_ERR_TOO_LARGE;
	}
	rc = check_joined();
	if (rc != 0) {
		return rc;
	}
	rzi_lorawan_mac_commands_on_uplink();
	return rzi_lorawan_backend.base.send(port, data, size, type);
```

The service produces these errors directly at the call site.

<a id="failure-recheck"></a>

## 5.3 USP result conversion

Immediate USP errors become negative RZI codes: BUSY becomes BUSY; an undifferentiated FAIL becomes IO.

Source: [usp_platform.c](https://github.com/RAKWireless/RZI/blob/main/src/usp/usp_platform.c).

```c
int rzi_usp_result(smtc_modem_return_code_t rc)
{
	switch (rc) {
	case SMTC_MODEM_RC_OK:
		return 0;
	case SMTC_MODEM_RC_NOT_INIT:
		return -RZI_ERR_NOT_READY;
	case SMTC_MODEM_RC_INVALID:
	case SMTC_MODEM_RC_INVALID_STACK_ID:
		return -RZI_ERR_INVALID;
	case SMTC_MODEM_RC_BUSY:
		return -RZI_ERR_BUSY;
	case SMTC_MODEM_RC_NO_TIME:
	case SMTC_MODEM_RC_NO_EVENT:
		return -RZI_ERR_NO_DATA;
	case SMTC_MODEM_RC_FAIL:
	default:
		return -RZI_ERR_IO;
	}
}
```

The private platform layer converts synchronous USP results.

The [Zephyr adapter](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/backend/zephyr/lorawan_backend_zephyr_priv.h) maps ENETDOWN to NOT_JOINED and other codes through [rzi_err_from_errno](https://github.com/RAKWireless/RZI/blob/main/src/common/common_err.c). Storage/Raw LoRa also use the generic converter. Native errno must not leak.

<a id="recovery-policy"></a>

## 5.4 Handle returns and events

Check the call result first. Handle negative codes per contract. After asynchronous acceptance, wait for completion. Inspect JOIN_FAILED, TXDONE/NOT_SENT, or ERROR fields. Loss/timeouts mean unknown outcomes, not confirmed failure.

BUSY rejects this send; zero followed by TXDONE/NOT_SENT reports failure after acceptance. See [troubleshooting](troubleshooting.md#exception-reference).

<a id="error-catalog"></a>

## 5.5 Error catalog

Values come from [err.h](https://github.com/RAKWireless/RZI/blob/main/include/rzi/err.h). Fallible integer APIs return zero or a negative catalog value, e.g. -RZI_ERR_BUSY (-1004). Each declaration defines its actual set.

| Code | Value | Meaning |
| --- | --- | --- |
| `RZI_SUCCESS` | 0 | Accepted or completed, as defined by the function. |
| `RZI_ERR_INVALID` | 1000 | Invalid argument/enum/port/payload. |
| `RZI_ERR_ISR` | 1001 | Thread-context API called from ISR. |
| `RZI_ERR_NOT_READY` | 1002 | Service/backend not ready. |
| `RZI_ERR_ALREADY` | 1003 | Repeated init/registration. |
| `RZI_ERR_BUSY` | 1004 | Conflicting work in progress. |
| `RZI_ERR_NOT_SUPPORTED` | 1005 | Unsupported capability. |
| `RZI_ERR_NO_RESOURCE` | 1006 | Slots/buffers/tables full. |
| `RZI_ERR_NOT_FOUND` | 1007 | Missing handle/key/session/image. |
| `RZI_ERR_NO_DATA` | 1008 | Downlink/beacon/image-size data unavailable. |
| `RZI_ERR_TOO_LARGE` | 1009 | Input too long. |
| `RZI_ERR_TIMEOUT` | 1010 | Timed out. |
| `RZI_ERR_OVERFLOW` | 1011 | Queue overflow. |
| `RZI_ERR_IO` | 1012 | Undifferentiated backend/hardware failure. |
| `RZI_ERR_BAD_MESSAGE` | 1013 | Corrupt CRC/protocol contents. |
| `RZI_ERR_NO_DEVICE` | 1014 | Device missing. |
| `RZI_ERR_NOT_JOINED` | 1015 | Active session required. |
| `RZI_ERR_DENIED` | 1016 | Caller/context not permitted. |
| `RZI_ERR_INVALID_STATE` | 1017 | Lifecycle forbids the operation. |

See also: [API design index](index.md).
