#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Ioniq 5 Gen5W_L EU 260128
# Build-time substitutions bind this loader to one complete tarball and the
# canonical inline cleanup helper definition.
# COMBO_FLAT_WRAPPER_REV=13
EXPECTED_SHA256="@TARBALL_SHA256@"
set -u
PATH="${COMBO_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-$(CDPATH= cd -- "$(dirname "$0")" && pwd)}"
RESULT="$USB_DIR/run_stuff_result.txt"
LOG="$USB_DIR/appcombo_install.log"
RW_DIR="${COMBO_RW_DIR:-/rw_data/combo}"
RUNTIME_ROOT="${COMBO_RUNTIME_ROOT:-/rw_data}"
QML_BASE="${COMBO_QML_DIR:-/app/share/AppHomeScreen/qml_eco3.0}"
WORK=""
finish() {
    [ -z "$WORK" ] || rm -rf "$WORK"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
fail() {
    printf 'COMBO_FLAT_WRAPPER\nstatus=FAIL\nerror=%s\n' "$*" > "$USB_DIR/wrapper_result.txt"
    cp -f "$USB_DIR/wrapper_result.txt" "$RESULT" 2>/dev/null || true
    printf '%s\n' "$*" >> "$LOG"
    # The inner master never ran; signal this failure using the proven call.
    # Label the copy like the master's beep_init so the micom floor rule
    # applies; best-effort only.
    beep="$RW_DIR/bin/dbus-send"
    if [ ! -x "$beep" ]; then
        mkdir -p "$RW_DIR/bin" 2>/dev/null || true
        helper=$(command -v dbus-send 2>/dev/null || true)
        [ -z "$helper" ] || cp -f "$helper" "$beep" 2>/dev/null || true
        chmod 755 "$beep" 2>/dev/null || true
        label=$(cat /proc/self/attr/current 2>/dev/null | tr -d '\r\n')
        [ -z "$label" ] || chsmack -e "$label" "$beep" 2>/dev/null || true
    fi
    if [ -x "$beep" ]; then
        for n in 1 2; do
            "$beep" --system --print-reply --reply-timeout=1000 \
                --dest=com.lge.car.micom /com/lge/car/micom \
                com.lge.car.micom.audio.SetBeepPlay array:byte:0x00 >> "$LOG" 2>&1 || true
            sleep 0.5
        done
    fi
    sync
    exit 1
}

# Canonical first-party cleanup helper, embedded verbatim at build time.
# The wrapper identity changes with the helper digest; no external helper file
# is required at runtime.  The plain two-file USB pair stays deployable.
RUNTIME_CLEANUP_DIGEST="@RUNTIME_CLEANUP_SHA256@"
SHA256SUM_BIN=sha256sum
command -v "$SHA256SUM_BIN" >/dev/null 2>&1 || SHA256SUM_BIN=sha256sum.coreutils
# @RUNTIME_CLEANUP_SHELL@
_cleanup_id_bad=0
case "$RUNTIME_CLEANUP_DIGEST" in
    ''|*[!0-9a-f]*) _cleanup_id_bad=1 ;;
esac
[ "${#RUNTIME_CLEANUP_DIGEST}" -eq 64 ] || _cleanup_id_bad=1
if [ "$_cleanup_id_bad" = 1 ] ||
   ! command -v cleanup_bounded_logs >/dev/null 2>&1 ||
   ! command -v cleanup_removed_wifi >/dev/null 2>&1; then
    printf 'COMBO_FLAT_WRAPPER\nstatus=FAIL\nerror=unexpanded_cleanup_helper\n' \
        > "$USB_DIR/wrapper_result.txt" 2>/dev/null || true
    cp -f "$USB_DIR/wrapper_result.txt" "$RESULT" 2>/dev/null || true
    exit 1
fi
RUNTIME_CLEANUP_SHA256="$SHA256SUM_BIN"

# FIRST: reclaim bounded runtime log space before any extraction or runtime
# write.  A full /rw_data makes every later write (including this installer's
# own state) fail.  Unknown files, symlinks and foreign paths are left alone.
RUNTIME_CLEANUP_LOG=""
cleanup_bounded_logs "$RUNTIME_ROOT" || true

# The tarball is hashed read-only before any recursive deletion.  A corrupt
# or missing bundle must never delete credentials or backups.
if [ ! -f "$USB_DIR/appcombo.tar.gz" ] || [ -L "$USB_DIR/appcombo.tar.gz" ]; then
    fail "missing or unsafe appcombo.tar.gz"
fi
actual=$("$SHA256SUM_BIN" "$USB_DIR/appcombo.tar.gz" | cut -d' ' -f1)
[ "$actual" = "$EXPECTED_SHA256" ] || fail "tarball SHA256 mismatch; copy the matching pair"
# Hash binding alone is not enough: verify the archive is readable and really
# contains the inner master before anything is deleted.
if ! tar -tzf "$USB_DIR/appcombo.tar.gz" >/dev/null 2>&1; then
    fail "tarball unreadable or corrupt; nothing was deleted"
