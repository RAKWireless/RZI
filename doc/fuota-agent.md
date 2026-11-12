# FUOTA receive agent

[Simplified Chinese](fuota-agent_cn.md)

The RZI FUOTA service exposes the public callbacks, status and installation API.
With the USP v1 backend, the receive agent coordinates TS003, TS004 and TS005
and gates application traffic (task 11). Incomplete reception and loss of
connectivity use the same multicast close and Class A/C restoration operations
(task 15). The packages handle parsing, keys and FEC.

## Receive lifecycle

`OFFLINE → READY → PRECONFIG → RECEIVING → RECOVERING → FINALIZING → READY`

Application traffic continues during unicast setup. Before multicast reception
the backend blocks pending business uplinks, and the SDK gates new business
uplinks and delivery of application downlinks. MAC and package control continue.
The backend supports context 0 with an original class of A or C. For Class B,
group setup uses IDerror and fragment setup uses the unsupported-index bit.
Unsupported Class B multicast commands do not schedule reception.

Completion, setup or receive timeout, and failure enter recovery. The agent
closes multicast, cancels the current schedule and restores the original
receiver, including RXC for an original Class C device. Security context,
TS004 control state and candidate data have separate lifetimes. A step allows
three attempts by default. On exhaustion the agent reports an error and leaves
network recovery to leave/rejoin.

| Original class | First periodic keepalive after reception |
| --- | --- |
| A | One period after restoration. The uplink opens receive windows for a queued unicast query. |
| C | Restore RXC immediately. Start the period when this generation's StatusAns or first concluding DeleteAns completes locally. Without a query, start the period at the status-hold deadline. |

The hold defaults to 120 seconds after the later of restoration and the original
multicast window end. If the receive window has expired, restoration anchors the
hold. The backend converts GPS time to a monotonic deadline and refreshes it
at START. Cancelling reception early preserves the original deadline. ALCSync
has whole-second precision. A backend without a window deadline uses START plus
the session timeout, or starts cleanup immediately if the window never opened.
The agent sleeps until an event or deadline.

With CONFIG_RZI_LORAWAN_FUOTA_TRACE enabled, UART records begin with
`RZI_FUOTA_TRACE` and carry source timestamps. A successful window start
without fragment configuration logs `fuota.preconfig_fail` with the generation
and validity mask. This diagnostic does not enqueue an FPort 101 report.

