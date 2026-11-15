#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Offline side-by-side install/restore test for Wifi1b.

Builds a fake head-unit root, installs the WiFi artifacts first, then runs the
real Wifi1b installer and restore scripts with fake system commands.  It never
touches the host /etc, /proc, /tmp, /app or /rw_data.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
WIFI = ROOT.parent / "wifi"

STUB_DAEMON = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* Fixture stub for the real wifi1b_daemon: never touches the host beyond
 * its arguments.  Fails --import-file (import-failure test), writes a
 * status.json with the build id from the environment, then stays alive so
 * /proc/<pid>/exe checks pass. */
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--import-file")) return 1;   /* import failure */
        if (!strcmp(argv[i], "--mode")) return 0;          /* one-shot mode */
        if (!strcmp(argv[i], "--status")) return 0;
        if (!strcmp(argv[i], "--choose-port")) { printf("{\"ok\":true,\"port\":2224}\n"); return 0; }
    }
    const char *status = NULL;
    for (int i = 1; i < argc - 1; i++)
        if (!strcmp(argv[i], "--status-file")) status = argv[i + 1];
    const char *build = getenv("WIFI1B_STUB_BUILD_ID");
    if (!build) build = "stub";
    if (status) {
        FILE *f = fopen(status, "w");
        if (f) {
            fprintf(f, "{\"ok\":true,\"app\":\"wifi1b\",\"build_id\":\"%s\",\"mode\":\"off\"}\n", build);
            fclose(f);
        }
    }
    for (;;) pause();
    return 0;
}
"""


FAKE = r'''#!/usr/bin/env python3
import json, os, pathlib, sys, hashlib
root = pathlib.Path(os.environ['FIXTURE'])
statefile = root / 'state.json'
state = json.loads(statefile.read_text())
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
failure = os.environ.get('WIFI1B_FAIL_POINT', '')
with (root / 'commands.log').open('a') as f:
    f.write(name + ' ' + repr(args) + '\n')
def save():
    statefile.write_text(json.dumps(state))
if name in ('sleep', 'mount', 'sync', 'chown', 'setfattr', 'iptables', 'ip6tables'):
    sys.exit(0)
if name == 'date':
    print('20260101T000000Z' if any(a.startswith('+') for a in args) else 'fixture time')
    sys.exit(0)
if name == 'getfattr':
    print('_')
    sys.exit(0)
if name == 'chsmack':
    sys.exit(0)
if name == 'cp':
    if failure == 'qml' and args[-1].endswith('/HomeScreen/Wifi1bPage.qml'):
        sys.exit(1)
    if failure == 'icons' and '/wifi1b/wifi1b_icon_' in args[-1]:
        sys.exit(1)
    os.execv('/usr/bin/cp', ['cp', *args])
if name == 'pidof':
    if args and args[0] == 'AppHomeScreen':
        print(state.get('home_pid', 1000))
        sys.exit(0)
    sys.exit(1)
if name == 'dbus-send':
    if any('.terminate' in a for a in args):
        state['home_pid'] = None
    if any('.launch' in a for a in args):
        state['serial'] += 1
        state['home_pid'] = 1000 + state['serial']
    save()
    sys.exit(0)
if name == 'systemctl':
    command = args[0] if args else ''
    unit = args[-1] if args else ''
    if failure == 'systemd' and unit == 'wifi1b-daemon.service' and command in ('enable', 'restart', 'start'):
        sys.exit(1)
    if command == 'is-active':
        if unit == 'wifi1b-daemon.service':
            rw = root / 'rw_data/wifi1b'
            rw.mkdir(parents=True, exist_ok=True)
            build = (pathlib.Path(os.environ['FIXTURE_USB']) / 'payload/build-id.txt').read_text().strip()
            (rw / 'status.json').write_text(json.dumps({'build_id': build, 'app': 'wifi1b', 'mode': 'off'}, separators=(',', ':')))
            proc = root / 'proc/1234'
            proc.mkdir(parents=True, exist_ok=True)
            exe = proc / 'exe'
            if exe.exists() or exe.is_symlink():
                exe.unlink()
            exe.symlink_to(rw / 'wifi1b_daemon')
            state['active'][unit] = True
            save()
            sys.exit(0)
        sys.exit(0 if state['active'].get(unit, False) else 3)
    if command == 'is-enabled':
        value = state['enabled'].get(unit, 'disabled')
        print(value)
        sys.exit(0 if value in ('enabled', 'static') else 1)
    if command == 'show':
        print('MainPID=1234')
        sys.exit(0)
    if command == 'enable':
        state['enabled'][unit] = 'enabled'
    elif command == 'disable':
        state['enabled'][unit] = 'disabled'
    elif command == 'stop':
        state['active'][unit] = False
    elif command in ('start', 'restart'):
        state['active'][unit] = True
        if unit == 'wifi1b-daemon.service':
            rw = root / 'rw_data/wifi1b'
            rw.mkdir(parents=True, exist_ok=True)
            build = (pathlib.Path(os.environ['FIXTURE_USB']) / 'payload/build-id.txt').read_text().strip()
            (rw / 'status.json').write_text(json.dumps({'build_id': build, 'app': 'wifi1b', 'mode': 'off'}, separators=(',', ':')))
            proc = root / 'proc/1234'
            proc.mkdir(parents=True, exist_ok=True)
            exe = proc / 'exe'
            if exe.exists() or exe.is_symlink():
                exe.unlink()
            exe.symlink_to(rw / 'wifi1b_daemon')
    save()
    sys.exit(0)
sys.exit(1)
'''


class Fixture:
    def __init__(self, directory: Path, tweaks: bool = False, standalone: bool = False):
        self.path = directory
        self.root = directory / "root"
        self.usb = directory / "usb"
        self.rw2 = self.root / "rw_data/wifi1b"
        self.rw1 = self.root / "rw_data/wifi"
        self.qml = self.root / "app/share/AppHomeScreen/qml_eco3.0"
        self.bin = directory / "bin"
        self.standalone = standalone
        self.bin.mkdir(parents=True)
        fake = self.bin / "fake"
        fake.write_text(FAKE)
        fake.chmod(0o755)
        for name in ("systemctl", "sleep", "mount", "sync", "chown", "date", "getfattr",
                     "setfattr", "chsmack", "cp", "pidof", "dbus-send", "iptables", "ip6tables"):
            (self.bin / name).symlink_to(fake)
        for directory in ("etc/systemd/system", "tmp", "proc", "run/wifi1b", "home",
                          "rw_data/wifi1b", "rw_data/wifi", "root/cache", "app/share/AppHomeScreen"):
            (self.root / directory).mkdir(parents=True, exist_ok=True)
        (self.root / "proc/mounts").write_text(f'device {self.root} ext4 rw 0 0\n')
        (self.root / "tmp/shells").write_text("")
        (self.root / "etc/passwd").write_text("root:x:0:0:root:/root:/bin/sh\ncar:x:0:0:Car User:/rw_data/wifi/home:/bin/sh\n")
        for name in ("passwd.legacy", "passwd.secure"):
            (self.root / "etc" / name).write_text("root:x:0:0:root:/root:/bin/sh\ncar:x:0:0:Car User:/rw_data/wifi/home:/bin/sh\n")
        (self.root / "etc/shadow.Global").write_text("root:*:0:0:99999:7:::\ncar:!:0:0:99999:7:::\n")
        (self.root / "etc/shadow").write_text("root:*:0:0:99999:7:::\ncar:!:0:0:99999:7:::\n")

        # Pretend the known WiFi build is installed.  A standalone fixture
        # instead uses the Tweaks-only canonical layout (no WiFi app).
        if standalone:
            qsrc = WIFI.parent / "tweaks/artifacts/qml/AppHomeScreen/qml_eco3.0"
            pairs = {
                "HomeScreen/QHMI_HomeScreenMain_Advanced.qml": "HomeScreen/QHMI_HomeScreenMain_Advanced.battery_tile.qml",
                "HomeScreen/QHMI_HomeScreenMain_Wide.qml": "HomeScreen/QHMI_HomeScreenMain_Wide.stock.qml",
                "HomeScreen/Screen_HomeScreen_AllMenus.qml": "HomeScreen/Screen_HomeScreen_AllMenus.stock.qml",
                "HomeScreen/Screen_HomeScreen_Submenu_Setup.qml": "HomeScreen/Screen_HomeScreen_Submenu_Setup.stock.qml",
                "component/QCustomizedMenuListAllowEmpty.qml": "component/QCustomizedMenuListAllowEmpty.stock.qml",
                "HomeScreen/TweaksPage.qml": "HomeScreen/TweaksPage.qml",
                "HomeScreen/RebootPage.qml": "HomeScreen/RebootPage.qml",
                "component/TweaksTile.qml": "component/TweaksTile.qml",
                "component/RebootTile.qml": "component/RebootTile.qml",
            }
        elif tweaks:
            qsrc = WIFI.parent / "tweaks/artifacts/qml/AppHomeScreen/qml_eco3.0"
            pairs = {
                "HomeScreen/QHMI_HomeScreenMain_Advanced.qml": "HomeScreen/QHMI_HomeScreenMain_Advanced.battery_tile_wifi.qml",
                "HomeScreen/QHMI_HomeScreenMain_Wide.qml": "HomeScreen/QHMI_HomeScreenMain_Wide.wifi.qml",
                "HomeScreen/Screen_HomeScreen_AllMenus.qml": "HomeScreen/Screen_HomeScreen_AllMenus.wifi.qml",
                "HomeScreen/Screen_HomeScreen_Submenu_Setup.qml": "HomeScreen/Screen_HomeScreen_Submenu_Setup.wifi.qml",
                "component/QCustomizedMenuListAllowEmpty.qml": "component/QCustomizedMenuListAllowEmpty.wifi.qml",
                "HomeScreen/TweaksPage.qml": "HomeScreen/TweaksPage.qml",
                "component/TweaksTile.qml": "component/TweaksTile.qml",
            }
        else:
            qsrc = WIFI / "artifacts/qml/AppHomeScreen/qml_eco3.0"
            pairs = {
                "HomeScreen/QHMI_HomeScreenMain_Advanced.qml": "HomeScreen/QHMI_HomeScreenMain_Advanced.hello_battery_tile_wifi.qml",
                "HomeScreen/QHMI_HomeScreenMain_Wide.qml": "HomeScreen/QHMI_HomeScreenMain_Wide.hello_wifi.qml",
                "HomeScreen/Screen_HomeScreen_AllMenus.qml": "HomeScreen/Screen_HomeScreen_AllMenus.hello_wifi.qml",
                "HomeScreen/Screen_HomeScreen_Submenu_Setup.qml": "HomeScreen/Screen_HomeScreen_Submenu_Setup.hello_wifi.qml",
                "component/QCustomizedMenuListAllowEmpty.qml": "component/QCustomizedMenuListAllowEmpty.hello_wifi.qml",
                "HomeScreen/WifiPage.qml": "HomeScreen/WifiPage.qml",
                "component/WifiTile.qml": "component/WifiTile.qml",
                "component/WifiSwitch.qml": "component/WifiSwitch.qml",
                "component/WifiButton.qml": "component/WifiButton.qml",
            }
        for dst, src in pairs.items():
            target = self.qml / dst
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(qsrc / src, target)
        if tweaks:
            wq = WIFI / "artifacts/qml/AppHomeScreen/qml_eco3.0"
            for dst, src in {
                "HomeScreen/WifiPage.qml": "HomeScreen/WifiPage.qml",
                "component/WifiTile.qml": "component/WifiTile.qml",
                "component/WifiSwitch.qml": "component/WifiSwitch.qml",
                "component/WifiButton.qml": "component/WifiButton.qml",
            }.items():
                shutil.copyfile(wq / src, self.qml / dst)
        self.wifi_originals = {p: p.read_bytes() for p in self.qml.rglob("*.qml")}
        if not standalone:
            (self.rw1 / "config.conf").write_text("config_version=2\nmode=off\nwlan_on=0\nssh=0\nssh_port=2222\n")
            (self.rw1 / "status.json").write_text('{"build_id":"wifi","mode":"off","wlan_on":false}\n')
            (self.root / "etc/systemd/system/wifi-daemon.service").write_text("[Service]\nExecStart=/bin/true\n")
            (self.rw1 / "wifi_daemon").write_text("#!/bin/sh\nexit 0\n")
            (self.rw1 / "wifi_daemon").chmod(0o755)

        shutil.copytree(ROOT / "out/usb", self.usb)
        self.manifest()
        self.install = self.path / "run_stuff.sh"
        self.restore = self.path / "run_stuff_restore.sh"
        for src, dst in ((ROOT / "scripts/run_stuff1b.sh", self.install),
                         (ROOT / "scripts/run_stuff_restore1b.sh", self.restore)):
            text = src.read_text()
            # Rewrite absolute host paths into the fixture.  Use placeholders
            # so the fixture path itself is not rewritten repeatedly.
            for prefix, target in [("/root", self.root / "root"), ("/rw_data", self.root / "rw_data"),
                                   ("/home", self.root / "home"), ("/app", self.root / "app"),
                                   ("/etc", self.root / "etc"), ("/proc", self.root / "proc"),
                                   ("/tmp", self.root / "tmp"), ("/run", self.root / "run")]:
                text = re.sub(re.escape(prefix) + r"(?=/|[^A-Za-z0-9_]|$)",
                              lambda _: "@@" + str(target.relative_to(self.path)) + "@@", text)
            text = re.sub(r"@@(.*?)@@", lambda m: str(self.path / m.group(1)), text)
            dst.write_text(text)
            dst.chmod(0o755)
        self.env = dict(os.environ,
                        INSTALL_IO_ROOT=str(self.root), INSTALL_IO_MOUNTS=str(self.root / 'proc/mounts'),
                        FIXTURE=str(self.root), FIXTURE_USB=str(self.usb),
                        WIFI1B_PATH=str(self.bin) + ":/usr/bin:/bin",
                        USB_DIR=str(self.usb),
                        WIFI1B_RW_DIR=str(self.rw2),
                        WIFI1B_QML_ROOT=str(self.qml.parent),
                        CLEANUP_PROC_ROOT=str(self.root / "proc"),
                        WIFI1B_SERVICE=str(self.root / "etc/systemd/system/wifi1b-daemon.service"))
        state = {"active": {}, "enabled": {"wifi-daemon.service": "enabled"},
                 "home_pid": 1000, "serial": 1000}
        (self.root / "state.json").write_text(json.dumps(state))
        self.stub_daemon()

    def run(self, script: Path, fail_point: str = "", expect_fail: bool = False):
        env = dict(self.env)
        if fail_point:
            env["WIFI1B_FAIL_POINT"] = fail_point
        result = subprocess.run(["sh", str(script)], env=env, capture_output=True,
                                text=True, timeout=90)
        report = (self.usb / "run_stuff_result.txt").read_text()
        failed = result.returncode != 0 or "status=FAIL" in report
        if failed and not expect_fail:
            status = (self.rw2 / "status.json").read_text() if (self.rw2 / "status.json").exists() else "<no status>"
            build = (self.usb / "payload/build-id.txt").read_text() if (self.usb / "payload/build-id.txt").exists() else "<no build>"
            raise AssertionError((report, result.stdout, result.stderr, status, build,
                                  (self.root / 'commands.log').read_text(),
                                  [str(x) for x in self.root.rglob('status.json')],
                                  (self.rw2 / "install.log").read_text() if (self.rw2 / "install.log").exists() else ""))
        if expect_fail and not failed:
            raise AssertionError(("expected failure but install succeeded", report))
        return report

    def kill_manual_daemon(self):
        """Stop fixture-owned stub daemons by exact executable path.

        Deliberately no host-wide pkill: only PIDs whose /proc/<pid>/exe
        resolves to this fixture's private daemon copy are signalled, and
        only their own process group is terminated.
        """
        wanted = str((self.rw2 / "wifi1b_daemon").resolve())
        own_group = os.getpgrp()
        for proc in Path("/proc").iterdir():
            if not proc.name.isdigit():
                continue
            try:
                exe = os.readlink(proc / "exe")
            except OSError:
                continue
            if exe not in (wanted, wanted + " (deleted)"):
                continue
            pid = int(proc.name)
            if pid == os.getpid():
                continue
            try:
                pgid = os.getpgid(pid)
            except OSError:
                pgid = pid
            try:
                if pgid != own_group and pgid == pid:
                    os.killpg(pgid, 15)
                else:
                    os.kill(pid, 15)
            except OSError:
                pass
            deadline = time.time() + 5
            while time.time() < deadline:
                if not proc.exists():
                    break
                try:
                    os.kill(pid, 0)
                except OSError:
                    break
                time.sleep(0.05)
            else:
                try:
                    os.kill(pid, 9)
                except OSError:
                    pass

    def manifest(self):
        lines = []
        for p in sorted((self.usb / "payload").rglob("*")):
            if p.is_file():
                lines.append(f"{hashlib.md5(p.read_bytes()).hexdigest()}  {p.relative_to(self.usb)}\n")
        (self.usb / "manifest.md5").write_text("".join(lines))

    def stub_daemon(self):
        """Replace the payload daemon with a compiled stub so no real daemon
        ever runs on the host.  The stub is the file the installer copies,
        starts and md5-verifies, so every check path stays honest."""
        stub_src = self.path / "wifi1b_stub.c"
        stub_src.write_text(STUB_DAEMON)
        stub_bin = self.path / "wifi1b_stub"
        subprocess.run(["gcc", "-O2", "-static", "-o", str(stub_bin), str(stub_src)],
                      check=True, timeout=60)
        target = self.usb / "payload/bin/wifi1b_daemon"
        shutil.copyfile(stub_bin, target)
        target.chmod(0o755)
        build = (self.usb / "payload/build-id.txt").read_text().strip()
        self.env["WIFI1B_STUB_BUILD_ID"] = build
        self.manifest()


def exercise(tweaks: bool) -> None:
    with tempfile.TemporaryDirectory(prefix="wifi1b-side-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=tweaks)
        report = fixture.run(fixture.install)
        assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
        adv = (fixture.qml / "HomeScreen/QHMI_HomeScreenMain_Advanced.qml").read_text()
        assert "WIFI_PAYLOAD_260128" in adv and "WIFI1B_PAYLOAD_260128" in adv
        assert 'id: wifiPage' in adv and 'id: wifi1bPage' in adv
        assert adv.count("WifiTile {") == 3 and adv.count("Wifi1bTile {") == 3
        if tweaks:
            assert "TWEAKS_PAYLOAD_260128" in adv
            assert adv.count("TweaksTile {") == 3
        expected = 1 + sum(f'id: {name}Tile1' in adv for name in ('hello','battery','tweaks','reboot'))
        assert f"firstEmptySlot({expected})" in adv
        for name in ("HomeScreen/WifiPage.qml", "component/WifiTile.qml",
                     "component/WifiSwitch.qml", "component/WifiButton.qml"):
            assert (fixture.qml / name).read_bytes() == fixture.wifi_originals[fixture.qml / name], name
        if tweaks:
            for name in ("HomeScreen/TweaksPage.qml", "component/TweaksTile.qml"):
                assert (fixture.qml / name).read_bytes() == fixture.wifi_originals[fixture.qml / name], name
        assert (fixture.rw1 / "config.conf").read_text().startswith("config_version=2")
        assert (fixture.rw1 / "status.json").read_text().find('"mode":"off"') >= 0
        assert (fixture.root / "etc/systemd/system/wifi-daemon.service").read_text().find("/bin/true") >= 0
        cfg = (fixture.rw2 / "config.conf").read_text()
        assert "mode=off" in cfg and "wlan_on=0" in cfg and "ssh=0" in cfg and "ssh_port=2224" in cfg
        assert "carplay_ssid=IONIQ5-CarPlay-B" in cfg
        restore_point = (fixture.rw2 / 'backup/latest').resolve()
        fixture.run(fixture.install)
        assert (fixture.rw2 / 'backup/latest').resolve() == restore_point
        assert (fixture.rw2 / 'config.conf').read_text() == cfg
        passwd = (fixture.root / "etc/passwd").read_text()
        assert "wifi1b:" in passwd and "car:" in passwd
        for name in ("HomeScreen/Wifi1bPage.qml", "component/Wifi1bTile.qml",
                     "component/Wifi1bSwitch.qml", "component/Wifi1bButton.qml"):
            assert (fixture.qml / name).exists(), name
        assert (fixture.root / "etc/systemd/system/wifi1b-daemon.service").exists()
        dropin = fixture.root / 'etc/systemd/system/carplay_mdnsd.service.d/60-wifi1b-foreground.conf'
        assert dropin.read_bytes() == (fixture.usb / 'payload/systemd/60-wifi1b-foreground.conf').read_bytes()

        report = fixture.run(fixture.restore)
        assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
        for path, original in fixture.wifi_originals.items():
            assert path.read_bytes() == original, path
        assert "WIFI1B_PAYLOAD_260128" not in (fixture.qml / "HomeScreen/QHMI_HomeScreenMain_Advanced.qml").read_text()
        for name in ("HomeScreen/Wifi1bPage.qml", "component/Wifi1bTile.qml",
                     "component/Wifi1bSwitch.qml", "component/Wifi1bButton.qml"):
            assert not (fixture.qml / name).exists(), name
        assert "wifi1b:" not in (fixture.root / "etc/passwd").read_text()
        assert "car:" in (fixture.root / "etc/passwd").read_text()
        assert (fixture.rw1 / "config.conf").exists()
        assert (fixture.rw1 / "status.json").exists()
        assert (fixture.root / "etc/systemd/system/wifi-daemon.service").exists()
        assert not (fixture.root / "etc/systemd/system/wifi1b-daemon.service").exists()
        assert not (fixture.rw2 / "bin/wifi1b_qmlpatch").exists()
        assert not dropin.exists()

def exercise_hardening() -> None:
    """The two on-car install failures must be non-fatal in Wifi1b."""
    with tempfile.TemporaryDirectory(prefix="wifi1b-hard-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        # A wifi.txt at the stick root makes the installer run the daemon's
        # --import-file; the stub fails it.  The install must still succeed:
        # on 2026-11-10 this exact failure aborted the whole WiFi install.
        (fixture.usb / "wifi.txt").write_text("ssid=FixtureNet\npsk=fixture-password\n")
        report = fixture.run(fixture.install)
        assert "wifi1b_import=FAIL" in report, report
        assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
        assert (fixture.qml / "HomeScreen/Wifi1bPage.qml").exists()
        assert (fixture.root / "etc/systemd/system/wifi1b-daemon.service").exists()

    with tempfile.TemporaryDirectory(prefix="wifi1b-sysfail-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        try:
            report = fixture.run(fixture.install, fail_point="systemd")
            # systemd enable/restart failed; the manual start must have taken over.
            assert "daemon=manual-ok" in report, report
            assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
            adv = (fixture.qml / "HomeScreen/QHMI_HomeScreenMain_Advanced.qml").read_text()
            assert "WIFI1B_PAYLOAD_260128" in adv and "id: wifi1bPage" in adv
            status = (fixture.rw2 / "status.json").read_text()
            assert "\"app\":\"wifi1b\"" in status
        finally:
            fixture.kill_manual_daemon()


exercise(False)
exercise(True)
exercise_hardening()
for failure in ('qml', 'icons', 'unknown'):
    with tempfile.TemporaryDirectory(prefix='wifi1b-failure-', dir='/tmp/opencode') as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        advanced = fixture.qml / 'HomeScreen/QHMI_HomeScreenMain_Advanced.qml'
        inode = advanced.stat().st_ino
        if failure == 'unknown':
            advanced.write_bytes(advanced.read_bytes() + b'\n// unknown owner change\n')
            before = advanced.read_bytes()
            fixture.run(fixture.install, expect_fail=True)
            assert advanced.read_bytes() == before
        elif failure == 'qml':
            fixture.run(fixture.install, fail_point='qml', expect_fail=True)
            for path, original in fixture.wifi_originals.items():
                assert path.read_bytes() == original, path
            assert not (fixture.root / 'etc/systemd/system/carplay_mdnsd.service.d/60-wifi1b-foreground.conf').exists()
        else:
            fixture.run(fixture.install, fail_point='icons')
            assert 'WIFI1B_PAYLOAD_260128' in advanced.read_text()
        assert advanced.stat().st_ino == inode
print('PASS: Wifi1b reinstall retains restore point/settings; unknown QML fails closed; core-copy rollback and optional icons preserve live inodes')
print("PASS: Wifi1b installs beside WiFi/Tweaks, survives import and systemd failures, and restores cleanly")


def exercise_standalone() -> None:
    """Wifi1b must install and restore with no WiFi app present at all."""
    with tempfile.TemporaryDirectory(prefix="wifi1b-standalone-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), standalone=True)
        # Owner key material must survive install and restore even though the
        # payload ships no key at the stick root.
        (fixture.rw2 / "ssh").mkdir(parents=True, exist_ok=True)
        (fixture.rw2 / "ssh/authorized_keys").write_text("owner-key\n")
        report = fixture.run(fixture.install)
        assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
        assert (fixture.rw2 / "ssh/authorized_keys").read_text() == "owner-key\n"
        assert "layout=standalone" in report, report
        adv = (fixture.qml / "HomeScreen/QHMI_HomeScreenMain_Advanced.qml").read_text()
        assert "WIFI1B_PAYLOAD_260128" in adv and "TWEAKS_PAYLOAD_260128" in adv
        assert "WIFI_PAYLOAD_260128" not in adv
        assert "id: wifi1bPage" in adv and adv.count("Wifi1bTile {") == 3
        assert "WifiTile {" not in adv
        for name in ("HomeScreen/Wifi1bPage.qml", "component/Wifi1bTile.qml"):
            assert (fixture.qml / name).exists(), name
        assert (fixture.root / "etc/systemd/system/wifi1b-daemon.service").exists()
        # Idempotent reinstall keeps the restore point and settings.
        cfg = (fixture.rw2 / "config.conf").read_text()
        restore_point = (fixture.rw2 / "backup/latest").resolve()
        fixture.run(fixture.install)
        assert (fixture.rw2 / "backup/latest").resolve() == restore_point
        assert (fixture.rw2 / "config.conf").read_text() == cfg
        report = fixture.run(fixture.restore)
        assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
        for path, original in fixture.wifi_originals.items():
            assert path.read_bytes() == original, path
        assert "WIFI1B_PAYLOAD_260128" not in (fixture.qml / "HomeScreen/QHMI_HomeScreenMain_Advanced.qml").read_text()
        for name in ("HomeScreen/Wifi1bPage.qml", "component/Wifi1bTile.qml"):
            assert not (fixture.qml / name).exists(), name
        assert not (fixture.root / "etc/systemd/system/wifi1b-daemon.service").exists()
        assert (fixture.rw2 / "ssh/authorized_keys").read_text() == "owner-key\n"


exercise_standalone()
print("PASS: Wifi1b installs and restores standalone (no WiFi app), anchored on Tweaks/Battery/Reboot")


def exercise_service_restore() -> None:
    """A pre-existing service file and enablement are restored from the
    install transaction; a foreign-modified service is never removed."""
    original = b"# owner service\n[Service]\nExecStart=/bin/true\n"
    for enabled in ("enabled", "disabled"):
        with tempfile.TemporaryDirectory(prefix="wifi1b-svc-", dir="/tmp/opencode") as directory:
            fixture = Fixture(Path(directory), tweaks=True)
            service = fixture.root / "etc/systemd/system/wifi1b-daemon.service"
            service.write_bytes(original)
            statefile = fixture.root / "state.json"
            state = json.loads(statefile.read_text())
            state["enabled"]["wifi1b-daemon.service"] = enabled
            state["active"]["wifi1b-daemon.service"] = False
            statefile.write_text(json.dumps(state))
            report = fixture.run(fixture.install)
            assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
            assert service.read_bytes() != original, "install should have staged its own unit"
            report = fixture.run(fixture.restore)
            assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
            assert service.read_bytes() == original, "pre-existing service bytes not restored"
            state = json.loads(statefile.read_text())
            assert state["enabled"]["wifi1b-daemon.service"] == enabled, state
    with tempfile.TemporaryDirectory(prefix="wifi1b-svc-foreign-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        report = fixture.run(fixture.install)
        assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
        service = fixture.root / "etc/systemd/system/wifi1b-daemon.service"
        foreign = b"# owner changed this after install\n[Service]\nExecStart=/bin/true\n"
        service.write_bytes(foreign)
        statefile = fixture.root / "state.json"
        state = json.loads(statefile.read_text())
        state["enabled"]["wifi1b-daemon.service"] = "enabled"
        statefile.write_text(json.dumps(state))
        report = fixture.run(fixture.restore)
        assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
        assert service.read_bytes() == foreign, "foreign-modified service was overwritten/removed"
        state = json.loads(statefile.read_text())
        assert state["enabled"]["wifi1b-daemon.service"] == "enabled", (
            state,
            (fixture.rw2 / "backup/latest/transaction/service.state").read_text(),
            (fixture.rw2 / "restore.log").read_text())


exercise_service_restore()
print("PASS: pre-existing service bytes and enabled/disabled state restored; foreign-modified service preserved")

for prior in ('identical', 'unknown'):
    with tempfile.TemporaryDirectory(prefix='wifi1b-mdns-', dir='/tmp/opencode') as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        dropin = fixture.root / 'etc/systemd/system/carplay_mdnsd.service.d/60-wifi1b-foreground.conf'
        dropin.parent.mkdir(parents=True)
        original = ((fixture.usb / 'payload/systemd/60-wifi1b-foreground.conf').read_bytes()
                    if prior == 'identical' else b'# owner override\n[Service]\nRestart=no\n')
        dropin.write_bytes(original)
        fixture.run(fixture.install, expect_fail=prior == 'unknown')
        if prior == 'identical':
            fixture.run(fixture.restore)
        assert dropin.read_bytes() == original
print('PASS: mDNS override install/rollback/removal and pre-existing override preservation')
