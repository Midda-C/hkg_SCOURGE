#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
BIN=/tmp/opencode/test_wifi1b_apconfig.$$
APP=/tmp/opencode/wifi1b_apconfig_app.$$
rm -rf "$APP"
gcc -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation \
    -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable \
    -DWIFI_TEST -DAPP_DIR="\"$APP\"" -DWIFI_BUILD_ID='"test"' -static \
    -o "$BIN" "$ROOT/tests/test_apconfig.c"
"$BIN"
rm -f "$BIN"
rm -rf "$APP"
