#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Gen5W_L EU 260128; DISPOSABLE VENDOR-KERNEL VM ONLY.
set -eu
[ "${SCOURGE_VM_TEST:-}" = 1 ] && [ "$(uname -r)" = 4.1.27-abl ] || exit 77
P=/media/v7
mount -t tmpfs tmpfs /run
mkdir -p /run/v7
LOG=/run/v7/io.log
RW_DIR=/run/v7
log() { printf '%s\n' "$*" >> "$LOG"; }
. "$P/install_io.sh"
io_init
case ",$IO_ORIGINAL_OPTIONS," in *,rw,*) :;; *) echo 'unexpected initial VM root state'; exit 1;; esac
trap 'mount -o remount,rw / >/dev/null 2>&1 || true' EXIT
ulimit -c 0
if [ "${SCOURGE_VM_REPRO_AWK:-0}" = 1 ]; then
echo '=== vendor awk baseline (negative control; harness must report the abort) ==='
set +e
awk -v root=/ '$2 == root { print $4; exit }' /proc/mounts >/run/v7/awk.out 2>/run/v7/awk.err
rc=$?
set -e
echo "original_awk_rc=$rc"
cat /run/v7/awk.out
head -n 2 /run/v7/awk.err
fi
echo '=== replacement helper, initially read-only root ==='
mount -o remount,ro /
io_init
case ",$IO_ORIGINAL_OPTIONS," in *,ro,*) :;; *) exit 1;; esac
io_settle_root
printf 'verified-copy\n' >/run/v7/source
printf 'old\n' >/tmp/scourge-v7-target
inode=$(stat -c%i /tmp/scourge-v7-target)
io_copy /run/v7/source /tmp/scourge-v7-target
cmp /run/v7/source /tmp/scourge-v7-target
[ "$(stat -c%i /tmp/scourge-v7-target)" = "$inode" ]
io_capture_qml /app/share/AppHomeScreen/qml_eco3.0 /run/v7/qml
(cd /run/v7/qml && md5sum -c hashes.md5)
rm /tmp/scourge-v7-target
io_restore_root
case ",$(io_root_options)," in *,ro,*) :;; *) exit 1;; esac
cat "$LOG"
echo 'PASS: vendor ash mount parser, writable probes, real copy and original ro restoration'
mount -o remount,rw /
fail() { echo "FAIL: $*"; exit 1; }
. "$P/config_migration.sh"
printf 'main_crtc=0\nkeep=literal\\text & value\nconfig_version=1' >/run/v7/config
set_conf_key main_crtc 1 /run/v7/config
set_conf_key config_version 2 /run/v7/config
set_conf_key new_key 'value & backslash\literal' /run/v7/config
grep -qx 'main_crtc=1' /run/v7/config
grep -qx 'config_version=2' /run/v7/config
grep -Fqx 'keep=literal\text & value' /run/v7/config
grep -Fqx 'new_key=value & backslash\literal' /run/v7/config
echo 'PASS: vendor shell config migration; missing newline, literal values, replacement and append'
echo 'VENDOR_INSTALL_IO_COMPLETE (QML runtime tested separately on host Qt Quick)'
