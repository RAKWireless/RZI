# Board design

Status: normative

Describes board-definition choices, ownership of device-tree configuration, and the composition of Zephyr, RZI, usp_zephyr, and application overlays. The source baseline was reviewed on 2026-11-11; hardware conformance remains a separate acceptance task.

See also: [architecture.md](overview.md), [boot.md](boot.md), [lorawan-backends.md](../api/backends.md), [api-design.md](../api-design/index.md).

<a id="purpose"></a>

## 1. Scope and board selection

RZI reuses upstream hardware definitions and maintains product-specific choices. Partition addresses and flashing are documented in [boot.md](boot.md).

<a id="variant-rule"></a>

### 1.1 Choosing a board definition

Choose according to physical connections, defaults, and maintenance cost.

| Definition | Choose when | Implementation | Maintenance |
| --- | --- | --- | --- |
| Upstream extension / variant | An upstream board matches the hardware and product differences fit local overrides. | Extend the upstream board in board.yml; include its DTS and override product choices. | Upstream owns base hardware; RZI owns differences, partitions, and default devices. |
| Independent board | No matching upstream board exists, or inheritance would require repeatedly undoing fundamental hardware choices. | Provide board.yml and a board DTS; reuse SoC/module DTSI files. | RZI owns the product board while reusing lower-level hardware descriptions. |

