# Contributing to RZI

Contributions are welcome: bug reports, documentation, tests, board support,
and implementation changes. Keep each pull request focused on one problem
and explain the resulting behavior for users.

Read the [coding standards](doc/development/coding-standards.md),
[architecture](doc/architecture/overview.md), and [CI guide](doc/development/ci.md) before changing
code. Report vulnerabilities through the
[security policy](.github/SECURITY.md), rather than a public issue.

## Report a bug or propose a feature

Search existing issues and pull requests first. For a bug, include:

- The RZI commit or tag, board target, and relevant hardware revision.
- The workspace revisions (`west list zephyr usp_zephyr usp`), toolchain,
  build command, and relevant Kconfig or Devicetree overrides.
- Reproduction steps, expected and actual behavior, and relevant logs.

Remove device credentials, session keys, and other secrets from reports and
examples. Discuss public API changes, new backends, dependency upgrades, and
large refactors in an issue before implementing them.

## Prepare a development workspace

Follow the [README setup instructions](https://github.com/RAKWireless/RZI#setup), using the revisions
in `west.yml` and applying the documented patches. Build commands below run
from the west workspace root; Git and `tools/` commands run from `rzi/`.

Use a fork where organization policy permits, or a feature branch in the
shared repository when you have write access. The examples assume:

- `origin` is the repository where you push your feature branch.
- `upstream` is the RZI repository receiving the pull request.

For the official repository, configure `upstream` once:

```sh
git remote add upstream git@github.com:RAKWireless/RZI.git
git fetch upstream
git switch -c fix/lorawan-settings upstream/main
git config core.hooksPath .githooks
```

If `upstream` already exists, check `git remote -v` instead of adding it
again. A private repository requires access before you can fetch or submit
changes. For a pull request targeting a maintenance branch, substitute that
branch for `main` throughout this guide.

The optional hooks run staged C style checks and commit-message checks.
Their dependencies and local commands are documented in [CI](doc/development/ci.md).

## clangd setup

The checked-in .clangd contains shared parsing adjustments only. It does
not select a developer's build directory, CPU target, or floating-point ABI.
Clangd uses the active build's compile_commands.json and compiler settings.

After configuring a build with CMAKE_EXPORT_COMPILE_COMMANDS enabled, link
its application-domain database into the RZI repository. For example, from
the west workspace root:

```sh
ln -s ../build/uplink-rak3172-rzi/periodical_uplink/compile_commands.json \
  rzi/compile_commands.json
```

The example assumes an existing sysbuild under build/uplink-rak3172-rzi.
Choose the matching application-domain path for your board and build;
the sysbuild root is not the application's compilation database. The
repository-root compile_commands.json is ignored by Git. Update the local
link when changing builds. Alternatively, select the build database using
your editor's local --compile-commands-dir setting.

Configure --query-driver in local editor settings when clangd needs to query
the cross compiler for its target and system includes. Limit that setting
to the trusted toolchain in your workspace. Keep absolute paths and any
board-specific parser workarounds in local settings or user config rather
than the shared .clangd. See the [clangd configuration guide](https://clangd.llvm.org/config).

## Write and commit changes

- Follow the repository's Zephyr-based formatting and naming rules.
- Keep public APIs under `include/rzi/`; backend types and headers stay
  private. Document API behavior, errors, context, and lifetime rules.
- Update relevant documentation and samples when behavior changes. Add
  regression coverage for bug fixes and tests for new behavior where useful.
- Record user-visible changes in [CHANGELOG.md](CHANGELOG.md) under
  `Unreleased`. Describe the behavior and any migration steps; omit internal
  edits that do not affect users or contributors. Use only applicable
  Added, Changed, Deprecated, Removed, Fixed, Security, and Upgrade notes
  sections.
- Keep generated files, build output, and credentials out of commits.
- Preserve existing copyright and license notices. Add appropriate SPDX
  metadata to new files, or cover them through `REUSE.toml`.

Each commit must represent a complete logical change. Keep independent
features, fixes, and unrelated refactors separate. Each commit should build
and pass the relevant tests so the history remains useful for `git bisect`.
A pull request may contain several such commits; it does not need to be
squashed into one.

Before requesting review, fold temporary debugging, partial implementations,
and follow-up corrections into the commits they belong to. Use interactive
rebase when needed:

```sh
git rebase -i upstream/main
```

Commit messages follow the existing Conventional Commits format, with an
explanatory body. For example:

```text
fix(lorawan): preserve settings when persistence fails

Keep the active settings unchanged when the storage write fails so a
failed update does not leave runtime and persisted configuration apart.

Signed-off-by: Your Name <your-email@example.com>
```

Wrap body lines at 75 characters; see the
[commit rules](doc/development/coding-standards.md#9-commits) for exceptions. Stage explicit
paths, inspect the staged diff, and sign off each commit:

```sh
git add src/lorawan/<changed-file> tests/lorawan/<changed-file>
git diff --cached
git diff --cached --check
git commit -s
```

Replace the example paths with your files. `git commit -s` records your
agreement with the [Developer Certificate of Origin](https://developercertificate.org/).
It adds a `Signed-off-by` trailer, not a cryptographic signature. Use your
own configured name and email, and preserve attribution when editing someone
else's commits. The optional local commit-message hook checks the message
and the author sign-off, and also runs Zephyr gitlint. CI does not. See the
[maintainer workflow](doc/development/maintainer-workflow.md)
for ownership, protected branches, and the exact merge gates.

## Rebase and resolve conflicts

CI validates GitHub's integration result and does not rewrite your branch.
When you need newer mainline changes or have a conflict, update your feature
branch with rebase:

```sh
git fetch upstream
git rebase upstream/main
```

Do not use `git merge upstream/main` to update the PR branch. Rebase rewrites
your branch's commits; it does not modify `upstream/main` or submit your code.
Start with a clean working tree and coordinate before rebasing a branch
other people are using. Do not rewrite published mainline or release history.

If a conflict occurs, edit the conflicting files, remove the conflict
markers, stage the resolved paths, and continue:

```sh
git add <resolved-files>
git rebase --continue
```

Repeat as needed, or use `git rebase --abort` to return to the state before
the rebase. Re-run the relevant checks after resolving conflicts. Update an
already-pushed branch with `git push --force-with-lease origin HEAD`; if the
lease is rejected, inspect the remote changes before trying again.

During review, avoid rebasing onto a newer target just to stay current.
Rebase when conflicts or required upstream changes make it necessary, and
tell reviewers when history has been rewritten. Fold review corrections into
the appropriate commits and preserve their sign-offs.

## Validate your changes

Run checks relevant to the affected code and record their results. From the
repository directory:

```sh
tools/ci/check-style.sh
python3 tools/ci/check-commits.py upstream/main..HEAD
tools/ci/check-compliance.sh upstream/main..HEAD
reuse lint
```

Style checks cover the working tree, or HEAD when the tree is clean; the
compliance command covers the PR commit range. To check only staged changes,
use `tools/ci/check-style.sh --staged`.

From the workspace root, run the host integration tests and build an affected
sample for the relevant board, for example:

```sh
west twister -T rzi/tests --inline-logs -v --integration
west build --sysbuild -b rak4631/nrf52840/rzi \
  -d build/contrib-at-4631 rzi/samples/at
```

Choose board targets and samples from the
[README](https://github.com/RAKWireless/RZI#supported-boards).
For public API changes, also run the C/C++ header checks and Doxygen checks
described in [CI](doc/development/ci.md#local-reproduction). CI additionally checks the
sample integration builds, license compliance, version consistency, and
SBOMs. Full reproduction commands and dependencies live in that guide.

Build success does not prove RF or hardware behavior. For radio, power,
storage, board, or FUOTA changes, describe the relevant hardware checks and
their results. State explicitly what you could not test.

## Submit and review a pull request

```sh
git push -u origin HEAD
```

Open a PR against the intended RZI target branch. Use a draft PR for work
that is not ready for review. The description should include:

- The problem, resulting behavior, and related issue, if any.
- Compatibility impact, including API, AT commands, stored settings, and
  board configuration where applicable.
- Validation commands and results, hardware used, and any untested behavior.

Self-review the diff, respond to review comments, and fix failing checks.
Request another review after making the requested changes.

Maintainers may use **Create a merge commit**, **Rebase and merge** to preserve
independently meaningful commits, or **Squash and merge** to land the PR as one
commit. Keep authorship and DCO trailers intact. Other contributors require
approval from the code owner. New code pushes invalidate earlier approvals;
request review again after updating the PR. The designated maintainer
`@DanielCao0` may merge their own PRs without another person's approval through
the separate review ruleset's PR-only exception. Everyone must resolve review
threads, satisfy the up-to-date rule, and pass `CI required`. See the
[maintainer workflow](doc/development/maintainer-workflow.md#activate-github-protection)
for the exception's scope and GitHub activation steps.

## Releases

Maintainers publish releases from tested commits using `v<version>` tags.
The release workflow validates the tag, reruns CI, builds firmware, generates
SBOMs and checksums, and prepares a draft Release for maintainer review; see
[release details](doc/development/sbom.md#releases) and the
[release procedure](doc/development/maintainer-workflow.md#release-procedure).
Keep version metadata consistent as required by CI, document user-visible
changes and upgrade steps, and complete the relevant hardware validation.
Before publishing, move the relevant `Unreleased` entries into a versioned
changelog section with the actual release date. Keep `Unreleased` at the top
for subsequent work, and use the versioned entry to prepare release notes.
Retain commit history across releases; do not reinitialize the repository
or move an existing published version tag to replace a release.
