#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Compile the daemon against a temp APP_DIR and verify import/status isolation."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="wifi1b-daemon-", dir="/tmp/opencode") as d:
    tmp = Path(d)
    app = tmp / "app"
    app.mkdir()
    binary = tmp / "wifi1b_daemon_test"
    cmd = ["gcc", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter",
           "-Wno-format-truncation", "-Wno-unused-function", "-Wno-unused-variable",
           "-Wno-unused-but-set-variable", "-DWIFI_TEST",
           f'-DAPP_DIR="{app}"', f'-DRADIO_OWNER_PATH="{tmp / "radio"}"', '-DWIFI_BUILD_ID="test"', "-static",
           "-o", str(binary), str(ROOT / "src/wifi1b_daemon.c")]
    subprocess.run(cmd, check=True, timeout=90)
    profile = tmp / "iphone_wifi.txt"
    profile.write_text("ssid=iPhone\npsk=Sup3rSecretPass\nssid2=HomeFixture\npsk2=BackupFixtureSecret\nssh=1\nssh_port=2222\n")
    out = subprocess.run([str(binary), "--import-file", str(profile)],
                         capture_output=True, text=True, check=True, timeout=20).stdout
    assert json.loads(out)["ok"] is True
    cfg = (app / "config.conf").read_text()
    assert "mode=off" in cfg and "wlan_on=0" in cfg
    assert "ssh=0" in cfg and "ssh_port=2224" in cfg
    assert "ssid=iPhone" in cfg
    assert "ssid2=HomeFixture" in cfg and "psk2=BackupFixtureSecret" in cfg
    assert "Sup3rSecretPass" not in subprocess.run([str(binary), "--status"],
                                                   capture_output=True, text=True, timeout=20).stdout
    status = json.loads(subprocess.run([str(binary), "--status"], capture_output=True,
                                       text=True, check=True, timeout=20).stdout)
    assert status["app"] == "wifi1b"
    assert 'BackupFixtureSecret' not in json.dumps(status)
    assert status["backend"] == "static_supplicant"
    assert status["mode"] == "off" and status["wlan_on"] is False
    assert status["ssh_enabled"] is False and status["ssh_port"] == 2224
    assert status["carplay_ssid"] == "IONIQ5-CarPlay-B"
    assert status["carplay_control"] == "wifi1b"
    assert status["ssh_fw_open"] is False
    assert isinstance(status["disk_free_kb"], int) and status["disk_free_kb"] >= 0
    assert status["disk_low"] is False
    assert "carplay_control=wifi1b" in cfg
    # Config round-trip for the stock USB handoff (no side effects in --status).
    cfg_path = app / "config.conf"
    cfg_path.write_text(cfg.replace("carplay_control=wifi1b", "carplay_control=stock"))
    stock = json.loads(subprocess.run([str(binary), "--status"], capture_output=True,
                                      text=True, check=True, timeout=20).stdout)
    assert stock["carplay_control"] == "stock"
    cfg_path.write_text(cfg)
    port = subprocess.run([str(binary), "--choose-port", "2224"], capture_output=True,
                          text=True, check=True, timeout=20).stdout
    assert json.loads(port)["port"] == 2224
    mode = subprocess.run([str(binary), "--mode", "off"], capture_output=True,
                          text=True, check=True, timeout=30).stdout
    assert json.loads(mode)["mode"] == "off"

src = (ROOT / "src/wifi1b_daemon.c").read_text() + (ROOT / "src/wifi1b_backend.inc").read_text()
for needle in ("ap_scan=2", "iw reg set GB", "carplay_id=%s", "app_info=1",
               "-p %d", "DHCP", "10.42.0.1",
               "carplay_control", "apply_carplay_control", "Stock USB",
               # The CarPlay chain must be hooked into OUTPUT as well as INPUT:
               # the vendor HARDEN_LOUT chain drops multicast unaccepted first.
               "OUTPUT -o wlan0 -j %s", "-D OUTPUT -o wlan0 -j %s"):
    assert needle in src, needle
connman_dropin = (ROOT / "artifacts/systemd/60-connman-nodevice.conf").read_text()
assert "rmnet_usb0" in connman_dropin and "ExecStart=" in connman_dropin
assert "60-connman-nodevice.conf" in (ROOT / "scripts/run_stuff1b.sh").read_text()
assert "60-connman-nodevice.conf" in (ROOT / "scripts/run_stuff_restore1b.sh").read_text()
restore = (ROOT / "scripts/run_stuff_restore1b.sh").read_text()
assert "-D OUTPUT -o wlan0" in restore, "restore leaves the OUTPUT hook behind"
assert "connmanctl" not in (ROOT / "scripts/run_stuff1b.sh").read_text()
assert "connmand" not in (ROOT / "artifacts/systemd/wifi1b-daemon.service").read_text()
print("PASS: Wifi1b daemon import is non-activating, status is isolated, ports/paths are distinct")
