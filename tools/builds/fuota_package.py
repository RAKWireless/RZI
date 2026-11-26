# SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless
# SPDX-License-Identifier: Apache-2.0
"""Full FUOTA package header, CRC, and MD5.

Header layout follows the FUOTA detailed design, section 2.3. Multi-byte
integers are little-endian. target_version and base_version are four bytes in
major, minor, patch, build order. The magic is ASCII ``@RAK``. CRC-32/IEEE
covers only the payload (``image_length`` bytes after the 32-byte header).
The CRC field itself is written after the calculation.
"""

from __future__ import annotations

import hashlib
import json
import struct
import zlib
from pathlib import Path

HEADER_SIZE = 32
HEADER_VERSION = 1
MAGIC = b"@RAK"
MCUBOOT_MAGIC = 0x96F3B83D
PACKAGE_FULL = 0
PACKAGE_DIFF = 1
ALGO_NONE = 0
ALGO_HPATCH = 1

BIN_NAME = "firmware.bin"
MD5_NAME = "firmware.md5"
MANIFEST_NAME = "manifest.json"
UNSIGNED_BIN_NAME = "firmware.unsigned.bin"
UNSIGNED_MD5_NAME = "firmware.unsigned.md5"


class PackageError(Exception):
    """A package input or verification check failed."""


class Version:
    """Four-part firmware version stored in the business header."""

    def __init__(self, major: int, minor: int, patch: int, build: int) -> None:
        self.major = major
        self.minor = minor
        self.patch = patch
        self.build = build

    def bytes(self) -> bytes:
        return bytes((self.major, self.minor, self.patch, self.build))

    def text(self) -> str:
        return f"{self.major}.{self.minor}.{self.patch}.{self.build}"

    def imgtool(self) -> str:
        return f"{self.major}.{self.minor}.{self.patch}+{self.build}"

    def __eq__(self, other: object) -> bool:
        return isinstance(other, Version) and self.bytes() == other.bytes()


def parse_version(text: str) -> Version:
    """Accept ``major.minor.patch.build`` or imgtool's ``major.minor.patch+build``."""
    normalized = text.strip().replace("+", ".")
    parts = normalized.split(".")
    if len(parts) != 4:
        raise PackageError(f"version must be four numbers from 0 to 255, got {text!r}")
    try:
        numbers = [int(part, 10) for part in parts]
    except ValueError as exc:
        raise PackageError(f"version must be four numbers from 0 to 255, got {text!r}") from exc
    if any(number < 0 or number > 255 for number in numbers):
        raise PackageError(f"version must be four numbers from 0 to 255, got {text!r}")
    return Version(*numbers)


def parse_device_code(value: int) -> int:
    if value < 0 or value > 255:
        raise PackageError(f"device_code must be 0..255, got {value}")
    return value


def crc32(data: bytes) -> int:
    """CRC-32/IEEE (poly 0x04C11DB7, reflected, init and xor-out 0xFFFFFFFF)."""
    return zlib.crc32(data) & 0xFFFFFFFF


def is_mcuboot_image(payload: bytes) -> bool:
    if len(payload) < 4:
        return False
    magic = struct.unpack_from("<I", payload, 0)[0]
    return magic == MCUBOOT_MAGIC


def build_firmware(
    payload: bytes,
    device_code: int,
    target: Version,
    *,
    base: Version | None = None,
    package_kind: int = PACKAGE_FULL,
    diff_algo: int = ALGO_NONE,
) -> bytes:
    """Return header + payload. ``base`` defaults to 0.0.0.0 for a full package."""
    if not payload:
        raise PackageError("image is empty")
    device_code = parse_device_code(device_code)
    if package_kind not in (0, 1) or diff_algo not in (0, 1):
        raise PackageError("package_kind and diff_algo must be 0 or 1")
    base = base or Version(0, 0, 0, 0)
    header = bytearray(HEADER_SIZE)
    header[0:4] = MAGIC
    struct.pack_into("<HH", header, 4, HEADER_VERSION, HEADER_SIZE)
    header[8] = device_code
    header[12:16] = target.bytes()
    header[16:20] = base.bytes()
    header[20] = package_kind
    header[21] = diff_algo
    struct.pack_into("<H", header, 22, 0)
    struct.pack_into("<I", header, 24, len(payload))
    struct.pack_into("<I", header, 28, crc32(payload))
    return bytes(header) + payload


