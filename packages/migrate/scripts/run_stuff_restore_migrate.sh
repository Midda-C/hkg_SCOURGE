#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Gen5W_L EU 260128
# MIGRATE_PAYLOAD_RESTORE=1
# The WiFi-removal migration is one-way on purpose: reinstalling WiFi by
# rewriting shared QML would race Wifi1b and can silently revert Battery/
# Tweaks.  Restore leaves the migrated state alone and tells the owner to use
# the original WiFi-bearing bundle if WiFi is wanted again.
set -u
umask 077
PATH="${MIGRATE_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-/media/usb}"
RW_WIFI1B="${MIGRATE_WIFI1B_RW_DIR:-/rw_data/wifi1b}"
LOG="$RW_WIFI1B/migrate-restore.log"
OUT="$USB_DIR/run_stuff_result.txt"
MARKER="$RW_WIFI1B/migration.done"

mkdir -p "$RW_WIFI1B" 2>/dev/null || true
: > "$LOG" 2>/dev/null
log() { echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null; }
result() {
    { echo "MIGRATE_RESTORE"; echo "time=$(date -u 2>/dev/null)"; echo "usb=$USB_DIR"; echo "$@"; } > "$OUT" 2>&1
    cp -f "$OUT" "$RW_WIFI1B/restore-result.txt" 2>/dev/null
}

if [ -f "$MARKER" ]; then
    log "migration marker present; nothing to undo"
    result "status=OK" "restore=one-way" "migrated=1" \
           "note=use the WiFi-bearing bundle to reinstall WiFi"
else
    log "no migration marker; nothing to undo"
    result "status=OK" "restore=one-way" "migrated=0"
fi
log "=== migrate restore done ==="
exit 0
