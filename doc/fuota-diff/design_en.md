# FUOTA design and implementation summary (156-169)

Date: 2026-11-26
Applies to: `feat/fuota-diff` (based on `fuota a70713b`)
Audience: colleagues who need to understand or merge this change
Organization: chapters follow the **runtime order**, each tagged with the ticket numbers, and each uses the same four parts: **why / interface and data flow / code location / pitfalls and trade-offs**.

---

## 0. One-page overview

### 0.1 Where the data comes from and goes

```text
[host]   167 sign  --> 165 full package
                      169 delta package (two signed images)
                                 |
                                 v
[air]    LNS delivers fragments (TS004/TS005)
                                 |
                                 v
[device] reception (161/157) --> header checks (161)
                                 |
                     reject --> erase target + reason (158/161)
                                 |
                     accept --> install (157, using the 156 decoder)
                                 |
                              boot_request_upgrade(TEST) --> reboot
                                 |
                         MCUboot verifies --> new image runs (confirm not implemented, ticket 160)
```

### 0.2 Ticket vs module vs code

| Ticket | Module | Main code |
|---|---|---|
| 167 | signing | `tools/builds/sign_image.py` |
| 165 | full package + vectors | `tools/builds/pack_image.py`, `fuota_package.py` |
| 169 | delta package | `pack_image.py diff`, `build/hpatch-host` |
| 161 | header checks + state machine | `fuota_header.c`, `lorawan_fuota.c` |
| 157 | installation | `fuota_install.c`, `lorawan_fuota.c` |
| 156 | delta decoding | `fuota_diff.c` + HPatchLite/tinyuz |
| 158 | failure handling | `lorawan_fuota.c` (`finish_window`) |

> Suggested reading order: this chapter, then chapter 1 (package), 2 (checks), 3 (install), 4 (failure), 5 (state), 6 (storage).

---

## 1. Package and toolchain (167 / 165 / 169)

### Why

The device and the Server agree on exactly one package: **32-byte business header + payload**. The payload is either a complete signed image or a patch. The signature proves authenticity, the CRC proves the content is intact, and the MD5 is used by the Server for whole-package verification; the three never overlap.

### Interface and data flow

```text
zephyr.bin --167 sign--> zephyr.signed.bin
             --165 pack--> firmware.bin (header + full image)
             --169 diff--> firmware.bin (header + patch)
```

- Header magic `@RAK`; the CRC covers the payload only.
- Both sides of a delta must be **signed images** signed with the **same key**, otherwise host verification fails.
- Tool order: hdiffi produces the patch, hpatchi restores it, imgtool verifies the signature, the result is compared byte-for-byte with the target image, and only then is the package written.

### Code location

- `sign_image.py`: calls `imgtool sign --pad-header`; rejects in-repo keys and sample keys.
- `pack_image.py pack/vectors/verify`: all go through `fuota_package.build_firmware()`.
- `pack_image.py diff`: inserts the patch before `build_firmware()` and the host acceptance after it.

### Pitfalls and trade-offs

| Item | Note |
|---|---|
| Key | sysbuild defaults to `root-rsa-2048.pem`; the test signature must use the same key or you get `No signature found` |
| Capacity | same-build small change is 6,427 B; two real versions are 103,500 B, which **does not fit the 32 KB `fuota_delta`**, so the Server must fall back to full |
| Old format | `FUOT` vectors are obsolete; use `@RAK` |

---

## 2. Reception and header checks (161)

### Why

Fragments arrive as a stream, so they must be stored while receiving. But "is this package for me, is the kind right, is the content intact, is the base correct" can only be decided once everything has arrived and the CRC can be computed. Hence **storage and verification are separate**: store by logical offset first, verify once before installation.

### Interface and data flow

32-byte header rules (field offsets exist only in `fuota_header.c`):

| Field | Rule | Reason |
|---|---|---|
| magic | `@RAK` | 0x11 |
| header_version | 1 | 0x11 |
| header_length | 32 | 0x11 |
| product_id | byte0 == `device_code`; 1-3 == 0 | 0x12 |
| target_version | **not checked** (same and lower versions allowed) | - |
| base_version | diff == running; full == 0 | 0x14 |
| package_kind | matches the Descriptor | 0x11 |
| diff_algo | 0 full, 1 diff | 0x11 |
| flags | 0 | 0x11 |
| image_length | == payload length | 0x15 |
| crc32 | payload only | 0x16 |

### Code location

- `verify_stored()`: reads 32 B from the header cache, fills `expect`, calls `fuota_hdr_verify()`.
- The payload read callback maps logical offset `-32` to the target area.
- Failure: erase target -> `FAILED` -> `last_reason` -> `notify_complete(-1013)`.

### Pitfalls and trade-offs

| Item | Note |
|---|---|
| Same version | The new design explicitly allows it; the old AC says "reject same version" and needs a PM wording update |
| Header location | The design says `storage.header_slot`; the implementation uses the second scratch sector because storage is occupied by NVS |
| Check timing | The design wants Class C kept during VERIFYING; the current code restores Class A before checking (upstream structure) |
| Non-RAK | A `RAK1` Host package has no 32-byte header; `session_has_header` was added, but RAK1 is not enabled yet |

---

## 3. Installation (157 / 156)

### Why

The install layer only turns a payload into a bootable image in slot1; it does not care where the package came from or whether it is valid. The header is handled by 161, so the install interface only looks at `header_length` and does not re-check magic/CRC.

