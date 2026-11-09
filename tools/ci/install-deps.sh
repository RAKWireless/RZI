#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless
# SPDX-License-Identifier: Apache-2.0
# Use separate build and compliance Python environments; both profiles are locked.
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
profile="${1:-}"
case "${profile}" in
bootstrap|build|sbom)
    python3 -m pip install --require-hashes -r "${REPO}/tools/requirements/${profile}.lock"
    ;;
compliance)
    zephyr="${ZEPHYR_BASE:-${REPO}/../zephyr}"
    python3 -m pip install --require-hashes -r "${zephyr}/scripts/requirements-actions.txt"
    ;;
*) echo "usage: $0 bootstrap|build|sbom|compliance" >&2; exit 2 ;;
esac
python3 -m pip check
mkdir -p "${REPO}/reports"
python3 -m pip freeze > "${REPO}/reports/dependencies-${profile}.txt"
