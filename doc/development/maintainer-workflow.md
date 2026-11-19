# Maintainer workflow

Status: normative

RZI keeps independently meaningful commits, reviews changes through pull
requests, and tests the proposed integration before it enters mainline.
This document defines the merge and release gates and the GitHub settings
needed to enforce them. Files in Git alone do not activate repository rules.

See also: [contribution guide](https://github.com/RAKWireless/RZI/blob/main/CONTRIBUTING.md), [CI](ci.md),
[coding standards](coding-standards.md), [SBOM and releases](sbom.md).

## Review and merge

1. Contributors branch from `main` (or an explicitly supported maintenance
   branch), prepare logical commits, and open a focused PR.
2. CI checks the integration build, compliance, tests, documentation, and
   SBOMs. Commit messages and DCO sign-offs are not CI gates. CI does not
   rebase or push changes to the contributor branch.
3. A maintainer reviews correctness, API and compatibility impact, test
   evidence, attribution, and commit boundaries. Every logical commit should
   build and pass relevant tests; current automation tests the integration
   tip, so per-commit buildability remains a contributor/reviewer obligation.
4. Other contributors require one independent approval from the code owner.
   New code pushes invalidate those approvals, and the latest reviewable push
   must be approved by someone other than its pusher. `@DanielCao0` may merge
   their own PRs without another person's approval, using the PR-only review
   exception. All PRs must resolve review threads and pass CI.
5. Merge only when `CI required` succeeds and the branch satisfies the
   up-to-date rule. Use **Rebase and merge** to retain the reviewed logical
   commits, **Squash and merge** when the PR should land as one commit, or
   **Create a merge commit**.

Do not push directly to protected branches, force-push their history, or
override failed checks. Diagnose a failing gate instead of disabling it.
For an urgent regression, use a small revert or fix PR with the same gates.
An incident that requires changing repository protection must have a named
approver, a recorded reason, and restoration of protection afterward.

The initial owner is `@DanielCao0` in `.github/CODEOWNERS`. That account must
have explicit write access in RAKWireless/RZI. GitHub does not accept a PR
author's self-approval; the designated maintainer instead has an explicit
review exception. Use it for their own PRs; approve other contributors' PRs
through the normal review UI. The exception belongs only to that account,
not to every administrator. As the team grows, add owners or assign subsystem
teams. Changes
to CI, ownership, public APIs, storage formats, or release tooling deserve
an additional specialist review when a qualified reviewer is available.

## Activate GitHub protection

Bootstrap the empty repository with its initial signed-off commit and these
workflow files before enabling required checks. Run CI and confirm the exact
`CI required` check exists. Initial commits follow the same message rules,
for example `chore: initialize RZI` with a body and DCO sign-off.

In **Settings > General > Pull Requests**:

- Enable **Allow merge commits**, **Allow rebase merging**, and **Allow squash merging**.
- Enable automatic deletion of merged feature branches.
- Keep automatic merging disabled until branch protections are active.

In **Settings > Rules > Rulesets**, import the following JSON files and
confirm enforcement is **Active**:

| File | Effect |
|---|---|
| [main.json](https://github.com/RAKWireless/RZI/blob/main/.github/rulesets/main.json) | Protects `main` and `release/**` for everyone: PRs, resolved threads, merge, rebase, or squash, no deletion or force push, and required `CI required` from GitHub Actions; no bypass actors |
| [reviews.json](https://github.com/RAKWireless/RZI/blob/main/.github/rulesets/reviews.json) | Requires owner approval, dismissal of stale reviews, and latest-push approval; only `@DanielCao0` (user ID `144674500`) can bypass this ruleset when merging a PR |
| [release-tags.json](https://github.com/RAKWireless/RZI/blob/main/.github/rulesets/release-tags.json) | Allows creation of `v*` tags but blocks subsequent tag changes and deletion |

Import all three files. Review requirements deliberately live in a separate
ruleset: a bypass applies to the entire ruleset containing it. Do not add the
maintainer to the baseline or tag ruleset's bypass list. The PR-only exception
does not permit direct pushes, failed CI, force pushes, tag changes, or deletion.
GitHub grants this exception to the merging actor rather than matching the
PR author; the account can technically use it on any PR, so use the normal
approval path for other contributors. GitHub records explicit bypass usage.
If a GitHub Enterprise Server version lacks individual-user bypass support,
use an organization team containing only this maintainer in the review
ruleset; do not grant a blanket administrator exception.

Confirm the expected status source is **GitHub Actions**, not an arbitrary external
status. Remove superseded required checks, including `Check DCO sign-off`
and `Commit policy`. Neither is produced anymore. The only required status
is `CI required`. Leaving a removed check required blocks all PRs.

Private organization repositories require a GitHub plan supporting these
protections. If the organization plan does not expose/enforce them, treat
this as an incomplete rollout; a document alone cannot enforce a merge gate.

Use **Settings > Actions > General** to keep default workflow permissions
read-only. Do not allow PR jobs to use release secrets or production signing
credentials. Fork contributions run through `pull_request`, never through a
privileged workflow that executes unreviewed fork code. Keep the default
branch's workflow and ownership files under code-owner review.

Verify the rollout with disposable PRs: a failing required validation must
prevent merging. A missing DCO sign-off alone is not a CI or merge gate;
local commit hooks and contributor expectations are separate. Another contributor's PR must require an
owner review. A passing PR by `@DanielCao0` must be mergeable without another
review through the explicit review bypass. Confirm direct pushes, force
pushes, tag updates, and tag deletions remain blocked for that account too.
Verify rules in the GitHub UI; local files
cannot prove server-side enforcement.

## Merge queue when needed

For the current review volume, strict up-to-date checks are the baseline.
When concurrent merges become frequent, an organization with an eligible
plan can require a merge queue using the **rebase** method. CI already handles
`merge_group: checks_requested` and validates the queued integration. Queue
integration commits are generated by GitHub. Maintainers review original PR
commit boundaries before admission; CI does not enforce commit topology.

When enabling a queue, replace the strict up-to-date requirement with the
queue rule, retain reviews and the `CI required` gate, require non-failing
PRs, and set the queue timeout above the measured full CI runtime. Test an
actual queued PR before considering the queue active. Availability differs
between public repositories and private organization plans.

## CI and dependency maintenance

All validation jobs must explicitly succeed before the stable `CI required`
gate passes. Failed, cancelled, and unexpectedly skipped dependencies fail
the gate. Preserve this check name when reorganizing jobs. New blocking jobs
must be included in its `needs` list.

External GitHub Actions are pinned to complete commit SHAs with readable
major-version comments. Dependabot proposes updates; review upstream changes
and run the complete CI before merging. A maintainer taking authorship of a
bot-generated patch reviews it and keeps the original attribution. CI does
not compare that authorship with a `Signed-off-by` trailer.

Workflow validation runs a checksum-verified actionlint release. This checks
workflow syntax and expressions; it is not a replacement for executing jobs.
Dependency workspace caches contain downloads, not trusted test results.
Only the release draft job has `contents: write`; PR validation uses
read-only permissions and checkouts do not retain credentials.

## Release procedure

1. Prepare a version PR updating `include/rzi/version.h`, `doc/Doxyfile`,
   and `zephyr/module.yml` together. Move the relevant `Unreleased` entries
   in [CHANGELOG.md](https://github.com/RAKWireless/RZI/blob/main/CHANGELOG.md) into the release's version section,
   use the actual release date, and retain `Unreleased` for future work.
   Document user-visible changes,
   compatibility and upgrade steps, known limitations, and hardware results.
2. Complete applicable RF, power, storage, and FUOTA validation on hardware.
   CI provides build and host-test evidence; it does not certify these
   hardware behaviors. Merge the release PR through normal review and gates.
3. A release maintainer creates a `v<version>` tag on the reviewed mainline
   commit. Keep tag creation rights limited to release maintainers using
   organization access controls; the shipped tag rules prevent later edits
   but do not restrict initial creation to a particular team.
4. Release preflight checks the version and mainline ancestry. The reusable
   CI workflow validates the tagged source again. Only after validation
   succeeds are release firmware, SBOMs, source archives, and checksums built.
5. The workflow creates a **draft** Release. A maintainer inspects the assets,
   checksums, SBOMs, release notes and hardware evidence, then publishes it.
   Mark release candidates as pre-releases and review automatically generated
   notes before publication. Drafts provide a review step; repository roles
   determine who can publish them.

The CI firmware uses the repository's configured MCUboot development setup;
the release workflow does not provision or protect production signing keys.
Products that require production signing need a separate controlled signing
process before distributing production firmware.

Retain history and published version tags. Correct a published defect with a
new patch release. A failed build can be re-run on the same tag without
moving it; an existing draft is inspected and reconciled explicitly rather
than automatically overwritten by a rerun.

## References

- [GitHub rulesets](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-rulesets/available-rules-for-rulesets)
- [Individual-user ruleset bypass](https://github.blog/changelog/2026-05-07-repository-rulesets-user-bypass-and-branch-renaming/)
- [Merge queues and CI](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/configuring-pull-request-merges/managing-a-merge-queue)
- [Secure use of Actions](https://docs.github.com/en/actions/reference/security/secure-use)
- [Zephyr contributor expectations](https://docs.zephyrproject.org/latest/contribute/contributor_expectations.html)
