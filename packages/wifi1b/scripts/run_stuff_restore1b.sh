#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Gen5W_L EU 260128
# WIFI1B_PAYLOAD_RESTORE=1
# Removes Wifi1b and restores the pre-Wifi1b shared QML, leaving WiFi intact.
set -u
umask 077
PATH="${WIFI1B_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-/media/usb}"
PAYLOAD="$USB_DIR/payload"
QML_ROOT="${WIFI1B_QML_ROOT:-/app/share/AppHomeScreen}"
QML_DIR="$QML_ROOT/qml_eco3.0"
ASSET_DIR="$QML_ROOT/wifi1b"
RW_DIR="${WIFI1B_RW_DIR:-/rw_data/wifi1b}"
LOG="$RW_DIR/restore.log"
OUT="$USB_DIR/run_stuff_result.txt"
SERVICE="${WIFI1B_SERVICE:-/etc/systemd/system/wifi1b-daemon.service}"

ADV="HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
WIDE="HomeScreen/QHMI_HomeScreenMain_Wide.qml"
ALL="HomeScreen/Screen_HomeScreen_AllMenus.qml"
SETUP="HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
CUSTOM="component/QCustomizedMenuListAllowEmpty.qml"

if [ -L "$RW_DIR" ]; then
    printf 'WIFI1B_RESTORE\nstatus=FAIL\nerror=runtime directory is a symlink\n' > "$OUT" 2>/dev/null || true
    exit 1
fi
mkdir -p "$RW_DIR" 2>/dev/null
: > "$LOG" 2>/dev/null
log() { echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null; }
result() {
    { echo "WIFI1B_RESTORE"; echo "time=$(date -u 2>/dev/null)"; echo "usb=$USB_DIR"; echo "$@"; } > "$OUT" 2>&1
    cp -f "$OUT" "$RW_DIR/restore-result.txt" 2>/dev/null
}
fail() { log "FAIL: $*"; result "status=FAIL" "error=$*"; exit 1; }

was_ro=0
cleanup_ro() { if [ "${IO_ACTIVE:-0}" = 1 ]; then io_restore_root || true; fi; }
trap cleanup_ro EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

mount -o remount,rw "$USB_DIR" 2>/dev/null || true
[ -f "$USB_DIR/manifest.md5" ] || fail "manifest.md5 missing"
MD5SUM_BIN=md5sum
command -v md5sum >/dev/null 2>&1 || MD5SUM_BIN=md5sum.coreutils
if ! ( cd "$USB_DIR" && "$MD5SUM_BIN" -c manifest.md5 ) > "$RW_DIR/restore-manifest.log" 2>&1; then
    fail "manifest check failed"
fi

