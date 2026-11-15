#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Offline migration test: WiFi present -> Wifi1b-only standalone layout.

Builds a fake head unit containing Battery/Tweaks + WiFi + Wifi1b, runs the
real migration script with stub system commands, and verifies removal,
archiving, QML rebasing, account cleanup and failure recovery.  It never
touches the host /etc, /proc, /tmp, /app or /rw_data.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
WIFI1B = ROOT.parent / "wifi1b"
WIFI = ROOT.parent / "wifi"
TWEAKS = ROOT.parent / "tweaks"

QML = "HomeScreen"
ADV = "HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
WIDE = "HomeScreen/QHMI_HomeScreenMain_Wide.qml"
ALL = "HomeScreen/Screen_HomeScreen_AllMenus.qml"
SETUP = "HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
CUSTOM = "component/QCustomizedMenuListAllowEmpty.qml"

BASE = {
    ADV: TWEAKS / "artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.battery_tile.qml",
    WIDE: TWEAKS / "artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Wide.stock.qml",
    ALL: TWEAKS / "artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/Screen_HomeScreen_AllMenus.stock.qml",
    SETUP: TWEAKS / "artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/Screen_HomeScreen_Submenu_Setup.stock.qml",
    CUSTOM: TWEAKS / "artifacts/qml/AppHomeScreen/qml_eco3.0/component/QCustomizedMenuListAllowEmpty.stock.qml",
    "component/common/Q_ThemeImage.qml": TWEAKS / "artifacts/qml/AppHomeScreen/qml_eco3.0/component/common/Q_ThemeImage.qml",
}
WIFI_VARIANT = {
    ADV: WIFI / "artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.battery_tile_wifi.qml",
    WIDE: WIFI / "artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Wide.wifi.qml",
    ALL: WIFI / "artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/Screen_HomeScreen_AllMenus.wifi.qml",
    SETUP: WIFI / "artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/Screen_HomeScreen_Submenu_Setup.wifi.qml",
    CUSTOM: WIFI / "artifacts/qml/AppHomeScreen/qml_eco3.0/component/QCustomizedMenuListAllowEmpty.wifi.qml",
}

STUB_DAEMON = """#!/bin/sh
echo "$0 $*" >> "${MIGRATE_TEST_LOG:-/dev/null}"
case " $* " in
  *" --mode off "*) printf '{"ok":true,"mode":"off","wlan_on":false}\\n' ;;
  *) printf '{"ok":true,"mode":"off","wlan_on":false}\\n' ;;
esac
exit 0
"""

FAKE = r'''#!/usr/bin/env python3
import os, pathlib, sys
root = pathlib.Path(os.environ['FIXTURE'])
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
with (root / 'commands.log').open('a') as f:
    f.write(name + ' ' + repr(args) + '\n')
if name == 'sleep':
    sys.exit(0)
if name == 'systemctl':
    sys.exit(0)
if name in ('mount', 'sync', 'chown', 'setfattr', 'chsmack'):
    sys.exit(0)
if name == 'getfattr':
    print('_')
    sys.exit(0)
if name == 'pidof':
    print('1001')
    sys.exit(0)
if name == 'dbus-send':
    sys.exit(0)
sys.exit(0)
'''


