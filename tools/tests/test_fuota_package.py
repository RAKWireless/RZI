# SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless
# SPDX-License-Identifier: Apache-2.0
"""Pack and sign checks for the full and differential FUOTA package tools."""

import importlib.util
import json
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from contextlib import contextmanager
from pathlib import Path

ROOT = Path(__file__).parents[1]
WORKSPACE = ROOT.parents[1]
IMGTOOL = WORKSPACE / "bootloader" / "mcuboot" / "scripts" / "imgtool.py"
HOST = WORKSPACE / "build" / "hpatch-host"


def load(name: str, relative: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


PKG = load("fuota_package", "builds/fuota_package.py")
PACK = load("pack_image", "builds/pack_image.py")
def sign_file(source, output, key, version, header_size, slot_size, align, imgtool):
    """Sign one image with the repository imgtool (test helper)."""
    subprocess.run(
        [
            *imgtool,
            "sign",
            "--key",
            str(key),
            "--align",
            str(align),
            "--version",
            version.imgtool(),
            "--header-size",
            str(header_size),
            "--slot-size",
            str(slot_size),
            "--pad-header",
            str(source),
            str(output),
        ],
        check=True,
        capture_output=True,
        text=True,
    )



def mcuboot_image(payload: bytes = b"application-body") -> bytes:
    header = struct.pack("<I", PKG.MCUBOOT_MAGIC) + payload
    return header


class PackageTests(unittest.TestCase):
    def test_header_layout_and_crc(self) -> None:
        image = mcuboot_image()
        version = PKG.parse_version("1.2.3+4")
        blob = PKG.build_firmware(image, 7, version)
        self.assertEqual(blob[:4], b"@RAK")
        self.assertEqual(blob[4:8], b"\x01\x00\x20\x00")
        self.assertEqual(blob[8], 7)
        self.assertEqual(blob[9:12], b"\x00\x00\x00")
        self.assertEqual(blob[12:16], bytes((1, 2, 3, 4)))
        self.assertEqual(blob[16:20], b"\x00\x00\x00\x00")
        self.assertEqual(blob[20:22], b"\x00\x00")
        self.assertEqual(struct.unpack_from("<I", blob, 24)[0], len(image))
        expect = zlib.crc32(image) & 0xFFFFFFFF
        self.assertEqual(struct.unpack_from("<I", blob, 28)[0], expect)
        self.assertNotEqual(struct.unpack_from("<I", blob, 28)[0], zlib.crc32(blob[:28] + image) & 0xFFFFFFFF)
        self.assertEqual(struct.unpack_from("<I", blob, 0)[0], 0x4B415240)
        self.assertEqual(PKG.header_errors(blob), [])
        mutated = bytearray(blob)
        mutated[12] ^= 0x5A
        self.assertEqual(PKG.header_errors(bytes(mutated)), [])

    def test_pack_writes_md5_and_verify_rejects_a_changed_byte(self) -> None:
        with self._temp() as out:
            image = out / "app.signed.bin"
            image.write_bytes(mcuboot_image())
            self.assertEqual(
                PACK.main(
                    ["pack", "--image", str(image), "--device-code", "3", "--version", "2.0.0.1", "-o", str(out)]
                ),
                0,
            )
            firmware = (out / "firmware.bin").read_bytes()
            md5_text = (out / "firmware.md5").read_text(encoding="utf-8")
            self.assertTrue(md5_text.startswith(PKG.md5_hex(firmware)))
            self.assertEqual(PACK.main(["verify", str(out)]), 0)

            flipped = bytearray(firmware)
            flipped[-1] ^= 0xFF
            (out / "firmware.bin").write_bytes(flipped)
            self.assertEqual(PACK.main(["verify", str(out)]), 1)
            with self.assertRaises(PKG.PackageError):
                PKG.commit_package(out, bytes(flipped), signed=True, build_id=None)
            self.assertFalse((out / "firmware.bin").is_file())
            self.assertFalse((out / "firmware.md5").is_file())

    def test_invalid_input_writes_nothing(self) -> None:
        with self._temp() as out:
            missing = out / "missing.bin"
            code = PACK.main(
                ["pack", "--image", str(missing), "--device-code", "1", "--version", "1.2.3.4", "-o", str(out)]
            )
            self.assertEqual(code, 1)
            self.assertFalse((out / "firmware.bin").is_file())

            image = out / "app.bin"
            image.write_bytes(mcuboot_image())
            code = PACK.main(
                ["pack", "--image", str(image), "--device-code", "256", "--version", "1.2.3.4", "-o", str(out)]
            )
            self.assertEqual(code, 1)
            code = PACK.main(
                ["pack", "--image", str(image), "--device-code", "1", "--version", "1.2", "-o", str(out)]
            )
            self.assertEqual(code, 1)
            self.assertFalse((out / "firmware.bin").is_file())

    def test_unsigned_stand_in_uses_a_distinct_name(self) -> None:
        with self._temp() as out:
            image = out / "stand-in.bin"
            image.write_bytes(b"not-an-mcuboot-image")
            self.assertEqual(
                PACK.main(
                    ["pack", "--image", str(image), "--device-code", "1", "--version", "1.0.0.0", "-o", str(out)]
                ),
                1,
            )
            self.assertEqual(
                PACK.main(
                    [
                        "pack",
                        "--unsigned",
                        "--image",
                        str(image),
                        "--device-code",
                        "1",
                        "--version",
                        "1.0.0.0",
                        "-o",
                        str(out),
                    ]
                ),
                0,
            )
            self.assertTrue((out / "firmware.unsigned.bin").is_file())
            self.assertFalse((out / "firmware.bin").is_file())

    def test_vectors_match_the_rejection_sheet(self) -> None:
        with self._temp() as out:
            image = out / "app.signed.bin"
            image.write_bytes(mcuboot_image(b"payload-bytes"))
            self.assertEqual(
                PACK.main(
                    [
                        "vectors",
                        "--image",
                        str(image),
                        "--device-code",
                        "4",
                        "--version",
                        "3.1.0+7",
                        "-o",
                        str(out),
                    ]
                ),
                0,
            )
            legal = (out / "firmware.bin").read_bytes()
            device = (out / "vectors" / "device_mismatch.bin").read_bytes()
            crc = (out / "vectors" / "header_crc.bin").read_bytes()
            same = (out / "vectors" / "same_version.bin").read_bytes()
            base = (out / "vectors" / "base_mismatch.bin").read_bytes()
            self.assertEqual(device[8], 5)
            self.assertEqual(PKG.header_errors(device), [])
            self.assertTrue(any("crc32" in item for item in PKG.header_errors(crc)))
            self.assertEqual(same, legal)
            self.assertEqual(base[16:20], bytes((9, 9, 9, 9)))
            self.assertTrue(any("base_version" in item for item in PKG.header_errors(base)))
            self.assertTrue((out / "expected.json").is_file())

    def _temp(self):
        return temporary_dir()


class DiffPackageTests(unittest.TestCase):
    def test_diff_header_survives_verify(self) -> None:
        base = PKG.parse_version("1.0.0+0")
        target = PKG.parse_version("1.1.0+0")
        blob = PKG.build_firmware(
            b"patch-bytes",
            4,
            target,
            base=base,
            package_kind=PKG.PACKAGE_DIFF,
            diff_algo=PKG.ALGO_HPATCH,
        )
        self.assertEqual(blob[20:22], bytes((PKG.PACKAGE_DIFF, PKG.ALGO_HPATCH)))
        self.assertEqual(blob[16:20], bytes((1, 0, 0, 0)))
        self.assertEqual(PKG.package_errors(blob), [])
        wrong_algo = PKG.build_firmware(
            b"patch-bytes",
            4,
            target,
            base=base,
            package_kind=PKG.PACKAGE_DIFF,
            diff_algo=PKG.ALGO_NONE,
        )
        self.assertTrue(PKG.package_errors(wrong_algo))
        with temporary_dir() as out:
            PKG.commit_package(out, blob, signed=True, build_id=None)
            self.assertEqual(PACK.main(["verify", str(out)]), 0)
            manifest = json.loads((out / "manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["package_kind"], "diff")
            self.assertEqual(manifest["diff_algo"], "hpatchlite+tinyuz")
            self.assertEqual(manifest["base_version"], "1.0.0.0")
            self.assertEqual(manifest["fw_version"], "1.1.0.0")

    def test_missing_key_or_tool_writes_nothing(self) -> None:
        with temporary_dir() as out:
            old = out / "old.signed.bin"
            new = out / "new.signed.bin"
            old.write_bytes(mcuboot_image(b"old"))
            new.write_bytes(mcuboot_image(b"new"))
            code = PACK.main(
                [
                    "diff",
                    "--old",
                    str(old),
                    "--new",
                    str(new),
                    "--device-code",
                    "1",
                    "--base-version",
                    "1.0.0+0",
                    "--version",
                    "1.1.0+0",
                    "--key",
                    str(out / "missing.pem"),
                    "-o",
                    str(out / "pkg"),
                ]
            )
            self.assertEqual(code, 1)
            self.assertFalse((out / "pkg" / "firmware.bin").is_file())

    def test_roundtrip_restores_the_signed_image(self) -> None:
        hdiffi = HOST / "hdiffi.exe"
        hpatchi = HOST / "hpatchi.exe"
        if not hdiffi.is_file() or not hpatchi.is_file() or not IMGTOOL.is_file():
            self.fail(f"host patch tools or imgtool are missing under {HOST}")
        with temporary_dir() as out:
            key = out / "fuota-169-test.pem"
            subprocess.run(
                [sys.executable, str(IMGTOOL), "keygen", "-k", str(key), "-t", "ecdsa-p256"],
                check=True,
                capture_output=True,
                text=True,
            )
            old_raw = out / "old.bin"
            new_raw = out / "new.bin"
            old_signed = out / "old.signed.bin"
            new_signed = out / "new.signed.bin"
            old_raw.write_bytes(b"\x11" * 128)
            new_raw.write_bytes(b"\x22" * 256)
            imgtool = [sys.executable, str(IMGTOOL)]
            sign_file(
                old_raw,
                old_signed,
                key,
                PKG.parse_version("1.0.0+1"),
                0x200,
                0x2000,
                8,
                imgtool,
            )
            sign_file(
                new_raw,
                new_signed,
                key,
                PKG.parse_version("1.1.0+2"),
                0x200,
                0x2000,
                8,
                imgtool,
            )
            package = out / "pkg"
            code = PACK.main(
                [
                    "diff",
                    "--old",
                    str(old_signed),
                    "--new",
                    str(new_signed),
                    "--device-code",
                    "9",
                    "--base-version",
                    "1.0.0+1",
                    "--version",
                    "1.1.0+2",
                    "--key",
                    str(key),
                    "--hdiffi",
                    str(hdiffi),
                    "--hpatchi",
                    str(hpatchi),
                    "--imgtool",
                    str(IMGTOOL),
                    "-o",
                    str(package),
                ]
            )
            self.assertEqual(code, 0)
            firmware = (package / "firmware.bin").read_bytes()
            self.assertEqual(firmware[20:22], bytes((PKG.PACKAGE_DIFF, PKG.ALGO_HPATCH)))
            self.assertEqual(PKG.package_errors(firmware), [])
            self.assertGreater(len(firmware), PKG.HEADER_SIZE)
            manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["base_version"], "1.0.0.1")
            self.assertEqual(manifest["fw_version"], "1.1.0.2")
            self.assertEqual(PACK.main(["verify", str(package)]), 0)

            broken = PACK.main(
                [
                    "diff",
                    "--old",
                    str(old_signed),
                    "--new",
                    str(new_signed),
                    "--device-code",
                    "9",
                    "--base-version",
                    "1.0.0+1",
                    "--version",
                    "1.1.0+2",
                    "--key",
                    str(key),
                    "--hdiffi",
                    str(hdiffi),
                    "--hpatchi",
                    sys.executable,
                    "--imgtool",
                    str(IMGTOOL),
                    "-o",
                    str(package),
                ]
            )
            self.assertEqual(broken, 1)
            self.assertFalse((package / "firmware.bin").is_file())
            self.assertFalse((package / "firmware.md5").is_file())

    def _temp(self):
        return temporary_dir()


@contextmanager
def temporary_dir():
    with tempfile.TemporaryDirectory() as name:
        yield Path(name)


if __name__ == "__main__":
    unittest.main()