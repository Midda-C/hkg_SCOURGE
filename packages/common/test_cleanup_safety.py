#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Fixture-only safety tests for packages/common/runtime_cleanup.sh.

Every path lives under /tmp/opencode.  Nothing here touches the host /etc,
/proc, /rw_data or /app, and no real process is signalled.
"""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tarfile
import tempfile

PACKAGES = Path(__file__).resolve().parents[1]
HELPER = PACKAGES / "common/runtime_cleanup.sh"
QML_FILES = (
    "HomeScreen/QHMI_HomeScreenMain_Advanced.qml",
    "HomeScreen/QHMI_HomeScreenMain_Wide.qml",
    "HomeScreen/Screen_HomeScreen_AllMenus.qml",
    "HomeScreen/Screen_HomeScreen_Submenu_Setup.qml",
    "component/QCustomizedMenuListAllowEmpty.qml",
)


class Fixture:
    def __init__(self, base: Path):
        self.base = base
        self.root = base / "root"
        self.runtime = self.root / "rw_data"
        self.qml = self.root / "app/share/AppHomeScreen/qml_eco3.0"
        self.proc = self.root / "proc"
        self.service = self.root / "etc/systemd/system/wifi-daemon.service"
        self.stick = base / "stick"
        self.wifi = self.runtime / "wifi"
        self.victim = base / "victim"
        self.victim.mkdir(parents=True)
        (self.victim / "keep.txt").write_text("owner data\n")
        (self.victim / "daemon.log").write_text("owner log\n")
        for name in QML_FILES:
            target = self.qml / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(f"// fixture {name}\n")
        self.proc.mkdir(parents=True)
        self.service.parent.mkdir(parents=True, exist_ok=True)
        self.wifi.mkdir(parents=True)
        (self.wifi / "config.conf").write_text("owner-credential\n")
        (self.wifi / "backup").mkdir()

    def shell(self, body: str, env_extra=None) -> subprocess.CompletedProcess:
        env = dict(os.environ,
                   CLEANUP_PROC_ROOT=str(self.proc),
                   RUNTIME_CLEANUP_LOG="",
                   HELPER=str(HELPER),
                   RUNTIME=str(self.runtime),
                   QML=str(self.qml),
                   STICK=str(self.stick),
                   SERVICE=str(self.service))
        env.update(env_extra or {})
        script = (
            '. "$HELPER"\n'
            'RUNTIME_CLEANUP_NOTES=""\n'
            f'{body}\n'
            'printf "NOTES=%s\\n" "$RUNTIME_CLEANUP_NOTES"\n'
        )
        return subprocess.run(["sh", "-c", script], env=env,
                              capture_output=True, text=True, timeout=90)


def make_archive(fixture: Fixture, name="wifi-pre-migration-20260101T000000Z.tar.gz",
                 corrupt=False) -> Path:
    directory = fixture.stick / "backup"
    directory.mkdir(parents=True)
    archive = directory / name
    if corrupt:
        archive.write_bytes(b"not a gzip tar archive\n")
        return archive
    source = fixture.base / "archive-source/wifi"
    (source / "backup").mkdir(parents=True)
    (source / "config.conf").write_text("owner-credential\n")
    (source / "backup/keys").write_text("private-key\n")
    with tarfile.open(archive, "w:gz") as output:
        output.add(source, arcname="wifi")
    return archive


def marker_absent(fixture: Fixture) -> None:
    for name in QML_FILES:
        (fixture.qml / name).write_text(f"// no WiFi marker {name}\n")


def remove_call(fixture: Fixture, expected: str = "", archive: str = "") -> str:
    return (f'cleanup_removed_wifi "$RUNTIME" "$QML" "$STICK" '
            f'{shlex.quote(archive)} {shlex.quote(expected)} "$SERVICE"; '
            'printf "RC=%s\\n" "$?"')


def test_bounded_logs() -> None:
    with tempfile.TemporaryDirectory(prefix="cleanup-logs-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        wifi1b = fixture.runtime / "wifi1b"
        wifi1b.mkdir()
        (wifi1b / "daemon.log").write_text("wifi1b log\n")
        (fixture.wifi / "daemon.log").symlink_to(fixture.victim / "daemon.log")
        (fixture.runtime / "battery").symlink_to(fixture.victim)
        (fixture.wifi / "owner.log").write_text("keep me\n")
        result = fixture.shell('cleanup_bounded_logs "$RUNTIME" || true')
        assert result.returncode == 0, result
        assert (wifi1b / "daemon.log").read_text() == "", "removed WiFi app log not truncated"
        assert (fixture.victim / "daemon.log").read_text() == "owner log\n"
        assert (fixture.wifi / "daemon.log").is_symlink()
        assert (fixture.wifi / "owner.log").read_text() == "keep me\n"
        assert "refused symlink log" in result.stdout
        assert "refused symlink runtime dir" in result.stdout
    print("PASS: bounded cleanup truncates known logs (including removed WiFi), refuses symlink files/dirs and keeps unknown logs")


def test_stale_temps() -> None:
    with tempfile.TemporaryDirectory(prefix="cleanup-temps-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        wifi1b = fixture.runtime / "wifi1b"
        wifi1b.mkdir()
        dead = wifi1b / "status.json.tmp.999999"
        dead.write_text("stale\n")
        unknown = wifi1b / "owner.tmp.999999"
        unknown.write_text("unknown\n")
        live = wifi1b / f"status.json.tmp.{os.getpid()}"
        live.write_text("live pid\n")
        (fixture.proc / str(os.getpid())).mkdir()
        invalid = wifi1b / "status.json.tmp.1"
        invalid.write_text("pid one\n")
        nonnumeric = wifi1b / "config.conf.tmp.abc"
        nonnumeric.write_text("unproven\n")
        linked = wifi1b / "wpa_supplicant.conf.tmp.999998"
        linked.symlink_to(fixture.victim / "daemon.log")
        result = fixture.shell('cleanup_stale_temps "$RUNTIME" || true')
        assert dead.exists(), "temps must be deferred before radios are verified stopped"
        assert "deferred stale temp cleanup" in result.stdout
        result = fixture.shell('cleanup_stale_temps "$RUNTIME" || true',
                               {"RUNTIME_CLEANUP_RADIOS_STOPPED": "1"})
        assert not dead.exists(), result.stdout
        assert unknown.exists() and live.exists() and invalid.exists() and nonnumeric.exists()
        assert linked.is_symlink()
        assert (fixture.victim / "daemon.log").read_text() == "owner log\n"
        assert "kept unknown temp" in result.stdout
        assert "kept live-pid temp" in result.stdout
        assert "kept invalid-pid temp" in result.stdout
        assert "refused symlink temp" in result.stdout
    print("PASS: stale temp cleanup requires stopped radios, deletes only dead owned numeric PIDs and preserves unknown/live/symlink temps")


def test_removed_wifi_evidence() -> None:
    with tempfile.TemporaryDirectory(prefix="cleanup-wifi-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        result = fixture.shell(remove_call(fixture))
        assert (fixture.wifi / "config.conf").exists(), result.stdout
        assert "no verified private archive" in result.stdout
    with tempfile.TemporaryDirectory(prefix="cleanup-wifi-ok-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        archive = make_archive(fixture)
        expected = hashlib.sha256(archive.read_bytes()).hexdigest()
        result = fixture.shell(remove_call(fixture, expected, str(archive)))
        assert not fixture.wifi.exists(), result.stdout
        receipt = fixture.runtime / "combo/wifi-removed.receipt"
        assert receipt.read_text().splitlines()[0] == "SCOURGE_WIFI_REMOVAL_RECEIPT"
        assert f"archive_sha256={expected}" in receipt.read_text()
        assert "removed owned WiFi runtime" in result.stdout
    with tempfile.TemporaryDirectory(prefix="cleanup-wifi-corrupt-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        make_archive(fixture, corrupt=True)
        result = fixture.shell(remove_call(fixture))
        assert (fixture.wifi / "config.conf").read_text() == "owner-credential\n", result.stdout
        assert "no verified private archive" in result.stdout
    with tempfile.TemporaryDirectory(prefix="cleanup-wifi-installed-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        make_archive(fixture)
        (fixture.qml / QML_FILES[0]).write_text("// WIFI_PAYLOAD_260128\n")
        (fixture.qml / "HomeScreen/WifiPage.qml").write_text("// page\n")
        fixture.service.write_text("[Service]\nExecStart=/bin/true\n")
        result = fixture.shell(remove_call(fixture))
        assert (fixture.wifi / "config.conf").read_text() == "owner-credential\n", result.stdout
        assert "WiFi marker" in result.stdout or "WiFi UI still present" in result.stdout or "service still present" in result.stdout
    with tempfile.TemporaryDirectory(prefix="cleanup-wifi-unreadable-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        make_archive(fixture)
        advanced = fixture.qml / QML_FILES[0]
        advanced.chmod(0)
        result = fixture.shell(remove_call(fixture))
        assert (fixture.wifi / "config.conf").read_text() == "owner-credential\n", result.stdout
        assert "unreadable marker" in result.stdout
        advanced.chmod(0o644)
    with tempfile.TemporaryDirectory(prefix="cleanup-wifi-symlink-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        make_archive(fixture)
        target = fixture.victim / "wifi-real"
        target.mkdir()
        (target / "config.conf").write_text("owner-credential\n")
        for child in fixture.wifi.iterdir():
            if child.is_file():
                child.unlink()
        shutil.rmtree(fixture.wifi)
        fixture.wifi.symlink_to(target)
        result = fixture.shell(remove_call(fixture))
        assert (target / "config.conf").read_text() == "owner-credential\n", result.stdout
        assert "refused symlink WiFi runtime dir" in result.stdout
    with tempfile.TemporaryDirectory(prefix="cleanup-wifi-dangling-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        make_archive(fixture)
        fixture.service.symlink_to(fixture.base / "missing-service")
        result = fixture.shell(remove_call(fixture))
        assert (fixture.wifi / "config.conf").read_text() == "owner-credential\n", result.stdout
        assert "service still present" in result.stdout or "unsafe WiFi service path" in result.stdout
        fixture.service.unlink()
        (fixture.qml / "HomeScreen/WifiPage.qml").symlink_to(fixture.base / "missing-page")
        result = fixture.shell(remove_call(fixture))
        assert (fixture.wifi / "config.conf").read_text() == "owner-credential\n", result.stdout
        assert "WiFi UI still present" in result.stdout
    with tempfile.TemporaryDirectory(prefix="cleanup-wifi-bundle-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        make_archive(fixture)
        bundle = fixture.base / "appcombo.tar.gz"
        bundle.write_bytes(b"corrupt bundle\n")
        result = fixture.shell(remove_call(fixture, "0" * 64, str(bundle)))
        assert (fixture.wifi / "config.conf").read_text() == "owner-credential\n", result.stdout
        assert "bundle archive hash mismatch" in result.stdout
    print("PASS: removed-WiFi deletion needs readable absence evidence plus a verified archive; corrupt/missing bundle, symlinks and installed WiFi preserve owner state")


def make_wrapper_bundle(fixture: Fixture, corrupt=False) -> Path:
    stick = fixture.stick
    source = fixture.base / "bundle"
    source.mkdir()
    (source / "run_stuff.sh").write_text(
        '#!/bin/sh\nprintf "dummy master\\nstatus=OK\\n" > "$USB_DIR/run_stuff_result.txt"\nexit 0\n')
    archive = stick / "appcombo.tar.gz"
    with tarfile.open(archive, "w:gz") as output:
        output.add(source / "run_stuff.sh", arcname="./run_stuff.sh")
    if corrupt:
        archive.write_bytes(archive.read_bytes() + b"corrupt")
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    cleanup_sha = hashlib.sha256(HELPER.read_bytes()).hexdigest()
    wrapper = stick / "run_stuff.sh"
    wrapper.write_text((PACKAGES / "flat_wrapper.sh").read_text()
                       .replace("@TARBALL_SHA256@", digest)
                       .replace("@RUNTIME_CLEANUP_SHA256@", cleanup_sha)
                       .replace("# @RUNTIME_CLEANUP_SHELL@", HELPER.read_text()))
    assert "@RUNTIME_CLEANUP_SHELL@" not in wrapper.read_text()
    assert not (stick / "runtime_cleanup.sh").exists()
    beep = fixture.runtime / "combo/bin/dbus-send"
    beep.parent.mkdir(parents=True, exist_ok=True)
    beep.write_text("#!/bin/sh\nexit 0\n")
    beep.chmod(0o755)
    return wrapper


def run_wrapper(fixture: Fixture, wrapper: Path) -> subprocess.CompletedProcess:
    env = dict(os.environ,
               USB_DIR=str(fixture.stick),
               COMBO_RW_DIR=str(fixture.runtime / "combo"),
               COMBO_RUNTIME_ROOT=str(fixture.runtime),
               COMBO_QML_DIR=str(fixture.qml),
               CLEANUP_PROC_ROOT=str(fixture.proc),
               CLEANUP_WIFI_SERVICE=str(fixture.service))
    return subprocess.run(["sh", str(wrapper)], env=env, capture_output=True,
                          text=True, timeout=90)


def test_wrapper_cleanup() -> None:
    with tempfile.TemporaryDirectory(prefix="cleanup-wrapper-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        make_archive(fixture)
        wrapper = make_wrapper_bundle(fixture)
        result = run_wrapper(fixture, wrapper)
        assert result.returncode == 0, (result.stdout, result.stderr,
                                        (fixture.stick / "appcombo_install.log").read_text())
        assert not fixture.wifi.exists(), "verified archive did not authorise wrapper cleanup"
        assert (fixture.runtime / "combo/wifi-removed.receipt").exists()
        assert "status=OK" in (fixture.stick / "wrapper_result.txt").read_text()
    with tempfile.TemporaryDirectory(prefix="cleanup-wrapper-corrupt-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        make_archive(fixture)
        wrapper = make_wrapper_bundle(fixture, corrupt=True)
        result = run_wrapper(fixture, wrapper)
        assert result.returncode != 0
        assert (fixture.wifi / "config.conf").read_text() == "owner-credential\n"
        assert "status=FAIL" in (fixture.stick / "wrapper_result.txt").read_text()
    with tempfile.TemporaryDirectory(prefix="cleanup-wrapper-marker-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        make_archive(fixture)
        advanced = fixture.qml / QML_FILES[0]
        advanced.chmod(0)
        wrapper = make_wrapper_bundle(fixture)
        result = run_wrapper(fixture, wrapper)
        assert result.returncode == 0, result.stderr
        assert (fixture.wifi / "config.conf").read_text() == "owner-credential\n", \
            "unreadable QML marker authorised credential deletion"
        advanced.chmod(0o644)
    print("PASS: flat wrapper cleans only with a verified archive, preserves state on a corrupt bundle and fails closed on an unreadable marker")


test_bounded_logs()
test_stale_temps()
test_removed_wifi_evidence()
test_wrapper_cleanup()
print("PASS: runtime cleanup safety fixtures are host-isolated and preserve owner data")