LATEST="$RW_DIR/backup/latest"
[ -d "$LATEST" ] || fail "no backup/latest; cannot restore"
[ ! -L "$RW_DIR/backup" ] || fail "backup directory is a symlink; refusing restore"
LATEST_REAL=$(readlink -f "$LATEST" 2>/dev/null || true)
case "$LATEST_REAL" in
    "$RW_DIR"/backup/*) : ;;
    *) fail "restore point escapes the Wifi1b backup directory" ;;
esac

. "$PAYLOAD/lib/install_io.sh"
io_init || fail "cannot determine root mount state"
io_capture_qml "$QML_DIR" "$USB_DIR/qml_before" || true
io_settle_root || fail "root did not settle writable"

# Read the original service transaction.  A pre-existing file and its
# enablement/active state are restored; a foreign-modified service is never
# removed or stopped.  Missing/legacy ledger entries preserve the file.
SERVICE_ITEM=""
SERVICE_EXISTED=""
SERVICE_ENABLED=""
SERVICE_ACTIVE=""
if [ -f "$LATEST/transaction/files" ]; then
    while IFS='|' read -r _item _existed _destination; do
        [ "$_destination" = "$SERVICE" ] || continue
        SERVICE_ITEM=$_item
        SERVICE_EXISTED=$_existed
    done < "$LATEST/transaction/files"
fi
if [ -f "$LATEST/transaction/service.state" ]; then
    SERVICE_ENABLED=$(sed -n 's/^enabled=//p' "$LATEST/transaction/service.state" 2>/dev/null)
    SERVICE_ACTIVE=$(sed -n 's/^active=//p' "$LATEST/transaction/service.state" 2>/dev/null)
fi
[ -n "$SERVICE_ENABLED" ] || SERVICE_ENABLED=unknown
[ -n "$SERVICE_ACTIVE" ] || SERVICE_ACTIVE=0
service_is_owned=0
if [ -f "$SERVICE" ] && [ ! -L "$SERVICE" ] &&
   cmp -s "$SERVICE" "$PAYLOAD/systemd/wifi1b-daemon.service"; then
    service_is_owned=1
fi
service_may_stop=0
if [ "$service_is_owned" = 1 ]; then
    service_may_stop=1
elif [ ! -e "$SERVICE" ] && [ ! -L "$SERVICE" ]; then
    # No service file, but a stale unit process may still need stopping.
    service_may_stop=1
fi

# Stop Wifi1b and release wlan0.  Never stop/disable a foreign-modified
# service: only exact owned bytes (or an absent service) are touched.
if [ "$service_may_stop" = 1 ] && command -v systemctl >/dev/null 2>&1; then
    systemctl stop wifi1b-daemon.service 2>/dev/null || true
    if [ "$SERVICE_EXISTED" = 0 ] && [ "$service_is_owned" = 1 ]; then
        systemctl disable wifi1b-daemon.service 2>/dev/null || true
    fi
else
    log "kept independently changed/foreign wifi1b service; not stopping or disabling it"
fi
if [ "$service_may_stop" = 1 ]; then
    if [ -x "$RW_DIR/wifi1b_daemon" ]; then
        "$RW_DIR/wifi1b_daemon" --mode off >> "$LOG" 2>&1 || fail "Wifi1b network cleanup failed; retain daemon for recovery"
    fi
    rm -f /run/wifi1b/active 2>/dev/null || true
    for p in /proc/[0-9]*; do
        executable=$(readlink "$p/exe" 2>/dev/null || true)
        case "$executable" in
            "$RW_DIR/wifi1b_daemon"|"$RW_DIR/wifi1b_daemon (deleted)"|\
            "$RW_DIR/bin/dropbearmulti"|"$RW_DIR/bin/dropbearmulti (deleted)")
                kill -TERM "${p#/proc/}" 2>/dev/null || true ;;
        esac
    done
else
    log "kept foreign-modified service process; not stopping or cleaning its network state"
fi
for family in iptables ip6tables; do
    IPT=$(command -v "$family" 2>/dev/null || true)
    [ -n "$IPT" ] || continue
    for chain in WIFI1B_APP_SSH WIFI1B_APP_CP WIFI1B_APP_AP; do
        for attempt in 1 2 3 4; do "$IPT" -D INPUT -i wlan0 -j "$chain" 2>/dev/null || break; done
        for attempt in 1 2 3 4; do "$IPT" -D OUTPUT -o wlan0 -j "$chain" 2>/dev/null || break; done
        "$IPT" -F "$chain" 2>/dev/null || true
        "$IPT" -X "$chain" 2>/dev/null || true
    done
done

# Restore the shared QML exactly as it was before Wifi1b was installed.
# The backup still contains the WiFi page/tile and the WiFi installer's
# fixes, because Wifi1b only ran after WiFi.
current_adv="$QML_DIR/$ADV"
if ! grep -q 'WIFI1B_PAYLOAD_260128' "$current_adv" 2>/dev/null && [ "${WIFI1B_FORCE_RESTORE:-0}" != "1" ]; then
    fail "current Advanced QML has no Wifi1b marker; refusing to overwrite unknown changes"
fi
for f in "$ADV" "$WIDE" "$ALL" "$SETUP" "$CUSTOM"; do
    if [ -f "$LATEST/$f" ]; then
        label=$(getfattr -n security.SMACK64 --only-values "$QML_DIR/$f" 2>/dev/null || true)
        mkdir -p "$QML_DIR/$(dirname "$f")" 2>/dev/null
        io_copy "$LATEST/$f" "$QML_DIR/$f" || fail "restore failed: $f"
        chmod 644 "$QML_DIR/$f" 2>/dev/null || true
        chown 0:0 "$QML_DIR/$f" 2>/dev/null || true
        if [ -n "$label" ]; then
            if setfattr -n security.SMACK64 -v "$label" "$QML_DIR/$f" 2>/dev/null; then
                actual=$(getfattr -n security.SMACK64 --only-values "$QML_DIR/$f" 2>/dev/null || true)
                [ "$actual" = "$label" ] || log "WARNING: SMACK64 readback mismatch on $f"
            else
                log "WARNING: cannot set SMACK64=$label on $f; keeping inherited label"
            fi
        fi
        log "restored $f"
    fi
done

# Remove only Wifi1b's own QML components and assets.
for f in HomeScreen/Wifi1bPage.qml component/Wifi1bTile.qml component/Wifi1bSwitch.qml component/Wifi1bButton.qml; do
    if [ -f "$PAYLOAD/qml/AppHomeScreen/qml_eco3.0/$f" ] && cmp -s "$PAYLOAD/qml/AppHomeScreen/qml_eco3.0/$f" "$QML_DIR/$f"; then
        rm -f "$QML_DIR/$f" 2>/dev/null || true
    else
        log "kept changed/unknown component: $f"
    fi
done
for source in "$PAYLOAD/assets/wifi1b/"*; do
    [ -f "$source" ] || continue
    target="$ASSET_DIR/$(basename "$source")"
    if cmp -s "$source" "$target"; then rm -f "$target" 2>/dev/null || true; fi
done
rmdir "$ASSET_DIR" 2>/dev/null || true

# Restore the service file and its original enabled/active state from the
# install transaction.  Never remove or stop an owner-modified/foreign file.
restore_service() {
    if [ "$SERVICE_EXISTED" = 1 ]; then
        if [ -n "$SERVICE_ITEM" ] &&
           { [ ! -e "$SERVICE" ] && [ ! -L "$SERVICE" ] || [ "$service_is_owned" = 1 ]; }; then
            io_restore_entry "$LATEST/transaction/$SERVICE_ITEM" "$SERVICE" || \
                fail "cannot restore original wifi1b service"
            chmod 644 "$SERVICE" 2>/dev/null || true
            log "restored pre-existing wifi1b service from transaction"
            if command -v systemctl >/dev/null 2>&1; then
                systemctl daemon-reload >> "$LOG" 2>&1 || true
                case "$SERVICE_ENABLED" in
                    enabled) systemctl enable wifi1b-daemon.service >> "$LOG" 2>&1 || \
                                 log "WARNING: could not re-enable pre-existing wifi1b service" ;;
                    disabled) systemctl disable wifi1b-daemon.service >> "$LOG" 2>&1 || true ;;
                esac
                if [ "$SERVICE_ACTIVE" = 1 ]; then
                    systemctl start wifi1b-daemon.service >> "$LOG" 2>&1 || \
                        log "WARNING: could not re-start pre-existing wifi1b service"
                else
                    systemctl stop wifi1b-daemon.service >> "$LOG" 2>&1 || true
                fi
            fi
        else
            log "kept independently changed/foreign wifi1b service; no restore"
        fi
        return 0
    fi
    if [ -z "$SERVICE_EXISTED" ]; then
        # Legacy ledger without a service entry: remove only exact owned bytes.
        if [ "$service_is_owned" = 1 ]; then
            rm -f "$SERVICE" 2>/dev/null || true
            command -v systemctl >/dev/null 2>&1 && \
                systemctl disable wifi1b-daemon.service >> "$LOG" 2>&1 || true
            log "removed owned wifi1b service (legacy ledger)"
        else
            log "kept unknown wifi1b service: legacy ledger has no ownership entry"
        fi
        return 0
    fi
    # Originally absent: remove only our exact bytes, never a foreign file.
    if [ "$service_is_owned" = 1 ]; then
        rm -f "$SERVICE" 2>/dev/null || true
        if command -v systemctl >/dev/null 2>&1; then
            systemctl disable wifi1b-daemon.service >> "$LOG" 2>&1 || true
            systemctl stop wifi1b-daemon.service >> "$LOG" 2>&1 || true
            systemctl daemon-reload >> "$LOG" 2>&1 || true
        fi
        log "removed owned wifi1b service"
    else
        log "kept foreign wifi1b service (originally absent)"
    fi
}
restore_service
MDNS_DROPIN="${WIFI1B_MDNS_DROPIN:-/etc/systemd/system/carplay_mdnsd.service.d/60-wifi1b-foreground.conf}"
if cmp -s "$MDNS_DROPIN" "$PAYLOAD/systemd/60-wifi1b-foreground.conf"; then
    # An identical override may have existed before installation. Preserve it
    # using the original restore point, not the latest reinstall transaction.
    while IFS='|' read -r item existed destination; do
        [ "$destination" = "$MDNS_DROPIN" ] || continue
        if [ "$existed" = 1 ]; then
            io_copy "$LATEST/transaction/$item" "$MDNS_DROPIN" || fail "cannot restore original mdns override"
        else
            io_remove "$MDNS_DROPIN" || fail "cannot remove owned mdns foreground override"
        fi
    done < "$LATEST/transaction/files"
fi
CONNMAN_DROPIN="${WIFI1B_CONNMAN_DROPIN:-/etc/systemd/system/connman.service.d/60-scourge-nodevice.conf}"
if cmp -s "$CONNMAN_DROPIN" "$PAYLOAD/systemd/60-connman-nodevice.conf"; then
    while IFS='|' read -r item existed destination; do
        [ "$destination" = "$CONNMAN_DROPIN" ] || continue
        if [ "$existed" = 1 ]; then
            io_copy "$LATEST/transaction/$item" "$CONNMAN_DROPIN" || fail "cannot restore original connman override"
        else
            io_remove "$CONNMAN_DROPIN" || fail "cannot remove owned connman override"
        fi
    done < "$LATEST/transaction/files"
    rmdir "$(dirname "$CONNMAN_DROPIN")" 2>/dev/null || true
    systemctl daemon-reload >> "$LOG" 2>&1 || true
fi

# Restore VBT only when Wifi1b was the last app to change it and WiFi is not
# installed.  With both apps installed, leave the shared flag alone.
if [ ! -f /rw_data/wifi/wifi_daemon ]; then
    vbt_changed=0
    restore_vbt() {
        identity=$1
        vbt=$2
        bak="$RW_DIR/backup/vbt-$identity.original"
        applied="$RW_DIR/backup/vbt-$identity.applied"
        [ -f "$vbt" ] && [ -f "$bak" ] && [ -f "$applied" ] || return 0
        cmp -s "$vbt" "$applied" || { log "kept independently changed VBT: $vbt"; return 0; }
        cp -p "$bak" "$vbt.wifi1b-restore.$$" || return 0
        mv -f "$vbt.wifi1b-restore.$$" "$vbt" || return 0
        vbt_changed=1
    }
    restore_vbt app /app/data/vbt/VBTConfig.json
    restore_vbt rw /rw_data/app_data/vbt/VBTConfig.json
    [ "$vbt_changed" != 1 ] || systemctl restart vbtd.service >> "$LOG" 2>&1 || true
fi

# Remove only the wifi1b: entries this installer recorded.
remove_wifi1b() {
    f="$1"
    [ -f "$f" ] && [ ! -L "$f" ] && [ -r "$f" ] && [ -w "$f" ] || {
        log "kept unreadable/unwritable account file: $f"; return 0; }
    marker="$RW_DIR/backup/accounts/$(basename "$f")"
    [ -f "$marker" ] && [ ! -L "$marker" ] && [ -r "$marker" ] || return 0
    expected=$(cat "$marker" 2>/dev/null) || { log "cannot read account ownership marker: $marker"; return 0; }
    current=$(grep '^wifi1b:' "$f" 2>/dev/null || true)
    [ "$current" = "$expected" ] || { log "kept independently changed account in $f"; return 0; }
    # Stage in the same directory and rename: a read error or short write must
    # never truncate /etc/passwd or /etc/shadow.
    tmp=$(mktemp "${f}.tmp.XXXXXX" 2>/dev/null) || {
        log "cannot stage account removal for $f"; return 0; }
    if grep -v '^wifi1b:' "$f" > "$tmp" 2>/dev/null; then
        case "$f" in
            *shadow*) chmod 600 "$tmp" 2>/dev/null || true ;;
            *) chmod 644 "$tmp" 2>/dev/null || true ;;
        esac
        if mv -f "$tmp" "$f" 2>/dev/null; then
            rm -f "$marker"
            log "removed owned wifi1b account entry from $f"
            return 0
        fi
    fi
    rm -f "$tmp" 2>/dev/null || true
    log "FAILED to rewrite account file safely: $f"
}
remove_wifi1b /etc/passwd
remove_wifi1b /etc/passwd.legacy
remove_wifi1b /etc/passwd.secure
remove_wifi1b /etc/shadow
remove_wifi1b /etc/shadow.Global

# Keep backups; remove runtime pieces and secrets.
rm -f "$RW_DIR/wifi1b_daemon" "$RW_DIR/status.json" "$RW_DIR/daemon.log" \
      "$RW_DIR/config.conf" "$RW_DIR/wifi.txt" "$RW_DIR/home.json" \
      "$RW_DIR/apply.result" "$RW_DIR/bt.state" "$RW_DIR/ssh.bound" \
      "$RW_DIR/wpa_supplicant.conf" "$RW_DIR/wpa_supplicant.log" \
      "$RW_DIR/wpa_supplicant.pid" "$RW_DIR/scan.json" \
      "$RW_DIR/dhcp.leases" "$RW_DIR/dhcpd.pid" "$RW_DIR/dhcpd.log" \
      "$RW_DIR/udhcpc.pid" "$RW_DIR/udhcpc.sh" "$RW_DIR/udhcpc.log" 2>/dev/null || true
rm -f "$RW_DIR/bin/dropbearmulti" "$RW_DIR/bin/dropbear" "$RW_DIR/bin/dropbearkey" \
      "$RW_DIR/bin/wifi1b_qmlpatch" \
      "$RW_DIR/ssh/dropbear.pid" \
      "$RW_DIR/ssh/dropbear_ed25519_host_key" "$RW_DIR/ssh/dropbear_rsa_host_key" 2>/dev/null || true
auth_ledger=0
if [ -f "$LATEST/transaction/files" ] &&
   grep -Fq "|$RW_DIR/ssh/authorized_keys" "$LATEST/transaction/files"; then
    auth_ledger=1
fi
if [ "$auth_ledger" = 1 ]; then
    rm -f "$RW_DIR/ssh/authorized_keys" "$RW_DIR/home/.ssh/authorized_keys" 2>/dev/null || true
else
    log "kept authorized_keys: no transaction ownership record"
fi
rmdir "$RW_DIR/bin" "$RW_DIR/ssh" "$RW_DIR/home/.ssh" "$RW_DIR/home" 2>/dev/null || true

if [ -f "$LATEST/transaction/files" ]; then
    while IFS='|' read -r item existed destination; do
        [ "$existed" = 1 ] || continue
        case "$destination" in
            "$RW_DIR/ssh/authorized_keys"|"$RW_DIR/home/.ssh/authorized_keys"|"$QML_ROOT/wifi/"*|"$QML_ROOT/hello/"*|"$QML_ROOT/battery/"*)
                mkdir -p "$(dirname "$destination")" || fail "cannot restore original file directory"
                cp -pP "$LATEST/transaction/$item" "$destination" || fail "cannot restore original owner file: $destination"
                ;;
        esac
    done < "$LATEST/transaction/files"
fi

if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload 2>/dev/null || true
fi
sync

# Restart AppHomeScreen.
restart="not-attempted"
old_home_pid=$(pidof AppHomeScreen 2>/dev/null || true)
launcher_pid=""
for p in /proc/[0-9]*; do
    [ -r "$p/comm" ] || continue
    if [ "$(cat "$p/comm" 2>/dev/null)" = "AppLauncher" ]; then launcher_pid="${p#/proc/}"; break; fi
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
    restart="dbus-launch-failed"
    for i in 1 2 3; do
        if dbus-send --session --type=method_call --dest=com.lge.avn.applauncher /Request \
            com.lge.avn.applauncher.request.launch int32:11 \
            string:AppHomeScreen string:/app/bin/AppHomeScreen >> "$LOG" 2>&1; then
            restart="dbus-ok"; break
        fi
        sleep 2
    done
else
    restart="dbus-send-missing"
fi
if [ "$restart" != "dbus-ok" ]; then
    for p in /proc/[0-9]*; do
        [ -r "$p/comm" ] || continue
        if [ "$(cat "$p/comm" 2>/dev/null)" = "AppHomeScreen" ]; then kill -TERM "${p#/proc/}" 2>/dev/null || true; fi
    done
    restart="$restart (killed AppHomeScreen; reboot if UI does not return)"
fi
sleep 3
new_home_pid=$(pidof AppHomeScreen 2>/dev/null || true)
if [ -n "$new_home_pid" ] && [ "$new_home_pid" != "$old_home_pid" ]; then restart="$restart verified=1"; else restart="$restart verified=0 REBOOT_REQUIRED"; fi
log "restart=$restart"
sync

final_status=OK
case "$restart" in *verified=0*) final_status=REBOOT_REQUIRED;; esac
result "status=$final_status" "backup=$LATEST" "restart=$restart" "log=$LOG"
log "=== Wifi1b restore done ==="
exit 0
