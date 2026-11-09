# Developer tools

Repository-maintained tools for development, CI, documentation, and releases
live here. Run them from the RZI repository root unless noted otherwise.
Zephyr's own tools remain under the workspace's `zephyr/scripts/` directory.

| Tool | Purpose |
|---|---|
| [`ci/check-commits.py`](ci/check-commits.py) | Commit topology, messages, and DCO; supports GitHub event ranges |
| [`ci/check-commit-msg.sh`](ci/check-commit-msg.sh) | Proposed message checks for the Git commit hook |
| [`ci/check-style.sh`](ci/check-style.sh) | C formatting, file documentation, and checkpatch |
| [`ci/check-compliance.sh`](ci/check-compliance.sh) | Zephyr compliance checks adapted for this module |
| [`ci/check-public-headers.py`](ci/check-public-headers.py) | Compile public headers as C and C++ using a configured build |
| [`sbom/check-sbom.py`](sbom/check-sbom.py) | Validate minimum SBOM elements |
| [`docs/generate-doxygen.sh`](docs/generate-doxygen.sh) | Generate the API documentation |
| [`sbom/generate-sbom.sh`](sbom/generate-sbom.sh) / [`sbom/generate-sbom.py`](sbom/generate-sbom.py) | Generate the module SBOM and source archive |
| [`sbom/generate-firmware-sbom.py`](sbom/generate-firmware-sbom.py) | Generate SBOMs for built firmware images |
| [`patches/generate-patch-manifest.py`](patches/generate-patch-manifest.py) | Refresh the west patch manifest |
| `tests/` | Regression tests for developer tooling and CI gates |

See [CI](../doc/ci.md) for dependencies and commands and the
[maintainer workflow](../doc/maintainer-workflow.md) for merge and release
policy.

Keep reusable developer tools here as the collection grows. Use descriptive
lowercase kebab-case names, provide usage documentation, and return nonzero
on failure. Resolve repository paths relative to the tool's location rather
than a user's home directory. Group tools one level deep by responsibility:
`ci/`, `docs/`, `sbom/`, and `patches/`. Keep tooling tests together in
`tests/`; add new categories when actual tools need them. Application examples belong in
`samples/`, and firmware behavior tests belong in `tests/`.

## Local CI preflight

Use `ci/preflight.py --mode quick` or `--mode full` with
`--compliance-python PATH` to run shared CI gates. `--staged` checks the Git
index in a disposable clone without including other working changes.
See [CI local reproduction](../doc/ci.md#local-reproduction) for isolated
Python environments, dependency locks, prerequisites and diagnostic logs.
