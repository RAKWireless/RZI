#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REPORTS="${RZI_REPORT_DIR:-${REPO}/reports}"
mkdir -p "${REPORTS}"
cd "${REPO}"
tools/sbom/generate-sbom.sh "$@" 2>&1 | tee "${REPORTS}/module-sbom-generation.log"
spdx3-validate -q -j build/sbom/rzi.spdx.jsonld 2>&1 | tee "${REPORTS}/module-sbom-schema.log"
python3 tools/sbom/check-sbom.py --strict build/sbom/rzi.spdx.jsonld build/sbom/rzi.cdx.json \
    2>&1 | tee "${REPORTS}/module-sbom-elements.log"
