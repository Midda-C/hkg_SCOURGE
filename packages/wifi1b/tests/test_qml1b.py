#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Check first-party overlay identity, foreground/input and radio isolation."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
qml = root / 'artifacts/qml/AppHomeScreen/qml_eco3.0'
page = (qml / 'HomeScreen/Wifi1bPage.qml').read_text()
for needle in ('127.0.0.1:8093', 'sshPort: 2224', 'requested && !atOtherApp',
               'preventStealing: true', 'acceptedButtons: Qt.AllButtons',
               'onCurrentScreenChanged', 'onRequestHomeByPPChanged', 'homeSeq'):
    assert needle in page, needle
assert '127.0.0.1:8090' not in page
assert 'wifi1b_icon_n.png' in (qml / 'component/Wifi1bTile.qml').read_text()
# The AP key must be readable on the landing Status tab, not only one tab deep.
assert page.count('"Key: "') >= 2, "AP key is not shown on the Status tab"
assert 'textBold: true' in page.split('text: "Access point"')[1].split('SSH')[0], \
    "Status-tab key line is not emphasized"
print('PASS: Wifi1b overlay ports, foreground close hooks, input shield and Status-tab AP key')
