#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Gen5W_L EU 260128
"""Record only current canonical variants and their additive Wifi1b results."""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
names = {'QHMI_HomeScreenMain_Advanced': 'advanced', 'QHMI_HomeScreenMain_Wide': 'wide',
         'Screen_HomeScreen_AllMenus': 'menu', 'Screen_HomeScreen_Submenu_Setup': 'menu',
         'QCustomizedMenuListAllowEmpty': 'custom'}
digests = set()
standalone = 0
with tempfile.TemporaryDirectory(prefix='wifi1b-accepted-') as d:
    for app in ('wifi', 'tweaks'):
        qml = root.parent / app / 'artifacts/qml/AppHomeScreen/qml_eco3.0'
        for source in qml.rglob('*.qml'):
            stem = source.name.split('.')[0]
            if stem not in names:
                continue
            data = source.read_bytes()
            wifi_based = b'WIFI_PAYLOAD_260128' in data
            # WiFi-based canonical layouts carry the WiFi marker; standalone
            # (no WiFi app) Tweaks-based layouts carry only the Tweaks marker.
            if not wifi_based and b'TWEAKS_PAYLOAD_260128' not in data:
                continue
            if names[stem] == 'advanced' and b'SCOURGE_LAYOUT_V2' not in data:
                continue
            target = Path(d) / source.name
            target.write_bytes(data)
            subprocess.run([str(root/'src/wifi1b_qmlpatch'), names[stem], str(target)], check=True)
            digests.add(hashlib.md5(data).hexdigest())
            digests.add(hashlib.md5(target.read_bytes()).hexdigest())
            if not wifi_based:
                standalone += 1
assert digests, 'no canonical QML variants built'
assert standalone >= 5, 'no standalone (no-WiFi) canonical variants built'
legacy = root / 'artifacts/accepted-qml-legacy.md5'
if legacy.is_file():
    for line in legacy.read_text().splitlines():
        line = line.strip()
        if len(line) == 32 and all(c in '0123456789abcdef' for c in line):
            digests.add(line)
Path(sys.argv[1]).write_text('\n'.join(sorted(digests)) + '\n')
