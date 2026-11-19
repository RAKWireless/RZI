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
  -> FragSessionSetupReq selects the store from the descriptor
  -> 0x00000000 erases slot1; 0x304B4152 (bytes 52 41 4B 30, ASCII RAK0)
     erases fuota_delta; any other value is rejected
  -> Class C multicast writes reconstructed bytes into the selected partition
  -> the application skips uplinks while rzi_fuota_uplink_allowed() is false
  -> multicast window ends, the service returns the device to Class A
  -> slot1: MCUboot test upgrade, reboot
  -> fuota_delta: file stays in the 32 KiB partition, device stays up
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
   - Let ChirpStack compute fragment size and multicast timeout. EU868
     DR2, DR3 and DR4 yield 48, 112 and 219 byte fragments
   - Keep redundant fragments at or below 200. That is about 20% at DR4,
     10% at DR3 and 4.5% at DR2
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

USP writes the reassembled bytes into the partition selected by the
fragmentation descriptor. Descriptor 0 uses the MCUboot secondary slot
(`slot1` / `image-1`) and, after the multicast window, requests a test
upgrade and reboots. Descriptor `0x304B4152` is the little-endian value of
the bytes `52 41 4B 30` (ASCII `RAK0`); it uses `fuota_delta` (32 KiB at
`0x36000` on RAK3372-F). That file is left in place and the device does not
reboot. Any other descriptor is rejected and nothing is erased. A file
whose fragment span exceeds the selected partition is rejected. The context
partition is no longer the firmware image store.

The decoder caps one session at `FRAG_MAX_NB` fragments of at most
`FRAG_MAX_SIZE` bytes, and the number of lost fragments at
`FRAG_MAX_REDUNDANCY`. The sample sets these to 4438, 242 bytes and 200
fragments. On EU868 that accepts one 208 KiB image at DR2 (48-byte
fragments), DR3 (112) and DR4 (219). The 200-fragment redundancy cap is
about 20% of a DR4 session, 10% of DR3 and 4.5% of DR2; a 20% cap at DR2
does not fit in 64 KB RAM. A slot1 upload must be a signed MCUboot image.
After that multicast window the service requests a test upgrade and reboots
without checking the image; the application does not call
`rzi_fuota_apply()`. A `RAK0` upload can be any file that fits in 32 KiB.

Without `CONFIG_IMG_MANAGER`, installation returns `-RZI_ERR_NOT_SUPPORTED`.
`rzi_fuota_read_image()` can still read the stored bytes from the partition
selected for that session.

`rak3172/stm32wle5xx/rzi` is single-slot (no `image-1`), so
`CONFIG_RZI_LORAWAN_FUOTA` cannot be selected. `rak3372_f` places
`image-1` on the on-module MX25L400 (208 KiB, same size as `image-0`).
MCUboot mode is overwrite. A whole-image upgrade still needs a fragment
count large enough for that slot.

RAK3372-F previously used 200 KiB then 192 KiB image slots. A bootloader
built for an older layout is not compatible with a 208 KiB update because
the MCUboot trailer offset changes. Migrate using a matching merged image
before testing current updates and invalidate any old external image.
See [partition migration](../architecture/boot.md#rak3372-f-partition-migration).

## Public API

See `include/rzi/lorawan/fuota.h`:

- `rzi_fuota_register_callbacks()`
- `rzi_fuota_init()`
- `rzi_fuota_get_status()`
- `rzi_fuota_set_expected_size()` (optional override; the backend reports
  reassembly length, and MCUboot images are measured automatically)
- `rzi_fuota_read_image()`
- `rzi_fuota_uplink_allowed()`
- `rzi_fuota_apply()` (used by the service after a reconstructed session)

Reference sample: `samples/lorawan/fuota`.
