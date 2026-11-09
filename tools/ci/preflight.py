#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless
# SPDX-License-Identifier: Apache-2.0
"""Run the shared CI checks locally; --staged validates a disposable index snapshot."""

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def plan(repo, workspace, mode, revision_range, compliance_python):
    """Return named commands, working directories, and Python profiles."""
    python = sys.executable
    west = [python, "-m", "west"]
    checks = [
        ("workflows", ["tools/ci/check-workflows.sh"], repo, False),
        ("commits", [python, "tools/ci/check-commits.py", revision_range], repo, False),
        (
            "tool-tests",
            [python, "-m", "unittest", "discover", "-s", "tools/tests", "-p", "test_*.py", "-v"],
            repo,
            False,
        ),
        ("style", ["tools/ci/check-style.sh", "--no-checkpatch"], repo, True),
        ("compliance", ["tools/ci/check-compliance.sh", revision_range], repo, True),
        ("source", ["tools/ci/check-source.sh"], repo, False),
        ("reuse", [str(compliance_python), "-m", "reuse", "lint"], repo, True),
        ("documentation", ["tools/docs/generate-doxygen.sh"], repo, False),
        ("module-sbom", ["tools/ci/check-module-sbom.sh"], repo, False),
    ]
    if mode == "full":
        output = repo / "build/preflight"
        checks += [
            (
                "native-tests",
                [
                    *west,
                    "twister",
                    "-T",
                    str(repo / "tests"),
                    "--integration",
                    "--inline-logs",
                    "-v",
                    "-O",
                    str(output / "tests"),
                ],
                workspace,
                False,
            ),
            (
                "header-build",
                [
                    *west,
                    "build",
                    "-b",
                    "native_sim/native/64",
                    str(repo / "tests/lorawan/service"),
                    "-d",
                    str(output / "headers"),
                    "--pristine=always",
                ],
                workspace,
                False,
            ),
            (
                "headers",
                [python, str(repo / "tools/ci/check-public-headers.py"), str(output / "headers")],
                workspace,
                False,
            ),
            (
                "samples",
                [
                    *west,
                    "twister",
                    "-T",
                    str(repo / "samples"),
                    "--integration",
                    "--inline-logs",
                    "-v",
                    "-O",
                    str(output / "samples"),
                ],
                workspace,
                False,
            ),
            (
                "firmware-build",
                [
                    *west,
                    "build",
                    "-b",
                    "rak4631/nrf52840/rzi",
                    "--sysbuild",
                    str(repo / "samples/at"),
                    "-d",
                    str(output / "firmware"),
                    "--pristine=always",
                    "--",
                    "-DCONFIG_BUILD_OUTPUT_META=y",
                    "-Dmcuboot_CONFIG_BUILD_OUTPUT_META=y",
                ],
                workspace,
                False,
            ),
            (
                "firmware-sbom",
                [str(repo / "tools/ci/check-firmware-sbom.sh"), str(output / "firmware")],
                workspace,
                False,
            ),
        ]
    return checks


def execute(checks, reports, compliance_python, zephyr):
    reports.mkdir(parents=True, exist_ok=True)
    results = []
    for name, command, cwd, compliance in checks:
        env = os.environ.copy()
        env["PATH"] = str(Path(sys.executable).parent) + os.pathsep + env["PATH"]
        env["ZEPHYR_BASE"] = str(zephyr)
        env["RZI_REPORT_DIR"] = str(reports)
        if compliance:
            env["PATH"] = str(compliance_python.parent) + os.pathsep + env["PATH"]
        if name in ("native-tests", "header-build", "headers"):
            env["ZEPHYR_TOOLCHAIN_VARIANT"] = "host"
        elif name in ("samples", "firmware-build"):
            env["ZEPHYR_TOOLCHAIN_VARIANT"] = "zephyr"
        print(f"[{name}] {' '.join(map(str, command))}", flush=True)
        with (reports / f"{name}.log").open("w", encoding="utf-8") as log:
            try:
                result = subprocess.run(
                    command, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT, check=False
                )
                code = result.returncode
            except OSError as exc:
                log.write(str(exc))
                code = 127
        if name == "documentation":
            warnings = cwd / "doc/doxygen/warnings.log"
            if warnings.is_file() and warnings.stat().st_size:
                code = 1
        results.append({"check": name, "exit_code": code})
        (reports / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
        print(f"[{name}] {'PASS' if code == 0 else 'FAIL'}", flush=True)
    return int(any(result["exit_code"] for result in results))


def staged_snapshot(repo, destination):
    """Copy only the Git index; leave unstaged and untracked user files untouched."""
    subprocess.run(
        ["git", "clone", "--shared", "--quiet", "--no-checkout", str(repo), str(destination)],
        check=True,
    )
    subprocess.run(["git", "checkout", "--quiet", "--detach", "HEAD"], cwd=destination, check=True)
    patch = subprocess.check_output(["git", "diff", "--cached", "--binary", "HEAD"], cwd=repo)
    if not patch:
        raise ValueError("--staged requires staged changes")
    subprocess.run(["git", "apply", "--index"], input=patch, cwd=destination, check=True)
    subprocess.run(
        [
            "git",
            "-c",
            "user.name=RZI preflight",
            "-c",
            "user.email=preflight@localhost",
            "commit",
            "-q",
            "-s",
            "-m",
            "chore(ci): validate staged source",
            "-m",
            "Create a disposable snapshot for local checks.",
        ],
        cwd=destination,
        check=True,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--mode", choices=("quick", "full"), default="quick")
    parser.add_argument("--range", dest="revision_range", default="HEAD^!")
    parser.add_argument("--staged", action="store_true", help="quick mode: validate the Git index")
    parser.add_argument("--compliance-python", type=Path, required=True)
    parser.add_argument("--reports", type=Path)
    parser.add_argument("--list", action="store_true", help="print the check plan without running")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    try:
        workspace = Path(
            subprocess.check_output(
                [sys.executable, "-m", "west", "topdir"], cwd=repo, text=True
            ).strip()
        )
    except (OSError, subprocess.CalledProcessError):
        parser.error("activate the build environment and configure a west workspace first")
    zephyr = Path(os.environ.get("ZEPHYR_BASE", workspace / "zephyr")).resolve()
    compliance_python = args.compliance_python.absolute()
    reports = (args.reports or repo / "reports/preflight").resolve()
    if args.staged and args.mode != "quick":
        parser.error("--staged supports quick mode; full builds require a clean tracked tree")
    if args.list:
        for name, command, _, _ in plan(
            repo, workspace, args.mode, args.revision_range, compliance_python
        ):
            print(f"{name}: {' '.join(map(str, command))}")
        return 0
    if not compliance_python.is_file():
        parser.error(
            "compliance Python missing; create its venv and install the compliance profile"
        )
    if args.staged:
        with tempfile.TemporaryDirectory(prefix="rzi-preflight-") as temporary:
            snapshot = Path(temporary) / "rzi"
            try:
                staged_snapshot(repo, snapshot)
            except ValueError as exc:
                parser.error(str(exc))
            return execute(
                plan(snapshot, workspace, args.mode, args.revision_range, compliance_python),
                reports,
                compliance_python,
                zephyr,
            )
    if subprocess.run(["git", "diff", "--quiet", "HEAD"], cwd=repo, check=False).returncode:
        parser.error(
            "tracked changes found; use --staged for quick checks or commit before --mode full"
        )
    return execute(
        plan(repo, workspace, args.mode, args.revision_range, compliance_python),
        reports,
        compliance_python,
        zephyr,
    )


if __name__ == "__main__":
    raise SystemExit(main())
