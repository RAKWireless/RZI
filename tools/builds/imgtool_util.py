# SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless
# SPDX-License-Identifier: Apache-2.0
"""Shared imgtool helpers for the FUOTA host tools.

Signing itself is done by the build (sysbuild) or by an external signing
environment; these helpers only locate imgtool, build verify commands and run
subprocesses for the packaging tools.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path


class ImgtoolError(Exception):
    """imgtool invocation failed."""


def find_imgtool(explicit: str | None) -> list[str]:
    """Return the command prefix that runs imgtool."""
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


def verify_command(imgtool: list[str], key: Path, image: Path) -> list[str]:
    """Return the imgtool command that verifies an image with the given key."""
    return [*imgtool, "verify", "--key", str(key), str(image)]


def run(command: list[str]) -> None:
    """Run a command and raise ImgtoolError with its output on failure."""
    completed = subprocess.run(command, check=False, text=True, capture_output=True)
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout or "").strip()
        raise ImgtoolError(detail or f"command failed: {command[0]}")
