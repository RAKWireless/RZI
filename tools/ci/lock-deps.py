#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless
# SPDX-License-Identifier: Apache-2.0
"""Generate Linux/Python 3.12 hash locks from a reviewed pip --report resolution."""

import argparse
import json
from pathlib import Path

from packaging.requirements import Requirement
from packaging.utils import canonicalize_name


def closure(items, seeds, environment):
    selected, expanded = set(), set()
    pending = [Requirement(seed) for seed in seeds]
    while pending:
        requirement = pending.pop()
        name = canonicalize_name(requirement.name)
        selected.add(name)
        for extra in set(requirement.extras) | {""}:
            if (name, extra) in expanded:
                continue
            expanded.add((name, extra))
            for value in items[name]["metadata"].get("requires_dist", []):
                dependency = Requirement(value)
                if not dependency.marker or dependency.marker.evaluate(
                    {**environment, "extra": extra}
                ):
                    pending.append(dependency)
    return selected


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    report = json.loads(args.report.read_text())
    environment = report["environment"]
    if (
        environment["python_version"],
        environment["sys_platform"],
        environment["platform_machine"],
    ) != ("3.12", "linux", "x86_64"):
        parser.error("resolve on Linux x86_64 with Python 3.12, matching the CI runner")
    items = {canonicalize_name(item["metadata"]["name"]): item for item in report["install"]}
    profiles = {
        "build": set(items),
        "bootstrap": closure(items, ["west", "PyYAML"], environment),
        "sbom": closure(
            items,
            ["PyYAML", "reuse", "spdx3-validate", "cyclonedx-python-lib[json-validation]"],
            environment,
        ),
    }
    root = Path(__file__).resolve().parents[1] / "requirements"
    for profile, names in profiles.items():
        # REUSE-IgnoreStart
        text = (
            "# SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless\n"
            "# SPDX-License-Identifier: Apache-2.0\n"
            "# Python 3.12, Linux x86_64; Zephyr revision in west.yml.\n"
            "# Update with tools/ci/lock-deps.py; review dependency changes together.\n\n"
        )
        # REUSE-IgnoreEnd
        for name in sorted(names):
            item = items[name]
            digest = item["download_info"]["archive_info"]["hashes"]["sha256"]
            text += (
                f"{item['metadata']['name']}=={item['metadata']['version']} "
                f"--hash=sha256:{digest}\n"
            )
        (root / f"{profile}.lock").write_text(text)
        print(f"{profile}: {len(names)} pinned packages")


if __name__ == "__main__":
    main()
