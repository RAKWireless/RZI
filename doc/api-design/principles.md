# 2. API design principles

Status: normative

<a id="usage"></a>

<a id="contracts"></a>

<a id="execution"></a>

Public APIs must define usage contracts. The template/example cover isolation, support, completion, ownership, and updates. Chapter 3 traces execution; Chapter 5 covers errors.

<a id="contract-template"></a>

## Contract requirements and comment template

Contracts state caller obligations and interface guarantees. Check every category for new/changed APIs. Omit inapplicable items with reviewer agreement. These are requirements, not a claim that all current APIs satisfy them.

| Contract item | Questions to answer | Location |
| --- | --- | --- |
| Preconditions | Ranges, units, nullability, initialization, READY/joined state, capabilities. | `@param`, behavior, `@retval` |
| Context | ISR/thread/callback use, blocking, delivery thread. | `@note`, callbacks |
| Success | Completed read, saved/applied setting, or accepted request? Error triggers? | Behavior, `@retval` |
| Completion | Events, correlation, cancellation, stop/reset, loss, guarantees. | Behavior, events/callbacks |
| Concurrency | Multiple callers/requests, reentry, conflicts, impact on existing work. | Behavior, `@retval`, `@note` |
| Ownership | Input copying/reuse; output mutability/lifetime; user-data release after unregister. | `@param`, fields/callbacks |
| Persistence | RAM/flash/backend ordering, restart behavior, absence of persistence. | Behavior, `@note` |
| Failure state | Output validity, rollback, retries, state/resource recovery. | `@retval`, recovery |

### Public header comment template

Use `@brief` for one sentence. Prose explains completion, concurrency, and side effects. Replace bracketed placeholders and remove inapplicable items.

```c
/**
 * @brief [Describe the operation.]
 * @param input [Range, units, nullability, ownership, lifetime.]
 * @param output [Capacity, meaning, validity after failure.]
 *
 * [Preconditions and capabilities.]
 * [Synchronous success; completion events and delivery limits.]
 * [Concurrency/reentry restrictions and conflicts.]
 * [RAM, persistence, backend ordering; failure state and recovery.]
 *
 * @retval 0 [Exact success condition.]
 * @retval -RZI_ERR_... [Trigger and relevant side effects.]
 * @note [Execution context, blocking, callback thread.]
 */
```

### Review checklist

- **Completeness:** answer all eight categories or explain inapplicability. Missing text does not imply no side effects or asynchronous results.
- **Consistency:** compare declarations, services, and backends for ranges, errors, copying, failure side effects, and enabled/disabled optional paths.
- **Evidence:** use static checks, tests, or hardware acceptance for input reuse, non-destructive conflicts, and unchanged caches after save failures. Separate pending/passed checks.

Send covers acceptance, completion, concurrency, and copying; it does not define universal behavior. See Section 2.5 and Chapter 5 for partial failure.

<a id="api-example"></a>

## Example: a complete public API contract

The following send comment/declaration defines acceptance, completion, concurrency, errors, and ownership.

