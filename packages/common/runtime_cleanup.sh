#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Gen5W_L EU 260128
# Bounded, ownership-aware runtime cleanup shared by the flat wrapper, the
# composed master and the Wifi1b payload.  Read this as data: no function here
# follows a symlink, scans a foreign path, or deletes a directory on absence
# evidence alone.
#
# Removal of a removed WiFi app's /rw_data/wifi (credentials, keys, backups)
# additionally requires a verified private archive of that state, or a receipt
# this helper wrote after an earlier verified removal.  A corrupt or missing
# bundle therefore preserves credentials and backups.
#
# Callers set RUNTIME_CLEANUP_LOG or leave it empty and report
# RUNTIME_CLEANUP_NOTES after the first runtime write is allowed.

RUNTIME_CLEANUP_NOTES="${RUNTIME_CLEANUP_NOTES:-}"
RUNTIME_CLEANUP_SHA256="${RUNTIME_CLEANUP_SHA256:-sha256sum}"

runtime_cleanup_note() {
    case "$RUNTIME_CLEANUP_NOTES" in
        "") RUNTIME_CLEANUP_NOTES=$1 ;;
        *) RUNTIME_CLEANUP_NOTES="$RUNTIME_CLEANUP_NOTES; $1" ;;
    esac
    if [ -n "${RUNTIME_CLEANUP_LOG:-}" ] && [ -f "$RUNTIME_CLEANUP_LOG" ] &&
       [ ! -L "$RUNTIME_CLEANUP_LOG" ]; then
        printf 'cleanup: %s\n' "$1" >> "$RUNTIME_CLEANUP_LOG" 2>/dev/null || true
    fi
}

# Refuse relative, dot-containing and symlinked paths.  Every ancestor of the
# leaf is checked, so an attacker-owned symlink cannot redirect a write.
runtime_cleanup_safe_path() {
    _rc_path=$1
    case "$_rc_path" in
        /*) : ;;
        *) return 1 ;;
    esac
    case "$_rc_path" in
        *'/../'*|*/..|*'/./'*|*/.|*'//'*) return 1 ;;
    esac
    _rc_walk=$_rc_path
    while [ -n "$_rc_walk" ] && [ "$_rc_walk" != "/" ]; do
        [ ! -L "$_rc_walk" ] || return 1
        _rc_walk=$(dirname "$_rc_walk")
    done
    return 0
}

# Only these product-owned files may have stale atomic-write temporaries.
runtime_cleanup_owned_name() {
    case "$1" in
        status.json|config.conf|wifi.txt|home.json|bt.state|apply.result|scan.json|ssh.bound|\
        wpa_supplicant.conf|wpa_supplicant.pid|dhcp.leases|dhcpd.pid|udhcpc.pid|udhcpc.sh|\
        udhcpc.log|authorized_keys|dropbear.pid|dropbear_ed25519_host_key|\
        dropbear_rsa_host_key|migration.done) return 0 ;;
    esac
    return 1
}

# True when a process still runs the exact product-owned executable.  Never
# matches by name or command-line substring.
runtime_cleanup_process_alive() {
    _rc_root=$1
    _rc_app=$2
    _rc_exe=$3
    _rc_proc=${CLEANUP_PROC_ROOT:-/proc}
    [ -d "$_rc_proc" ] || return 1
    for _rc_p in "$_rc_proc"/[0-9]*; do
        [ -d "$_rc_p" ] || continue
        _rc_link=$(readlink "$_rc_p/exe" 2>/dev/null || true)
        case "$_rc_link" in
            "$_rc_root/$_rc_app/$_rc_exe"|"$_rc_root/$_rc_app/$_rc_exe (deleted)") return 0 ;;
        esac
    done
    return 1
}

# Kill only exact owned executables: TERM first, then KILL.  Used by the
# installer preflight so no manual WiFi Off step is needed.
runtime_cleanup_kill_owned() {
    _rc_root=$1
    _rc_app=$2
    _rc_exe=$3
    _rc_proc=${CLEANUP_PROC_ROOT:-/proc}
    [ -d "$_rc_proc" ] || return 0
    for _rc_sig in TERM KILL; do
        _rc_found=0
        for _rc_p in "$_rc_proc"/[0-9]*; do
            [ -d "$_rc_p" ] || continue
            _rc_link=$(readlink "$_rc_p/exe" 2>/dev/null || true)
            case "$_rc_link" in
                "$_rc_root/$_rc_app/$_rc_exe"|"$_rc_root/$_rc_app/$_rc_exe (deleted)")
                    kill -"$_rc_sig" "${_rc_p##*/}" 2>/dev/null || true
                    _rc_found=1 ;;
            esac
        done
        [ "$_rc_found" = 1 ] || return 0
        sleep 1 2>/dev/null || sleep 1
    done
    return 1
}

