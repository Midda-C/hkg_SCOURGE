#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Offline checks for car asset capture/extraction utilities."""
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

VM = Path(__file__).resolve().parent.parent
ASSETS = VM.parent / "car-assets"


def sha_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def make_tar(path: Path, entries: dict[str, bytes]) -> None:
    with tarfile.open(path, "w") as tar:
        for name, data in entries.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            tar.addfile(info, io_bytes(data))


def io_bytes(data: bytes):
    import io
    return io.BytesIO(data)


class CaptureFixture:
    def __init__(self, root: Path) -> None:
        self.root = root
        (root / "meta").mkdir(parents=True)
        (root / "archives").mkdir()
        (root / "partitions").mkdir()
        (root / "critical").mkdir()
        self.make_archives()
        sums = []
        for path in sorted(root.rglob("*")):
            if path.is_file() and path.name not in {"SHA256SUMS", "capture.log"}:
                sums.append(f"{sha_bytes(path.read_bytes())}  ./{path.relative_to(root)}")
        (root / "meta" / "SHA256SUMS").write_text("\n".join(sums) + "\n")

    def make_archives(self) -> None:
        make_tar(self.root / "archives" / "rootfs.tar", {
            "app/share/AppHomeScreen/foo.qml": b"qml",
            "app/share/images/hmc/a.png": b"png",
            "etc/shadow": b"secret",
            "../evil": b"bad",
        })
        make_tar(self.root / "archives" / "rw_data.tar", {
            "app_data/db.sqlite": b"db",
            "etc/shadow": b"secret",
        })
        make_tar(self.root / "archives" / "update.tar", {
            "updateVersionInfo": b"version",
            "fota/.object_uri": b"uri",
        })
        make_tar(self.root / "critical" / "critical.tar", {
            "app/bin/AppUpgrade": b"binary",
            "app/share/AppUpgrade/teruten5_pub.key": b"key",
            "app/share/dmengine/tree.xml": b"dm",
            "etc/shadow": b"secret",
            "etc/gshadow": b"secret",
        })
        (self.root / "partitions" / "p5-rootfs-a.img").write_bytes(b"raw")

    def make_navi2(self) -> None:
        make_tar(self.root / "archives" / "navi2-nonmap.tar", {
            "system/system/new_gui/a.png": b"newgui",
            "Bin/AppNavi": b"bin",
        })
        sums = self.root / "meta" / "SHA256SUMS"
        rel = "archives/navi2-nonmap.tar"
        with sums.open("a") as handle:
            handle.write(f"{sha_bytes((self.root / rel).read_bytes())}  ./{rel}\n")


