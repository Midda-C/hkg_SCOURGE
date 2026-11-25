#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Ioniq 5 Gen5W_L, firmware NE_E.EUR.S5W_L.001.001.260128 (EU).
"""Build a fidelity or usable VM workspace from the owner's own rootfs.

This does not download or decrypt firmware. Run the documented stage flow
first (docs/03, docs/14) and point --rootfs-tree at the local decrypted tree.

fidelity
  GPT eMMC projection with p5 active root, p8/p9/p10/p12/p13 roles, and the
  vendor kernel. This is the closest available boot path, not stock hardware.

usable
  Flat ext4 rootfs for the compat kernel (Mode C) with the same local rootfs.
  Graphics/UI assets are supplied separately through the car-assets lane.

Outputs are gitignored and are never OEM material.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

VM = Path(__file__).resolve().parent
sys.path.insert(0, str(VM))
import build_partitioned_image as part  # noqa: E402


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def find_kernel(rootfs_tree: Path) -> Path:
    kernel = rootfs_tree / "boot" / "bzImage-4.1.27-abl"
    if not kernel.is_file():
        raise ValueError(f"vendor kernel not found: {kernel}")
    return kernel


def build_fidelity(rootfs_tree: Path, output_dir: Path, layout: str,
                   layout_json: Path | None, rw_data: Path | None,
                   navi: Path | None, navi2: Path | None,
                   fill_inactive: bool, weston_wrapper: bool,
                   micom_standin: bool, assets: Path | None,
                   micom_helper: Path | None) -> dict:
    rootfs_tree = rootfs_tree.resolve()
    output_dir = output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    image = output_dir / "rootfs.img"
    kernel = output_dir / "bzImage"
    if image.exists():
        raise ValueError(f"output already exists: {image}")
    if kernel.exists():
        raise ValueError(f"output already exists: {kernel}")
    if rw_data is not None:
        rw_data = rw_data.resolve()
    if navi is not None:
        navi = navi.resolve()
    if navi2 is not None:
        navi2 = navi2.resolve()
    if layout_json is not None:
        layout_json = layout_json.resolve()
    if micom_helper is not None:
        micom_helper = micom_helper.resolve()
    items = part.load_layout(layout, layout_json)
    summary = part.build_image(image, rootfs_tree, items, rw_data, navi,
                               navi2, fill_inactive, weston_wrapper,
                               micom_standin, micom_helper)
    shutil.copy2(find_kernel(rootfs_tree), kernel)
    if assets is not None:
        assets = assets.resolve()
        if not assets.is_dir():
            raise ValueError(f"assets directory not found: {assets}")
        shutil.copytree(assets, output_dir / "assets", symlinks=False,
                        dirs_exist_ok=False)
    substitutions = [
        "MICOM SPI stand-in / init-esinit-spi.sh",
        "lge_usb_adapter neutralised for non-vendor hardware",
    ]
    if weston_wrapper:
        substitutions.append(
            "software-GL/pixman wrapper (WIP; runtime not installed)")
    if micom_standin:
        substitutions.append("micom-dbus-sim service stand-in")
    profile = {
        "schema_version": 1,
        "profile": "fidelity",
        "status": "WIP; unit-tested build path, not VM- or car-validated",
        "kernel": {"source": str(find_kernel(rootfs_tree)),
                   "output": str(kernel)},
        "rootfs_tree": str(rootfs_tree),
        "rootfs_image": str(image),
        "layout": summary,
        "boot": {
            "root": "/dev/mmcblk0p5",
            "init": "/lib/systemd/esinit",
            "helper_init": "/init-esinit-spi.sh",
            "substitutions": substitutions,
            "warnings": summary.get("warnings", []),
        },
        "built_at": datetime.now(timezone.utc).isoformat(),
    }
    (output_dir / "profile.json").write_text(json.dumps(profile, indent=2))
    manifest = {
        "schema_version": 1,
        "profile": "fidelity",
        "files": [],
        "built_at": profile["built_at"],
    }
    for path in (image, kernel):
        manifest["files"].append({
            "name": path.name,
            "size": path.stat().st_size,
            "sha256": sha256_file(path),
        })
    (output_dir / "manifest.json").write_text(json.dumps(manifest, indent=2))
    return profile


def build_usable(rootfs_tree: Path, output_dir: Path,
                 assets: Path | None) -> dict:
    rootfs_tree = rootfs_tree.resolve()
    output_dir = output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    image = output_dir / "rootfs.img"
    kernel = output_dir / "bzImage"
    if image.exists():
        raise ValueError(f"output already exists: {image}")
    if kernel.exists():
        raise ValueError(f"output already exists: {kernel}")
    script = VM / "build_rootfs.sh"
    if not script.is_file():
        raise ValueError(f"missing rootfs builder: {script}")
    # build_rootfs.sh changes to tools/vm; pass absolute inputs/outputs so the
    # documented relative workspace paths do not resolve under tools/vm.
    env = {**os.environ, "ROOTFS": str(rootfs_tree)}
    subprocess.run(["sh", str(script), "--output", str(image),
                    "--kernel-output", str(kernel)],
                   check=True, env=env)
    if assets is not None:
        assets = assets.resolve()
        if not assets.is_dir():
            raise ValueError(f"assets directory not found: {assets}")
        shutil.copytree(assets, output_dir / "assets", symlinks=False,
                        dirs_exist_ok=False)
    profile = {
        "schema_version": 1,
        "profile": "usable",
        "status": "WIP; unit-tested build path, not VM- or car-validated",
        "rootfs_tree": str(rootfs_tree),
        "rootfs_image": str(image),
        "kernel": {"output": str(kernel),
                   "note": "vendor kernel reference; the compat kernel is separate"},
        "boot": {
            "kernel_target": "compat",
            "init": "/init",
            "fidelity": "compat kernel; not policy evidence",
        },
        "built_at": datetime.now(timezone.utc).isoformat(),
    }
    (output_dir / "profile.json").write_text(json.dumps(profile, indent=2))
    manifest = {
        "schema_version": 1,
        "profile": "usable",
        "files": [],
        "built_at": profile["built_at"],
    }
    for path in (image, kernel):
        manifest["files"].append({
            "name": path.name,
            "size": path.stat().st_size,
            "sha256": sha256_file(path),
        })
    (output_dir / "manifest.json").write_text(json.dumps(manifest, indent=2))
    return profile


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", choices=("fidelity", "usable"))
    parser.add_argument("--rootfs-tree", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--layout", default="captured",
                        choices=("captured", "projection"))
    parser.add_argument("--layout-json", type=Path)
    parser.add_argument("--rw-data", type=Path)
    parser.add_argument("--navi", type=Path)
    parser.add_argument("--navi2", type=Path)
    parser.add_argument("--assets", type=Path)
    parser.add_argument("--fill-inactive", action="store_true")
    parser.add_argument("--weston-wrapper", action="store_true")
    parser.add_argument("--micom-standin", action="store_true")
    parser.add_argument("--micom-helper", type=Path,
                        default=VM / "guest" / "micom-dbus-sim",
                        help="prebuilt /micom-dbus-sim helper (build_guest_tools.sh)")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    if not args.rootfs_tree.is_dir():
        parser.exit(1, f"rootfs tree not found: {args.rootfs_tree}\n")
    if args.dry_run:
        if args.profile == "fidelity":
            layout = part.load_layout(args.layout, args.layout_json)
            print(json.dumps(part.layout_summary(part.validate_layout(layout)),
                             indent=2))
        else:
            print(json.dumps({"profile": "usable", "rootfs_tree":
                              str(args.rootfs_tree), "output_dir":
                              str(args.output_dir)}, indent=2))
        return 0

    try:
        if args.profile == "fidelity":
            result = build_fidelity(args.rootfs_tree, args.output_dir,
                                    args.layout, args.layout_json, args.rw_data,
                                    args.navi, args.navi2, args.fill_inactive,
                                    args.weston_wrapper, args.micom_standin,
                                    args.assets, args.micom_helper)
        else:
            result = build_usable(args.rootfs_tree, args.output_dir, args.assets)
        print(json.dumps(result, indent=2))
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"VM build failed: {exc}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