# Fixed, append-only daemon logs only.  This is deliberately the first runtime
# write: a full /rw_data otherwise breaks the installer's own state.  Unknown
# files, symlinks and non-regular files are left alone.
cleanup_bounded_logs() {
    _rc_root=$1
    if ! runtime_cleanup_safe_path "$_rc_root" || [ "$_rc_root" = "/" ]; then
        runtime_cleanup_note "refused unsafe runtime root: $_rc_root"
        return 1
    fi
    for _rc_app in wifi wifi1b battery tweaks; do
        _rc_dir="$_rc_root/$_rc_app"
        if [ -L "$_rc_dir" ]; then
            runtime_cleanup_note "refused symlink runtime dir: $_rc_dir"
            continue
        fi
        [ -d "$_rc_dir" ] || continue
        for _rc_name in daemon.log daemon.log.1 wpa_supplicant.log \
                        wpa_supplicant.log.1 ssh.log ssh.log.1 dhcpd.log \
                        dhcpd.log.1 tweaks_failures.log; do
            _rc_file="$_rc_dir/$_rc_name"
            if [ -L "$_rc_file" ]; then
                runtime_cleanup_note "refused symlink log: $_rc_file"
                continue
            fi
            [ -f "$_rc_file" ] || continue
            : > "$_rc_file" 2>/dev/null || \
                runtime_cleanup_note "cannot truncate log: $_rc_file"
        done
    done
    return 0
}

# Delete only <owned-name>.tmp.<pid> where <pid> is numeric, no longer exists,
# and every owned radio has been verified stopped.  Anything unproven stays.
cleanup_stale_temps() {
    _rc_root=$1
    if [ "${RUNTIME_CLEANUP_RADIOS_STOPPED:-0}" != 1 ]; then
        runtime_cleanup_note "deferred stale temp cleanup: radios not verified stopped"
        return 1
    fi
    if ! runtime_cleanup_safe_path "$_rc_root"; then
        runtime_cleanup_note "refused unsafe runtime root: $_rc_root"
        return 1
    fi
    _rc_proc=${CLEANUP_PROC_ROOT:-/proc}
    for _rc_dir in "$_rc_root/wifi" "$_rc_root/wifi1b" \
                    "$_rc_root/wifi1b/ssh" "$_rc_root/wifi1b/home/.ssh" \
                    "$_rc_root/battery" "$_rc_root/tweaks"; do
        if ! runtime_cleanup_safe_path "$_rc_dir"; then
            runtime_cleanup_note "refused unsafe temp dir: $_rc_dir"
            continue
        fi
        if [ -L "$_rc_dir" ]; then
            runtime_cleanup_note "refused symlink temp dir: $_rc_dir"
            continue
        fi
        [ -d "$_rc_dir" ] || continue
        for _rc_file in "$_rc_dir"/*.tmp.*; do
            [ -e "$_rc_file" ] || [ -L "$_rc_file" ] || continue
            if [ -L "$_rc_file" ]; then
                runtime_cleanup_note "refused symlink temp: $_rc_file"
                continue
            fi
            if [ ! -f "$_rc_file" ]; then
                runtime_cleanup_note "kept unproven temp: $_rc_file"
                continue
            fi
            _rc_base=${_rc_file##*/}
            _rc_pid=${_rc_base##*.}
            case "$_rc_pid" in
                ''|*[!0-9]*)
                    runtime_cleanup_note "kept unproven temp: $_rc_file"
                    continue ;;
            esac
            _rc_owned=${_rc_base%.tmp."$_rc_pid"}
            if ! runtime_cleanup_owned_name "$_rc_owned"; then
                runtime_cleanup_note "kept unknown temp: $_rc_file"
                continue
            fi
            [ "$_rc_pid" -gt 1 ] 2>/dev/null || {
                runtime_cleanup_note "kept invalid-pid temp: $_rc_file"
                continue
            }
            if [ -d "$_rc_proc/$_rc_pid" ]; then
                runtime_cleanup_note "kept live-pid temp: $_rc_file"
                continue
            fi
            if rm -f "$_rc_file" 2>/dev/null &&
               [ ! -e "$_rc_file" ] && [ ! -L "$_rc_file" ]; then
                runtime_cleanup_note "removed stale owned temp: $_rc_file"
            else
                runtime_cleanup_note "cannot remove stale temp: $_rc_file"
            fi
        done
    done
    return 0
}