fi
if ! tar -tzf "$USB_DIR/appcombo.tar.gz" 2>/dev/null | grep -Eq '(^|/)run_stuff\.sh$'; then
    fail "tarball has no inner master; nothing was deleted"
fi

MODE=all
if [ -f "$USB_DIR/MODE" ]; then MODE=$(head -n 1 "$USB_DIR/MODE" | tr -d '\r'); fi
[ ! -f "$USB_DIR/RESTORE" ] || MODE=restore
if [ "$MODE" = all ]; then
    # Full removed-WiFi cleanup is evidence-gated inside the helper: no
    # manual WiFi Off step, no deletion on a missing marker/page alone.
    cleanup_removed_wifi "$RUNTIME_ROOT" "$QML_BASE" "$USB_DIR" \
        "$USB_DIR/appcombo.tar.gz" "$actual" \
        "${CLEANUP_WIFI_SERVICE:-/etc/systemd/system/wifi-daemon.service}" || true
fi
mkdir -p "$RW_DIR" 2>/dev/null || exit 1
: > "$LOG" 2>/dev/null
RUNTIME_CLEANUP_LOG="$LOG"
[ -z "$RUNTIME_CLEANUP_NOTES" ] || printf 'cleanup: %s\n' "$RUNTIME_CLEANUP_NOTES" >> "$LOG" 2>/dev/null

# A stage that dies before writing its result must not leave the previous
# run's per-stage result looking current on the stick.
rm -f "$USB_DIR"/*_result.txt 2>/dev/null || true
for app in battery wifi tweaks wifi1b migrate; do
    rm -f "$USB_DIR/${app}_install.log" "$USB_DIR/${app}_restore.log" \
        "$USB_DIR/${app}_status.json" "$USB_DIR/runtime_snapshot_${app}_status.json"
done
rm -f "$USB_DIR/combo.log"
WORK=$(mktemp -d /tmp/appcombo_run.XXXXXX) || fail "cannot create extraction directory"
tar -xzf "$USB_DIR/appcombo.tar.gz" -C "$WORK" >> "$LOG" 2>&1 || fail "cannot extract tarball"
[ -f "$WORK/run_stuff.sh" ] || fail "missing inner master"
for input in wifi.txt iphone_wifi.txt authorized_keys MDNSD_ROOT_ARM; do
    [ ! -f "$USB_DIR/$input" ] || cp -f "$USB_DIR/$input" "$WORK/$input"
done
SCOURGE_MODE="$MODE" USB_DIR="$WORK" SCOURGE_STICK_DIR="$USB_DIR" \
    COMBO_RW_DIR="$RW_DIR" COMBO_RUNTIME_ROOT="$RUNTIME_ROOT" \
    COMBO_QML_DIR="$QML_BASE" SCOURGE_ARCHIVE="$USB_DIR/appcombo.tar.gz" \
    SCOURGE_TARBALL_SHA256="$actual" \
    /bin/sh "$WORK/run_stuff.sh" >> "$LOG" 2>&1
rc=$?
for src in "$WORK"/*_result.txt; do
    [ ! -f "$src" ] || cp -f "$src" "$USB_DIR/$(basename "$src")"
done
for app in battery wifi tweaks wifi1b migrate; do
    app_rw="${COMBO_RUNTIME_ROOT:-/rw_data}/$app"
    # Stage result files above are authoritative. Never overwrite them with
    # possibly historical /rw_data/result.txt after an early stage failure.
    action=install
    stage_name=$app
    if [ "$MODE" = restore ]; then action=restore; stage_name=restore_$app; fi
    if [ -f "$WORK/${stage_name}_result.txt" ]; then
        [ ! -f "$app_rw/$action.log" ] || cp -f "$app_rw/$action.log" "$USB_DIR/${app}_$action.log"
    fi
    # Do not export status.json: it carries cleartext psk/carplay_psk fields.
    # The stale-snapshot cleanup above removes copies left by older wrappers.
    for phase in before after; do
        snapshot="$WORK/combo/$app/qml_$phase"
        if [ -d "$snapshot" ]; then
            readback="$USB_DIR/qml_readback_$$/$app"
            mkdir -p "$readback" && cp -R "$snapshot" "$readback/" || true
        fi
    done
done
if [ -f "$RW_DIR/combo.log" ]; then
    cp -f "$RW_DIR/combo.log" "$USB_DIR/combo.log"
    cat "$RW_DIR/combo.log" >> "$LOG"
fi
if [ -f "$WORK/README.txt" ]; then cp -f "$WORK/README.txt" "$USB_DIR/README_COMBO.txt"; fi
status=$(sed -n 's/^status=\([^ ]*\).*/\1/p' "$RESULT" 2>/dev/null | tail -n 1)
verdict=FAIL
if [ "$rc" = 0 ] && [ "$status" = OK ]; then verdict=OK; fi
printf 'COMBO_FLAT_WRAPPER\nstatus=%s\ninner_rc=%s\ntarball_sha256=%s\ncleanup_helper_sha256=%s\n' "$verdict" "$rc" "$actual" "$RUNTIME_CLEANUP_DIGEST" > "$USB_DIR/wrapper_result.txt"
sync
[ "$verdict" = OK ]
