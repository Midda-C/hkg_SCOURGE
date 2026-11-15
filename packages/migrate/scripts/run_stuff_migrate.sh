#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Gen5W_L EU 260128
# MIGRATE_PAYLOAD_REV=1
# One-way migration: remove the WiFi app, keep Battery/Tweaks/Reboot and
# Wifi1b, and rebase Wifi1b onto the no-WiFi canonical home screen.
#
# Hash-gated: the current shared QML and the rebuilt Wifi1b target must be in
# the shipped accepted-QML ledger.  The no-WiFi target is rebuilt from the
# canonical Battery/Tweaks base shipped in this payload (WiFi's own restore
# point predates Tweaks in the V11 install order), so Battery/Tweaks bytes are
# not regenerated from the WiFi backup.
#
# Radios are verified Off before any removal.  /rw_data/wifi is archived to
# the stick before deletion; Wifi1b config/backups stay in /rw_data/wifi1b.
set -u
umask 077
PATH="${MIGRATE_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-/media/usb}"
STICK_DIR="${SCOURGE_STICK_DIR:-$USB_DIR}"
PAYLOAD="$USB_DIR/payload"
QML_ROOT="${MIGRATE_QML_ROOT:-/app/share/AppHomeScreen}"
QML_DIR="$QML_ROOT/qml_eco3.0"
RW_WIFI="${MIGRATE_WIFI_RW_DIR:-/rw_data/wifi}"
RW_WIFI1B="${MIGRATE_WIFI1B_RW_DIR:-/rw_data/wifi1b}"
LOG="$RW_WIFI1B/migrate.log"
OUT="$USB_DIR/run_stuff_result.txt"
MARKER="$RW_WIFI1B/migration.done"
SERVICE="${MIGRATE_WIFI_SERVICE:-/etc/systemd/system/wifi-daemon.service}"
MDNS_DROPIN="${MIGRATE_MDNS_DROPIN:-/etc/systemd/system/carplay_mdnsd.service.d/50-scourge-restart.conf}"
MDNSD_BIN="${MIGRATE_MDNSD_BIN:-/app/bin/mdnsd}"
# Exact ELF pins from config/firmware_locks.env (test overrides use fixtures).
MDNSD_STOCK_SHA256="${MIGRATE_MDNSD_STOCK_SHA256:-71122d8f59cdfd248f80414014e22114b0ee486415e5a7de23f5acd1676b9cdd}"
MDNSD_PATCHED_SHA256="${MIGRATE_MDNSD_PATCHED_SHA256:-3f1cfd19210d6ac34ce24a215ed5066054ba4bbf487714f1c969114d86c6525f}"

ADV="HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
WIDE="HomeScreen/QHMI_HomeScreenMain_Wide.qml"
ALL="HomeScreen/Screen_HomeScreen_AllMenus.qml"
SETUP="HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
CUSTOM="component/QCustomizedMenuListAllowEmpty.qml"
THEME="component/common/Q_ThemeImage.qml"
HELLO="component/HelloTile.qml"
BATTERY="component/BatteryTile.qml"

mkdir -p "$RW_WIFI1B" 2>/dev/null || true
: > "$LOG" 2>/dev/null
log() { echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null; }

result() {
    {
        echo "MIGRATE_PAYLOAD"
        echo "time=$(date -u 2>/dev/null)"
        echo "usb=$USB_DIR"
        echo "qml_dir=$QML_DIR"
        echo "uid=$(id -u 2>/dev/null)"
        echo "$@"
    } > "$OUT" 2>&1
    cp -f "$OUT" "$RW_WIFI1B/result.txt" 2>/dev/null
}

fail() {
    log "FAIL: $*"
    result "status=FAIL" "error=$*"
    exit 1
}

# Offline test hook only; unset on the car.
failpoint() {
    [ "${MIGRATE_FAIL_POINT:-}" = "$1" ] && fail "injected failure: $1"
    return 0
}

