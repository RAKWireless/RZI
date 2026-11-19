# Network mode persistence API

Status: implemented

This page defines persistence of the radio network working mode exposed by
`include/rzi/network_mode.h`.

See also: [architecture.md](../architecture/overview.md),
[storage-api.md](storage.md), [coding-standards.md](../development/coding-standards.md).

## Ownership

The module-level network-mode API owns the storage namespace, key, binary
representation, record version, and validation for the persisted mode. Its
implementation is `src/common/network_mode_store.c`. Applications and inbound
adapters such as AT, BLE, and Shell use this typed API and do not read or write
its Storage key directly.

Storage remains a persistence mechanism, not the owner of configuration
meaning. Configuration belonging to one business service stays on that
service's API. This API covers only the network working mode; it is not a
general product-configuration service.

## Network working mode

The persisted network mode is one byte in a versioned record, stored under
the `rzi` namespace and `network_mode` key:

```c
enum rzi_network_mode mode;

rzi_get_network_mode(&mode);
rzi_set_network_mode(RZI_NETWORK_MODE_LORAWAN);
```

The API validates `P2P_LORA`, `LORAWAN`, and `P2P_FSK`. It intentionally does
not start, stop, or arbitrate radio services. The caller coordinates active
services before committing a new mode. `AT+NWM` currently performs that
coordination and uses this API only for persistence.

The public API consists only of get and set operations. Factory reset writes
the frontend's documented build-default mode through `rzi_set_network_mode()`;
it does not expose deletion of an individual storage record as a public
network-mode operation.

If Storage is disabled, these operations return
`-RZI_ERR_NOT_SUPPORTED`. A missing persisted value returns
`-RZI_ERR_NOT_FOUND`; each frontend may then select its documented build
default without inventing a storage format.
