#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Gen5W_L EU 260128
"""Gate the exact packed Wifi1b patcher against the exact packed final QML."""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'common'))
from check_layout import check

combo, output = map(Path, sys.argv[1:])
qml = combo / 'tweaks/payload/qml/AppHomeScreen/qml_eco3.0'
patcher = combo / 'wifi1b/payload/bin/wifi1b_qmlpatch'
standalone = ((combo / 'standalone').is_file() or
              ((combo / 'migrate').is_dir() and not (combo / 'wifi').is_dir()))
if standalone:
    variants = [('advanced', 'HomeScreen/QHMI_HomeScreenMain_Advanced.battery_tile.qml'),
                ('wide', 'HomeScreen/QHMI_HomeScreenMain_Wide.stock.qml'),
                ('menu', 'HomeScreen/Screen_HomeScreen_AllMenus.stock.qml'),
                ('menu', 'HomeScreen/Screen_HomeScreen_Submenu_Setup.stock.qml'),
                ('custom', 'component/QCustomizedMenuListAllowEmpty.stock.qml')]
    required = ('battery', 'tweaks', 'reboot', 'wifi1b')
else:
    variants = [('advanced', 'HomeScreen/QHMI_HomeScreenMain_Advanced.battery_tile_wifi.qml'),
                ('wide', 'HomeScreen/QHMI_HomeScreenMain_Wide.wifi.qml'),
                ('menu', 'HomeScreen/Screen_HomeScreen_AllMenus.wifi.qml'),
                ('menu', 'HomeScreen/Screen_HomeScreen_Submenu_Setup.wifi.qml'),
                ('custom', 'component/QCustomizedMenuListAllowEmpty.wifi.qml')]
    required = ('battery', 'wifi', 'tweaks', 'reboot', 'wifi1b')
with tempfile.TemporaryDirectory(prefix='combined-qml-') as d:
    for kind, name in variants:
        target = Path(d) / Path(name).name
        source = (qml / name).read_bytes()
        target.write_bytes(source)
        subprocess.run([str(patcher), kind, str(target)], check=True)
        once = target.read_bytes()
        subprocess.run([str(patcher), kind, str(target)], check=True)
        assert target.read_bytes() == once, f'non-idempotent: {name}'
        assert b'WIFI1B_PAYLOAD_260128' in once
        if kind == 'wide':
            text = source.decode()
            best, pos = None, -1
            for ident in ('itemReboot', 'itemTweaks', 'itemWifi2', 'itemWifi', 'itemHello'):
                p = text.find('id: ' + ident)
                if p >= 0 and p > pos:
                    pos, best = p, ident
            assert best is not None
            assert f'x: {best}.right' in once.decode()
            if best in ('itemReboot', 'itemTweaks'):
                assert b'isWideScr ? 150 : 207' in once
                assert b'x_icon: AppHomeModel.isWideScr ? 38 : 55' in once
            else:
                assert b'isWideScr ? 166 : 207' in once
                assert b'x_icon: AppHomeModel.isWideScr ? 42 : 55' in once
        if kind == 'advanced':
            expected = {'battery': 0, 'tweaks': 1, 'reboot': 2, 'wifi1b': 3}
            if not standalone:
                expected = {'battery': 0, 'wifi': 1, 'tweaks': 2, 'reboot': 3, 'wifi1b': 4}
            assert check(once.decode(), required=required) == expected
            output.write_bytes(once)
print('PASS: packed additive QML, unique tiles, preserved Reboot, idempotency' +
      (' (standalone/no-WiFi)' if standalone else ''))
