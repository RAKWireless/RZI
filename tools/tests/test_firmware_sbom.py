# SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless
# SPDX-License-Identifier: Apache-2.0
"""Reject incomplete firmware provenance without treating workspace inventory as firmware."""

import copy
import importlib.util
import unittest
from pathlib import Path

SPEC = importlib.util.spec_from_file_location(
    "check_sbom", Path(__file__).parents[1] / "sbom/check-sbom.py"
)
CHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK)


class FirmwareGraphTests(unittest.TestCase):
    def setUp(self):
        self.graph = [
            {"type": "build_Build", "spdxId": "build", "build_buildType": "cmake"},
            {"type": "software_File", "spdxId": "source"},
            {"type": "software_File", "spdxId": "elf"},
            {"type": "Tool", "spdxId": "compiler"},
            {
                "type": "software_File",
                "spdxId": "image",
                "name": "zephyr.signed.bin",
                "software_primaryPurpose": "firmware",
                "verifiedUsing": [
                    {"algorithm": "sha256", "hashValue": "a" * 64},
                    {"algorithm": "sha512", "hashValue": "b" * 128},
                ],
            },
            {
                "type": "software_Sbom",
                "spdxId": "sbom",
                "software_sbomType": ["build"],
                "rootElement": ["image"],
            },
        ]
        for kind, source, target in [
            ("hasInput", "build", "source"),
            ("hasOutput", "build", "elf"),
            ("usesTool", "build", "compiler"),
            ("generates", "elf", "image"),
        ]:
            self.graph.append(
                {"type": "Relationship", "from": source, "relationshipType": kind, "to": [target]}
            )

    def check(self, graph):
        return CHECK.check_firmware_graph(graph, {e["spdxId"]: e for e in graph if "spdxId" in e})

    def test_complete_provenance(self):
        self.assertEqual(self.check(self.graph), [])

    def test_workspace_inventory_is_not_firmware(self):
        gaps = self.check([{"type": "software_Package", "spdxId": "unused-module"}])
        self.assertTrue(any("no build_Build" in gap for gap in gaps))
        self.assertTrue(any("no flashed image" in gap for gap in gaps))

    def test_missing_relationships(self):
        for kind in ["hasInput", "hasOutput", "usesTool", "generates"]:
            with self.subTest(kind=kind):
                graph = [e for e in self.graph if e.get("relationshipType") != kind]
                self.assertTrue(self.check(graph))

    def test_unresolved_build_input(self):
        graph = [e for e in self.graph if e.get("spdxId") != "source"]
        self.assertTrue(any("unresolved hasInput" in gap for gap in self.check(graph)))

    def test_missing_image_hash_or_root(self):
        for key in ["verifiedUsing", "rootElement"]:
            graph = copy.deepcopy(self.graph)
            for element in graph:
                element.pop(key, None)
            self.assertTrue(self.check(graph))

    def test_generation_must_start_from_a_build_output(self):
        graph = copy.deepcopy(self.graph)
        next(e for e in graph if e.get("relationshipType") == "generates")["from"] = "source"
        self.assertTrue(any("generation provenance" in gap for gap in self.check(graph)))


if __name__ == "__main__":
    unittest.main()