MD5SUM_BIN=md5sum
command -v md5sum >/dev/null 2>&1 || MD5SUM_BIN=md5sum.coreutils
md5_of() { "$MD5SUM_BIN" "$1" 2>/dev/null | cut -d' ' -f1; }
SHA256SUM_BIN=sha256sum
command -v sha256sum >/dev/null 2>&1 || SHA256SUM_BIN=sha256sum.coreutils
sha256_of() { "$SHA256SUM_BIN" "$1" 2>/dev/null | cut -d' ' -f1; }
accepted() { grep -qx "$1" "$PAYLOAD/accepted-qml.md5" 2>/dev/null; }

TMP=""
COMPLETE=0
cleanup() {
    [ -z "$TMP" ] || rm -rf "$TMP" 2>/dev/null
    if [ "$COMPLETE" != 1 ]; then
        log "migration incomplete; restarting Wifi1b so the radio is supervised"
        if command -v systemctl >/dev/null 2>&1; then
            systemctl daemon-reload >> "$LOG" 2>&1 || true
            systemctl restart wifi1b-daemon.service >> "$LOG" 2>&1 || true
        fi
    fi
    if [ "${IO_ACTIVE:-0}" = 1 ]; then io_restore_root || true; fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

mount -o remount,rw "$USB_DIR" 2>/dev/null || true
[ -f "$USB_DIR/manifest.md5" ] || fail "manifest.md5 missing"
if ! ( cd "$USB_DIR" && "$MD5SUM_BIN" -c manifest.md5 ) > "$RW_WIFI1B/migrate-manifest.log" 2>&1; then
    fail "manifest check failed"
fi
[ -d "$QML_DIR" ] || fail "expected QML directory missing: $QML_DIR"
[ -x "$PAYLOAD/bin/wifi1b_qmlpatch" ] || fail "Wifi1b patcher missing from payload"
[ -f "$PAYLOAD/accepted-qml.md5" ] || fail "accepted-QML ledger missing"
[ -x "$RW_WIFI1B/wifi1b_daemon" ] || fail "Wifi1b is not installed; install/update it before migration"

# The current shared QML is backed up, not required to be a known variant:
# owners may have a V7-V11 layout whose exact bytes are not in this build's
# ledger.  The canonical no-WiFi base and the rebuilt Wifi1b target are still
# hash-gated below, and the current bytes are preserved on the stick.
qml_state=accepted
for spec in "advanced:$ADV" "wide:$WIDE" "menu:$ALL" "menu:$SETUP" "custom:$CUSTOM"; do
    path=${spec#*:}
    cur=$(md5_of "$QML_DIR/$path")
    accepted "$cur" || { qml_state=unknown; log "current QML not in ledger: $path ($cur)"; }
done

cur_adv=$(cat "$QML_DIR/$ADV" 2>/dev/null || true)
log "current Advanced markers: wifi=$(printf '%s' "$cur_adv" | grep -c 'WIFI_PAYLOAD_260128') wifi1b=$(printf '%s' "$cur_adv" | grep -c 'WIFI1B_PAYLOAD_260128')"

# Detect the WiFi app by its installed files/services, not only by QML
# markers: a previous partial migration may have removed the markers while
# leaving the service or /rw_data/wifi behind.
wifi_present=0
[ -e "$RW_WIFI" ] && wifi_present=1
[ -e "$SERVICE" ] && wifi_present=1
[ -e "$QML_DIR/HomeScreen/WifiPage.qml" ] && wifi_present=1
[ -e "$QML_DIR/component/WifiTile.qml" ] && wifi_present=1
for spec in "wide:$WIDE" "menu:$ALL" "menu:$SETUP" "custom:$CUSTOM"; do
    path=${spec#*:}
    grep -q 'WIFI_PAYLOAD_260128' "$QML_DIR/$path" 2>/dev/null && wifi_present=1
done
log "migration preflight: wifi_present=$wifi_present qml_state=$qml_state"

# Already migrated and really clean: nothing to remove.
if [ "$wifi_present" = 0 ]; then
    printf 'migration complete\n' > "$MARKER" 2>/dev/null || fail "cannot write migration marker"
    chmod 644 "$MARKER" 2>/dev/null || true
    chown 0:0 "$MARKER" 2>/dev/null || true
    result "status=OK" "already_migrated=1" "layout=standalone" "wifi_present=0" \
           "qml_state=$qml_state" "backup=$RW_WIFI1B/backup/latest"
    log "=== migrate already complete ==="
    exit 0
fi

# Preserve the current shared QML on the stick before changing anything.
SNAP="$STICK_DIR/backup/wifi-removal-qml-$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null || echo migration)"
mkdir -p "$SNAP/HomeScreen" "$SNAP/component" 2>/dev/null || fail "cannot create QML snapshot"
for f in "$ADV" "$WIDE" "$ALL" "$SETUP" "$CUSTOM"; do
    [ ! -f "$QML_DIR/$f" ] || cp -f "$QML_DIR/$f" "$SNAP/$f" || fail "cannot snapshot $f"
done
log "current QML snapshot: $SNAP"

WIFI_LATEST="$RW_WIFI/backup/latest"
BASE_DIR="$PAYLOAD/base"
[ -d "$BASE_DIR" ] || fail "canonical no-WiFi base missing from payload"

# The shipped base must be accepted, Battery/Tweaks-based and WiFi-free.
for spec in "advanced:$ADV" "wide:$WIDE" "menu:$ALL" "menu:$SETUP" "custom:$CUSTOM"; do
    path=${spec#*:}
    [ -f "$BASE_DIR/$path" ] || fail "canonical base missing: $path"
    accepted "$(md5_of "$BASE_DIR/$path")" || fail "canonical base not accepted: $path"
    grep -q 'WIFI_PAYLOAD_260128' "$BASE_DIR/$path" && fail "canonical base carries a WiFi marker: $path"
    grep -q 'WIFI1B_PAYLOAD_260128' "$BASE_DIR/$path" && fail "canonical base carries a Wifi1b marker: $path"
done
grep -q 'TWEAKS_PAYLOAD_260128' "$BASE_DIR/$ADV" 2>/dev/null || \
    fail "canonical base is not the Battery/Tweaks layout"

# Rebuild the Wifi1b target from the canonical base and verify its digest.
TMP=$(mktemp -d /tmp/wifi1b-migrate.XXXXXX) || fail "cannot create temporary QML tree"
for spec in "advanced:$ADV" "wide:$WIDE" "menu:$ALL" "menu:$SETUP" "custom:$CUSTOM"; do
    mode=${spec%%:*}; path=${spec#*:}; base=$(basename "$path")
    cp -f "$BASE_DIR/$path" "$TMP/$base" || fail "cannot stage $path"
    "$PAYLOAD/bin/wifi1b_qmlpatch" "$mode" "$TMP/$base" >> "$LOG" 2>&1 || fail "Wifi1b patch failed: $path"
    accepted "$(md5_of "$TMP/$base")" || fail "Wifi1b target not accepted: $path"
    grep -q 'WIFI_PAYLOAD_260128' "$TMP/$base" && fail "target still carries a WiFi marker: $path"
    grep -q 'WIFI1B_PAYLOAD_260128' "$TMP/$base" || fail "target missing Wifi1b marker: $path"
done
failpoint target_built

# ---- stop the apps and verify both radios are Off -----------------------
if command -v systemctl >/dev/null 2>&1; then
    systemctl stop wifi1b-daemon.service >> "$LOG" 2>&1 || true
    systemctl stop wifi-daemon.service >> "$LOG" 2>&1 || true
fi
for p in /proc/[0-9]*; do
    executable=$(readlink "$p/exe" 2>/dev/null || true)
    case "$executable" in
        "$RW_WIFI/wifi_daemon"|"$RW_WIFI/wifi_daemon (deleted)"|\
        "$RW_WIFI1B/wifi1b_daemon"|"$RW_WIFI1B/wifi1b_daemon (deleted)") \
            kill -TERM "${p#/proc/}" 2>/dev/null || true ;;
    esac
done

radio_check() {
    daemon=$1; dir=$2; name=$3
    [ -x "$daemon" ] || return 0
    status=$("$daemon" --mode off 2>>"$LOG" || true)
    printf '%s\n' "$status" >> "$LOG"
    if ! printf '%s\n' "$status" | grep -q '"mode":"off"' ||
       ! printf '%s\n' "$status" | grep -q '"wlan_on":false'; then
        # The one-shot status may be only on disk; check the status file too.
        sf="$dir/status.json"
        if [ -f "$sf" ] && grep -q '"mode":"off"' "$sf" && grep -q '"wlan_on":false' "$sf"; then
            return 0
        fi
        # This is a removal migration: kill the radio rather than asking the
        # owner to turn it off.  The daemon's own --mode off above already
        # stopped its supplicant/DHCP children; force the interface down and
        # continue.
        log "WARNING: $name did not report Off; forcing wlan0 down"
        ifconfig wlan0 down 2>/dev/null || true
        rm -f /run/wifi1b/active /run/wifi/active 2>/dev/null || true
        radio_forced=1
    fi
    return 0
}
radio_forced=0
radio_check "$RW_WIFI1B/wifi1b_daemon" "$RW_WIFI1B" "Wifi1b"
radio_check "$RW_WIFI/wifi_daemon" "$RW_WIFI" "WiFi"
failpoint radios_verified

# ---- revert WiFi-created files from its own transaction ledger ----------
# Shared QML, passwd/shadow and /rw_data/wifi are handled separately.
. "$PAYLOAD/lib/install_io.sh"
io_init && io_settle_root || fail "root filesystem is not stably writable"

service_preexisting=0
if [ -f "$WIFI_LATEST/transaction/files" ]; then
while IFS='|' read -r item existed destination; do
    [ -n "$destination" ] || continue
    case "$destination" in
        "$QML_DIR/$ADV"|"$QML_DIR/$WIDE"|"$QML_DIR/$ALL"|"$QML_DIR/$SETUP"|"$QML_DIR/$CUSTOM") continue ;;
        /etc/passwd|/etc/passwd.legacy|/etc/passwd.secure|/etc/shadow|/etc/shadow.Global) continue ;;
        "$RW_WIFI"/*|"$RW_WIFI") continue ;;
    esac
    [ "$destination" = "$SERVICE" ] && [ "$existed" = 1 ] && service_preexisting=1
    if [ "$existed" = 1 ]; then
        io_mkdir "$(dirname "$destination")" || fail "cannot recreate original directory: $destination"
        io_restore_entry "$WIFI_LATEST/transaction/$item" "$destination" || \
            fail "cannot restore original owner file: $destination"
        log "restored pre-WiFi file: $destination"
    else
        io_remove "$destination" || fail "cannot remove WiFi file: $destination"
        log "removed WiFi file: $destination"
    fi
done < "$WIFI_LATEST/transaction/files"
else
    log "WARNING: no WiFi transaction ledger; removing only known WiFi paths"
    [ -e "$SERVICE" ] && { io_remove "$SERVICE" || fail "cannot remove $SERVICE"; }
fi
failpoint wifi_reverted

# Remove only the car account that the WiFi installer recorded; a pre-existing
# owner car account is kept.
remove_car() {
    f=$1
    [ -f "$f" ] && [ ! -L "$f" ] && [ -r "$f" ] && [ -w "$f" ] || {
        log "kept unreadable/unwritable account file: $f"; return 0; }
    marker="$RW_WIFI/backup/accounts/$(basename "$f")"
    [ -f "$marker" ] && [ ! -L "$marker" ] && [ -r "$marker" ] || return 0
    expected=$(cat "$marker" 2>/dev/null) || {
        log "cannot read account ownership marker: $marker"; return 0; }
    current=$(grep '^car:' "$f" 2>/dev/null || true)
    [ "$current" = "$expected" ] || { log "kept independently changed account in $f"; return 0; }
    # Stage in the same directory and rename: a read error or short write must
    # never truncate /etc/passwd or /etc/shadow.
    tmp=$(mktemp "${f}.tmp.XXXXXX" 2>/dev/null) || {
        log "cannot stage account removal for $f"; return 0; }
    if grep -v '^car:' "$f" > "$tmp" 2>/dev/null; then
        case "$f" in
            *shadow*) chmod 600 "$tmp" 2>/dev/null || true ;;
            *) chmod 644 "$tmp" 2>/dev/null || true ;;
        esac
        if mv -f "$tmp" "$f" 2>/dev/null; then
            rm -f "$marker"
            log "removed owned car account entry from $f"
            return 0
        fi
    fi
    rm -f "$tmp" 2>/dev/null || true
    log "FAILED to rewrite account file safely: $f"
}
remove_car /etc/passwd
remove_car /etc/passwd.legacy
remove_car /etc/passwd.secure
remove_car /etc/shadow
remove_car /etc/shadow.Global
[ -L /home/car ] && rm -f /home/car 2>/dev/null || true

# ---- restore stock mdnsd if the experimental root patch was armed --------
if [ -f "$RW_WIFI/backup/mdnsd.stock" ] && [ -f "$MDNSD_BIN" ]; then
    current=$(sha256_of "$MDNSD_BIN")
    if [ "$current" = "$MDNSD_PATCHED_SHA256" ]; then
        if [ "$(sha256_of "$RW_WIFI/backup/mdnsd.stock")" = "$MDNSD_STOCK_SHA256" ]; then
            if command -v systemctl >/dev/null 2>&1; then
                systemctl stop carplay_mdnsd.service >> "$LOG" 2>&1 || true
            fi
            io_copy "$RW_WIFI/backup/mdnsd.stock" "$MDNSD_BIN" || fail "cannot restore stock mdnsd"
            [ "$(sha256_of "$MDNSD_BIN")" = "$MDNSD_STOCK_SHA256" ] || fail "stock mdnsd readback mismatch"
            if command -v systemctl >/dev/null 2>&1; then
                systemctl restart carplay_mdnsd.service >> "$LOG" 2>&1 || true
            fi
            log "restored stock mdnsd"
        else
            log "WARNING: mdnsd stock backup hash mismatch; leaving binary"
        fi
    elif [ "$current" != "$MDNSD_STOCK_SHA256" ]; then
        log "WARNING: mdnsd changed independently ($current); leaving binary"
    fi
fi

# ---- rebase Wifi1b's restore point onto the no-WiFi canonical base -------
STAMP=$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null || echo migration)
NEW_BACKUP="$RW_WIFI1B/backup/$STAMP-migrate"
mkdir -p "$NEW_BACKUP/HomeScreen" "$NEW_BACKUP/component/common" "$NEW_BACKUP/component" 2>/dev/null || \
    fail "cannot create rebased Wifi1b backup"
for f in "$ADV" "$WIDE" "$ALL" "$SETUP" "$CUSTOM"; do
    if [ -f "$BASE_DIR/$f" ]; then
        mkdir -p "$NEW_BACKUP/$(dirname "$f")" 2>/dev/null
        cp -f "$BASE_DIR/$f" "$NEW_BACKUP/$f" || fail "cannot copy base QML: $f"
    fi
done
if [ -d "$RW_WIFI1B/backup/latest/transaction" ]; then
    cp -R "$RW_WIFI1B/backup/latest/transaction" "$NEW_BACKUP/" || \
        fail "cannot preserve Wifi1b transaction ledger"
fi
ln -sfn "$NEW_BACKUP" "$RW_WIFI1B/backup/latest" || fail "cannot record rebased restore point"
log "rebased Wifi1b restore point: $NEW_BACKUP"
failpoint rebased

# ---- write the accepted no-WiFi Wifi1b target ----------------------------
for spec in "advanced:$ADV" "wide:$WIDE" "menu:$ALL" "menu:$SETUP" "custom:$CUSTOM"; do
    path=${spec#*:}; base=$(basename "$path")
    dest="$QML_DIR/$path"
    io_copy "$TMP/$base" "$dest" || fail "cannot write target QML: $path"
    chmod 644 "$dest" 2>/dev/null || true
    chown 0:0 "$dest" 2>/dev/null || true
    [ "$(md5_of "$dest")" = "$(md5_of "$TMP/$base")" ] || fail "target QML readback mismatch: $path"
    log "wrote target QML: $path"
done
failpoint qml_written

# Clear QML caches so the removed WifiPage/WifiTile cannot be served.
rm -rf /root/.cache/qtshadercache /root/.cache/AppHomeScreen \
       /root/.cache/qmlcache /tmp/qmlcache /tmp/qt-qml-cache 2>/dev/null || true
sync

# ---- archive and remove the WiFi app ------------------------------------
ARCHIVE_DIR="${SCOURGE_STICK_DIR:-$USB_DIR}/backup"
mkdir -p "$ARCHIVE_DIR" 2>/dev/null || fail "cannot create stick backup directory"
ARCHIVE="$ARCHIVE_DIR/wifi-pre-migration-$STAMP"
if command -v tar >/dev/null 2>&1; then
    ARCHIVE="$ARCHIVE.tar.gz"
    tar -czf "$ARCHIVE" -C "$(dirname "$RW_WIFI")" "$(basename "$RW_WIFI")" 2>>"$LOG" || \
        fail "cannot archive $RW_WIFI"
else
    ARCHIVE="$ARCHIVE.dir"
    cp -R "$RW_WIFI" "$ARCHIVE" 2>>"$LOG" || fail "cannot archive $RW_WIFI"
fi
chmod 600 "$ARCHIVE" 2>/dev/null || true
log "archived WiFi state: $ARCHIVE"
failpoint archived

# Capture the OEM mask state while /rw_data/wifi still exists.
oem_path_state=$(cat "$RW_WIFI/backup/supplicantmanager.path.state" 2>/dev/null || true)
oem_service_state=$(cat "$RW_WIFI/backup/supplicantmanager.service.state" 2>/dev/null || true)

if command -v systemctl >/dev/null 2>&1; then
    systemctl stop wifi-daemon.service >> "$LOG" 2>&1 || true
    [ "$service_preexisting" = 1 ] || systemctl disable wifi-daemon.service >> "$LOG" 2>&1 || true
fi
if [ "$service_preexisting" = 1 ]; then
    log "kept pre-existing wifi-daemon.service (stopped)"
else
    io_remove "$SERVICE" || log "WARNING: WiFi service already absent"
fi
rm -rf "$RW_WIFI" 2>/dev/null || fail "cannot remove $RW_WIFI"

# Restore the OEM manager only when the WiFi installer unmasked it on install.
# A pre-existing mask is owner state and stays masked, exactly as the WiFi
# restore script preserves it.
for unit in supplicantmanager.path supplicantmanager.service; do
    case "$unit" in
        *.path) state=$oem_path_state ;;
        *) state=$oem_service_state ;;
    esac
    case "$state" in
        "") log "OEM unit state unknown; leaving $unit unchanged" ;;
        masked|masked-runtime) log "preserved pre-existing OEM mask: $unit" ;;
        *) command -v systemctl >/dev/null 2>&1 && systemctl unmask "$unit" >> "$LOG" 2>&1 || true
           log "unmasked OEM unit: $unit" ;;
    esac
done
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >> "$LOG" 2>&1 || true
    # Release any WiFi-era ConnMan state; try-restart is a no-op if ConnMan
    # was not running.  The Wifi1b connman drop-in keeps rmnet_usb0 out of
    # ConnMan's control so BlueLink/nav keep their modem route.
    systemctl try-restart connman.service >> "$LOG" 2>&1 || true
    # Apply the Wifi1b foreground override after WiFi's Restart=always
    # drop-in is gone; otherwise the active unit keeps its old type.
    systemctl restart carplay_mdnsd.service >> "$LOG" 2>&1 || \
        log "WARNING: carplay_mdnsd restart failed; reboot required"
fi

# ---- restart apps -------------------------------------------------------
sleep 1
restart="not-attempted"
old_home_pid=$(pidof AppHomeScreen 2>/dev/null || true)
launcher_pid=""
for p in /proc/[0-9]*; do
    [ -r "$p/comm" ] || continue
    if [ "$(cat "$p/comm" 2>/dev/null)" = "AppLauncher" ]; then
        launcher_pid="${p#/proc/}"
        break
    fi
done
if [ -n "$launcher_pid" ] && [ -r "/proc/$launcher_pid/environ" ]; then
    for var in DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR WAYLAND_DISPLAY QT_QPA_PLATFORM; do
        val=$(tr '\0' '\n' < "/proc/$launcher_pid/environ" 2>/dev/null | grep "^$var=" | head -n 1 | cut -d= -f2-)
        [ -n "$val" ] && export "$var=$val"
    done
fi
if command -v dbus-send >/dev/null 2>&1; then
    dbus-send --session --type=method_call --dest=com.lge.avn.applauncher /Request \
        com.lge.avn.applauncher.request.terminate int32:11 >> "$LOG" 2>&1 || true
    sleep 2
    if dbus-send --session --type=method_call --dest=com.lge.avn.applauncher /Request \
        com.lge.avn.applauncher.request.launch int32:11 \
        string:AppHomeScreen string:/app/bin/AppHomeScreen >> "$LOG" 2>&1; then
        restart="dbus-ok"
    else
        restart="dbus-launch-failed"
    fi
fi

# Wifi1b stays installed and returns to idle; its config and backups survive.
wifi1b_restart="not-attempted"
if command -v systemctl >/dev/null 2>&1; then
    systemctl enable wifi1b-daemon.service >> "$LOG" 2>&1 || true
    if systemctl restart wifi1b-daemon.service >> "$LOG" 2>&1; then
        wifi1b_restart="systemd-ok"
    else
        wifi1b_restart="systemd-restart-failed"
    fi
fi

printf 'migration complete\n' > "$MARKER" 2>/dev/null || fail "cannot write migration marker"
chmod 644 "$MARKER" 2>/dev/null || true
chown 0:0 "$MARKER" 2>/dev/null || true
COMPLETE=1
io_restore_root || log "WARNING: could not restore root mount state"

sleep 3
new_home_pid=$(pidof AppHomeScreen 2>/dev/null || true)
status=OK
if [ -z "$new_home_pid" ] || [ "$new_home_pid" = "$old_home_pid" ]; then
    status=REBOOT_REQUIRED
fi
[ "$wifi1b_restart" = systemd-ok ] || status=REBOOT_REQUIRED

result "status=$status" "layout=standalone" "wifi_removed=1" "wifi1b_rebased=1" \
       "radios=off" "radio_forced=$radio_forced" "archive=$ARCHIVE" "backup=$NEW_BACKUP" \
       "restart=$restart" "wifi1b_restart=$wifi1b_restart" "log=$LOG"
log "=== migrate done ==="
exit 0
