# Developer tools

Repository-maintained tools for development, CI, documentation, and releases
live here. Run them from the RZI repository root unless noted otherwise.
Zephyr's own tools remain under the workspace's `zephyr/scripts/` directory.

| Tool | Purpose |
|---|---|
| [`ci/check-commits.py`](https://github.com/RAKWireless/RZI/blob/main/tools/ci/check-commits.py) | Commit topology, messages, and DCO; supports GitHub event ranges |
| [`ci/check-commit-msg.sh`](https://github.com/RAKWireless/RZI/blob/main/tools/ci/check-commit-msg.sh) | Proposed message checks for the Git commit hook |
| [`ci/check-style.sh`](https://github.com/RAKWireless/RZI/blob/main/tools/ci/check-style.sh) | C formatting, file documentation, and checkpatch |
| [`ci/check-compliance.sh`](https://github.com/RAKWireless/RZI/blob/main/tools/ci/check-compliance.sh) | Zephyr compliance checks adapted for this module |
| [`ci/check-public-headers.py`](https://github.com/RAKWireless/RZI/blob/main/tools/ci/check-public-headers.py) | Compile public headers as C and C++ using a configured build |
| [`sbom/check-sbom.py`](https://github.com/RAKWireless/RZI/blob/main/tools/sbom/check-sbom.py) | Validate minimum SBOM elements |
| [`docs/generate-doxygen.sh`](https://github.com/RAKWireless/RZI/blob/main/tools/docs/generate-doxygen.sh) | Generate the API documentation |
| [docs/generate-mdbook.sh](docs/generate-mdbook.sh) | Build the chapter-based HTML guide without stale output |
| [docs/resolve-doxygen-links.py](docs/resolve-doxygen-links.py) | Resolve generated chapter links and validate their anchors |
| [`sbom/generate-sbom.sh`](https://github.com/RAKWireless/RZI/blob/main/tools/sbom/generate-sbom.sh) / [`sbom/generate-sbom.py`](https://github.com/RAKWireless/RZI/blob/main/tools/sbom/generate-sbom.py) | Generate the module SBOM and source archive |
| [`sbom/generate-firmware-sbom.py`](https://github.com/RAKWireless/RZI/blob/main/tools/sbom/generate-firmware-sbom.py) | Generate SBOMs for built firmware images |
| [`patches/generate-patch-manifest.py`](https://github.com/RAKWireless/RZI/blob/main/tools/patches/generate-patch-manifest.py) | Refresh the west patch manifest |
| `tests/` | Regression tests for developer tooling and CI gates |

See [CI](../doc/development/ci.md) for dependencies and commands and the
[maintainer workflow](../doc/development/maintainer-workflow.md) for merge and release
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
See [CI local reproduction](../doc/development/ci.md#local-reproduction) for isolated
Python environments, dependency locks, prerequisites and diagnostic logs.
