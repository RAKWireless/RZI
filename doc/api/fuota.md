# FUOTA

Status: implemented

RZI owns update state, image read, and optional MCUboot installation. Clock
Synchronization, Remote Multicast Setup, and Fragmentation stay in the
selected backend. Independent GenAppKey multicast is complete only on
USP/LBM.

See also: [lorawan-backends.md](backends.md),
[lorawan-backend-zephyr.md](zephyr-backend.md),
[boot.md](../architecture/boot.md).

`CONFIG_RZI_LORAWAN_FUOTA` depends on `CONFIG_RZI_MCUBOOT_DUAL_SLOT` when
MCUBoot is enabled. `rak4631/nrf52840/rzi` and `rak3372_f` select it.
`rak3172/stm32wle5xx/rzi` is single-slot and cannot.

## Device-side flow

The application owns OTAA join and periodic uplinks, and observes progress
with `rzi_fuota_register_callbacks()`. Protocol and reassembly run
automatically when `CONFIG_RZI_LORAWAN_FUOTA=y`:

```text
rzi_lorawan_init() -> automatic rzi_fuota_init()
  -> READY -> application requests OTAA join -> JOINED
  -> start ALCSync and request MAC DeviceTime
  -> periodic uplink (ChirpStack advances unicast steps from this)
  -> FPort 202 / 200 / 201 handled transparently by the backend
  -> Class C multicast receives fragments
  -> record the image header; state becomes COMPLETE
  -> the application calls rzi_fuota_apply() to write slot1 and reboot
```

Provision AppKey with `rzi_lorawan_set_app_key()` and the separate
ChirpStack Gen App Key with `rzi_lorawan_set_gen_app_key()`. On USP,
GenAppKey falls back to AppKey when unset. Applications do not construct
private backend join configurations.

## ChirpStack configuration

1. Device profile → Application layer, enable:
   - Application Layer Clock Synchronization **v1**
   - Remote Multicast Setup **v1**
   - Fragmented Data Block Transport **v1**
2. Set Expected uplink interval to 5–10 seconds. ChirpStack checks unicast
   step completion on that interval; a longer interval looks like a stall
   in Multicast Setup.
3. Fill the device OTAA keys: AppKey and Gen App Key.
4. Create a FUOTA deployment:
   - Multicast group type: **Class C**
   - Let ChirpStack compute fragment size and multicast timeout
   - Configure fragmentation redundancy (for example 10–20%)
5. Upload the file to send. For protocol bring-up use
   `samples/lorawan/fuota/test_payload/rzi-fuota-test.bin` (4096 bytes,
   ASCII `RZI1` at the start). That is not bootable firmware. For MCUboot
   installation, switch to a signed image.

Device firmware must select the same package versions:

```
CONFIG_RZI_LORAWAN_FUOTA=y
CONFIG_LORA_BASICS_MODEM_FUOTA_V1=y
```

Optional RZI knobs (see `zephyr/Kconfig.lorawan`):

- `CONFIG_RZI_LORAWAN_FUOTA_KEEPALIVE_INTERVAL_S` — automatic one-byte
  unicast keepalive. Default `0` (off); the application should send its
  own periodic uplinks.
- `CONFIG_RZI_LORAWAN_FUOTA_KEEPALIVE_PORT` — LoRaWAN port used by the
  keepalive uplink when the interval is non-zero.
- `CONFIG_RZI_LORAWAN_FUOTA_HW_VERSION` — 32-bit hardware version reported
  to FMP.

If the ChirpStack device profile uses v2, switch to
`CONFIG_LORA_BASICS_MODEM_FUOTA_V2=y`.

ChirpStack core FUOTA does **not** use the Firmware Management Protocol
(TS006). LBM still builds FMP by default; it only runs when the network
server sends FPort 203.

## Image capacity

USP reconstructs fragments in `CONTEXT_FUOTA`, starting at offset 4096 in
the selected modem-context partition. The configured fragment limit
(200 x 100 = 20,000 bytes in this sample) is not a storage guarantee.
The effective limit is the smaller of that configuration and the remaining
physical context capacity. Account for any other enabled context users.

| Current sample target | Modem-context area | Space after the FUOTA offset |
|---|---|---|
| RAK4631 | Separate 8 KiB modem-context partition | At most 4 KiB |
| RAK3372-F | Separate 8 KiB modem-context partition | At most 4 KiB |

The 4096-byte protocol test file is at that context limit.
It does not demonstrate whole-firmware capacity. Settings NVS uses
`storage_partition` and does not share sectors with this context area.

A whole-firmware update needs all of the following:

1. Enough configured fragments for the complete signed image.
2. A non-overlapping context area or adapted reconstruction backend with
   enough capacity to hold that image before installation.
3. A secondary MCUboot slot large enough for the image and required trailer.
4. A validated signing, boot-mode, and recovery process.

`rzi_fuota_apply()` reads the already-reconstructed image through backend
`read_image`, copies it to slot1 using `flash_img`, requests an upgrade, and
reboots. Enabling MCUboot or IMG_MANAGER does not increase the reconstruction
area. Without IMG_MANAGER, apply returns NOT_SUPPORTED; read_image remains
available where the backend supports it.

Standard RAK3172 is single-slot and cannot select FUOTA with MCUboot.
RAK3372-F has two 208 KiB image slots, with the secondary slot on the
MX25L400. Its board default uses scratch swap.

The current backend reconstruction area is 4 KiB. The 32 KiB delta
partition is reserved, and the current apply path still reads from the
backend reconstruction area. See the
[RAK3372-F flash layout](../architecture/boot.md#rak3372-f-flash-layout).

## Public API

See `include/rzi/lorawan/fuota.h`:

- `rzi_fuota_register_callbacks()`
- `rzi_fuota_init()`
- `rzi_fuota_get_status()`
- `rzi_fuota_set_expected_size()` (optional override; the backend reports
  reassembly length, and MCUboot images are measured automatically)
- `rzi_fuota_read_image()`
- `rzi_fuota_apply()`

Reference sample: `samples/lorawan/fuota`.
