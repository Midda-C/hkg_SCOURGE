#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Gen5W_L EU 260128
# Build the WiFi-removal migration USB payload tree.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
OUT=${1:-"$ROOT/out/usb"}
WIFI1B="$ROOT/../wifi1b/out/usb/payload"
TWEAKS="$ROOT/../tweaks/out/usb/payload"
BASE_QML="qml/AppHomeScreen/qml_eco3.0"

[ -f "$TWEAKS/$BASE_QML/HomeScreen/QHMI_HomeScreenMain_Advanced.battery_tile.qml" ] || {
    echo "missing Tweaks no-WiFi base QML; build tweaks first" >&2
    exit 1
}

[ -f "$WIFI1B/bin/wifi1b_qmlpatch" ] || {
    echo "missing $WIFI1B/bin/wifi1b_qmlpatch; build wifi1b first" >&2
    exit 1
}
[ -f "$WIFI1B/accepted-qml.md5" ] || {
    echo "missing $WIFI1B/accepted-qml.md5; build wifi1b first" >&2
    exit 1
}
[ -f "$WIFI1B/systemd/wifi1b-daemon.service" ] || {
    echo "missing wifi1b-daemon.service; build wifi1b first" >&2
    exit 1
}

cd "$ROOT"
rm -rf "$OUT"
mkdir -p "$OUT/payload/bin" "$OUT/payload/lib" "$OUT/payload/systemd" "$OUT/backup"

cp "$WIFI1B/bin/wifi1b_qmlpatch" "$OUT/payload/bin/"
cp "$WIFI1B/accepted-qml.md5" "$OUT/payload/"
cp "$WIFI1B/systemd/wifi1b-daemon.service" "$OUT/payload/systemd/"
cp "$ROOT/../common/install_io.sh" "$OUT/payload/lib/"
printf 'migration complete\n' > "$OUT/payload/migration.done"

# Canonical no-WiFi Battery/Tweaks base.  WiFi's own restore point predates
# Tweaks in the V11 install order, so it must not be used as this base.
mkdir -p "$OUT/payload/base/HomeScreen" "$OUT/payload/base/component"
cp "$TWEAKS/$BASE_QML/HomeScreen/QHMI_HomeScreenMain_Advanced.battery_tile.qml" \
   "$OUT/payload/base/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
cp "$TWEAKS/$BASE_QML/HomeScreen/QHMI_HomeScreenMain_Wide.stock.qml" \
   "$OUT/payload/base/HomeScreen/QHMI_HomeScreenMain_Wide.qml"
cp "$TWEAKS/$BASE_QML/HomeScreen/Screen_HomeScreen_AllMenus.stock.qml" \
   "$OUT/payload/base/HomeScreen/Screen_HomeScreen_AllMenus.qml"
cp "$TWEAKS/$BASE_QML/HomeScreen/Screen_HomeScreen_Submenu_Setup.stock.qml" \
   "$OUT/payload/base/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
cp "$TWEAKS/$BASE_QML/component/QCustomizedMenuListAllowEmpty.stock.qml" \
   "$OUT/payload/base/component/QCustomizedMenuListAllowEmpty.qml"
chmod 755 "$OUT/payload/bin/wifi1b_qmlpatch"

cp scripts/run_stuff_migrate.sh "$OUT/run_stuff.sh"
cp scripts/run_stuff_restore_migrate.sh "$OUT/run_stuff_restore.sh"
# The initial-root loader md5-gates run_stuff.sh.  Change its content on
# every build so a rebuilt payload actually runs.
printf '\n# MIGRATE_STAMP=%s\n' "$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null)-$$" >> "$OUT/run_stuff.sh"
printf '\n# MIGRATE_RESTORE_STAMP=%s\n' "$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null)-$$" >> "$OUT/run_stuff_restore.sh"
chmod 755 "$OUT/run_stuff.sh" "$OUT/run_stuff_restore.sh"

printf 'WiFi-removal migration archives are written here.\n' > "$OUT/backup/README.txt"

cat > "$OUT/README.txt" <<'EOT'
WiFi removal / Wifi1b rebase
============================

One-way migration for an existing Battery + Tweaks + WiFi + Wifi1b install.
It removes the WiFi app, keeps Battery/Tweaks/Reboot and Wifi1b, and rebases
Wifi1b's restore point onto the no-WiFi canonical home screen.

Guards:
  * current, pre-WiFi and rebuilt Wifi1b QML must match the shipped ledger;
  * both radios must report Off before anything is removed;
  * /rw_data/wifi is archived to this stick before deletion;
  * system accounts and files created by WiFi are removed only when the WiFi
    transaction ledger proves they were not pre-existing.

Install:
  1. Copy this USB tree to a FAT/exFAT stick.
  2. Plug it in; check run_stuff_result.txt for status=OK.
  3. Reboot if the result says REBOOT_REQUIRED.

Restore:
  This stage is intentionally one-way. Its restore is a no-op; to put WiFi
  back after migrating, use the original WiFi-bearing bundle and reinstall
  WiFi before Wifi1b. Keep the archived wifi-pre-migration-*.tar.gz private:
  it contains WiFi configuration and credentials.
EOT

python3 scripts/package_release.py "$OUT" --metadata-only
( cd "$OUT" && find payload -type f -print0 | sort -z | xargs -0 md5sum > manifest.md5 )
python3 scripts/package_release.py "$OUT"

echo "== done =="
echo "USB tree: $OUT"
echo "manifest entries: $(wc -l < "$OUT/manifest.md5")"