class Fixture:
    def __init__(self, directory: Path, fail_point: str = "", preexisting_service: bool = False):
        self.preexisting_service = preexisting_service
        self.path = directory
        self.root = directory / "root"
        self.usb = directory / "usb"
        self.stick = directory / "stick"
        self.stick.mkdir()
        self.qml = self.root / "app/share/AppHomeScreen/qml_eco3.0"
        self.rw1 = self.root / "rw_data/wifi"
        self.rw2 = self.root / "rw_data/wifi1b"
        self.bin = directory / "bin"
        self.fail_point = fail_point
        for d in (self.qml / "HomeScreen", self.qml / "component/common",
                  self.rw1 / "backup/latest/transaction", self.rw1 / "backup/accounts",
                  self.rw2 / "backup", self.bin,
                  self.root / "etc/systemd/system/carplay_mdnsd.service.d",
                  self.root / "proc", self.root / "tmp", self.root / "home",
                  self.root / "root", self.root / "run", self.root / "app/bin",
                  self.root / "app/share/AppHomeScreen/wifi",
                  self.root / "app/share/AppHomeScreen/hello",
                  self.root / "app/share/AppHomeScreen/battery"):
            d.mkdir(parents=True, exist_ok=True)
        (self.root / "proc/mounts").write_text(f"rootfs {self.root} ext4 rw 0 0\n")
        self.install_fake()
        self.build_state()

    def install_fake(self):
        fake = self.path / "fake.py"
        fake.write_text(FAKE)
        fake.chmod(0o755)
        for name in ("systemctl", "mount", "sync", "chown", "setfattr",
                     "getfattr", "chsmack", "pidof", "dbus-send", "sleep"):
            link = self.bin / name
            link.symlink_to(fake)

    def build_state(self):
        # Pre-WiFi live tree is Battery+Tweaks.  WiFi's own restore point is
        # deliberately older (Battery-only) to match V11's install order; the
        # migration must not use it as the no-WiFi QML base.
        for rel, src in BASE.items():
            shutil.copyfile(src, self.qml / rel)
            dst = self.rw1 / "backup/latest" / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            if rel in WIFI_VARIANT:
                dst.write_bytes(b"// battery-only pre-WiFi fixture\n")
            else:
                shutil.copyfile(src, dst)
        self.base_bytes = {rel: (self.qml / rel).read_bytes() for rel in BASE}

        # WiFi installer's transaction ledger: shared QML and theme existed;
        # WiFi-only files did not.
        entries = []
        index = 0
        for rel in BASE:
            index += 1
            shutil.copyfile(self.rw1 / "backup/latest" / rel,
                            self.rw1 / f"backup/latest/transaction/{index}")
            entries.append(f"{index}|1|{self.qml / rel}")
        (self.root / "etc/passwd").write_text("root:x:0:0:root:/root:/bin/sh\ncar:x:0:0:WiFi:/home/car:/bin/sh\n")
        (self.root / "etc/shadow").write_text("root:*:0:0:99999:7:::\ncar:!:0:0:99999:7:::\n")
        index += 1
        shutil.copyfile(self.root / "etc/passwd", self.rw1 / f"backup/latest/transaction/{index}")
        entries.append(f"{index}|1|{self.root / 'etc/passwd'}")
        index += 1
        shutil.copyfile(self.root / "etc/shadow", self.rw1 / f"backup/latest/transaction/{index}")
        entries.append(f"{index}|1|{self.root / 'etc/shadow'}")
        (self.rw1 / "backup/accounts/passwd").write_text(
            "car:x:0:0:WiFi:/home/car:/bin/sh\n")
        (self.rw1 / "backup/accounts/shadow").write_text("car:!:0:0:99999:7:::\n")

        # Pre-WiFi Hello/Battery icons existed and were refreshed by WiFi.
        for app in ("hello", "battery"):
            before = b"pre-wifi-" + app.encode() + b"-icon"
            (self.root / "app/share/AppHomeScreen" / app / "icon_n.png").write_bytes(before)
            index += 1
            (self.rw1 / f"backup/latest/transaction/{index}").write_bytes(before)
            entries.append(f"{index}|1|{self.root / 'app/share/AppHomeScreen' / app / 'icon_n.png'}")
        self.pre_icons = {app: (self.root / "app/share/AppHomeScreen" / app / "icon_n.png").read_bytes()
                          for app in ("hello", "battery")}

        # WiFi install: variants, components, assets, service, drop-in.
        for rel, src in WIFI_VARIANT.items():
            shutil.copyfile(src, self.qml / rel)
        wifi_payload = WIFI / "out/usb/payload"
        for rel in ("HomeScreen/WifiPage.qml", "component/WifiTile.qml",
                    "component/WifiSwitch.qml", "component/WifiButton.qml",
                    "component/HelloTile.qml", "component/BatteryTile.qml"):
            shutil.copyfile(wifi_payload / "qml/AppHomeScreen/qml_eco3.0" / rel, self.qml / rel)
            index += 1
            entries.append(f"{index}|0|{self.qml / rel}")
        for app in ("wifi", "hello", "battery"):
            src = wifi_payload / "assets" / app / "wifi_icon_n.png"
            dst = None
            if app == "wifi":
                dst = self.root / "app/share/AppHomeScreen/wifi/wifi_icon_n.png"
            else:
                dst = self.root / "app/share/AppHomeScreen" / app / "icon_n.png"
            if app == "wifi":
                shutil.copyfile(src, dst)
                index += 1
                entries.append(f"{index}|0|{dst}")
            else:
                dst.write_bytes(b"wifi-refreshed-" + app.encode())
        service = self.root / "etc/systemd/system/wifi-daemon.service"
        self.service_original = b"[Service]\nExecStart=/bin/owner-wifi-service\n"
        if self.preexisting_service:
            service.write_bytes(self.service_original)
            index += 1
            (self.rw1 / f"backup/latest/transaction/{index}").write_bytes(self.service_original)
            entries.append(f"{index}|1|{service}")
        service.write_text("[Service]\nExecStart=/bin/true\n")
        if not self.preexisting_service:
            index += 1
            entries.append(f"{index}|0|{service}")
        dropin = self.root / "etc/systemd/system/carplay_mdnsd.service.d/50-scourge-restart.conf"
        dropin.write_text("# SCOURGE_MDNSD_RESTART=1\n[Service]\nRestart=always\n")
        index += 1
        entries.append(f"{index}|0|{dropin}")

        # Runtime WiFi state and secrets that must be archived.
        (self.rw1 / "status.json").write_text('{"mode":"off","wlan_on":false}\n')
        (self.rw1 / "config.conf").write_text("mode=off\ncarplay_psk=fixture-carplay-key\n")
        (self.rw1 / "ssh").mkdir(exist_ok=True)
        (self.rw1 / "ssh/authorized_keys").write_text("ssh-ed25519 PRIVATE-WIFI-KEY\n")
        shutil.copyfile(WIFI / "out/usb/payload/bin/wifi_daemon", self.rw1 / "wifi_daemon")
        (self.rw1 / "wifi_daemon").chmod(0o755)
        # Keep the fixture daemon inert.
        (self.rw1 / "wifi_daemon").write_text(STUB_DAEMON)
        (self.rw1 / "wifi_daemon").chmod(0o755)
        index += 1
        entries.append(f"{index}|0|{self.rw1 / 'wifi_daemon'}")
        (self.rw1 / "backup/supplicantmanager.path.state").write_text("enabled\n")
        (self.rw1 / "backup/supplicantmanager.service.state").write_text("disabled\n")

        # Stock mdnsd patch simulation with test-only hash overrides.
        self.mdns_stock = b"stock-mdnsd-bytes\n"
        self.mdns_patched = b"patched-mdnsd-bytes\n"
        (self.rw1 / "backup/mdnsd.stock").write_bytes(self.mdns_stock)
        (self.root / "app/bin/mdnsd").write_bytes(self.mdns_patched)

        # Wifi1b was installed after WiFi: its restore point is the WiFi tree.
        # The live install records latest as a symlink to a timestamped dir.
        w1b_latest = self.rw2 / "backup/20260101T000000Z"
        w1b_latest.mkdir(parents=True, exist_ok=True)
        self.wifi1b_pre = {}
        for rel in BASE:
            data = (self.qml / rel).read_bytes()
            if rel in WIFI_VARIANT:
                data = WIFI_VARIANT[rel].read_bytes()
            self.wifi1b_pre[rel] = data
            dst = w1b_latest / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(data)
        (w1b_latest / "transaction").mkdir()
        (w1b_latest / "transaction/files").write_text(
            f"1|0|{self.rw2 / 'ssh/authorized_keys'}\n")
        (w1b_latest / "transaction/1").write_text("wifi1b-key\n")
        (self.rw2 / "backup/latest").symlink_to(w1b_latest)
        (self.rw2 / "config.conf").write_text("config_version=2\ncarplay_psk=fixture-wifi1b-key\n")
        (self.rw2 / "status.json").write_text('{"mode":"off","wlan_on":false}\n')
        (self.rw2 / "wifi1b_daemon").write_text(STUB_DAEMON)
        (self.rw2 / "wifi1b_daemon").chmod(0o755)

        # Apply Wifi1b additively to the WiFi layout: the current state.
        for mode, rel in (("advanced", ADV), ("wide", WIDE), ("menu", ALL),
                          ("menu", SETUP), ("custom", CUSTOM)):
            subprocess.run([str(WIFI1B / "out/usb/payload/bin/wifi1b_qmlpatch"),
                            mode, str(self.qml / rel)], check=True, timeout=20)
        self.current_adv = (self.qml / ADV).read_bytes()

        (self.rw1 / "backup/latest/transaction/files").write_text(
            "".join(e + "\n" for e in entries))

        shutil.copytree(ROOT / "out/usb", self.usb)
        self.manifest()
        self.script = self.path / "run_stuff.sh"
        text = (ROOT / "scripts/run_stuff_migrate.sh").read_text()
        for prefix, target in [("/root", self.root / "root"), ("/rw_data", self.root / "rw_data"),
                               ("/home", self.root / "home"), ("/app", self.root / "app"),
                               ("/etc", self.root / "etc"), ("/proc", self.root / "proc"),
                               ("/tmp", self.root / "tmp"), ("/run", self.root / "run")]:
            text = re.sub(re.escape(prefix) + r"(?=/|[^A-Za-z0-9_]|$)",
                          lambda _: "@@" + str(target.relative_to(self.path)) + "@@", text)
        text = re.sub(r"@@(.*?)@@", lambda m: str(self.path / m.group(1)), text)
        self.script.write_text(text)
        self.script.chmod(0o755)
        self.env = dict(
            os.environ,
            INSTALL_IO_ROOT=str(self.root),
            INSTALL_IO_MOUNTS=str(self.root / "proc/mounts"),
            FIXTURE=str(self.root),
            MIGRATE_PATH=str(self.bin) + ":/usr/bin:/bin",
            MIGRATE_QML_ROOT=str(self.root / "app/share/AppHomeScreen"),
            MIGRATE_WIFI_RW_DIR=str(self.rw1),
            MIGRATE_WIFI1B_RW_DIR=str(self.rw2),
            MIGRATE_WIFI_SERVICE=str(self.root / "etc/systemd/system/wifi-daemon.service"),
            MIGRATE_MDNS_DROPIN=str(self.root / "etc/systemd/system/carplay_mdnsd.service.d/50-scourge-restart.conf"),
            MIGRATE_MDNSD_BIN=str(self.root / "app/bin/mdnsd"),
            MIGRATE_MDNSD_STOCK_SHA256=hashlib.sha256(self.mdns_stock).hexdigest(),
            MIGRATE_MDNSD_PATCHED_SHA256=hashlib.sha256(self.mdns_patched).hexdigest(),
            USB_DIR=str(self.usb),
            SCOURGE_STICK_DIR=str(self.stick),
            MIGRATE_TEST_LOG=str(self.root / "daemons.log"),
        )
        if self.fail_point:
            self.env["MIGRATE_FAIL_POINT"] = self.fail_point

    def manifest(self):
        lines = []
        for p in sorted((self.usb / "payload").rglob("*")):
            if p.is_file():
                lines.append(f"{hashlib.md5(p.read_bytes()).hexdigest()}  {p.relative_to(self.usb)}\n")
        (self.usb / "manifest.md5").write_text("".join(lines))

    def run(self, expect_fail: bool = False):
        result = subprocess.run(["sh", str(self.script)], env=self.env,
                                capture_output=True, text=True, timeout=120)
        report = (self.usb / "run_stuff_result.txt").read_text()
        failed = result.returncode != 0 or "status=FAIL" in report
        if failed and not expect_fail:
            raise AssertionError((report, result.stdout, result.stderr,
                                  (self.root / "commands.log").read_text(),
                                  (self.rw2 / "migrate.log").read_text()
                                  if (self.rw2 / "migrate.log").exists() else ""))
        if expect_fail and not failed:
            raise AssertionError(("expected failure but migration succeeded", report))
        return report

    def commands(self):
        return (self.root / "commands.log").read_text()


