#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
if grep -rnE '\b(smtc_[a-z]|LoRaMac|LmHandler|Mcps|Mlme|ral_[a-z]|sx126|sx127|lr11)' \
    include/rzi/; then
    echo "private backend symbols leaked into include/rzi/" >&2
    exit 1
fi
if LC_ALL=C grep -rnP '[^\x00-\x7F]' src include; then
    echo "non-ASCII text in source" >&2
    exit 1
fi
version="$(grep -oP 'RZI_VERSION_STRING "\K[^"]+' include/rzi/version.h)"
grep -q "PROJECT_NUMBER         = ${version}" doc/Doxyfile
grep -q "cpe:2.3:a:rakwireless:rzi:${version}:" zephyr/module.yml
grep -q "pkg:github/RAKWireless/RZI@v${version}$" zephyr/module.yml
