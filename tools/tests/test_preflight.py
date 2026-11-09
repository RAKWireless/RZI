# SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless
# SPDX-License-Identifier: Apache-2.0
"""Test local gate coverage, failure status propagation, and retained diagnostics."""

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).parents[2]
SPEC = importlib.util.spec_from_file_location("preflight", ROOT / "tools/ci/preflight.py")
PREFLIGHT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PREFLIGHT)


class PreflightTests(unittest.TestCase):
    def test_quick_has_full_compliance_and_full_adds_builds(self):
        quick = {
            item[0]
            for item in PREFLIGHT.plan(ROOT, ROOT.parent, "quick", "HEAD^!", Path(sys.executable))
        }
        full = {
            item[0]
            for item in PREFLIGHT.plan(ROOT, ROOT.parent, "full", "HEAD^!", Path(sys.executable))
        }
        self.assertTrue({"compliance", "style", "reuse", "module-sbom", "workflows"} <= quick)
        self.assertTrue(quick < full)
        self.assertTrue({"native-tests", "headers", "samples", "firmware-sbom"} <= full)

    def test_failing_command_remains_failure_with_log_and_summary(self):
        with tempfile.TemporaryDirectory() as temporary:
            reports = Path(temporary)
            checks = [
                ("failure", [sys.executable, "-c", "print('evidence'); exit(7)"], reports, False)
            ]
            result = PREFLIGHT.execute(checks, reports, Path(sys.executable), reports)
            self.assertEqual(result, 1)
            self.assertIn("evidence", (reports / "failure.log").read_text())
            self.assertEqual(json.loads((reports / "summary.json").read_text())[0]["exit_code"], 7)

    def test_firmware_validator_keeps_failure_and_checks_other_images(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bindir = root / "bin"
            bindir.mkdir()
            for name, body in [
                ("python3", "echo generation-or-elements; exit 0"),
                ("spdx3-validate", "echo schema-failure; exit 9"),
            ]:
                executable = bindir / name
                executable.write_text("#!/bin/sh\n" + body + "\n")
                executable.chmod(0o755)
            for image in ["app", "mcuboot"]:
                path = root / "spdx" / image
                path.mkdir(parents=True)
                (path / "app.jsonld").write_text("{}")
            result = subprocess.run(
                [str(ROOT / "tools/ci/check-firmware-sbom.sh"), str(root), str(root / "spdx")],
                env={
                    **os.environ,
                    "PATH": str(bindir) + os.pathsep + os.environ["PATH"],
                    "RZI_REPORT_DIR": str(root / "reports"),
                },
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            for image in ["app", "mcuboot"]:
                self.assertIn(
                    "schema-failure",
                    (root / "reports" / f"firmware-{image}-schema.log").read_text(),
                )
                self.assertTrue((root / "reports" / f"firmware-{image}-elements.log").exists())


if __name__ == "__main__":
    unittest.main()
