#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Ioniq 5 Gen5W_L, firmware NE_E.EUR.S5W_L.001.001.260128 (EU).
"""Build a sparse GPT eMMC projection for the fidelity VM.

The image is assembled from a local decrypted rootfs tree and optional
runtime/navi trees. It is not a stock firmware image and must be built from
the owner's own staged inputs. Outputs are local and gitignored.

Layouts:
  captured   - 17 partitions using sizes observed on the owner's EU car
  projection - the smaller documented Gen5W projection used by the public
               fidelity reference; useful for a lightweight lab image
  json       - a validated layout.json with partition/label/size_mib records

Partitions are created with parted, exposed with losetup -P, and formatted
with conservative ext4 options that the vendor 4.1 kernel can mount.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

VM = Path(__file__).resolve().parent
sys.path.insert(0, str(VM))
from harness.images import validate_output  # noqa: E402

# Sizes are MiB, derived from the owner's own capture (docs/23) and the
# public projection. They are best-effort where the capture did not measure
# a partition (p4/p16). The boot-critical roles are p5, p8, p11 and p12/p13.
CAPTURED = [
    (1, "plk1_iasImage", 64),
    (2, "plk2_iasPreload", 64),
    (3, "rootfs_a", 2969),
    (4, "misc_unknown", 16),
    (5, "rootfs_b_active", 2969),
    (6, "vr_a", 450),
    (7, "vr_b", 450),
    (8, "log_data", 2048),
    (9, "update", 20275),
    (10, "voicememo", 150),
    (11, "rw_data", 1300),
    (12, "mnt_c", 40),
    (13, "mnt_backup", 40),
    (14, "navi", 41984),
    (15, "navi2", 41984),
    (16, "unknown16", 1),
    (17, "space", 4608),
]

PROJECTION = [
    (1, "plk1_iasImage", 64),
    (2, "plk2_iasPreload", 64),
    (3, "rootfs_a", 8192),
    (4, "misc_unknown", 16),
    (5, "rootfs_b_active", 8192),
    (6, "vr_a", 64),
    (7, "vr_b", 64),
    (8, "log_data", 2048),
    (9, "update", 12288),
    (10, "voicememo", 150),
    (11, "rw_data", 1280),
    (12, "mnt_c", 40),
    (13, "mnt_backup", 40),
    (14, "navi", 2048),
    (15, "navi2", 2048),
    (16, "unknown16", 64),
    (17, "space", 256),
]


def load_layout(name: str, json_path: Path | None = None) -> list[dict]:
    if json_path is not None:
        data = json.loads(json_path.read_text())
        if not isinstance(data, list) or not data:
            raise ValueError("layout JSON must be a non-empty list")
        items = []
        for entry in data:
            try:
                partition = int(entry["partition"])
                label = str(entry["label"])
                size_mib = int(entry["size_mib"])
            except (KeyError, TypeError, ValueError) as exc:
                raise ValueError(f"invalid layout entry: {entry!r}") from exc
            if partition < 1 or size_mib < 1 or not label:
                raise ValueError(f"invalid layout entry: {entry!r}")
            items.append({"partition": partition, "label": label,
                          "size_mib": size_mib})
        return items
    if name == "captured":
        return [{"partition": p, "label": l, "size_mib": s}
                for p, l, s in CAPTURED]
    if name == "projection":
        return [{"partition": p, "label": l, "size_mib": s}
                for p, l, s in PROJECTION]
    raise ValueError(f"unknown layout: {name}")


def validate_layout(items: list[dict]) -> list[dict]:
    items = sorted(items, key=lambda item: item["partition"])
    if [item["partition"] for item in items] != list(range(1, len(items) + 1)):
        raise ValueError("layout partitions must be consecutive 1..N")
    if len(items) != 17:
        raise ValueError("the Gen5W projection needs exactly 17 partitions")
    labels = [item["label"] for item in items]
    if len(set(labels)) != len(labels):
        raise ValueError("layout partition labels must be unique")
    total_mib = 1 + sum(item["size_mib"] for item in items)  # 1 MiB GPT header
    start_mib = 1
    for item in items:
        item["start_mib"] = start_mib
        item["end_mib"] = start_mib + item["size_mib"]
        start_mib = item["end_mib"]
    return items


def layout_summary(items: list[dict]) -> dict:
    # One MiB for the primary GPT header plus one for the backup header.
    total_mib = items[-1]["end_mib"] + 1
    return {
        "partitions": len(items),
        "total_mib": total_mib,
        "total_bytes": total_mib * 1024 * 1024,
        "active_root": "p5",
        "items": items,
    }


def _run(argv: list[str], **kwargs) -> None:
    kwargs.setdefault("check", True)
    subprocess.run(argv, **kwargs)


def _source_size(path: Path) -> int:
    total = 0
    for entry in path.rglob("*"):
        if entry.is_file() and not entry.is_symlink():
            total += entry.stat().st_size
    return total


def _check_fits(source: Path, size_mib: int) -> None:
    if source is None:
        return
    if not source.is_dir():
        raise ValueError(f"source tree is not a directory: {source}")
    source_bytes = _source_size(source)
    partition_bytes = size_mib * 1024 * 1024
    if source_bytes > partition_bytes:
        raise ValueError(
            f"{source} is {source_bytes} bytes, larger than its "
            f"{size_mib} MiB partition")
    if source_bytes == 0:
        raise ValueError(f"source tree is empty: {source}")


def _debugfs_write(image: Path, host_path: Path, guest_path: str,
                   mode: str | None = None) -> None:
    for value in (str(host_path), guest_path):
        if any(char.isspace() for char in value):
            raise ValueError(f"debugfs path contains whitespace: {value!r}")
    if host_path.is_symlink() or not host_path.is_file():
        raise ValueError(f"helper source is not a regular file: {host_path}")
    expected = host_path.read_bytes()
    # debugfs write does not replace an existing inode reliably; remove first.
    _run(["debugfs", "-w", "-R", f"rm {guest_path}", str(image)], check=False)
    _run(["debugfs", "-w", "-R", f"write {host_path} {guest_path}", str(image)])
    if mode is not None:
        _run(["debugfs", "-w", "-R", f"sif {guest_path} mode {mode}", str(image)])
    with tempfile.TemporaryDirectory() as td:
        dumped = Path(td) / "dump"
        proc = subprocess.run(["debugfs", "-R", f"dump {guest_path} {dumped}",
                               str(image)], capture_output=True, text=True,
                              check=False)
        if proc.returncode != 0 or not dumped.is_file():
            raise RuntimeError(f"debugfs write did not persist: {guest_path}")
        if dumped.read_bytes() != expected:
            raise RuntimeError(f"debugfs content mismatch: {guest_path}")


def _debugfs_exists(image: Path, guest_path: str) -> bool:
    proc = subprocess.run(["debugfs", "-R", f"stat {guest_path}", str(image)],
                          capture_output=True, text=True, check=False)
    return "Inode:" in proc.stdout


def inject_fidelity_helpers(partition_image: Path,
                            install_weston_wrapper: bool = False,
                            install_micom_standin: bool = False,
                            micom_helper: Path | None = None,
                            vendor_weston: Path | None = None) -> list[str]:
    """Install the projection-only helpers into the active-root partition.

    `partition_image` must be the p5 filesystem (a loop partition device or an
    ext4 file), never the whole GPT disk: debugfs cannot open a GPT container.
    """
    warnings: list[str] = []
    helper = VM / "fidelity" / "init-esinit-spi.sh"
    modprobe = VM / "fidelity" / "no-lge-usb.conf"
    for directory in ("/etc", "/etc/modprobe.d", "/usr", "/usr/bin",
                      "/etc/systemd", "/etc/systemd/system"):
        _run(["debugfs", "-w", "-R", f"mkdir {directory}",
              str(partition_image)], check=False)
    _debugfs_write(partition_image, helper, "/init-esinit-spi.sh", "0100755")
    _debugfs_write(partition_image, modprobe,
                   "/etc/modprobe.d/zz-vm-research-no-lge-usb.conf")

    if install_weston_wrapper:
        if vendor_weston is None or not vendor_weston.is_file():
            raise ValueError(
                "weston wrapper requested but the vendor /usr/bin/weston "
                "binary was not supplied")
        wrapper = VM / "fidelity" / "weston-wrapper.sh"
        _debugfs_write(partition_image, vendor_weston, "/usr/bin/weston.vendor",
                       "0100755")
        _debugfs_write(partition_image, wrapper, "/usr/bin/weston", "0100755")
        warnings.append(
            "weston-wrapper installed; software-GL assets and the "
            "AppHomeScreen show handshake remain WIP")

    if install_micom_standin:
        if micom_helper is None or not micom_helper.is_file():
            raise ValueError(
                "micom stand-in requested but no helper binary was provided; "
                "build tools/vm/guest/micom-dbus-sim with "
                "sh tools/vm/build_guest_tools.sh first")
        service = VM / "fidelity" / "micom-standin.service"
        _run(["debugfs", "-w", "-R", "mkdir /etc/systemd/system",
              str(partition_image)], check=False)
        _debugfs_write(partition_image, micom_helper, "/micom-dbus-sim",
                       "0100755")
        _debugfs_write(partition_image, service,
                       "/etc/systemd/system/dbus-com.lge.car.micom.service")
        warnings.append(
            "micom stand-in is a lab substitute for the SPI MICOM handshake")
    required = ["/init-esinit-spi.sh",
                "/etc/modprobe.d/zz-vm-research-no-lge-usb.conf"]
    if install_weston_wrapper:
        required += ["/usr/bin/weston", "/usr/bin/weston.vendor"]
    if install_micom_standin:
        required += ["/micom-dbus-sim",
                     "/etc/systemd/system/dbus-com.lge.car.micom.service"]
    missing = [path for path in required
               if not _debugfs_exists(partition_image, path)]
    if missing:
        raise RuntimeError(f"helper injection did not persist: {missing}")
    return warnings


def create_gpt(output: Path, items: list[dict]) -> None:
    """Create the sparse GPT container; parted does not read commands on stdin."""
    total_bytes = (items[-1]["end_mib"] + 1) * 1024 * 1024
    _run(["truncate", "-s", str(total_bytes), str(output)])
    _run(["parted", "--script", str(output), "mklabel", "gpt"])
    for item in items:
        _run(["parted", "--script", str(output), "mkpart", item["label"],
              "ext4", f"{item['start_mib']}MiB", f"{item['end_mib']}MiB"])
    proc = subprocess.run(["parted", "--machine", str(output), "unit", "MiB",
                           "print"], capture_output=True, text=True, check=False)
    lines = proc.stdout.splitlines()
    disk_line = lines[1] if len(lines) > 1 else ""
    if proc.returncode != 0 or ":gpt:" not in disk_line:
        raise RuntimeError(f"GPT label was not created: {proc.stdout}{proc.stderr}")
    rows = [line for line in lines if re.match(r"^\d+:", line)]
    if len(rows) != len(items):
        raise RuntimeError(
            f"GPT partition count mismatch: {len(rows)} != {len(items)}")
    names = {row.split(":")[5] for row in rows if len(row.split(":")) > 5}
    missing = [item["label"] for item in items if item["label"] not in names]
    if missing:
        raise RuntimeError(f"GPT partitions missing: {missing}")


def build_image(output: Path, rootfs_tree: Path, layout: list[dict],
                rw_data: Path | None = None, navi: Path | None = None,
                navi2: Path | None = None, fill_inactive: bool = False,
                install_weston_wrapper: bool = False,
                install_micom_standin: bool = False,
                micom_helper: Path | None = None,
                populate: bool = True) -> dict:
    output = validate_output(output)
    items = validate_layout(layout)
    summary = layout_summary(items)
    if rootfs_tree is None or not rootfs_tree.is_dir():
        raise ValueError("rootfs tree is required and must be a directory")

    _check_fits(rootfs_tree, items[4]["size_mib"])
    if rw_data is not None:
        _check_fits(rw_data, items[10]["size_mib"])
    if navi is not None:
        _check_fits(navi, items[13]["size_mib"])
    if navi2 is not None:
        _check_fits(navi2, items[14]["size_mib"])

    # Sparse total image, then a GPT label with explicit partition extents.
    create_gpt(output, items)

    loop = subprocess.check_output(
        ["losetup", "-P", "-f", "--show", str(output)], text=True
    ).strip()
    if not loop:
        raise RuntimeError("losetup did not return a loop device")

    try:
        for item in items:
            idx = item["partition"]
            dev = f"{loop}p{idx}"
            for _ in range(50):
                if Path(dev).exists():
                    break
                time.sleep(0.1)
            if not Path(dev).exists():
                raise RuntimeError(f"partition device not found: {dev}")
            source = None
            if populate:
                if idx == 5:
                    source = rootfs_tree
                elif idx == 11 and rw_data is not None:
                    source = rw_data
                elif idx == 14 and navi is not None:
                    source = navi
                elif idx == 15 and navi2 is not None:
                    source = navi2
                elif idx == 3 and fill_inactive:
                    source = rootfs_tree
            argv = ["fakeroot", "mkfs.ext4", "-b", "4096",
                    "-O", "^metadata_csum,^64bit,^flex_bg"]
            if source is not None:
                argv += ["-d", str(source)]
            argv.append(dev)
            _run(argv)

        warnings: list[str] = []
        if populate:
            # The helpers belong on the active-root filesystem (p5), not on
            # the GPT container. debugfs cannot open a GPT disk.
            active_root = Path(f"{loop}p5")
            if not active_root.exists():
                raise RuntimeError(f"active-root partition not found: {active_root}")
            warnings = inject_fidelity_helpers(
                active_root, install_weston_wrapper, install_micom_standin,
                micom_helper, rootfs_tree / "usr" / "bin" / "weston")
    except BaseException:
        output.unlink(missing_ok=True)
        raise
    finally:
        _run(["losetup", "-d", loop], check=False)

    summary["warnings"] = warnings
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rootfs-tree", type=Path, required=True)
    parser.add_argument("--layout", default="captured",
                        choices=("captured", "projection"))
    parser.add_argument("--layout-json", type=Path)
    parser.add_argument("--rw-data", type=Path)
    parser.add_argument("--navi", type=Path)
    parser.add_argument("--navi2", type=Path)
    parser.add_argument("--fill-inactive", action="store_true")
    parser.add_argument("--install-weston-wrapper", action="store_true")
    parser.add_argument("--install-micom-standin", action="store_true")
    parser.add_argument("--micom-helper", type=Path,
                        default=VM / "guest" / "micom-dbus-sim",
                        help="prebuilt /micom-dbus-sim helper for --install-micom-standin")
    parser.add_argument("--no-populate", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    try:
        layout = load_layout(args.layout, args.layout_json)
        items = validate_layout(layout)
        summary = layout_summary(items)
        if args.dry_run:
            print(json.dumps(summary, indent=2))
            return 0
        out = build_image(args.output, args.rootfs_tree, items,
                          args.rw_data, args.navi, args.navi2,
                          args.fill_inactive, args.install_weston_wrapper,
                          args.install_micom_standin, args.micom_helper,
                          not args.no_populate)
        print(json.dumps(out, indent=2))
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"partitioned image build failed: {exc}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