Prefer definitions matching actual hardware. Partition and console changes often fit a variant. An independent board makes hardware ownership explicit; it does not require copying SoC definitions. Current implementations are listed in [Chapter 3](#boards).

<a id="repositories"></a>

## 2. Device-tree layers and composition

<a id="file-placement"></a>

### 2.1 Configuration ownership

Assign hardware facts and product choices to the correct layer before selecting the repository. Reference existing nodes and override only the properties that change.

| Layer | Files | Owns |
| --- | --- | --- |
| SoC | Zephyr SoC DTSI | On-chip peripherals, controllers, hardware resources. |
| Module | Upstream or RZI module DTSI | Fixed radio, memory, and pin connections. |
| Upstream board | Zephyr board DTS | Base peripheral defaults, console, aliases, partitions. |
| RZI product board | Variant DTS or independent board DTS | Product console/radio choice, driver properties, enabled peripherals, pin conflicts. |
| Product partitions | Partition DTSI | Boot, image slots, persistent storage. |
| Bootloader | mcuboot.overlay | MCUboot-specific clock and console differences. |
| Application | app.overlay or board-specific overlay | Application peripherals within physical board/module constraints. |

#### Examples and constraints

- Reuse module definitions such as `arm/rakwireless/rak3172.dtsi`. RZI supplies missing modules such as `rzi_rak3172_f.dtsi` and `rzi_rak3162.dtsi`. Reference the existing `&lora` node.
- Zephyr LoRa uses `DT_ALIAS(lora0)`; USP uses `DT_CHOSEN(zephyr_lorawan_transceiver)`. Product definitions provide those selections.
- RZI variants remove inherited partitions and include the product layout. MCUboot and application share that definition.
- RAK3372-F applications use MSI; MCUboot uses HSE PLL. MCUboot-specific configuration expresses this difference.
- Check system and radio-reference clocks independently. Verify memory connections, capacity, JEDEC ID, and package-specific pinctrl against the hardware revision. See [3.2](#wl-comparison).

<a id="repo-roles"></a>

### 2.2 Repository entry points

| Repository / layer | Entry | Owns |
| --- | --- | --- |
| Zephyr | zephyr/dts/ and zephyr/boards/rakwireless/ | SoC, pinctrl, generic bindings, upstream RAK4631/RAK3172. |
| RZI | [module.yml](https://github.com/RAKWireless/RZI/blob/main/zephyr/module.yml), boards/rakwireless/, dts/vendor/rakwireless/ | Exports board_root/dts_root, variants, module DTSI, partitions. |
| usp_zephyr | [module.yml](https://github.com/Lora-net/usp_zephyr/blob/v1.2.1/zephyr/module.yml), dts/bindings/usp/ | USP radio bindings and drivers. A compatible selects its binding; this is not another override layer. |
| Application | app.overlay, boards/target.overlay | Application-only configuration without changing shared hardware facts. |

The protocol implementation in `modules/lib/usp` supplies no product-board DTS. It enters through usp_zephyr adaptation and build configuration.

<a id="merge-example"></a>

### 2.3 Includes and overrides

#### Upstream variant: RAK4631

```text
Zephyr rak4630.dtsi -> upstream rak4631.dts
                    -> RZI rak4631_nrf52840_rzi.dts
                       + rak4631-rzi-partitions.dtsi
                    -> selected application overlay
                    -> generated zephyr.dts
```

Zephyr owns the base module/board; RZI adds a product variant and partitions. Application overlays apply afterward.

#### Independent board: RAK3372-F using the RAK3172-F module

Build target: `rak3372_f/stm32wle5xx`. For the periodic-uplink sample:

```text
Zephyr SoC DTSI + package pinctrl
  -> RZI rzi_rak3172_f.dtsi
  -> RZI rak3372_f board DTS + product partitions
  -> selected application overlay
  -> generated zephyr.dts
```

Bindings interpret properties by compatible; they are not extra overriding layers. Modules export DTSI files, bindings, and dt-bindings through dts_root.

Chosen/alias properties merge individually. A later property replaces the same name; untouched properties remain. Reference existing nodes rather than redeclaring radios.

The [RAK3172 sample overlay](https://github.com/RAKWireless/RZI/blob/main/samples/lorawan/periodical_uplink/boards/rak3172_stm32wle5xx_rzi.overlay) explicitly includes ../app.overlay. Do not assume automatic app.overlay stacking when a board-specific overlay is selected. Board selection chooses the board DTS; application overlays need not include it. Check selected overlay logs and final zephyr.dts.

RAK4631's semtech,sx1262-new binding comes from usp_zephyr. A switch to Zephyr LoRa requires coordinated compatible, properties, and Kconfig changes.

<a id="boards"></a>

## 3. Implementations and differences

<a id="board-overview"></a>

### 3.1 Board definitions

| Target | Definition | Hardware source | Product configuration |
| --- | --- | --- | --- |
| rak4631/nrf52840/rzi | Upstream variant | rak4631.dts; rak4630.dtsi; nRF52840 | RZI partitions, UART1 console, USP radio binding. |
| rak3172/stm32wle5xx/rzi | Upstream variant | rak3172.dts/dtsi; STM32WLE5CC | RZI partitions, LPUART1, conflicting USART2 disabled, standard RF switch retained. |
| rak3372_f/stm32wle5xx | Independent | rzi_rak3172_f.dtsi; configured STM32WLE5JC; module SPI NOR | External secondary slot, MCUboot-specific clocks. |
| rak3362/nrf54l15/cpuapp | Independent | rzi_rak3162.dtsi; nRF54L15; SX1262 | UART20 console/AT, product partitions. |

RAK4631 adapts semtech,sx1262 to semtech,sx1262-new. The standard RAK3172 variant removes TCXO/DIO2 properties inappropriate for that module or driver. Recheck bindings and Kconfig when switching backends.

<a id="wl-comparison"></a>

### 3.2 Standard RAK3172 versus RAK3172-F

This compares the upstream standard RAK3172 definition with the RAK3172-F module used by the RAK3372-F product board. Values describe source configuration, not verified physical hardware. Confirm package, RF frontend, clocks, and connections against schematics, BOM, and board testing.

| Item | Standard upstream RAK3172 | RZI RAK3172-F module | Verification |
| --- | --- | --- | --- |
| SoC/package | STM32WLE5CC; stm32wle5ccux-pinctrl.dtsi | STM32WLE5JC; stm32wle5jcix-pinctrl.dtsi | Exact part, package, internal wiring. |
| RF switch, PC13 / PB8 | TX high/low; RX low/high | TX high/high; RX low/high | F-module source follows RTC66006 selection/enable logic; check the actual truth table. |
| PA | HP PA limit 22 dBm | HP PA limit 22 dBm | Configuration limit is not measured or regionally permitted output. |
| Radio reference | 32 MHz HSE crystal; no TCXO supply control | 32 MHz TCXO input; 3.0 V, 10 ms startup | Source type, supply wiring, voltage, timing. |
| Application clock | HSE -> PLL -> 48 MHz | MSI -> 48 MHz | Separate CPU/radio clocks; test startup and low-power resume. |
| Console | Upstream USART1 PB6/PB7 | Product LPUART1 PA2/PA3; USART2 disabled | Carrier wiring and conflicts. |
| Module NOR | Not defined | SPI2, 512 KiB; CS PB9, SCK PB10, MOSI PC1, MISO PC2 | Assembly, capacity, pins, JEDEC ID. |
| Image layout | Internal boot 32 KiB; two 108 KiB slots | Internal boot 48 KiB and primary 208 KiB; external secondary 208 KiB and scratch 8 KiB | MCUboot/application addresses, sizes, and devices must agree. |

The standard RAK3172 RZI variant retains the upstream RF switch but changes console and partitions; its final settings are not the upstream defaults in this table.

RAK3372-F uses an independent definition to own module wiring, clocks, and storage. These differences justify its configuration, not a universal rule requiring independent boards. See the [RAK3372-F flash layout](boot.md#rak3372-f-flash-layout).

<a id="checks"></a>

## 4. Build checks

- Match board.yml extension/name to the selected target.
- Inspect application and MCUboot zephyr.dts for chosen devices, compatible, status, and partitions.
- Check final .config for backend, binding, and driver agreement.
- Check selected overlays. Explicitly include shared app.overlay when a board overlay needs it.
- Use consistent partition definitions. Keep boot-specific clock/console changes in MCUboot configuration.

These checks establish build selection, not physical behavior. Record startup, RF, current, and upgrade results per target board.
