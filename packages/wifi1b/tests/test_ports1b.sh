#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
BIN="$ROOT/src/wifi1b_daemon"
[ -x "$BIN" ] || make -C "$ROOT/src"
python3 - <<'PY' &
import socket, time
socks = []
for port in (2224, 2225):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen(1)
    socks.append(s)
time.sleep(8)
PY
DUMMY=$!
sleep 0.5
out=$("$BIN" --choose-port 2224 2>/dev/null)
echo "$out" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["ok"] is True; assert d["port"]==2226, d'
kill "$DUMMY" 2>/dev/null || true
wait "$DUMMY" 2>/dev/null || true
out=$("$BIN" --choose-port 2224 2>/dev/null)
echo "$out" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["ok"] is True; assert d["port"]==2224, d'
status=$("$BIN" --status 2>/dev/null)
echo "$status" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["app"]=="wifi1b"; assert d["ssh_port"]==2224; assert d["backend"]=="static_supplicant"'
echo "PASS: Wifi1b SSH port fallback and status fields"
