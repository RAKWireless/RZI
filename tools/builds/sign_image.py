#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless
# SPDX-License-Identifier: Apache-2.0
"""Sign a full image with imgtool on the signing machine (FUOTA-167).

The private key is read from --key or RZI_SIGNING_KEY. The path must be
outside this repository, and MCUboot's published sample keys are refused.
This script never writes the key next to the signed image.

Usage, on the signing machine::

    python tools/builds/sign_image.py \\
        --key /secure/fuota-sign.pem \\
        --version 1.2.3+4 \\
        --header-size 0x200 \\
        --slot-size 0x34000 \\
        --align 8 \\
        zephyr.bin -o zephyr.signed.bin

header-size, slot-size, and align must match that board's MCUboot geometry.
imgtool is taken from --imgtool, ZEPHYR_BASE/scripts/imgtool.py, PATH, or
``python -m imgtool``.
"""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import fuota_package as pkg  # noqa: E402

REPO_ROOT = _HERE.parents[1]
SAMPLE_KEY_NAMES = frozenset(
    {
        "root-ec-p256.pem",
        "root-ed25519.pem",
        "root-rsa-2048.pem",
    }
)
MCUBOOT_MAGIC = pkg.MCUBOOT_MAGIC


class SignError(Exception):
    """Signing input or imgtool failed."""


def key_policy_error(key: Path, repo: Path = REPO_ROOT) -> str | None:
    """Return why this key must not be used, or None when the path is allowed."""
    if not key.is_file():
        return f"signing key not found: {key}"
    try:
        key.resolve().relative_to(repo.resolve())
    except ValueError:
        inside = False
    else:
        inside = True
    if inside:
        return "signing key must stay outside the RZI repository"
    if key.name in SAMPLE_KEY_NAMES:
        return "refusing an MCUboot sample key; use the key held by the signing environment"
    return None


def find_imgtool(explicit: str | None) -> list[str]:
    if explicit:
        return [sys.executable, explicit]
    zephyr_base = os.environ.get("ZEPHYR_BASE")
    if zephyr_base:
        script = Path(zephyr_base) / "scripts" / "imgtool.py"
        if script.is_file():
            return [sys.executable, str(script)]
    found = shutil.which("imgtool")
    if found:
        return [found]
    return [sys.executable, "-m", "imgtool"]


def parse_size(text: str) -> int:
    try:
        value = int(text, 0)
    except ValueError as exc:
        raise SignError(f"not a size: {text}") from exc
    if value <= 0:
        raise SignError(f"size must be positive, got {text}")
    return value


def sign_command(
    imgtool: list[str],
    key: Path,
    version: pkg.Version,
    header_size: int,
    slot_size: int,
    align: int,
    source: Path,
    output: Path,
) -> list[str]:
    return [
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
    ]


def verify_command(imgtool: list[str], key: Path, output: Path) -> list[str]:
    return [*imgtool, "verify", "--key", str(key), str(output)]


def _run(command: list[str]) -> None:
    completed = subprocess.run(command, check=False, text=True, capture_output=True)
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout or "").strip()
        raise SignError(detail or f"command failed: {command[0]}")


def sign_file(
    source: Path,
    output: Path,
    key: Path,
    version: pkg.Version,
    header_size: int,
    slot_size: int,
    align: int,
    imgtool: list[str],
) -> None:
    if not source.is_file():
        raise SignError(f"image not found: {source}")
    problem = key_policy_error(key)
    if problem:
        raise SignError(problem)
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        _run(sign_command(imgtool, key, version, header_size, slot_size, align, source, output))
        if not output.is_file() or output.stat().st_size < 4:
            raise SignError("imgtool did not write a signed image")
        magic = struct.unpack_from("<I", output.read_bytes(), 0)[0]
        if magic != MCUBOOT_MAGIC:
            raise SignError("signed image is missing the MCUboot magic")
        _run(verify_command(imgtool, key, output))
    except Exception:
        if output.is_file():
            output.unlink()
        raise


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Sign a full image with an external MCUboot key.")
    parser.add_argument("image", help="unsigned zephyr.bin")
    parser.add_argument("-o", "--output", required=True, help="zephyr.signed.bin to write")
    parser.add_argument("--key", default=os.environ.get("RZI_SIGNING_KEY"), help="private key outside the repo")
    parser.add_argument("--version", required=True, help="M.m.p.b or M.m.p+b, matching the package target_version")
    parser.add_argument("--header-size", required=True, help="MCUboot header size, for example 0x200")
    parser.add_argument("--slot-size", required=True, help="MCUboot slot size, for example 0x34000")
    parser.add_argument("--align", required=True, type=int, help="flash write alignment")
    parser.add_argument("--imgtool", default=None, help="path to imgtool.py when it is not on PATH")
    args = parser.parse_args(argv)
    try:
        if not args.key:
            raise SignError("pass --key or set RZI_SIGNING_KEY; the sample MCUboot key is not accepted")
        version = pkg.parse_version(args.version)
        sign_file(
            Path(args.image),
            Path(args.output),
            Path(args.key),
            version,
            parse_size(args.header_size),
            parse_size(args.slot_size),
            args.align,
            find_imgtool(args.imgtool),
        )
    except (SignError, pkg.PackageError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
