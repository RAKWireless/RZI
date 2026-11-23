# USP FEC unsigned-shift regression

After applying the RZI USP patch series in a west workspace, run:

```sh
python3 RZI/tests/lorawan/fuota_fec/run_host.py --usp modules/lib/usp
```

GCC (or `--cc clang`) and ASan/UBSan are required. The test includes the actual
patched helper translation unit to check its private `IsPowerOfTwo` predicate
at zero, low bits, bit 31 and multi-bit values. It also drives the real decoder
with a synthetic 4096-byte image, missing originals and redundancy, then compares
every output byte against `(37 * offset + 11) & 255`. No modem, radio or board is used.

`replay.bin` contains the 143 Server-generated test packets retained from the
2026-11-20 host replay: each record is one length byte plus one TS004 DataFragment
payload. Parameters are N=128, S=32. The fixture contains only deterministic test
image/parity bytes, with no keys, device settings or deployment identifiers.
The test stops feeding the decoder once reconstruction finishes. This is a
decoder regression, not a complete radio, package-parser or firmware upgrade test.