class CarAssetTests(unittest.TestCase):
    def setUp(self) -> None:
        self.td = tempfile.TemporaryDirectory()
        self.root = Path(self.td.name)
        self.base = Path(tempfile.mkdtemp(prefix="car-assets-test-",
                                          dir=self.root.parent))
        self.fixture = CaptureFixture(self.root)

    def tearDown(self) -> None:
        self.td.cleanup()
        if self.base.exists():
            import shutil
            shutil.rmtree(self.base)

    def run_script(self, name: str, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, str(ASSETS / name), *args],
                              capture_output=True, text=True)

    def test_extract_ui_skips_secrets_and_traversal(self):
        out = self.base / "asset-out"
        proc = self.run_script("extract_assets.py", str(self.root),
                               "--out", str(out), "--class", "ui")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        manifest = json.loads((out / "manifest.json").read_text())
        paths = {asset["path"] for asset in manifest["assets"]}
        self.assertIn("app/share/AppHomeScreen/foo.qml", paths)
        self.assertIn("app/share/images/hmc/a.png", paths)
        self.assertNotIn("system/system/new_gui/a.png", paths)
        self.assertNotIn("etc/shadow", paths)
        self.assertNotIn("../evil", paths)
        self.assertTrue((out / "manifest.json").is_file())
        self.assertFalse((out / "etc/shadow").exists())
        self.assertFalse((out / "evil").exists())
        self.assertIn("missing optional", proc.stderr)

    def test_navi_class_without_opt_in_is_skipped(self):
        out = self.base / "navi-out"
        proc = self.run_script("extract_assets.py", str(self.root),
                               "--out", str(out), "--class", "navi")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("missing optional", proc.stderr)
        manifest = json.loads((out / "manifest.json").read_text())
        self.assertEqual(manifest["assets"], [])

    def test_navi2_archive_is_used_when_present(self):
        self.fixture.make_navi2()
        out = self.base / "navi2-out"
        proc = self.run_script("extract_assets.py", str(self.root),
                               "--out", str(out), "--class", "ui")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        manifest = json.loads((out / "manifest.json").read_text())
        paths = {asset["path"] for asset in manifest["assets"]}
        self.assertIn("system/system/new_gui/a.png", paths)

    def test_critical_archive_skips_secret_paths(self):
        out = self.base / "critical-out"
        proc = self.run_script("extract_assets.py", str(self.root),
                               "--out", str(out), "--class", "critical",
                               "--allow-sensitive")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        manifest = json.loads((out / "manifest.json").read_text())
        paths = {asset["path"] for asset in manifest["assets"]}
        self.assertIn("app/bin/AppUpgrade", paths)
        self.assertNotIn("app/share/AppUpgrade/teruten5_pub.key", paths)
        self.assertNotIn("app/share/dmengine/tree.xml", paths)
        self.assertNotIn("etc/shadow", paths)
        self.assertNotIn("etc/gshadow", paths)

    def test_partitions_are_verified_and_extracted(self):
        out = self.base / "parts-out"
        proc = self.run_script("extract_assets.py", str(self.root),
                               "--out", str(out), "--class", "partitions",
                               "--allow-sensitive")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertTrue((out / "partitions/p5-rootfs-a.img").is_file())

    def test_outputs_are_private(self):
        out = self.base / "private-out"
        proc = self.run_script("extract_assets.py", str(self.root),
                               "--out", str(out), "--class", "ui")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(out.stat().st_mode & 0o777, 0o700)
        self.assertEqual((out / "manifest.json").stat().st_mode & 0o777, 0o600)
        asset = out / "app/share/AppHomeScreen/foo.qml"
        self.assertEqual(asset.stat().st_mode & 0o777, 0o600)

    def test_sensitive_class_requires_allow(self):
        out = self.base / "runtime-out"
        proc = self.run_script("extract_assets.py", str(self.root),
                               "--out", str(out), "--class", "runtime")
        self.assertEqual(proc.returncode, 1)
        self.assertIn("--allow-sensitive", proc.stderr)

    def test_import_verifies_and_copies(self):
        dest = self.base / "imported"
        proc = self.run_script("import_capture.py", str(self.root), str(dest))
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertTrue((dest / "meta" / "SHA256SUMS").is_file())
        self.assertTrue((dest / "archives" / "rootfs.tar").is_file())
        self.assertEqual(dest.stat().st_mode & 0o777, 0o700)
        self.assertEqual((dest / "meta" / "SHA256SUMS").stat().st_mode & 0o777,
                         0o600)

    def test_import_rejects_unlisted_file(self):
        (self.root / "meta" / "unlisted.txt").write_text("extra")
        dest = self.base / "imported-extra"
        proc = self.run_script("import_capture.py", str(self.root), str(dest))
        self.assertEqual(proc.returncode, 1)
        self.assertIn("unlisted capture file", proc.stderr)

    def test_import_rejects_hash_mismatch(self):
        (self.root / "archives" / "rootfs.tar").write_bytes(b"tampered")
        dest = self.base / "imported-bad"
        proc = self.run_script("import_capture.py", str(self.root), str(dest))
        self.assertEqual(proc.returncode, 1)
        self.assertIn("hash mismatch", proc.stderr)

    def test_extract_rejects_hash_mismatch(self):
        (self.root / "archives" / "rootfs.tar").write_bytes(b"tampered")
        out = self.base / "extract-bad"
        proc = self.run_script("extract_assets.py", str(self.root),
                               "--out", str(out), "--class", "ui")
        self.assertEqual(proc.returncode, 1)
        self.assertIn("hash mismatch", proc.stderr)

    def test_manifest_validation(self):
        out = self.base / "manifest-out"
        proc = self.run_script("extract_assets.py", str(self.root),
                               "--out", str(out), "--class", "ui")
        self.assertEqual(proc.returncode, 0)
        proc = self.run_script("manifest.py", str(out / "manifest.json"))
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("manifest ok", proc.stdout)

    def test_manifest_rejects_bad_hash_and_empty_path(self):
        out = self.base / "manifest-bad"
        proc = self.run_script("extract_assets.py", str(self.root),
                               "--out", str(out), "--class", "ui")
        self.assertEqual(proc.returncode, 0)
        path = out / "manifest.json"
        data = json.loads(path.read_text())
        data["assets"][0]["sha256"] = "z" * 64
        path.write_text(json.dumps(data))
        proc = self.run_script("manifest.py", str(path))
        self.assertEqual(proc.returncode, 1)
        self.assertIn("sha256", proc.stderr)
        data["assets"][0]["sha256"] = "0" * 64
        data["assets"][0]["path"] = ""
        path.write_text(json.dumps(data))
        proc = self.run_script("manifest.py", str(path))
        self.assertEqual(proc.returncode, 1)
        self.assertIn("path", proc.stderr)

    def test_install_copies_assets(self):
        out = self.base / "install-src"
        proc = self.run_script("extract_assets.py", str(self.root),
                               "--out", str(out), "--class", "ui")
        self.assertEqual(proc.returncode, 0)
        target = self.base / "install-target"
        proc = self.run_script("install_assets.py", str(out), str(target))
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertTrue((target / "app/share/AppHomeScreen/foo.qml").is_file())
        self.assertEqual(target.stat().st_mode & 0o777, 0o700)
        self.assertEqual((target / "app/share/AppHomeScreen/foo.qml").stat().st_mode & 0o777,
                         0o600)

    def test_install_rejects_manifest_hash_mismatch(self):
        out = self.base / "install-bad"
        proc = self.run_script("extract_assets.py", str(self.root),
                               "--out", str(out), "--class", "ui")
        self.assertEqual(proc.returncode, 0)
        (out / "app/share/AppHomeScreen/foo.qml").write_bytes(b"QMl")
        target = self.base / "install-bad-target"
        proc = self.run_script("install_assets.py", str(out), str(target))
        self.assertEqual(proc.returncode, 1)
        self.assertIn("hash mismatch", proc.stderr)

    def test_capture_policy_reports_default_exclusions(self):
        proc = subprocess.run(
            ["bash", str(ASSETS / "capture_dump.sh"), "--check-policy"],
            capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("voicememo Data", proc.stdout)
        self.assertIn("etc/gshadow", proc.stdout)
        proc = subprocess.run(
            ["bash", str(ASSETS / "capture_dump.sh"), "--check-policy"],
            capture_output=True, text=True,
            env={"SCOURGE_CAPTURE_INCLUDE_SENSITIVE": "1",
                 "PATH": "/usr/bin:/bin"})
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("sensitive: enabled", proc.stdout)

    def test_capture_shell_scripts_parse(self):
        for name in ("capture_inventory.sh", "capture_dump.sh"):
            proc = subprocess.run(["bash", "-n", str(ASSETS / name)],
                                  capture_output=True, text=True)
            self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_car_scripts_require_confirmation(self):
        for name in ("capture_inventory.sh", "capture_dump.sh"):
            proc = subprocess.run(["bash", str(ASSETS / name)],
                                  capture_output=True, text=True)
            self.assertEqual(proc.returncode, 2)
            self.assertIn("refusing", proc.stderr)

    def test_capture_dump_rejects_non_media_mountpoint(self):
        proc = subprocess.run(
            ["bash", str(ASSETS / "capture_dump.sh"), str(self.root)],
            capture_output=True, text=True,
            env={"SCOURGE_CAPTURE_CONFIRM": "1", "PATH": "/usr/bin:/bin"})
        self.assertEqual(proc.returncode, 2)
        self.assertIn("removable-media mount", proc.stderr)


if __name__ == "__main__":
    unittest.main()