### Interface and data flow

```c
int rzi_fuota_install(uint32_t descriptor, uint16_t header_length,
                      const struct rzi_fuota_install_io *io);
```

- `read_pkg` points at the **payload** (offset 0 = first payload byte); `pkg_size` is the whole package length.
- Full: copy into slot1 in 256-byte chunks, then flush.
- Delta: `read_slot0` (running image) + `read_pkg` (patch) -> `write_slot1` (flash_img).
- On failure it returns a negative error and neither sets pending nor calls Boot.

Decoder (156): HPatchLite + tinyuz, dictionary <= 512, restore cache 256 B, patch cache 192 B, decompression input 64 B; no heap; the `CONFIG_RZI_LORAWAN_FUOTA_DIFF` switch defaults to `n`.

### Code location

- `fuota_install.c`: full-copy / delta-restore branches.
- `fuota_diff.c`: callback `rzi_fuota_diff_patch_io()` plus the memory version.
- `lorawan_fuota.c`: `finish_window()` -> `install_delta_pkg()`.

### Pitfalls and trade-offs

| Item | Note |
|---|---|
| Header stripping | The header is split into scratch while fragments are written, so slot1 already starts with the image at offset 0; no 172 KB move is needed |
| Flash footprint | Enabling diff links hpatch_lite + tuz; with it off the symbols are absent |
| RAM | The delta path uses fixed caches and stack only; bss is about 57 KB of 64 KB, so the margin is small |

---

## 4. Failure handling (158)

### Why

A bad package must never boot, and a failure must leave a trace. The rule: **erase the target, do not set pending, record the reason, hand it to reporting**.

### Interface and data flow

| Scenario | Action | Reason |
|---|---|---|
| header check failure | erase `fuota_delta`/slot1 -> `FAILED` | 0x11-0x16 |
| delta install failure | erase slot1 (delta is left for the next Setup) -> `FAILED` | 0x05 |
| erase/write failure | record | 0x07 |
| reception incomplete | report | 0x08 |

### Code location

- `finish_window()`: the check-failure branch and the `install_delta_pkg()` failure branch.
- `status.reason`: `enum rzi_fuota_reason`, values equal the FPort 101 event codes.

### Pitfalls and trade-offs

| Item | Note |
|---|---|
| Not done | no result persistence, no reboot, no pending (the design wants "fail then reboot and report", which belongs to tickets 160/17) |
| State | a failure stops in `FAILED`; there is no `REPORTING` |

---

## 5. State machine (161)

### Why

Separating configuration, reception, verification and installation makes the process observable and failures locatable.

### Data flow

```text
IDLE --joined--> PRECONFIG --first fragment--> RECEIVING --complete--> VERIFYING
                                                                        |
                                    reject --> FAILED (erase + reason)
                                                                        |
                                    accept --> INSTALLING --> reboot
```

Eight values: `IDLE/PRECONFIG/RECEIVING/VERIFYING/INSTALLING/RUNNING_NEW/REPORTING/FAILED`.

### Code location

- `set_state_locked()`; the first fragment sets `RECEIVING` in `fuota_store()`; `finish_window()` sets `VERIFYING`/`INSTALLING`/`FAILED`.

### Pitfalls and trade-offs

| Item | Note |
|---|---|
| `RUNNING_NEW`/`REPORTING` | declared but not driven (they depend on confirm and full reporting) |
| `MCUBOOT` | present in the design, absent here: the bootloader phase is not observable from the application |

---

## 6. Storage and partitions (cross-cutting)

| Partition | Address/size | Purpose |
|---|---|---|
| slot1 | external `0x00000` / 208 KB | full image, delta output |
| scratch | `0x34000` / 8 KB | first sector for fragment staging; **second sector `0x1000` holds the 32-byte header** |
| `fuota_delta` | `0x36000` / 32 KB | delta payload |
| storage | `0x3e000` / 8 KB | NVS (AT configuration) |
| `lora_context` | `0x40000` / 8 KB | LBM context |

Logical mapping:

```text
[0,32)      -> scratch[0x1000 + off]
[32, ...)   -> slot1 / fuota_delta[off - 32]
```

Pitfall: the header should go to storage, but storage is occupied by NVS; using the second scratch sector is a board-level deviation that must be noted in the design.

---

## 7. Gaps versus the `_new` design

| Item | Current state | Owner |
|---|---|---|
| confirm / rollback | not implemented | ticket 160 |
| reboot jitter after pending | not implemented | ticket 160 |
| `T_status_hold` | not implemented | ticket 163 |
| full reporting | reason code only | task 17 (no ticket) |
| Host MCU | RAK1 rejected | design pending |
| `cap_delta` | not set | task 18 |
| FEC rows in storage | not implemented | optional |

---

## 8. Code index and tests

| Function | File |
|---|---|
| Header parse/check | `src/lorawan/services/fuota/fuota_header.c/.h` |
| Delta decoding | `fuota_diff.c/.h` |
| Installation | `fuota_install.c/.h` |
| Service/state/routing | `lorawan_fuota.c` |
| Configuration | `zephyr/Kconfig.lorawan` |
| Tests | `tests/lorawan/fuota_header|fuota_diff|fuota_install` |
| Tools | `tools/builds/*.py` |

Verification entry points: `usage_en.md` section 5 (host tests) and section 9 (board checks without an LNS).
