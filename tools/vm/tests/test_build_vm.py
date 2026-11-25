#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Unit checks for the VM build scripts and fidelity boot checker."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

VM = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(VM))

import build_partitioned_image as part
import build_vm
from launch import _vendor_cmdline, qemu_argv


class LayoutTests(unittest.TestCase):
    def test_captured_layout_is_consecutive_and_ordered(self):
        items = part.validate_layout(part.load_layout("captured"))
        self.assertEqual(len(items), 17)
        self.assertEqual(items[0]["start_mib"], 1)
        self.assertEqual(items[4]["partition"], 5)
        self.assertEqual(items[4]["label"], "rootfs_b_active")
        self.assertGreater(items[-1]["end_mib"], items[-2]["end_mib"])
        summary = part.layout_summary(items)
        self.assertEqual(summary["partitions"], 17)
        self.assertEqual(summary["active_root"], "p5")
        self.assertGreater(summary["total_bytes"], 0)

    def test_projection_layout_is_smaller(self):
        captured = part.layout_summary(part.validate_layout(part.load_layout("captured")))
        projection = part.layout_summary(part.validate_layout(part.load_layout("projection")))
        self.assertLess(projection["total_bytes"], captured["total_bytes"])

    def test_bad_layouts_are_rejected(self):
        with self.assertRaises(ValueError):
            part.validate_layout([{"partition": 1, "label": "a", "size_mib": 1}])
        with self.assertRaises(ValueError):
            part.validate_layout([
                {"partition": 1, "label": "a", "size_mib": 1},
                {"partition": 2, "label": "a", "size_mib": 1},
            ])

    def test_json_layout_round_trip(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "layout.json"
            path.write_text(json.dumps([
                {"partition": i, "label": f"p{i}", "size_mib": 10}
                for i in range(1, 18)
            ]))
            items = part.validate_layout(part.load_layout("captured", path))
            self.assertEqual(items[0]["end_mib"], 11)

    def test_root_partition_override_is_explicit(self):
        cmdline = _vendor_cmdline(True, "/lib/systemd/esinit", "/dev/mmcblk0p5")
        self.assertIn("root=/dev/mmcblk0p5", cmdline)
        self.assertIn("init=/lib/systemd/esinit", cmdline)
        with self.assertRaises(ValueError):
            _vendor_cmdline(True, None, "/dev/sda")

    def test_kernel_image_reaches_qemu_argv(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            kernel = root / "bzImage"
            kernel.write_bytes(b"kernel")
            overlay = root / "rootfs.qcow2"
            overlay.write_bytes(b"overlay")
            argv = qemu_argv(root, overlays={
                "rootfs": {"path": str(overlay), "persistent": False}
            }, kernel_image=kernel)
            self.assertIn(str(kernel.resolve()), argv)
            self.assertIn("-kernel", argv)

    def test_helper_injection_targets_an_ext4_partition(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            image = root / "p5.img"
            image.write_bytes(b"")
            subprocess.run(["truncate", "-s", "16M", str(image)], check=True)
            subprocess.run(["mkfs.ext4", "-q", "-F", str(image)], check=True)
            weston = root / "weston"
            weston.write_bytes(b"vendor-weston")
            micom = root / "micom-dbus-sim"
            micom.write_bytes(b"lab-micom")
            warnings = part.inject_fidelity_helpers(
                image, True, True, micom, weston)
            self.assertTrue(warnings)
            listing = subprocess.run(
                ["debugfs", "-R", "ls -l /", str(image)],
                capture_output=True, text=True, check=True).stdout
            self.assertIn("init-esinit-spi.sh", listing)
            usr_listing = subprocess.run(
                ["debugfs", "-R", "ls -l /usr/bin", str(image)],
                capture_output=True, text=True, check=True).stdout
            self.assertIn("micom-dbus-sim", listing + usr_listing)
            self.assertIn("weston.vendor", usr_listing)
            content = subprocess.run(
                ["debugfs", "-R", "cat /init-esinit-spi.sh", str(image)],
                capture_output=True, text=True, check=True).stdout
            self.assertIn("spi_validation", content)

    def test_helper_injection_overwrites_existing_files(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            image = root / "p5.img"
            image.write_bytes(b"")
            subprocess.run(["truncate", "-s", "16M", str(image)], check=True)
            subprocess.run(["mkfs.ext4", "-q", "-F", str(image)], check=True)
            first = root / "weston-one"
            first.write_bytes(b"first")
            second = root / "weston-two"
            second.write_bytes(b"second")
            micom = root / "micom"
            micom.write_bytes(b"micom")
            part.inject_fidelity_helpers(image, True, True, micom, first)
            part.inject_fidelity_helpers(image, True, True, micom, second)
            dumped = root / "dumped"
            subprocess.run(["debugfs", "-R", f"dump /usr/bin/weston.vendor {dumped}",
                            str(image)], capture_output=True, text=True, check=True)
            self.assertEqual(dumped.read_bytes(), b"second")

    def test_create_gpt_actually_writes_seventeen_partitions(self):
        with tempfile.TemporaryDirectory() as td:
            output = Path(td) / "disk.img"
            items = part.validate_layout([
                {"partition": i, "label": f"p{i}", "size_mib": 1}
                for i in range(1, 18)
            ])
            part.create_gpt(output, items)
            proc = subprocess.run(
                ["parted", "--machine", str(output), "unit", "MiB", "print"],
                capture_output=True, text=True, check=True)
            rows = [line for line in proc.stdout.splitlines()
                    if line[:1].isdigit() and ":" in line]
            self.assertEqual(len(rows), 17)
            for item in items:
                self.assertIn(item["label"], proc.stdout)

    def test_build_image_injects_into_p5_not_the_gpt_disk(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            output = root / "disk.img"
            rootfs = root / "rootfs"
            (rootfs / "usr" / "bin").mkdir(parents=True)
            (rootfs / "usr" / "bin" / "weston").write_bytes(b"w")
            loop_base = root / "fakeloop"
            for idx in range(1, 18):
                Path(f"{loop_base}p{idx}").write_bytes(b"")
            calls = []

            def fake_run(argv, **kwargs):
                calls.append(argv)

            with patch.object(part, "_run", fake_run), \
                    patch.object(part, "create_gpt"), \
                    patch.object(part.subprocess, "check_output",
                                 return_value=str(loop_base)), \
                    patch.object(part, "inject_fidelity_helpers",
                                 return_value=[]) as inject:
                part.build_image(output, rootfs, part.validate_layout(
                    part.load_layout("projection")), populate=True)
            inject.assert_called_once()
            self.assertEqual(str(inject.call_args.args[0]),
                             f"{loop_base}p5")
            self.assertNotEqual(str(inject.call_args.args[0]), str(output))

    def test_build_usable_uses_absolute_paths(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            rootfs = root / "rootfs"
            rootfs.mkdir()
            out = root / "vm"

            def fake_run(argv, **kwargs):
                out.mkdir(parents=True, exist_ok=True)
                Path(argv[argv.index("--output") + 1]).write_bytes(b"img")
                Path(argv[argv.index("--kernel-output") + 1]).write_bytes(b"k")

            with patch.object(build_vm.subprocess, "run", side_effect=fake_run) as run:
                result = build_vm.build_usable(rootfs, out, None)
            call = run.call_args
            self.assertEqual(call.kwargs["env"]["ROOTFS"], str(rootfs.resolve()))
            self.assertTrue(Path(call.args[0][call.args[0].index("--output") + 1]).is_absolute())
            self.assertEqual(result["rootfs_image"], str((out / "rootfs.img").resolve()))

    def test_fidelity_profile_does_not_overclaim_unrequested_substitutions(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            rootfs = root / "rootfs"
            (rootfs / "boot").mkdir(parents=True)
            (rootfs / "boot" / "bzImage-4.1.27-abl").write_bytes(b"kernel")
            out = root / "vm"

            def fake_build(image, *args, **kwargs):
                image.write_bytes(b"image")
                return {"warnings": [], "partitions": 17}

            with patch.object(build_vm.part, "build_image", side_effect=fake_build):
                build_vm.build_fidelity(rootfs, out, "projection", None, None,
                                        None, None, False, False, False, None,
                                        None)
            profile = json.loads((out / "profile.json").read_text())
            substitutions = profile["boot"]["substitutions"]
            self.assertTrue(any("init-esinit-spi" in s for s in substitutions))
            self.assertFalse(any("software-GL" in s for s in substitutions))
            self.assertFalse(any("micom-dbus-sim" in s for s in substitutions))

    def test_build_vm_dry_run_does_not_create_output(self):
        with tempfile.TemporaryDirectory() as td:
            rootfs = Path(td) / "rootfs"
            rootfs.mkdir()
            (rootfs / "boot").mkdir()
            (rootfs / "boot" / "bzImage-4.1.27-abl").write_bytes(b"kernel")
            out = Path(td) / "vm"
            script = VM / "build_vm.py"
            proc = subprocess.run([
                sys.executable, str(script), "fidelity",
                "--rootfs-tree", str(rootfs), "--output-dir", str(out),
                "--dry-run",
            ], capture_output=True, text=True)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertFalse(out.exists())
            data = json.loads(proc.stdout)
            self.assertEqual(data["partitions"], 17)


class CheckerTests(unittest.TestCase):
    def test_check_stock_boot_reads_only_evidence(self):
        with tempfile.TemporaryDirectory() as td:
            run = Path(td)
            (run / "serial.log").write_text(
                "mmcblk0p5\nFDE SUPPORT: No\nuse fstab for non FDE\n"
            )
            script = VM / "fidelity" / "check-stock-boot.py"
            rc = subprocess.run([sys.executable, str(script), str(run)],
                                capture_output=True, text=True)
            self.assertEqual(rc.returncode, 0, rc.stdout + rc.stderr)
            self.assertIn("PASS", rc.stdout)

    def test_check_stock_boot_json_output(self):
        with tempfile.TemporaryDirectory() as td:
            run = Path(td)
            (run / "serial.log").write_text(
                "mmcblk0p5\nFDE SUPPORT: No\nuse fstab for non FDE\n"
            )
            script = VM / "fidelity" / "check-stock-boot.py"
            rc = subprocess.run([sys.executable, str(script), str(run),
                                 "--json"], capture_output=True, text=True)
            self.assertEqual(rc.returncode, 0, rc.stdout + rc.stderr)
            data = json.loads(rc.stdout)
            self.assertTrue(data["pass"])

    def test_check_stock_boot_ignores_result_json_markers(self):
        with tempfile.TemporaryDirectory() as td:
            run = Path(td)
            (run / "serial.log").write_text("")
            (run / "result.json").write_text(json.dumps({
                "scenario": {"boot_markers": {"FDE SUPPORT: No": "x",
                                              "use fstab for non FDE": "y"}},
                "root_partition": "/dev/mmcblk0p5",
            }))
            script = VM / "fidelity" / "check-stock-boot.py"
            rc = subprocess.run([sys.executable, str(script), str(run)],
                                capture_output=True, text=True)
            self.assertEqual(rc.returncode, 1)

    def test_check_stock_boot_fails_without_markers(self):
        with tempfile.TemporaryDirectory() as td:
            run = Path(td)
            (run / "serial.log").write_text("nothing here")
            script = VM / "fidelity" / "check-stock-boot.py"
            rc = subprocess.run([sys.executable, str(script), str(run)],
                                capture_output=True, text=True)
            self.assertEqual(rc.returncode, 1)
            self.assertIn("FAIL", rc.stdout)


if __name__ == "__main__":
    unittest.main()
