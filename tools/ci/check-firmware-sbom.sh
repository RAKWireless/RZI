#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="${1:?usage: check-firmware-sbom.sh BUILD_DIR [OUT_DIR]}"
output="${2:-${build}/spdx}"
REPORTS="${RZI_REPORT_DIR:-${REPO}/reports}"
mkdir -p "${REPORTS}"
python3 "${REPO}/tools/sbom/generate-firmware-sbom.py" "${build}" -o "${output}" \
    2>&1 | tee "${REPORTS}/firmware-sbom-generation.log"
shopt -s nullglob
images=("${output}"/*/)
if [[ ${#images[@]} == 0 ]]; then
    echo "No firmware SBOM images generated" >&2
    exit 1
fi
result=0
for image in "${images[@]}"; do
    name="$(basename "${image}")"
    args=()
    for doc in "${image}"*.jsonld; do args+=(-j "${doc}"); done
    spdx3-validate -q "${args[@]}" 2>&1 | tee "${REPORTS}/firmware-${name}-schema.log" || result=1
    python3 "${REPO}/tools/sbom/check-sbom.py" --strict --firmware "${image}"*.jsonld \
        2>&1 | tee "${REPORTS}/firmware-${name}-elements.log" || result=1
done
exit "${result}"