def verify_success(fx: Fixture, report: str):
    assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
    assert "wifi_removed=1" in report and "radios=off" in report, report
    adv = (fx.qml / ADV).read_text()
    assert "WIFI_PAYLOAD_260128" not in adv
    assert "WIFI1B_PAYLOAD_260128" in adv and "TWEAKS_PAYLOAD_260128" in adv
    assert adv.count("Wifi1bTile {") == 3 and "WifiTile {" not in adv
    for rel in ("HomeScreen/WifiPage.qml", "component/WifiTile.qml",
                "component/WifiSwitch.qml", "component/WifiButton.qml"):
        assert not (fx.qml / rel).exists(), rel
    assert not (fx.root / "etc/systemd/system/wifi-daemon.service").exists()
    assert not (fx.root / "etc/systemd/system/carplay_mdnsd.service.d/50-scourge-restart.conf").exists()
    assert not fx.rw1.exists()
    archives = list((fx.stick / "backup").glob("wifi-pre-migration-*.tar.gz"))
    assert len(archives) == 1, archives
    with tarfile.open(archives[0]) as tar:
        names = tar.getnames()
    assert any(n.endswith("wifi/status.json") for n in names), names
    assert any(n.endswith("wifi/ssh/authorized_keys") for n in names), names
    assert (fx.rw2 / "config.conf").read_text().find("fixture-wifi1b-key") >= 0
    marker = fx.rw2 / "migration.done"
    assert marker.is_file() and "complete" in marker.read_text()
    # Rebased restore point is the no-WiFi base, not the WiFi tree.
    latest = (fx.rw2 / "backup/latest").resolve()
    base_adv = (latest / ADV).read_bytes()
    assert base_adv == fx.base_bytes[ADV]
    assert b"WIFI_PAYLOAD_260128" not in base_adv and b"WIFI1B_PAYLOAD_260128" not in base_adv
    assert (latest / "transaction/files").is_file()
    # Battery/Tweaks pieces are byte-identical to pre-WiFi.
    assert (fx.qml / "component/common/Q_ThemeImage.qml").read_bytes() == fx.base_bytes["component/common/Q_ThemeImage.qml"]
    for app in ("hello", "battery"):
        assert (fx.root / "app/share/AppHomeScreen" / app / "icon_n.png").read_bytes() == fx.pre_icons[app]
    # Wifi1b target equals base + patcher.
    for mode, rel in (("advanced", ADV), ("wide", WIDE), ("menu", ALL),
                      ("menu", SETUP), ("custom", CUSTOM)):
        target = fx.qml / rel
        expected = fx.path / ("expected-" + target.name)
        shutil.copyfile(fx.rw2 / "backup/latest" / rel, expected)
        subprocess.run([str(WIFI1B / "out/usb/payload/bin/wifi1b_qmlpatch"),
                        mode, str(expected)], check=True, timeout=20)
        assert target.read_bytes() == expected.read_bytes(), rel
    # Accounts, masks, daemon cleanup.
    passwd = (fx.root / "etc/passwd").read_text()
    assert "root:x:" in passwd and "\ncar:" not in passwd
    cmds = fx.commands()
    assert "unmask" in cmds and "supplicantmanager.path" in cmds, cmds
    assert "try-restart" in cmds and "connman.service" in cmds, cmds
    assert "restart" in cmds and "wifi1b-daemon.service" in cmds, cmds
    daemons = (fx.root / "daemons.log").read_text()
    assert daemons.count("--mode off") == 2, daemons
    assert (fx.root / "app/bin/mdnsd").read_bytes() == fx.mdns_stock
    # Idempotent second run.
    report2 = fx.run()
    assert "already_migrated=1" in report2, report2
    assert (fx.qml / ADV).read_text() == adv
    assert len(list((fx.stick / "backup").glob("wifi-pre-migration-*.tar.gz"))) == 1


