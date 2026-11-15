#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Exercise the on-device additive QML patcher against WiFi-only and Tweaks layouts."""
import re
import shutil
import subprocess
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[1]
WIFI = ROOT.parent / "wifi/artifacts/qml/AppHomeScreen/qml_eco3.0"
TWEAKS = ROOT.parent / "tweaks/artifacts/qml/AppHomeScreen/qml_eco3.0"
BIN = ROOT / "src/wifi1b_qmlpatch"


def custom_index(s: str) -> int:
    n = 0
    for ident in ("helloTile", "batteryTile", "wifiTile", "tweaksTile", "rebootTile"):
        if re.search(r"id:\s*" + ident + r"[0-9]*\b", s):
            n += 1
    return n


def run_case(tmp: Path, name: str, adv_src: Path, wide_src: Path, menu_src: Path, custom_src: Path,
             pre_patch: Path | None = None):
    adv = tmp / f"{name}.Advanced.qml"
    wide = tmp / f"{name}.Wide.qml"
    menu = tmp / f"{name}.Menu.qml"
    custom = tmp / f"{name}.Custom.qml"
    for dst, src in ((adv, adv_src), (wide, wide_src), (menu, menu_src), (custom, custom_src)):
        shutil.copyfile(src, dst)
    if pre_patch is not None:
        # Wifi2 was installed first: its patcher already added its tiles.
        for mode, path in (("advanced", adv), ("wide", wide), ("menu", menu), ("custom", custom)):
            subprocess.run([str(pre_patch), mode, str(path)], check=True, timeout=20)
        a2 = adv.read_text()
        assert "WIFI2_PAYLOAD_260128" in a2, "pre-patch missing Wifi2 marker"
    for mode, path in (("advanced", adv), ("wide", wide), ("menu", menu), ("custom", custom)):
        subprocess.run([str(BIN), mode, str(path)], check=True, timeout=20)
    a = adv.read_text()
    w = wide.read_text()
    m = menu.read_text()
    c = custom.read_text()
    assert "WIFI1B_PAYLOAD_260128" in a and "WIFI1B_PAYLOAD_260128" in w
    assert a.count("Wifi1bTile {") == 3, name
    assert "id: wifi1bPage" in a and "Wifi1bPage {" in a
    expected = custom_index(adv_src.read_text())
    if pre_patch is not None:
        expected += 1  # Wifi2's tile is already installed
    assert f"firstEmptySlot({expected})" in a, (name, expected)
    assert w.count("id: itemWifi1b") == 1 and w.count("id: wifi1bPage") == 1
    # The patcher anchors after the textually-last known custom item, exactly
    # like the Wide patcher's best_end comparison; assert that anchor.
    wide_src_text = wide_src.read_text()
    best, pos = None, -1
    for ident in ("itemReboot", "itemTweaks", "itemWifi2", "itemWifi", "itemHello"):
        p = wide_src_text.find("id: " + ident)
        if p >= 0 and p > pos:
            pos, best = p, ident
    if best:
        assert f"x: {best}.right" in w, (name, best)
        if best in ("itemReboot", "itemTweaks"):
            assert "isWideScr ? 150 : 207" in w, name
        elif best == "itemWifi":
            assert "isWideScr ? 166 : 207" in w, name
    else:
        assert "id: itemWifi1b" in w, name
    assert "onWifi1bClicked: wifi1bPage.open()" in m and "id: wifi1bPage" in m
    assert "signal wifi1bClicked()" in c and "Wifi1bTile {" in c and "root.wifi1bClicked()" in c
    skip_expected = custom_index(custom_src.read_text())
    if pre_patch is not None:
        skip_expected += 1
    assert f"firstEmptyIndex(menuPage.pageModel, {skip_expected})" in c, name
    if "TweaksTile" in adv_src.read_text():
        assert a.count("TweaksTile {") == adv_src.read_text().count("TweaksTile {")
    # Idempotent second run.
    before = {p: p.read_bytes() for p in (adv, wide, menu, custom)}
    for mode, path in (("advanced", adv), ("wide", wide), ("menu", menu), ("custom", custom)):
        subprocess.run([str(BIN), mode, str(path)], check=True, timeout=20)
    for path, data in before.items():
        assert path.read_bytes() == data, path


with tempfile.TemporaryDirectory(prefix="wifi1b-patch-", dir="/tmp/opencode") as d:
    tmp = Path(d)
    run_case(tmp, "wifi",
             WIFI / "HomeScreen/QHMI_HomeScreenMain_Advanced.hello_battery_tile_wifi.qml",
             WIFI / "HomeScreen/QHMI_HomeScreenMain_Wide.hello_wifi.qml",
             WIFI / "HomeScreen/Screen_HomeScreen_AllMenus.hello_wifi.qml",
             WIFI / "component/QCustomizedMenuListAllowEmpty.hello_wifi.qml")
    run_case(tmp, "tweaks",
             TWEAKS / "HomeScreen/QHMI_HomeScreenMain_Advanced.battery_tile_wifi.qml",
             TWEAKS / "HomeScreen/QHMI_HomeScreenMain_Wide.wifi.qml",
             TWEAKS / "HomeScreen/Screen_HomeScreen_AllMenus.wifi.qml",
             TWEAKS / "component/QCustomizedMenuListAllowEmpty.wifi.qml")
    # Standalone: no WiFi app installed; Tweaks (Battery/Tweaks/Reboot) is
    # the latest canonical layout.  The patcher must anchor without any
    # WifiTile/wifiClicked hunks present.
    run_case(tmp, "standalone",
             TWEAKS / "HomeScreen/QHMI_HomeScreenMain_Advanced.battery_tile.qml",
             TWEAKS / "HomeScreen/QHMI_HomeScreenMain_Wide.stock.qml",
             TWEAKS / "HomeScreen/Screen_HomeScreen_AllMenus.stock.qml",
             TWEAKS / "component/QCustomizedMenuListAllowEmpty.stock.qml")
    a_standalone = (tmp / "standalone.Advanced.qml").read_text()
    assert "WifiTile {" not in a_standalone and "wifiPage" not in a_standalone
    assert a_standalone.count("Wifi1bTile {") == 3
    print("PASS: canonical additive patcher preserves WiFi/Tweaks/Reboot with unique Advanced, Wide and menu positions")
