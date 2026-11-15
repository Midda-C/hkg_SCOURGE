#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Ioniq 5 Gen5W_L EU 260128
# Build locally only; never format a drive or touch a car.
# Usage: sh packages/build_flat.sh [output directory]
# Owner input: SCOURGE_WIFI_TXT=/private/path (never a repository default).
set -eu
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
OUT=${1:-"$HERE/../out/appcombo-flat"}
mkdir -p "$OUT"
OUT=$(CDPATH= cd -- "$OUT" && pwd)
STAGE=$(mktemp -d "${TMPDIR:-/tmp}/scourge-flat.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/tree"
sh "$HERE/compose.sh" "$STAGE/tree" battery,tweaks,wifi1b,migrate
if [ -n "${SCOURGE_WIFI_TXT:-}" ]; then
    cp -f "$SCOURGE_WIFI_TXT" "$STAGE/tree/wifi.txt"
    chmod 600 "$STAGE/tree/wifi.txt"
fi
if [ -n "${SCOURGE_WIFI1B_TXT:-}" ]; then
    cp -f "$SCOURGE_WIFI1B_TXT" "$STAGE/tree/iphone_wifi.txt"
    chmod 600 "$STAGE/tree/iphone_wifi.txt"
fi
if [ -n "${SCOURGE_AUTHORIZED_KEYS:-}" ]; then
    cp -f "$SCOURGE_AUTHORIZED_KEYS" "$STAGE/tree/authorized_keys"
fi
# Exercise the exact packaged additive patcher on the final canonical base.
python3 "$HERE/wifi1b/scripts/check_combined.py" "$STAGE/tree/combo" "$STAGE/final.qml"
# Skip maps must reference manifest-covered payload files and packaged units:
# a typo would silently weaken the unchanged-stage check on the car.
python3 "$HERE/common/check_skip_maps.py" "$STAGE/tree/combo"
python3 "$HERE/common/test_layout_qt.py" \
    "$STAGE/final.qml"
(cd "$STAGE/tree" && tar -czf "$STAGE/appcombo.tar.gz" .)
digest=$(sha256sum "$STAGE/appcombo.tar.gz" | cut -d' ' -f1)
sed "s/@TARBALL_SHA256@/$digest/g" "$HERE/flat_wrapper.sh" > "$STAGE/run_stuff.sh"
sh -n "$STAGE/run_stuff.sh"
cp -f "$STAGE/appcombo.tar.gz" "$STAGE/run_stuff.sh" "$OUT/"
cat > "$OUT/README.txt" <<'EOF'
SCOURGE flat combo — Battery, Tweaks, Reboot and Wifi1b (WiFi removed)
This bundle REMOVES the WiFi app. The migrate stage rebuilds the shared
home screen from the canonical stock+Battery+Tweaks base shipped inside
the archive, then reapplies Wifi1b on top. It does not depend on the
car's current QML bytes; the current files are snapshotted to the stick
under backup/wifi-removal-qml-* before anything changes. The WiFi service,
its runtime state and /rw_data/wifi are removed, and the old WiFi state is
archived to backup/wifi-pre-migration-*.tar.gz.
No manual step is needed: the preflight stops both WiFi apps and kills any
active Wifi1b/WiFi daemon before the stages run. If a radio still reports
On, the migration forces wlan0 down and continues.
OFFLINE-TESTED; on-car validation pending. No firmware/update stick present.
Copy BOTH run_stuff.sh and appcombo.tar.gz to the stick root; don't extract.
The wrapper verifies the archive SHA256 before running any stage.
SENSITIVE: appcombo.tar.gz contains the compiled Wifi1b payload, which may
include owner credentials from the private source, plus any
SCOURGE_WIFI_TXT/SCOURGE_WIFI1B_TXT or SCOURGE_AUTHORIZED_KEYS inputs.
Treat the pair as private, store and dispose of it accordingly, and keep it
out of any shared location. The migration stage also archives the old WiFi
state (including WiFi credentials) to the stick.
CarPlay control: the Wifi1b page's CarPlay tab has a switch between
"Wifi1b wireless" and "Stock USB". Wifi1b clears the vendor wireless flag
whenever the AP is not running, so stock USB CarPlay is restored when
wireless is idle; the switch also stops the AP and gives the OEM WiFi
manager back. BlueLink/nav: Wifi1b installs a hash-gated ConnMan drop-in
that keeps ConnMan off rmnet_usb0 (the primary telematics data route), and
the migration restarts ConnMan after removing the WiFi app.
Icons are best-effort; critical files and readback checks still fail closed.
Beeps: 1 start, 2 per failed stage, 5 success, 3 finished with failures.
Read wrapper_result.txt, run_stuff_result.txt and *_result.txt.
combo.log, migrate.log and *_install.log contain the current run's errors.
qml_readback_* contains private before/after QML snapshots for diagnosis.
runtime_snapshot_*_status.json can be from an older running daemon; it is
not proof that this installation succeeded. Restore logs are not exported
during an install run.
Use the default all install. Restore runs migrate (no-op), Wifi1b, Tweaks,
Battery. The migration is one-way: to reinstall WiFi, use the original
WiFi-bearing bundle and install WiFi before Wifi1b.
Changing only MODE does NOT change the failsafe's run_stuff.sh MD5 gate:
also change a comment in run_stuff.sh when rerunning the same pair.
Keep owner credentials private. Optional wifi.txt/iphone_wifi.txt and
authorized_keys at the stick root override packed inputs. SSH stays manual.
The experimental MDNSD_ROOT_ARM marker is NOT included; leave it disarmed.
Restore uses saved app backups; do not erase backups after a failed install.
EOF
echo "Built $OUT (tarball SHA256=$digest); car validation pending"
