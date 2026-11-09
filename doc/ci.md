# CI

Status: implemented

GitHub Actions gates every push to `main` and every pull request. Compliance
follows Zephyr's `scripts/ci/check_compliance.py` workflow. Tests and samples
follow the `example-application` twister invocation (`--integration`).
Public-header, documentation, and SBOM checks are RZI gates. The commit-policy
job checks DCO, commit messages, and topology on the original commits.
`CI required` aggregates all gates and fails on any failure, cancellation,
or unexpected skip. Server-side activation is documented in
[maintainer-workflow.md](./maintainer-workflow.md).

See also: [coding-standards.md](./coding-standards.md),
[architecture.md](./architecture.md), [west-patch.md](./west-patch.md),
[sbom.md](./sbom.md).

## Workflow

`.github/workflows/ci.yml` runs independent checks in parallel, including
four sample-build shards and a separate firmware-SBOM job. Composite actions
`.github/actions/setup-workspace` and `.github/actions/setup-sdk` build the
west workspace and install the Zephyr SDK for the jobs that need them.

| Job | What it proves |
|---|---|
| Compliance | `tools/ci/check-compliance.sh` (Zephyr `check_compliance.py`), `tools/ci/check-style.sh --no-checkpatch` (clang-format, `@file` / `@brief`), no backend symbols in `include/rzi/`, ASCII-only source, `reuse lint`, version strings in sync (`version.h`, `Doxyfile`, `module.yml` CPE / PURL) |
| Commit policy | Original commits have Conventional Commit titles, explanatory bodies, matching author sign-offs, and no merge/fixup commits; policy regression tests pass |
| Workflow validation | Checksum-verified actionlint checks workflow syntax and expressions |
| CI required | Every validation job explicitly succeeded; the stable required check for branch protection |
| Twister (native_sim) | `west twister -T rzi/tests --integration` on the host |
| Samples (shards 1-4) | `west twister -T rzi/samples --integration --subset N/4` with the Zephyr SDK; the four deterministic subsets cover the original integration build set, including RAK4631, RAK3172, RAK3372-F, and RAK3362 |
| Firmware SBOM | `samples/at` on RAK4631 with build metadata, an SPDX 3 firmware SBOM, `spdx3-validate`, and `check-sbom.py --strict`; runs alongside the sample shards |
| Samples (product boards) | Stable aggregate check; requires all four sample shards and the firmware SBOM to succeed, including when dependencies fail or are skipped |
| Public headers | every `include/rzi/**.h` compiles standalone as C11 and as C++17 |
| Documentation | `tools/docs/generate-doxygen.sh` with zero warnings |
| SBOM | `tools/sbom/generate-sbom.sh`, `spdx3-validate`, CycloneDX 1.6 schema, and `check-sbom.py --strict` on the module SBOM |

Twister jobs always upload compact JSON/XML reports and logs (retained for
seven days), and upload the full `twister-out/` on failure. Shard artifact
names are unique. The SBOM jobs upload the module and firmware SBOMs.
Jobs have explicit timeouts; a failed sample shard does not cancel the
remaining shards, so their diagnostics are still collected.

The sample shards use Twister's built-in `--subset`, without random shuffling
or a fixed board allowlist. Newly added integration builds therefore join the
same four-way split automatically. Each shard builds from a clean workspace;
only dependency downloads are cached. Parallelism shortens elapsed time but
adds repeated runner setup, so compare both elapsed time and runner minutes.

