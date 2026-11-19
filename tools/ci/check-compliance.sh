#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless
# SPDX-License-Identifier: Apache-2.0
#
# Run Zephyr's scripts/ci/check_compliance.py against this module.
#
#   tools/ci/check-compliance.sh
#   tools/ci/check-compliance.sh origin/main..HEAD
#
# ZEPHYR_BASE, "git config rzi.zephyrbase", or a west workspace selects
# the Zephyr tree. Install that tree's scripts/requirements-actions.txt
# first; the compliance script imports those packages at startup.
#
# Checks that validate the Zephyr repository itself are excluded. ClangFormat
# is excluded the same way as zephyr/.github/workflows/compliance.yml;
# tools/ci/check-style.sh still enforces clang-format for the whole tree.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# Same set Zephyr's compliance workflow excludes, plus checks that parse
# the Zephyr tree (Kconfig / sysbuild / "no module.yml in the zephyr repo")
# rather than the module under test.
EXCLUDES=(
	ClangFormat
	# Commit policy (including DCO) is checked separately on original commits.
	# Synthetic PR / merge-queue integration commits are not author commits.
	Gitlint
	Kconfig
	KconfigBasic
	KconfigBasicNoModules
	KconfigHWMv2
	SysbuildKconfig
	SysbuildKconfigBasic
	SysbuildKconfigBasicNoModules
	ZephyrModuleFile
)

# REUSE is the blocking license gate for this independent module.
WARN_ONLY=(LicenseAndCopyrightCheck)

find_zephyr() {
	if [[ -n "${ZEPHYR_BASE:-}" && -f "${ZEPHYR_BASE}/scripts/ci/check_compliance.py" ]]; then
		printf '%s\n' "${ZEPHYR_BASE}"
		return 0
	fi
	local cfg
	cfg="$(git -C "${REPO}" config --get rzi.zephyrbase 2>/dev/null || true)"
	if [[ -n "${cfg}" && -f "${cfg}/scripts/ci/check_compliance.py" ]]; then
		printf '%s\n' "${cfg}"
		return 0
	fi
	local top
	top="$(west -C "${REPO}" topdir 2>/dev/null || true)"
	if [[ -n "${top}" && -f "${top}/zephyr/scripts/ci/check_compliance.py" ]]; then
		printf '%s\n' "${top}/zephyr"
		return 0
	fi
	return 1
}

if ! ZEPHYR_BASE="$(find_zephyr)"; then
	echo "check-compliance: Zephyr checkout not found" >&2
	echo "  set ZEPHYR_BASE or: git config rzi.zephyrbase <path>" >&2
	exit 1
fi
export ZEPHYR_BASE

if [[ $# -ge 1 ]]; then
	RANGE="$1"
elif [[ -n "${BASE_REF:-}" ]]; then
	RANGE="${BASE_REF}..HEAD"
else
	RANGE="HEAD^!"
fi

created_conf=0
cleanup() {
	if [[ "${created_conf}" == 1 ]]; then
		rm -f "${REPO}/.checkpatch.conf"
	fi
	rm -f "${REPO}/compliance.xml"
}
trap cleanup EXIT

# checkpatch.pl loads ./.checkpatch.conf and resolves --typedefsfile from
# the repository root. Point it at the Zephyr tree. EXECUTE_PERMISSIONS is
# ignored so hook and helper scripts stay executable; zephyr/patches is
# excluded because unified diffs carry leading-space blank context lines.
if [[ ! -e "${REPO}/.checkpatch.conf" ]]; then
	sed -e "s|^--typedefsfile=.*|--typedefsfile=${ZEPHYR_BASE}/scripts/checkpatch/typedefsfile|" \
		"${ZEPHYR_BASE}/.checkpatch.conf" > "${REPO}/.checkpatch.conf"
	printf '%s\n' \
		'--ignore EXECUTE_PERMISSIONS' \
		'--exclude zephyr/patches' >> "${REPO}/.checkpatch.conf"
	created_conf=1
fi

cd "${REPO}"

args=()
for check in "${EXCLUDES[@]}"; do
	args+=(-e "${check}")
done

compliance=("${ZEPHYR_BASE}/scripts/ci/check_compliance.py")
mapfile -t all_checks < <(python3 "${compliance[@]}" -l)

# check_compliance.py exits non-zero for warn-only checks too. Zephyr's
# workflow ignores that status and fails only when some other report is
# non-empty. Match that here.
set +e
python3 "${compliance[@]}" --annotate -c "${RANGE}" "${args[@]}"
rc=$?
set -e

hard=0
reports=0
for name in "${all_checks[@]}"; do
	report="${name}.txt"
	if [[ ! -s "${report}" ]]; then
		rm -f "${report}"
		continue
	fi
	reports=1
	warn_only=0
	for allowed in "${WARN_ONLY[@]}"; do
		if [[ "${name}" == "${allowed}" ]]; then
			warn_only=1
		fi
	done
	if [[ "${warn_only}" == 1 ]]; then
		echo "::warning title=${name}::see the report below"
	else
		echo "::error title=${name}::see the report below"
		hard=1
	fi
	cat "${report}"
	printf '\n'
	rm -f "${report}"
done

if [[ "${hard}" == 1 ]]; then
	exit 1
fi
# A crash that produced no report is still a failure.
if [[ "${rc}" -ne 0 && "${reports}" == 0 ]]; then
	exit "${rc}"
fi
exit 0