def parse_header(blob: bytes) -> dict:
    if len(blob) < HEADER_SIZE:
        raise PackageError("file is shorter than the 32-byte header")
    header_version, header_length = struct.unpack_from("<HH", blob, 4)
    flags = struct.unpack_from("<H", blob, 22)[0]
    image_length = struct.unpack_from("<I", blob, 24)[0]
    stored_crc = struct.unpack_from("<I", blob, 28)[0]
    return {
        "magic": blob[0:4],
        "header_version": header_version,
        "header_length": header_length,
        "device_code": blob[8],
        "product_reserved": blob[9:12],
        "target": Version(*blob[12:16]),
        "base": Version(*blob[16:20]),
        "package_kind": blob[20],
        "diff_algo": blob[21],
        "flags": flags,
        "image_length": image_length,
        "crc32": stored_crc,
    }


def header_errors(blob: bytes, *, full_only: bool = True) -> list[str]:
    """Return structural problems. An empty list means the full package is intact."""
    if len(blob) < HEADER_SIZE:
        return ["file is shorter than the 32-byte header"]
    info = parse_header(blob)
    errors: list[str] = []
    if info["magic"] != MAGIC:
        errors.append("magic is not @RAK")
    if info["header_version"] != HEADER_VERSION:
        errors.append("header_version is not 1")
    if info["header_length"] != HEADER_SIZE:
        errors.append("header_length is not 32")
    if info["product_reserved"] != b"\x00\x00\x00":
        errors.append("product_id bytes 1-3 are not 0")
    if info["flags"] != 0:
        errors.append("flags is not 0")
    payload = blob[HEADER_SIZE:]
    if info["image_length"] != len(payload):
        errors.append("image_length does not match the bytes after the header")
    if info["image_length"] <= 0:
        errors.append("image_length must be greater than 0")
    expect = crc32(payload[: info["image_length"]])
    if info["crc32"] != expect:
        errors.append("crc32 does not match the payload")
    if full_only:
        if info["package_kind"] != PACKAGE_FULL:
            errors.append("package_kind is not full")
        if info["diff_algo"] != ALGO_NONE:
            errors.append("diff_algo is not none")
        if info["base"] != Version(0, 0, 0, 0):
            errors.append("full package base_version is not 0.0.0.0")
    return errors


def md5_hex(blob: bytes) -> str:
    return hashlib.md5(blob).hexdigest()


def md5_line(blob: bytes, name: str = BIN_NAME) -> str:
    return f"{md5_hex(blob)}  {name}\n"


def manifest_for(blob: bytes, *, signed: bool, build_id: str | None) -> dict:
    info = parse_header(blob)
    body = {
        "device_code": info["device_code"],
        "fw_version": info["target"].text(),
        "base_version": (
            None
            if info["package_kind"] == PACKAGE_FULL and info["base"] == Version(0, 0, 0, 0)
            else info["base"].text()
        ),
        "package_kind": "full" if info["package_kind"] == PACKAGE_FULL else "diff",
        "diff_algo": "none" if info["diff_algo"] == ALGO_NONE else "hpatchlite+tinyuz",
        "size_bytes": len(blob),
        "signed": signed,
        "md5": md5_hex(blob),
    }
    if build_id:
        body["build_id"] = build_id
    return body


def output_names(signed: bool) -> tuple[str, str]:
    if signed:
        return BIN_NAME, MD5_NAME
    return UNSIGNED_BIN_NAME, UNSIGNED_MD5_NAME


def discard_outputs(out_dir: Path, signed: bool) -> None:
    bin_name, md5_name = output_names(signed)
    for name in (bin_name, md5_name, MANIFEST_NAME):
        path = out_dir / name
        if path.is_file():
            path.unlink()


