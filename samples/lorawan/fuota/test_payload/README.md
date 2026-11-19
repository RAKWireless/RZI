# ChirpStack FUOTA test payload

The 4096-byte rzi-fuota-test.bin verifies protocol transfer, not firmware
installation. It fits the current RAK3372-F reconstruction area exactly;
the configured 20,000-byte fragment limit is not the physical storage limit.
See [FUOTA capacity](../../../../doc/api/fuota.md#image-capacity).

| Offset | Content |
|---|---|
| 0-3 | ASCII RZI1 |
| 4-7 | Little-endian length 4096 |
| 8 onward | ChirpStack FUOTA test payload for RZI |

Upload this file to a ChirpStack FUOTA deployment for protocol bring-up.
It is not a bootable image. After reconstruction, the rzi_fuota log
prints FUOTA image head beginning with 52 5a 49 31 (RZI1).

Regenerate from the repository root:

```sh
python3 samples/lorawan/fuota/test_payload/gen_test_bin.py
```
