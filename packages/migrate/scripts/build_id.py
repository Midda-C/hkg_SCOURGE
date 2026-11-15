#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Content identity for the WiFi-removal migration scripts."""
import hashlib
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WIFI1B = ROOT.parent / "wifi1b"
TWEAKS_QML = ROOT.parent / "tweaks/out/usb/payload/qml/AppHomeScreen/qml_eco3.0"


def build_id():
    paths = [ROOT / "scripts/build_payload.sh",
             ROOT / "scripts/run_stuff_migrate.sh",
             ROOT / "scripts/run_stuff_restore_migrate.sh",
             ROOT / "skip.map", ROOT / "skip.services",
             WIFI1B / "out/usb/payload/accepted-qml.md5",
             WIFI1B / "out/usb/payload/bin/wifi1b_qmlpatch",
             WIFI1B / "out/usb/payload/systemd/wifi1b-daemon.service",
             TWEAKS_QML / "HomeScreen/QHMI_HomeScreenMain_Advanced.battery_tile.qml",
             TWEAKS_QML / "HomeScreen/QHMI_HomeScreenMain_Wide.stock.qml",
             TWEAKS_QML / "HomeScreen/Screen_HomeScreen_AllMenus.stock.qml",
             TWEAKS_QML / "HomeScreen/Screen_HomeScreen_Submenu_Setup.stock.qml",
             TWEAKS_QML / "component/QCustomizedMenuListAllowEmpty.stock.qml",
             ROOT.parent / "common/install_io.sh"]
    digest = hashlib.sha256()
    for path in sorted(paths):
        digest.update(os.path.relpath(path, ROOT).encode() + b"\0"
                      + path.read_bytes() + b"\0")
    return digest.hexdigest()


if __name__ == "__main__":
    print(build_id())