Public declaration: [lorawan.h](https://github.com/RAKWireless/RZI/blob/main/include/rzi/lorawan/lorawan.h).

```c
/**
 * @brief Request an application uplink.
 *
 * @param port LoRaWAN application port in the range 1 through 223.
 * @param data Payload copied by the backend before returning.
 * @param size Number of payload bytes.
 * @param type Confirmed or unconfirmed message type.
 * A return value of zero means the payload was copied and accepted, not that
 * it has been transmitted. Completion is reported through on_event() as
 * RZI_LORAWAN_EVENT_TXDONE. Only one outstanding uplink is currently supported.
 *
 * @retval 0 Request accepted.
 * @retval -RZI_ERR_INVALID The port, payload pointer, or message type is invalid.
 * @retval -RZI_ERR_TOO_LARGE The payload exceeds RZI_LORAWAN_MAX_PAYLOAD or
 *         the current data rate.
 * @retval -RZI_ERR_NOT_READY The service or backend is not ready.
 * @retval -RZI_ERR_NOT_JOINED No network session is active.
 * @retval -RZI_ERR_BUSY Another uplink is outstanding.
 * @retval -RZI_ERR_NOT_SUPPORTED The backend cannot send.
 * @retval -RZI_ERR_IO The backend operation failed.
 * @retval -RZI_ERR_ISR Called from an ISR.
 *
 * @note Thread context only. The backend copies data before returning.
 */
__must_check int rzi_lorawan_send(uint8_t port, const uint8_t *data, size_t size,
				  enum rzi_lorawan_message_type type);
```

| Comment section | Purpose | Example |
| --- | --- | --- |
| `@brief` | Operation summary. | Request an uplink. |
| `@param` | Ranges, units, ownership, enums. | Ports 1-223; copy before return; byte count; type. |
| Detailed behavior | Acceptance, completion, outstanding work. | Zero means accepted; TXDONE reports completion; one outstanding uplink. |
| `@retval` | Success/failure conditions. | NOT_JOINED means no session; BUSY means outstanding traffic. |
| `@note` and declaration | Context and checked returns. | Thread only; `__must_check` requires checking results. |

The caller knows how to supply inputs, interpret success, and obtain completion. The implementations connect these promises to execution.

### From declaration to transmission

The function bodies show validation, state checks, dispatch through `base.send`, and modem submission. Completion becomes TXDONE later.

Public entry: [lorawan.c](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/lorawan.c).

```c
int rzi_lorawan_send(uint8_t port, const uint8_t *data, size_t size,
		     enum rzi_lorawan_message_type type)
{
	int rc;

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
}
```

The service validates arguments/length and `check_joined()` checks state. It then processes MAC requests and calls `base.send`, returning synchronous submission status.

USP submission: [lorawan_backend_usp.c](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/backend/usp/lorawan_backend_usp.c).

```text
static int usp_send(uint8_t port, const uint8_t *data, size_t size,
		    enum rzi_lorawan_message_type type)
{
	int rc = rzi_lorawan_usp_enter();

	if (rc != 0) {
		return rc;
	}
	if (tx_pending) {
		return rzi_lorawan_usp_finish(-RZI_ERR_BUSY);
	}
	rc = rzi_usp_result(smtc_modem_request_uplink(
		STACK_ID, port, type == RZI_LORAWAN_MSG_CONFIRMED, data, size));
	if (rc == 0) {
		tx_pending = true;
	}
	return rzi_lorawan_usp_finish(rc);
}
```

`rzi_lorawan_usp_enter()` locks the context. `tx_pending` limits concurrency and is set only after successful modem submission.

On TXDONE, [handle_event](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/backend/usp/lorawan_backend_usp.c) clears the flag and publishes the converted result. [event_from_backend](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/lorawan.c) creates the public event. See [3.3](call-paths.md#call-send).

<a id="lifecycle"></a>

## 2.1 Decouple APIs from stack implementation

Applications use RZI types, errors, and events. Stack functions/objects remain private.

The application calls send; the service validates and dispatches through `base.send`; the backend invokes the stack.

The complete example above demonstrates this boundary.

<a id="raw-completion"></a>

## 2.2 Define optional support boundaries

An unavailable optional operation returns NOT_SUPPORTED rather than success without execution.

Lifecycle, joining, and sending are baseline operations. Class B/multicast and other packages use domain tables/build options. Check capabilities and API results.

Backend table with simplified comments: [contract](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/backend/lorawan_backend.h).

```c
struct rzi_lorawan_backend_api {
    uint32_t capabilities;  /* Optional capability bits. */
    struct rzi_lorawan_base_ops base;  /* Embedded baseline table. */

    /* Optional tables may be NULL. */
    const struct rzi_lorawan_network_ops *network;
    const struct rzi_lorawan_class_b_ops *class_b;
    const struct rzi_lorawan_session_ops *session;
    const struct rzi_lorawan_mac_ops *mac;
    const struct rzi_lorawan_multicast_ops *multicast;
    const struct rzi_lorawan_certification_ops *certification;
#ifdef CONFIG_RZI_LORAWAN_FUOTA
    const struct rzi_lorawan_fuota_ops *fuota;
#endif
};
```

A NULL domain table/member means unavailable. The base table also permits NULL get_class and get_protocol_version members.

Source: [contract](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/backend/lorawan_backend.h). See [Chapter 4](backend-coverage.md#backends) for groups/selection.

<a id="callback-lifetime"></a>

## 2.3 Separate acceptance from completion

Send returning zero means accepted. TXDONE reports the later result. Acceptance is not completion.

Fragments omit initialization and callback registration.

```c
/* Step 1: submit an uplink. */
int rc = rzi_lorawan_send(port, data, size, type);
if (rc == 0) {
    /* Accepted; wait for TXDONE. */
} else {
    /* Rejected; handle rc. */
}

/* Step 2: inspect results in the registered callback. */
if (event->type == RZI_LORAWAN_EVENT_TXDONE) {
    /* Inspect both event->tx.status and event->tx.error. */
}
```

The fragments run in different phases: synchronous submission and asynchronous completion.

Status distinguishes sent, acknowledged, and not-sent outcomes; error adds detail when available. Missing TXDONE or a local timeout does not prove failure. See [Chapter 5](errors.md#exceptions).

Source: [send/events](https://github.com/RAKWireless/RZI/blob/main/include/rzi/lorawan/lorawan.h). See [3.3](call-paths.md#call-send) for conversion.

<a id="error-codes"></a>

## 2.4 Define callback ownership and lifetime

The dispatcher invokes on_event. DOWNDATA bytes belong to RZI and are valid only during the callback. Copy them for later work or another thread.

Source: [downlink declaration](https://github.com/RAKWireless/RZI/blob/main/include/rzi/lorawan/lorawan.h).

```text
	/** Dispatcher-owned payload valid only during the callback. */
	const uint8_t *data;
```

The public structure defines payload ownership and lifetime.

Copy bytes within the callback, then hand off the copy. Do not retain the pointer. Return quickly. Wait for in-flight callbacks after unregistering before freeing user data.

<a id="settings"></a>

## 2.5 Define update order and partial failure

Document save/apply timing and rollback. An error can follow completed partial work.

Join saves auto_join, interval, and attempts before start controls joining.

- start = 1: save policy, then attempt joining.
- start = 0: save policy, then stop joining.
- start = KEEP: save supplied fields only. Other start values are invalid.

A later start/stop failure does not roll back saved policy. A join error does not imply unchanged settings.

Policy can save before a not-ready service rejects join start.

Sources: [join](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/lorawan.c), [policy](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/api/lorawan_credentials.c). Accepted joining still needs a completion event.

See also: [API design index](index.md).
