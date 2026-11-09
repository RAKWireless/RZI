#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
version=1.7.11
sha=900919a84f2229bac68ca9cd4103ea297abc35e9689ebb842c6e34a3d1b01b0a
cache="${XDG_CACHE_HOME:-${HOME}/.cache}/rzi-ci/actionlint-${version}"
mkdir -p "${cache}"
archive="${cache}/actionlint_${version}_linux_amd64.tar.gz"
if [[ ! -f "${archive}" ]]; then
    curl --fail --silent --show-error --location \
        "https://github.com/rhysd/actionlint/releases/download/v${version}/actionlint_${version}_linux_amd64.tar.gz" \
        --output "${archive}.tmp"
    mv "${archive}.tmp" "${archive}"
fi
printf '%s  %s\n' "${sha}" "${archive}" | sha256sum --check
# Always extract the verified archive, not a potentially stale executable.
tar -xzf "${archive}" -C "${cache}" actionlint
cd "${REPO}"
"${cache}/actionlint" -shellcheck= .github/workflows/*.yml