# 0 = readable and marker absent, 1 = marker present, 2 = unreadable/unsafe.
runtime_cleanup_marker_state() {
    _rc_file=$1
    [ -f "$_rc_file" ] && [ ! -L "$_rc_file" ] && [ -r "$_rc_file" ] || return 2
    grep -q 'WIFI_PAYLOAD_260128' "$_rc_file" 2>/dev/null
    case $? in
        0) return 1 ;;
        1) return 0 ;;
        *) return 2 ;;
    esac
}

# Remove /rw_data/wifi only with all positive evidence below:
#   * readable shared QML, all five files present and without the WiFi marker;
#   * no WifiPage/WifiTile file or dangling link;
#   * no wifi-daemon service file or dangling link;
#   * no live exact owned wifi_daemon process;
#   * a readback-verified private wifi-pre-migration archive on the stick, or a
#     receipt written by this helper after a previous verified removal.
# A caller-supplied archive/hash (the flat bundle) is re-hashed read-only
# before any recursive deletion.
cleanup_removed_wifi() {
    _rc_root=$1
    _rc_qml=$2
    _rc_stick=$3
    _rc_archive=$4
    _rc_expected=$5
    _rc_service=${6:-${CLEANUP_WIFI_SERVICE:-/etc/systemd/system/wifi-daemon.service}}

    if ! runtime_cleanup_safe_path "$_rc_root" || [ "$_rc_root" = "/" ]; then
        runtime_cleanup_note "refused unsafe runtime root: $_rc_root"
        return 1
    fi
    _rc_wifi="$_rc_root/wifi"
    if [ ! -e "$_rc_wifi" ] && [ ! -L "$_rc_wifi" ]; then
        return 0
    fi
    if [ -L "$_rc_wifi" ]; then
        runtime_cleanup_note "refused symlink WiFi runtime dir: $_rc_wifi"
        return 1
    fi
    if [ ! -d "$_rc_wifi" ]; then
        runtime_cleanup_note "refused non-directory WiFi runtime path: $_rc_wifi"
        return 1
    fi

    # Re-verify the bundle hash without extracting or writing anything.
    if [ -n "$_rc_expected" ]; then
        if [ -z "$_rc_archive" ] || [ ! -f "$_rc_archive" ] ||
           [ -L "$_rc_archive" ] || [ ! -r "$_rc_archive" ]; then
            runtime_cleanup_note "preserved WiFi state: bundle archive unreadable"
            return 1
        fi
        _rc_actual=$("$RUNTIME_CLEANUP_SHA256" "$_rc_archive" 2>/dev/null | cut -d' ' -f1)
        if [ -z "$_rc_actual" ] || [ "$_rc_actual" != "$_rc_expected" ]; then
            runtime_cleanup_note "preserved WiFi state: bundle archive hash mismatch"
            return 1
        fi
        if ! tar -tzf "$_rc_archive" >/dev/null 2>&1; then
            runtime_cleanup_note "preserved WiFi state: bundle archive unreadable"
            return 1
        fi
    fi

    if ! runtime_cleanup_safe_path "$_rc_qml" || [ ! -d "$_rc_qml" ] ||
       [ -L "$_rc_qml" ] || [ ! -r "$_rc_qml" ]; then
        runtime_cleanup_note "preserved WiFi state: unreadable QML directory"
        return 1
    fi
    for _rc_rel in HomeScreen/QHMI_HomeScreenMain_Advanced.qml \
                    HomeScreen/QHMI_HomeScreenMain_Wide.qml \
                    HomeScreen/Screen_HomeScreen_AllMenus.qml \
                    HomeScreen/Screen_HomeScreen_Submenu_Setup.qml \
                    component/QCustomizedMenuListAllowEmpty.qml; do
        _rc_file="$_rc_qml/$_rc_rel"
        runtime_cleanup_marker_state "$_rc_file"
        _rc_state=$?
        case "$_rc_state" in
            0) : ;;
            1) runtime_cleanup_note "preserved WiFi state: WiFi marker in $_rc_rel"
               return 1 ;;
            *) runtime_cleanup_note "preserved WiFi state: unreadable marker $_rc_rel"
               return 1 ;;
        esac
    done
    for _rc_rel in HomeScreen/WifiPage.qml component/WifiTile.qml \
                    component/WifiSwitch.qml component/WifiButton.qml; do
        if [ -e "$_rc_qml/$_rc_rel" ] || [ -L "$_rc_qml/$_rc_rel" ]; then
            runtime_cleanup_note "preserved WiFi state: WiFi UI still present ($_rc_rel)"
            return 1
        fi
    done
    if ! runtime_cleanup_safe_path "$_rc_service"; then
        runtime_cleanup_note "preserved WiFi state: unsafe WiFi service path"
        return 1
    fi
    if [ -e "$_rc_service" ] || [ -L "$_rc_service" ]; then
        runtime_cleanup_note "preserved WiFi state: WiFi service still present"
        return 1
    fi
    if runtime_cleanup_process_alive "$_rc_root" wifi wifi_daemon; then
        runtime_cleanup_note "preserved WiFi state: owned wifi_daemon still running"
        return 1
    fi

    _rc_proof=""
    _rc_source=""
    _rc_sha=""
    _rc_receipt="$_rc_root/combo/wifi-removed.receipt"
    if [ -f "$_rc_receipt" ] && [ ! -L "$_rc_receipt" ] &&
       [ -r "$_rc_receipt" ]; then
        if [ "$(sed -n '1p' "$_rc_receipt" 2>/dev/null)" = \
             "SCOURGE_WIFI_REMOVAL_RECEIPT" ]; then
            _rc_sha=$(sed -n 's/^archive_sha256=//p' "$_rc_receipt" 2>/dev/null | head -n 1)
            case "$_rc_sha" in
                *[!0-9a-f]*|'') _rc_sha="" ;;
            esac
            [ "${#_rc_sha}" -eq 64 ] && _rc_proof=receipt
        fi
    fi
    if [ -z "$_rc_proof" ] && [ -n "$_rc_stick" ] &&
       [ -d "$_rc_stick/backup" ] && [ ! -L "$_rc_stick/backup" ]; then
        for _rc_candidate in "$_rc_stick/backup"/wifi-pre-migration-*.tar.gz; do
            [ -f "$_rc_candidate" ] && [ ! -L "$_rc_candidate" ] &&
                [ -r "$_rc_candidate" ] || continue
            _rc_sha=$("$RUNTIME_CLEANUP_SHA256" "$_rc_candidate" 2>/dev/null | cut -d' ' -f1)
            [ -n "$_rc_sha" ] || continue
            # Read the archive back, not just its header: a corrupt or empty
            # archive must never authorise credential deletion.
            if tar -tzf "$_rc_candidate" 2>/dev/null | grep -q '^wifi/'; then
                _rc_proof=archive
                _rc_source=$_rc_candidate
                break
            fi
        done
    fi
    if [ -z "$_rc_proof" ]; then
        runtime_cleanup_note "preserved WiFi state: no verified private archive or removal receipt"
        return 1
    fi

    if ! rm -rf "$_rc_wifi" 2>/dev/null ||
       [ -e "$_rc_wifi" ] || [ -L "$_rc_wifi" ]; then
        runtime_cleanup_note "failed to remove owned WiFi runtime: $_rc_wifi"
        return 1
    fi
    runtime_cleanup_note "removed owned WiFi runtime after verified archive/receipt: $_rc_wifi"

    # Persist provenance for later standalone runs.  This is the only receipt
    # this helper trusts; it contains no credentials.
    _rc_combo="$_rc_root/combo"
    if [ ! -L "$_rc_combo" ] && { [ ! -e "$_rc_combo" ] || [ -d "$_rc_combo" ]; }; then
        if mkdir -p "$_rc_combo" 2>/dev/null; then
            {
                echo "SCOURGE_WIFI_REMOVAL_RECEIPT"
                echo "archive_sha256=$_rc_sha"
                echo "archive_source=${_rc_source:-receipt}"
                echo "runtime=$_rc_wifi"
            } > "$_rc_combo/wifi-removed.receipt" 2>/dev/null &&
                chmod 600 "$_rc_combo/wifi-removed.receipt" 2>/dev/null || \
                runtime_cleanup_note "could not record WiFi removal receipt"
        fi
    fi
    return 0
}