def commit_package(out_dir: Path, blob: bytes, *, signed: bool, build_id: str | None) -> None:
    """Write the package, read it back, and delete every output if the check fails."""
    out_dir.mkdir(parents=True, exist_ok=True)
    bin_name, md5_name = output_names(signed)
    bin_path = out_dir / bin_name
    md5_path = out_dir / md5_name
    manifest_path = out_dir / MANIFEST_NAME
    bin_path.write_bytes(blob)
    md5_path.write_text(md5_line(blob, bin_name), encoding="utf-8")
    manifest = manifest_for(blob, signed=signed, build_id=build_id)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    errors = verify_tree(out_dir, signed=signed)
    if errors:
        discard_outputs(out_dir, signed)
        raise PackageError("; ".join(errors))


def package_errors(blob: bytes) -> list[str]:
    """Structural problems for either a full package or a diff package."""
    if len(blob) < HEADER_SIZE:
        return header_errors(blob)
    info = parse_header(blob)
    if info["package_kind"] == PACKAGE_DIFF:
        errors = header_errors(blob, full_only=False)
        if info["diff_algo"] != ALGO_HPATCH:
            errors.append("diff_algo is not hpatchlite+tinyuz")
        return errors
    return header_errors(blob, full_only=True)


def verify_tree(out_dir: Path, *, signed: bool) -> list[str]:
    bin_name, md5_name = output_names(signed)
    bin_path = out_dir / bin_name
    if not bin_path.is_file():
        return [f"missing {bin_name}"]
    blob = bin_path.read_bytes()
    errors = package_errors(blob)
    md5_path = out_dir / md5_name
    if not md5_path.is_file():
        errors.append(f"missing {md5_name}")
    else:
        recorded = md5_path.read_text(encoding="utf-8").strip().split()
        if not recorded or recorded[0].lower() != md5_hex(blob):
            errors.append("md5 does not match firmware bytes")
    return errors


def load_image(path: Path, *, allow_unsigned: bool) -> bytes:
    if not path.is_file():
        raise PackageError(f"image not found: {path}")
    payload = path.read_bytes()
    if not payload:
        raise PackageError("image is empty")
    if not allow_unsigned and not is_mcuboot_image(payload):
        raise PackageError(
            "image is not an MCUboot signed image; sign it first or pass --unsigned "
            "for a stand-in that must not be released"
        )
    return payload


def _recrc(blob: bytearray) -> bytes:
    payload = bytes(blob[HEADER_SIZE:])
    struct.pack_into("<I", blob, 28, crc32(payload))
    return bytes(blob)


def write_vectors(out_dir: Path, legal: bytes, device_code: int, target: Version) -> None:
    """Write the four rejection fixtures consumed by the device header checks."""
    vector_dir = out_dir / "vectors"
    vector_dir.mkdir(parents=True, exist_ok=True)

    wrong_device = bytearray(legal)
    wrong_device[8] = (device_code + 1) & 0xFF
    (vector_dir / "device_mismatch.bin").write_bytes(_recrc(wrong_device))

    bad_crc = bytearray(legal)
    bad_crc[-1] ^= 0xFF
    (vector_dir / "header_crc.bin").write_bytes(bytes(bad_crc))

    (vector_dir / "same_version.bin").write_bytes(legal)

    wrong_base = bytearray(legal)
    wrong_base[16:20] = Version(9, 9, 9, 9).bytes()
    (vector_dir / "base_mismatch.bin").write_bytes(_recrc(wrong_base))

    sheet = {
        "legal": BIN_NAME,
        "running_version_for_same_version_vector": target.text(),
        "vectors": [
            {
                "file": "vectors/device_mismatch.bin",
                "expect": "E_HEADER",
                "reason": "device_code",
                "device_code": (device_code + 1) & 0xFF,
            },
            {
                "file": "vectors/header_crc.bin",
                "expect": "E_HEADER",
                "reason": "crc32",
            },
            {
                "file": "vectors/same_version.bin",
                "expect": "OK",
                "reason": "target_version may equal the running version",
                "running_version": target.text(),
            },
            {
                "file": "vectors/base_mismatch.bin",
                "expect": "E_HEADER",
                "reason": "base_version",
                "base_version": "9.9.9.9",
            },
        ],
    }
    (out_dir / "expected.json").write_text(json.dumps(sheet, indent=2) + "\n", encoding="utf-8")