A valid unicast StatusReq on RX1/RX2/RXC queues StatusAns at high priority.
The package bypasses its 0–3 s jitter, multicast BlockAckDelay and the extra
0.5–6 s TPM jitter on this path. MAC busy, regional access, relay and radio
constraints still apply. Multicast queries retain package and TPM scheduling.
See [TS004 v1.0.0, sections 3.2 and 3.5](https://lora-alliance.org/wp-content/uploads/2020/11/fragmented_data_block_transport_v1.0.0.pdf).
Status and delete answers are scheduled by the package. Pending setup answers
take priority over keepalive. A rejected keepalive waits for the next period.
With the timer disabled, a Class A application supplies its own uplink opportunity.

The project acknowledges an identical active fragment setup again and retains
the generation, decoder and received fragments. Conflicting setup cannot
overwrite occupied storage. The TS004 length comes from validated fragment
count, size and padding. TS003 distinguishes MAC DeviceTime from ALCSync.
Package version queries do not start a transfer.

## Public API and callbacks

Register callbacks through `<rzi/lorawan/fuota.h>` before LoRaWAN initialization.
The dispatcher copies backend events to the worker queue without blocking.
Copy a callback's event before returning and defer image processing to an
application thread. Receive callbacks normally run on the agent worker.
Successful explicit release can deliver a synchronous state change on the
calling thread. Application callbacks run outside the agent and service locks.

- `rzi_fuota_get_snapshot()` returns phase, session, clock source, data and exit
  status. The session and data getters return value copies without keys or USP pointers.
- On complete reception the service reports COMPLETE, DATA_READY and successful
  transfer completion before closing multicast. Read with `rzi_fuota_read_image()`
  when ready with a known length and valid offset/size. Expected size is an
  application hint. The agent does not infer the length from an MCUboot header
  or classify the received block as full or delta.
- CLOSED precedes the public session-ended callback. RESTORED follows successful
  restoration. For missing fragments, transfer completion reports the error after
  RESTORED. Inspect `exit.error` and `exit.cleanup_error` separately.
  On receive timeout a local StatusAns leaves the session available for an
  independent DeleteReq. Successful DeleteAns completes cleanup, otherwise the
  original hold deadline bounds it. Repeated queries do not extend the hold.
- `status_answered` and `delete_answered` record local control completion.
  `cleanup_uplink_done` remains false for this policy. A successful control
  answer permits clean finalization. Expiry without one records TIMEOUT.
  FINISHED reports local settlement. LNS receipt requires separate evidence.
- The backend retains complete data through finalization and protocol delete.
  The application owns the decision to install or release it. After the phase
  returns to READY, `rzi_fuota_release_data(generation)` releases that generation.
  Stale generations are rejected. Incomplete data are released on cleanup.
- The service provides `rzi_fuota_apply()`. During finalization of a retained
  image it returns BUSY. After settlement an application may authorize and call
  it outside callbacks. With IMG_MANAGER it copies the candidate to the secondary
  slot, requests MCUboot TEST and reboots. FMP callbacks do not initiate installation.
  Agent-specific getters return NOT_SUPPORTED in builds without the agent.

## Backend and storage contract

The observer tags accepted setup, window, start/end, FEC and local control TX
with the saved generation. Repeated completion/end and stale-generation events
must not advance a later transfer. On queue overflow the worker uses
`get_session_info` to identify the session and rejects untrusted completion.
`close_multicast` and `restore_settings` return zero only after completion.
The backend owns receiver settings. `cleanup_uplink(0)` requests the periodic
keepalive. Control completion may precede the first keepalive.

The checked provider uses USP PROVIDED_STORAGE_IMPL and the existing flash map,
checks capacity and bounds, protects modem context and propagates write errors.
The agent does not allocate or erase the storage on release. Error propagation
for USER_STORAGE_IMPL has not been validated. The receive example configures
200 fragments of up to 100 bytes. Partition layout and full/delta routing belong
to the storage integration. The provider's IsPowerOfTwo check uses unsigned
`1U << i` in the host FEC validation.

## GenAppKey

`rzi_lorawan_set_gen_app_key(key, 16)` and the corresponding getter manage a
separate key. An unset getter returns NO_DATA. LoRaWAN 1.0.x falls back to
AppKey for join configuration. An explicitly configured all-zero GenAppKey
remains a separate value. Changes take effect on the next OTAA join and failed
persistence leaves the previous value intact. Key derivation belongs to the
backend. Provision the same source on the LNS. A future AT frontend should
validate a 16-byte encoding, call the SDK setter and propagate storage errors.
Keep MCROOTKEY separate and keep keys out of logs.

## Examples and tests

The [FUOTA sample](../samples/lorawan/fuota/README.rst) retains the candidate
by default. Its optional receive-demo mode reads the opaque block after FINISHED
and releases it only after every read succeeds. Use the [host test guide](../tests/lorawan/fuota_agent/README.md)
to run the agent suite or one case. From the west workspace root:

```sh
west build -b native_sim -d build/fuota-agent-tests RZI/tests/lorawan/fuota_agent
build/fuota-agent-tests/zephyr/zephyr.exe
west build -b native_sim -d build/fuota-service-tests RZI/tests/lorawan/fuota
build/fuota-service-tests/zephyr/zephyr.exe
west build -b native_sim -d build/lorawan-settings-tests RZI/tests/lorawan/settings
build/lorawan-settings-tests/zephyr/zephyr.exe
python3 RZI/tests/lorawan/fuota_protocol/run.py
west build --sysbuild -b rak3372_f/stm32wle5xx -d build/fuota-demo RZI/samples/lorawan/fuota -- -Dfuota_CONFIG_APP_FUOTA_RECEIVE_DEMO=y
```

The protocol harness builds TS003/004/005 and FEC with ASan/UBSan and simulated
modem, radio and flash. Native tests cover generations, ordering, traffic gates,
timeouts, errors and key selection. RF, physical storage failures, class
restoration and the LNS result timeline require board and server evidence.
Historical cases I1-T01 through I1-T27 retain their identifiers.

The [west patch manifest](../zephyr/patches.yml) pins USP observer and protocol
changes, timestamp tracing, unicast scheduling and checked storage. Verify patch
replay in an isolated checkout before cleaning modified modules.
