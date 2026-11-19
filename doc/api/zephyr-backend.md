# Zephyr LoRaWAN backend

Status: evaluation

Second real stack used to test the RZI contract. Not the default backend
and not the planned Zephyr LBM path.

See also: [lorawan-backends.md](backends.md),
[lorawan-api.md](lorawan.md).

`CONFIG_RZI_LORAWAN_BACKEND_ZEPHYR` connects RZI to the Zephyr public
`<zephyr/lorawan/lorawan.h>`. The lower layer is fixed to **loramac-node**
(not native, and not the LoRa Basics Modem radio backend). That is a
different path from the planned Zephyr LBM LoRaWAN backend
(`CONFIG_RZI_LORAWAN_BACKEND_ZEPHYR_LBM`).

This adapter exists to test whether the RZI contract holds against a
second real stack. Conclusion: **asynchronous join/send, the event sink,
and the FUOTA ops table hold**. A few fields and capabilities were
biased toward the LBM shape; those now have comments or small fields in
the public header. The rest is absorbed by the backend.

## What the contract can adapt

| RZI contract | Zephyr API | Adapter |
| --- | --- | --- |
| `join()` / `send()` return immediately; completion is an event | Both calls block until finished | Dedicated work queue |
| `READY` / `JOINED` / `JOIN_FAILED` / `TX_DONE` / `DOWNLINK` | No peer events; success is the return value | Publish events after the worker finishes |
| `start_clock_sync` / image read-write / reboot | `lorawan_clock_sync_run()`, slot1 flash, `sys_reboot()` | FUOTA ops table |
| Runtime `rzi_lorawan_init()` | `lorawan_set_region()`, also trimmed by Kconfig | `init()` fails if the mapping fails |

## Friction exposed by the second backend

1. **No `leave()`**.
   Zephyr cannot end a session. The backend returns `-RZI_ERR_NOT_SUPPORTED` and the
   service stays JOINED.
2. **No `is_joined()`**.
   The adapter only records joins it started. An NVM-restored session does
   not automatically become joined.
3. **DevNonce is held by the application**.
   With `LORAWAN_NVM_NONE`, Zephyr requires a monotonically increasing
   nonce on every OTAA. Public clients use
   `rzi_lorawan_set_dev_nonce()` / `rzi_lorawan_get_dev_nonce()`.
   A configured value of zero selects the backend's incrementing fallback,
   which is lost across power-off.
4. **Key names are 1.1; semantics also overlay 1.0 GenAppKey**.
   `rzi_lorawan_set_app_key()` supplies the private value mapped to Zephyr
   `nwk_key`; the stored GenAppKey supplies the value mapped to `app_key`.
   These are backend mappings, not public join-structure fields.
   LoRaWAN 1.0.x **GenAppKey is not a Zephyr join parameter**. If
   ChirpStack multicast needs a separate GenAppKey, keep USP/LBM or fill
   both stored keys with AppKey when appropriate for that deployment.
5. **`join_backoff_bypass` is ignored**.
   Zephyr has no join duty-cycle bypass.
6. **FUOTA events are incomplete**.
   There is no public multicast session start/end callback; the
   FragSession descriptor approximates `SESSION_STARTED`. The finish
   callback of `lorawan_frag_transport_run()` has no success/failure or
   image length. There is no FMP, so no `REBOOT_REQUESTED`. Image length
   returns `-RZI_ERR_NO_DATA`; RZI infers it from the MCUboot header.
7. **AS923 is a single region**.
   RZI GRP1–4 all map to `LORAWAN_REGION_AS923`.
8. **Class B is unsupported**.
   `set_class(B)` returns `-RZI_ERR_NOT_SUPPORTED`.
9. **JOIN_FAILED now carries a negative `RZI_ERR_*`**.
   Zephyr `lorawan_join()` returns a concrete error. The service no longer
   always reports `-RZI_ERR_TIMEOUT`.

## Usage

Build commands, overlay replacement, and `.config` checks are in
[lorawan-backends.md](backends.md). The adapter accepts ABP and
advertises Class C. Class B is not advertised. `leave()` still returns
`-RZI_ERR_NOT_SUPPORTED`.

```text
CONFIG_RZI_LORAWAN=y
CONFIG_RZI_LORAWAN_BACKEND_ZEPHYR=y
CONFIG_LORA_MODULE_BACKEND_LORAMAC_NODE=y
CONFIG_LORAWAN_REGION_EU868=y
```

Applications still call only `rzi_lorawan_*`. The matching Zephyr region
Kconfig must be enabled, and board DTS must provide `lora0`. FUOTA also
needs `CONFIG_LORAWAN_SERVICES`, clock sync, fragmentation, and optional
remote multicast.