def exercise_success():
    with tempfile.TemporaryDirectory(prefix="migrate-ok-", dir="/tmp/opencode") as d:
        fx = Fixture(Path(d))
        verify_success(fx, fx.run())


def exercise_failure():
    with tempfile.TemporaryDirectory(prefix="migrate-fail-", dir="/tmp/opencode") as d:
        fx = Fixture(Path(d), fail_point="radios_verified")
        report = fx.run(expect_fail=True)
        assert "injected failure: radios_verified" in report, report
        assert not (fx.rw2 / "migration.done").exists()
        cmds = fx.commands()
        assert "restart" in cmds and "wifi1b-daemon.service" in cmds, cmds
        assert (fx.qml / ADV).read_bytes() == fx.current_adv


def exercise_preexisting():
    with tempfile.TemporaryDirectory(prefix="migrate-owner-", dir="/tmp/opencode") as d:
        fx = Fixture(Path(d), preexisting_service=True)
        report = fx.run()
        assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
        service = fx.root / "etc/systemd/system/wifi-daemon.service"
        assert service.is_file() and service.read_bytes() == fx.service_original
        log = (fx.rw2 / "migrate.log").read_text()
        assert "kept pre-existing wifi-daemon.service" in log, log
        assert "unmask" in fx.commands()
    with tempfile.TemporaryDirectory(prefix="migrate-mask-", dir="/tmp/opencode") as d:
        fx = Fixture(Path(d))
        (fx.rw1 / "backup/supplicantmanager.path.state").write_text("masked\n")
        (fx.rw1 / "backup/supplicantmanager.service.state").write_text("masked-runtime\n")
        report = fx.run()
        assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
        log = (fx.rw2 / "migrate.log").read_text()
        assert "preserved pre-existing OEM mask: supplicantmanager.path" in log, log
        assert "preserved pre-existing OEM mask: supplicantmanager.service" in log, log
        assert "unmask supplicantmanager" not in fx.commands()


def exercise_mdns_pins():
    locks = (ROOT.parents[1] / "config/firmware_locks.env").read_text()
    installer = (ROOT / "scripts/run_stuff_migrate.sh").read_text()
    for key in ("MDNSD_STOCK_SHA256", "MDNSD_ROOTED_SHA256"):
        pin = re.search(rf'^{key}="([a-f0-9]{{64}})"$', locks, re.M)
        assert pin and pin.group(1) in installer, key
    assert "MDNSD_STOCK_MD5" not in installer
    assert "MDNSD_PATCHED_MD5" not in installer
    with tempfile.TemporaryDirectory(prefix="migrate-mdns-pin-", dir="/tmp/opencode") as d:
        fx = Fixture(Path(d))
        (fx.rw1 / "backup/mdnsd.stock").write_bytes(b"unknown-backup-fixture\n")
        fx.run()
        assert (fx.root / "app/bin/mdnsd").read_bytes() == fx.mdns_patched


exercise_mdns_pins()
exercise_success()
exercise_failure()
exercise_preexisting()
print("PASS: migration removes WiFi, archives it, rebases Wifi1b and recovers on failure")
