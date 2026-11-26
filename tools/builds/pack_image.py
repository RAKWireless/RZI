#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless
# SPDX-License-Identifier: Apache-2.0
"""Pack a signed image into a FUOTA firmware.bin (FUOTA-165, FUOTA-169).

Usage, from the RZI repository root::

    python tools/builds/pack_image.py pack \\
        --image zephyr.signed.bin --device-code 1 --version 1.2.3+4 \\
        -o out/fuota

    python tools/builds/pack_image.py diff \\
        --old old.signed.bin --new new.signed.bin \\
        --device-code 1 --base-version 1.0.0+0 --version 1.1.0+0 \\
        --key C:/secure/fuota-sign.pem --hdiffi hdiffi --hpatchi hpatchi \\
        -o out/fuota

    python tools/builds/pack_image.py verify out/fuota

``pack`` writes a full image. ``diff`` writes an HPatchLite+tinyuz patch
(``hdiffi -c-tuz-512``) under the same 32-byte header. The patch is restored
with ``hpatchi -s-256`` and the result is compared to the new image, then
checked with ``imgtool verify``. A failure deletes the package outputs.
A stand-in image that is not MCUboot-signed must be passed with --unsigned;
that output is named firmware.unsigned.bin and is not a release package.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import fuota_package as pkg  # noqa: E402
import sign_image  # noqa: E402


def _read_args(args: argparse.Namespace) -> tuple[bytes, int, pkg.Version]:
    device_code = pkg.parse_device_code(args.device_code)
    version = pkg.parse_version(args.version)
    payload = pkg.load_image(Path(args.image), allow_unsigned=args.unsigned)
    return payload, device_code, version


def _pack(args: argparse.Namespace) -> None:
    payload, device_code, version = _read_args(args)
    blob = pkg.build_firmware(payload, device_code, version)
    pkg.commit_package(Path(args.output), blob, signed=not args.unsigned, build_id=args.build_id)


def _verify(args: argparse.Namespace) -> None:
    errors = pkg.verify_tree(Path(args.directory), signed=not args.unsigned)
    if errors:
        raise pkg.PackageError("; ".join(errors))


def _tool(explicit: str | None, name: str) -> str:
    if explicit:
        path = Path(explicit)
        if not path.is_file():
            raise pkg.PackageError(f"{name} not found: {explicit}")
        return str(path)
    found = shutil.which(name)
    if not found:
        raise pkg.PackageError(f"{name} not found; pass --{name}")
    return found


def _run_tool(command: list[str]) -> None:
    completed = subprocess.run(command, check=False, text=True, capture_output=True)
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout or "").strip()
        raise pkg.PackageError(detail or f"command failed: {command[0]}")


def _diff(args: argparse.Namespace) -> None:
    """Build a diff package only after the patch restores and verifies."""
    out_dir = Path(args.output)
    try:
        old_path = Path(args.old)
        new_path = Path(args.new)
        key = Path(args.key)
        device_code = pkg.parse_device_code(args.device_code)
        target = pkg.parse_version(args.version)
        base = pkg.parse_version(args.base_version)
        if not key.is_file():
            raise pkg.PackageError(f"verify key not found: {key}")
        if args.imgtool and not Path(args.imgtool).is_file():
            raise pkg.PackageError(f"imgtool not found: {args.imgtool}")
        pkg.load_image(old_path, allow_unsigned=False)
        new_bytes = pkg.load_image(new_path, allow_unsigned=False)
        hdiffi = _tool(args.hdiffi, "hdiffi")
        hpatchi = _tool(args.hpatchi, "hpatchi")
        imgtool = sign_image.find_imgtool(args.imgtool)
        with tempfile.TemporaryDirectory() as name:
            temp = Path(name)
            patch_path = temp / "patch.lite"
            packed_patch = temp / "packed.patch"
            restored = temp / "restored.bin"
            _run_tool([hdiffi, "-c-tuz-512", str(old_path), str(new_path), str(patch_path)])
            if not patch_path.is_file() or patch_path.stat().st_size == 0:
                raise pkg.PackageError("hdiffi did not write a patch")
            blob = pkg.build_firmware(
                patch_path.read_bytes(),
                device_code,
                target,
                base=base,
                package_kind=pkg.PACKAGE_DIFF,
                diff_algo=pkg.ALGO_HPATCH,
            )
            packed_patch.write_bytes(blob[pkg.HEADER_SIZE:])
            _run_tool([hpatchi, "-s-256", str(old_path), str(packed_patch), str(restored)])
            if restored.read_bytes() != new_bytes:
                raise pkg.PackageError("restored image does not match the new signed image")
            try:
                sign_image._run(sign_image.verify_command(imgtool, key, restored))
            except sign_image.SignError as exc:
                raise pkg.PackageError(str(exc)) from exc
        pkg.commit_package(out_dir, blob, signed=True, build_id=args.build_id)
    except pkg.PackageError:
        pkg.discard_outputs(out_dir, True)
        raise


def _vectors(args: argparse.Namespace) -> None:
    if args.unsigned:
        raise pkg.PackageError("rejection vectors are built from a signed full package")
    payload, device_code, version = _read_args(args)
    blob = pkg.build_firmware(payload, device_code, version)
    out_dir = Path(args.output)
    pkg.commit_package(out_dir, blob, signed=True, build_id=args.build_id)
    pkg.write_vectors(out_dir, blob, device_code, version)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Pack a full or differential FUOTA image.")
    sub = parser.add_subparsers(dest="command", required=True)

    pack = sub.add_parser("pack", help="write firmware.bin, firmware.md5, and manifest.json")
    diff = sub.add_parser("diff", help="write a verified HPatchLite+tinyuz differential package")
    verify = sub.add_parser("verify", help="re-read a package and check the header, CRC, and MD5")
    vectors = sub.add_parser("vectors", help="write a legal full package plus rejection fixtures")
    diff.add_argument("--old", required=True, help="old signed image")
    diff.add_argument("--new", required=True, help="new signed image")
    diff.add_argument("--device-code", required=True, type=int, help="product_id byte 0, 0..255")
    diff.add_argument("--base-version", required=True, help="version of the old image")
    diff.add_argument("--version", required=True, help="version of the new image")
    diff.add_argument("--key", required=True, help="key used to verify the restored image")
    diff.add_argument("--hdiffi", default=None, help="path to hdiffi; default is PATH")
    diff.add_argument("--hpatchi", default=None, help="path to hpatchi; default is PATH")
    diff.add_argument("--imgtool", default=None, help="path to imgtool.py")
    diff.add_argument("-o", "--output", default="out/fuota", help="output directory")
    diff.add_argument("--build-id", default=None, help="optional id copied into manifest.json")
    for command in (pack, vectors):
        command.add_argument("--image", required=True, help="signed full image (zephyr.signed.bin)")
        command.add_argument("--device-code", required=True, type=int, help="product_id byte 0, 0..255")
        command.add_argument("--version", required=True, help="target version M.m.p.b or M.m.p+b")
        command.add_argument("-o", "--output", default="out/fuota", help="output directory")
        command.add_argument("--build-id", default=None, help="optional id copied into manifest.json")
        command.add_argument(
            "--unsigned",
            action="store_true",
            help="accept a non-MCUboot image and write firmware.unsigned.bin",
        )
    verify.add_argument("directory", help="directory containing the package")
    verify.add_argument(
        "--unsigned",
        action="store_true",
        help="check firmware.unsigned.bin instead of firmware.bin",
    )
    pack.set_defaults(func=_pack)
    diff.set_defaults(func=_diff)
    verify.set_defaults(func=_verify)
    vectors.set_defaults(func=_vectors)

    args = parser.parse_args(argv)
    try:
        args.func(args)
    except pkg.PackageError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
