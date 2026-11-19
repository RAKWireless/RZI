# 4. USP operation groups and build selection

Status: normative

<a id="backends"></a>

This chapter describes private ops, assigned slots, and compile-time selection. Assigned pointers do not establish availability in every state, region, or board.

<a id="private-ops"></a>

## 4.1 Groups and coverage

LoRaWAN has one embedded base table and seven optional domain tables. Assignments count the union of enabled conditional features. Raw LoRa has its own table.

| Group | Responsibility | USP assigned / slots | Conditions/gaps |
| --- | --- | --- | --- |
| [base](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/backend/lorawan_backend.h) | Lifecycle, join/leave, send, basic state | 11 / 11 | init, join, leave, send, set_class, is_joined, is_busy required; other members follow their nullability contract. |
| [network](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/backend/ops/lorawan_network_ops.h) | ADR, DR, NbTrans, duty cycle, backoff, LBT, CSMA | 35 / 35 | 31 assignments without CSMA; members can be absent individually. |
| [session](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/backend/ops/lorawan_session_ops.h) | Identity/key reads | 3 / 6 | DevAddr/NwkSKey/AppSKey provided; NetID and two DevNonce operations absent. |
| [mac](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/backend/ops/lorawan_mac_ops.h) | LinkCheck, DeviceTime, time | 3 / 3 | Requests/queries, separate from network parameters. |
| [class_b](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/backend/ops/lorawan_class_b_ops.h) | Ping slots, beacons, state/stop | 4 / 8 | Requires CLASS_B; beacon frequency/time/data-rate/gateway queries absent. |
| [multicast](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/backend/ops/lorawan_multicast_ops.h) | Sessions | 5 / 5 | Requires MULTICAST. |
| [certification](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/backend/ops/lorawan_certification_ops.h) | Mode/port | 4 / 4 | Requires CERTIFICATION; no separate capability bit. |
| [fuota](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/backend/ops/lorawan_fuota_ops.h) | Sync, images, reboot | 4 / 4 | Requires RZI_LORAWAN_FUOTA; backend owns packages. |
| **LoRaWAN total** | Eight groups | **69 / 76** | Maximum assigned union; seven absent members. |
| [Raw LoRa](https://github.com/RAKWireless/RZI/blob/main/src/lora/backend/lora_backend_usp.c) | RAC traffic, configuration, tests, poll | 14 / 14 | 7 assignments without TEST_MODE; separate from LoRaWAN total. |

Credentials, masks, and schemas belong to the service and enter the backend through join config; no separate credential/channel ops. Class C uses a bit and base.set_class.

<a id="coverage"></a>

## 4.2 Public availability

- Missing ops may return NOT_SUPPORTED, but arguments/state can fail first. Assigned functions still have regional/session/hardware prerequisites.
- NetID uses a stored fallback without session.get_net_id. Both DevNonce APIs return NOT_SUPPORTED on USP.
- Capabilities report Class B/C, multicast, and FUOTA, not complete per-function availability.

69 / 76 counts source assignments, not tested public APIs or measured flash/RAM/RF capabilities.

<a id="build-selection"></a>

<a id="trimming"></a>

## 4.3 Build selection

Kconfig selects features, CMake selects sources, and the linker retains code/data. NULL ops express unsupported behavior, not compile switches.

| Configuration | Effect |
| --- | --- |
| `CONFIG_RZI`, `CONFIG_RZI_LORAWAN`, `CONFIG_RZI_LORAWAN_BACKEND_USP` | Foundation, service, USP backend; facades compile per service rather than per ops group. |
| `CONFIG_RZI_LORA`, `CONFIG_RZI_LORA_BACKEND_USP` | Raw LoRa facades/RAC; current USP P2P depends on LoRaWAN USP. |
| `CONFIG_LORA_BASICS_MODEM_CLASS_B`, `CONFIG_LORA_BASICS_MODEM_CLASS_C` | Class B ops/capability and Class C capability respectively; declarations remain. |
| `CONFIG_LORA_BASICS_MODEM_MULTICAST`, `CONFIG_LORA_BASICS_MODEM_CSMA` | Multicast ops and four CSMA members respectively. |
| `CONFIG_LORA_BASICS_MODEM_CERTIFICATION`, `CONFIG_LORA_BASICS_MODEM_TEST_MODE` | LoRaWAN certification ops/package and Raw LoRa radio-test ops/API respectively (both default `y`). |
| `CONFIG_RZI_LORAWAN_FUOTA`, `CONFIG_IMG_MANAGER` | FUOTA/backend integration and optional image-copy/upgrade path respectively. |
| `CONFIG_RZI_STORAGE`, `CONFIG_RZI_POWER` | Storage/Power. Without Storage, LoRaWAN settings stay in RAM and Network Mode is unsupported. Modem context is separate; system PM uses CONFIG_PM. |

USP cannot coexist with Zephyr LoRaWAN/LoRa driver selections. FUOTA selects LBM dependencies; inspect final .config. Stop releases runtime activity, not queues/dispatchers/static stacks from the image.

<a id="trim-profile"></a>

## 4.4 Class A profile and resource budgets

This expresses configuration intent, not a complete board prj.conf. Board, region, radio, and boot setup remain required.

```conf
CONFIG_RZI=y
CONFIG_RZI_LORAWAN=y
CONFIG_RZI_LORAWAN_BACKEND_USP=y
# CONFIG_RZI_LORA is not set
# CONFIG_RZI_LORAWAN_FUOTA is not set
# CONFIG_RZI_STORAGE is not set
# CONFIG_RZI_POWER is not set
# CONFIG_LORA_BASICS_MODEM_CLASS_B is not set
# CONFIG_LORA_BASICS_MODEM_CLASS_C is not set
# CONFIG_LORA_BASICS_MODEM_MULTICAST is not set
# CONFIG_LORA_BASICS_MODEM_CSMA is not set
# CONFIG_LORA_BASICS_MODEM_CERTIFICATION is not set
# CONFIG_LORA_BASICS_MODEM_TEST_MODE is not set
```

- `CONFIG_RZI_LORAWAN_EVENT_QUEUE_SIZE` controls static queue capacity; smaller queues save memory but increase burst-loss risk.
- `CONFIG_RZI_LORAWAN_MAX_CALLBACKS` and
  `CONFIG_RZI_LORAWAN_DISPATCHER_STACK_SIZE` control subscribers/dispatcher stack; `CONFIG_USP_MAIN_THREAD_STACK_SIZE` separately controls the engine stack.
- `CONFIG_RZI_STORAGE_RECORD_MAX_SIZE` affects caller-stack scratch buffers, not a flash quota.

Compare final configs, source lists, ELF/map data, and stacks with a fixed board/toolchain. No measured size savings are claimed here.

Sources: [ops](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/backend/lorawan_backend.h), [USP](https://github.com/RAKWireless/RZI/blob/main/src/lorawan/backend/usp/lorawan_backend_usp.c), [Raw LoRa](https://github.com/RAKWireless/RZI/blob/main/src/lora/backend/lora_backend_usp.c), [CMake](https://github.com/RAKWireless/RZI/blob/main/zephyr/CMakeLists.txt), [LoRaWAN Kconfig](https://github.com/RAKWireless/RZI/blob/main/zephyr/Kconfig.lorawan), [Raw LoRa Kconfig](https://github.com/RAKWireless/RZI/blob/main/zephyr/Kconfig.lora).

See also: [API design index](index.md).
