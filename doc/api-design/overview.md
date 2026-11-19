# 1. Design overview

Status: normative

<a id="design-overview"></a>

This chapter identifies consumers, capability sources, scope, and ownership. Chapter 2 defines contracts; Chapter 3 traces execution.

<a id="api-consumers"></a>

## 1.1 API consumers

Native applications, wrappers, inbound adapters, samples, and tests use the same public entry points. Each service owns shared configuration, state, and events.

Synchronous operations report results through returns. Asynchronous requests report acceptance first and completion through callbacks.

Consumers do not maintain separate service state or enter USP internals. Wrappers expose only implemented features; architectural relationships do not establish compatibility-test coverage.

<a id="api-origins"></a>

## 1.2 Capability sources and integration

RZI combines RUI3 product behavior, USP / LBM / RAC protocol capabilities, and shared RZI services.

These are sources of functionality, not additional runtime layers or a fixed execution sequence.

The [RUI3 mapping](../compatibility/rui3-mapping.md#rui3-mapping) distinguishes mapped, partial, unmapped, and declared-but-unavailable functions. Only selected, adapted USP capabilities are exposed. See [USP coverage](backend-coverage.md#coverage).

<a id="design-scope"></a>

## 1.3 Scope and evidence

RZI 0.1.0 on USP / LBM and USP RAC has 168 declarations in 11 service/foundation headers. Counts include conditional functions and placeholders; declarations do not guarantee availability. See [Chapter 4](backend-coverage.md#coverage).

Call paths and support statements come from source inspection. Documentation builds do not validate firmware execution or hardware. Record test passes separately from the existence of test suites.

<details>
<summary>Version, dependencies, and source baseline</summary>

| Baseline | Scope |
| --- | --- |
| SDK / backend | RZI 0.1.0; USP / LBM and USP RAC. Zephyr supplies the kernel and infrastructure. |
| Public surface | 11 service/foundation headers, 168 declarations including conditional functions and placeholders. |
| Source review | The original review used local HEAD `aee3710962502feb21438c65776e9ada6a5e833e` with working changes. This revision also reflects ABP simplification in `e3e0378`. These identifiers do not describe all uncommitted documentation. |
| Historical digest | The original review recorded SHA-256 `538f7094ddffe50f57c6f1ec6ebad2ce03cd356fa7e25f79b2c10921a73ffc4f` over 65 selected headers, service/backend sources, and configurations. It is not a digest of the current tree. |
| Dependencies | Revisions come from [west.yml](https://github.com/RAKWireless/RZI/blob/main/west.yml). Record board, region, final configuration, and patches with firmware builds. |
| Evidence | Design/static review. Firmware tests and hardware acceptance require separate results. |

</details>

<a id="boundary"></a>

## 1.4 Service responsibilities and dependency boundaries

Ownership and call order are separate. Shared settings/state belong to the service. Applications use public headers, not stack objects.

<a id="call-layers"></a>

### 1.4.1 Call layers

The service validates arguments and owns application-visible state. A private backend adapts requests to the stack. Optional services attach through hooks.

Dependency order: application -> public service -> private backend -> stack. Optional services attach to the core.

| Layer | Source | Responsibility | Boundary |
| --- | --- | --- | --- |
| Application | Native code, wrappers, adapters, [samples](https://github.com/RAKWireless/RZI/blob/main/samples/lorawan/periodical_uplink/src/main.c) | Scheduling, timeouts, retries. | Uses public APIs and combines returns with events. |
| Service core | [Header](https://github.com/RAKWireless/RZI/blob/main/include/rzi/lorawan/lorawan.h), [core](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/lorawan.c), [facades](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/api/lorawan_config.c) | Arguments, types, errors, configuration, state. | Calls private backends; cross-service clients use public APIs. Full queues drop new events and later report OVERFLOW without replay. |
| Private backend | [LoRaWAN USP](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/backend/usp/lorawan_backend_usp.c), [Raw LoRa USP](https://github.com/RAKWireless/RZI/blob/main/src/lora/backend/lora_backend_usp.c), [LoRaWAN contract](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/backend/lorawan_backend.h), [Raw LoRa contract](https://github.com/RAKWireless/RZI/blob/main/src/lora/backend/lora_backend.h) | Maps ops/events. | Uses [USP platform](https://github.com/RAKWireless/RZI/blob/main/src/usp/usp_platform.c), LBM/RAC. Vendor types stay private. Stacks own timing/sessions; boards own hardware. |
| Optional service | [FUOTA](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/services/fuota/lorawan_fuota.c), [hooks](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/lorawan_service.h) | Additional feature state. | Receives private lifecycle/event hooks. |

<a id="service-state"></a>

### 1.4.2 Service ownership

Each service owns a distinct state domain. Dependencies do not transfer ownership to frontends.

These boundaries are independent of call order.

| Service | Owns | Dependencies and limits |
| --- | --- | --- |
| LoRaWAN | Lifecycle, credentials, join policy, region, channels. | Session ops read DevAddr/keys. Frame counters remain in the stack. USP lacks DevNonce/NetID reads; NetID uses the stored fallback. |
| Raw LoRa / FSK | Modulation, configuration, activity, callbacks. | Selected radio backend; no LoRaWAN dispatcher. |
| Network Mode | Persistent working mode. | No service lifecycle or radio arbitration. |
| Storage | Namespaced values, records, serialization. | Calling services own schemas/migration. Namespaces do not reserve capacity. |
| Power | System-PM switch and constraint references. | Zephyr, SoC, drivers, constraints determine reachable states. |
| FUOTA | Session snapshots, image access, installation control. | Backend packages, DFU/MCUboot. Server behavior, signatures, boot modes, and image health need separate validation. |

Public headers expose C with C++ linkage guards, RZI types, and standard C types. Change configuration through its service API rather than bypassing the Storage cache.

Sources: [sample](https://github.com/RAKWireless/RZI/blob/main/samples/lorawan/periodical_uplink/src/main.c), [core](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/lorawan.c), [LoRaWAN selection](https://github.com/RAKWireless/RZI/blob/main/zephyr/Kconfig.lorawan), [Raw LoRa selection](https://github.com/RAKWireless/RZI/blob/main/zephyr/Kconfig.lora), [USP](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/backend/usp/lorawan_backend_usp.c), [hooks](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/lorawan_service.h), [Network Mode](https://github.com/RAKWireless/RZI/blob/main/include/rzi/network_mode.h), [Storage](https://github.com/RAKWireless/RZI/blob/main/include/rzi/storage/storage.h), [Power](https://github.com/RAKWireless/RZI/blob/main/include/rzi/power/power.h).

See also: [API design index](index.md).