`.github/workflows/release.yml` runs on a `v<version>` tag, validates its
version and mainline ancestry, calls the complete CI workflow on that tag,
and prepares a draft with release images, SBOMs, and checksums; see
[sbom.md](./sbom.md#releases).

CI runs for PRs, pushes to `main` and `release/**`, manual invocations, and
merge-queue groups. `workflow_call` supports release validation. PR build
jobs check out GitHub's synthetic merge result, not just the contributor's
tip. The compliance range still selects original PR commits for patch-based
checks. CI never rebases or pushes a PR branch. Merge queues test the exact
queued integration; their generated merge commits are excluded from the
author-message and DCO policy, while original commits remain checked.

External Actions use full commit SHA pins; checkouts do not retain credentials.
Validation has read-only permissions. No job may pass the aggregate gate
through `continue-on-error`; cache failures alone may fall back to a fresh
download. Import the documented rulesets to activate branch protection.

## Workspace construction

RZI is a module, so CI builds the workspace a customer would:

1. A temporary manifest is generated from the `zephyr`, `usp_zephyr`, and
   `usp` projects in `west.yml`. The checked-out RZI tree is the code under
   test; it is not cloned again.
2. `west update --narrow -o=--filter=blob:none` fetches those revisions
   without a depth-1 clone. A shallow fetch of a pinned SHA is an unadvertised
   object and git rejects it.
3. `zephyr`, `usp_zephyr`, and `usp` are each reset to the revision in
   `west.yml`. The workspace cache otherwise keeps the previous run's
   patched tree, and the next patch then fails its context check. The
   patches under `zephyr/patches/zephyr/`, `zephyr/patches/usp_zephyr/`,
   and `zephyr/patches/usp/` are applied with `git apply`. This mirrors
   `west patch -sm rzi apply` without making RZI a west project of the
   CI manifest.
4. `ZEPHYR_EXTRA_MODULES` points at the checkout, so builds and twister see
   RZI exactly as committed.

The workspace and the Zephyr SDK are cached between runs; the workspace cache
key includes the `west.yml` hash, with an older workspace as a fetch fallback.
Pinned revisions are still fetched and patched on every run.
`setup-workspace` also caches pip downloads before installing west and the
job's requirements. Its key includes the runner OS/architecture, Python 3.12,
job ID, and `west.yml` hash. Package installation and dependency resolution
still run on a cache hit; installed environments and build outputs are not
restored. All callers currently select Python 3.12; update the cache key if
that version changes.

## Version pins

| Pin | Where | Bump policy |
|---|---|---|
| Zephyr revision | `west.yml` | Together with `zephyr/patches/`; drop patches the new revision already contains |
| `usp_zephyr` / `usp` revisions | `west.yml` | Together with `zephyr/patches/` |
| `ZEPHYR_SDK_VERSION` | `ci.yml` env | Match the SDK required by the Zephyr revision |
| `CLANG_FORMAT_VERSION` | `ci.yml` env | Same release developers run locally |
| `DOXYGEN_VERSION` | `ci.yml` env | Same release developers run locally |
| REUSE 6.2.0 | SBOM installs in `ci.yml` and `release.yml` | Verify the pinned Zephyr scanner API before changing |

`west.yml` is the only copy of the Zephyr and USP revisions; CI reads them
from there.

The SBOM jobs pin REUSE because the Zephyr 4.5 scanner calls
`ReuseTOML.from_file(path)`. A REUSE release requiring an additional `root`
argument prevents license scanning and leaves source packages unlicensed
in the generated SBOM. Strict license validation remains enabled.

## Compliance scope

`tools/ci/check-compliance.sh` runs Zephyr's `check_compliance.py` with
`ZEPHYR_BASE` set, on the original PR range (`<base-sha>..<head-sha>`),
selected from the event payload.
Pushes use their before/after range; a newly created branch includes the root
commit. Manual and tag invocations use `HEAD^!`, which also works for a root
commit without a parent. `tools/ci/check-commits.py --event <event-json>
--range-only` emits these ranges without rewriting commits.

The script loads Zephyr's `.checkpatch.conf`, rewriting the typedefs path to
the workspace's Zephyr tree. `EXECUTE_PERMISSIONS` is ignored because hook
and helper scripts must keep the executable bit. The optional local gitlint
hook ignores rule `UC2`, which requires a two-token name; the blocking RZI
policy instead matches the author's actual name and email to a DCO trailer.

These checks are excluded because they validate the Zephyr repository, not a
module:

- `Kconfig`, `KconfigBasic`, `KconfigBasicNoModules`, `KconfigHWMv2`
- `SysbuildKconfig`, `SysbuildKconfigBasic`, `SysbuildKconfigBasicNoModules`
- `ZephyrModuleFile`

`ClangFormat` is excluded the same way as Zephyr's compliance workflow.
`tools/ci/check-style.sh` still fails the job when clang-format or a missing
`@file` / `@brief` is wrong. `LicenseAndCopyrightCheck` produces warnings;
`reuse lint` is the blocking module license gate. `Gitlint` is excluded from
compliance because the blocking `Commit policy` job enforces RZI's message
and DCO rules on original commits. Synthetic integration messages are not
contributor messages. Every other compliance report fails the job. The
optional local `commit-msg` hook runs both RZI policy and Zephyr gitlint.

`native_sim` keeps `ZEPHYR_TOOLCHAIN_VARIANT=host`. The SDK install in CI is
the ARM toolchain the product boards need; the host compiler builds the
simulator.

## Local reproduction

Prerequisites match the runner: a Python environment with
`zephyr/scripts/requirements-{base,build-test,run-test}.txt` installed,
`device-tree-compiler`, `gperf`, and `gcc-multilib` / `g++-multilib` for the
32-bit `native_sim/native` variant. Sample builds need the Zephyr SDK
(`ZEPHYR_SDK_INSTALL_DIR`). Compliance also needs
`zephyr/scripts/requirements-actions.txt` and `libmagic1`.

```bash
# clang-format, @file / @brief, and checkpatch
tools/ci/check-style.sh

# Zephyr gitlint on one message. .githooks/commit-msg runs this.
tools/ci/check-commit-msg.sh .git/COMMIT_EDITMSG

# Zephyr check_compliance.py. ZEPHYR_BASE may be omitted when this repo
# sits in a west workspace.
python3 tools/ci/check-commits.py origin/main..HEAD
tools/ci/check-compliance.sh origin/main..HEAD

# Commit-policy regression tests, using temporary Git repositories
python3 -m unittest discover -s tools/tests -p 'test_*.py' -v

# Tests and samples, from the west workspace topdir
west twister -T rzi/tests --inline-logs -v --integration
west twister -T rzi/samples --inline-logs -v --integration

# Reproduce one CI sample shard (run 1/4 through 4/4 for the full set)
west twister -T rzi/samples --inline-logs -v --integration --subset 1/4

# Public headers against a configured build
west build -b native_sim/native/64 rzi/tests/lorawan/service -d build-headers
python3 tools/ci/check-public-headers.py build-headers

# Documentation, REUSE, and SBOM
tools/docs/generate-doxygen.sh
reuse lint
tools/sbom/generate-sbom.sh --allow-dirty
spdx3-validate -j build/sbom/rzi.spdx.jsonld
python3 tools/sbom/check-sbom.py --strict build/sbom/rzi.spdx.jsonld build/sbom/rzi.cdx.json
```

Firmware SBOM steps are in [sbom.md](./sbom.md#firmware-sbom).
`spdx3-validate` downloads the SPDX 3 context and SHACL model, so it needs
network access.

On a host without the Zephyr SDK, the native_sim jobs also work with
`ZEPHYR_TOOLCHAIN_VARIANT=host`, which skips the SDK lookup that
`verify-toolchain.cmake` performs.

## Out of scope

CI proves host-side and build-time behavior. RF verification on RAK4631 /
RAK3372 hardware (OTAA, RX windows, sleep current, FUOTA recovery) remains a
release gate; see [architecture.md](./architecture.md) section 16.
