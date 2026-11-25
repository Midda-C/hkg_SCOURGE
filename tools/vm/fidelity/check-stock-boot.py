#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Ioniq 5 Gen5W_L, firmware NE_E.EUR.S5W_L.001.001.260128 (EU).
"""Check recorded fidelity boot evidence in a run directory.

This reads only local run artifacts. It does not execute firmware. A PASS here
means the recorded evidence contains the requested markers at the selected
stage; it is not proof of stock hardware equivalence.

Levels:
  esinit   - p5 root, non-FDE decision and the non-FDE fstab selection
  systemd  - esinit + Local File Systems + Multi-User System
  full     - systemd + vendor Weston + AppHomeScreen bus name
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


LEVELS = {
    "esinit": (
        ("root-p5", "mmcblk0p5"),
        ("esinit-no-fde", "FDE SUPPORT: No"),
        ("esinit-fstab", "use fstab for non FDE"),
    ),
    "systemd": (
        ("root-p5", "mmcblk0p5"),
        ("local-fs", "Reached target Local File Systems"),
        ("multi-user", "Reached target Multi-User System"),
    ),
    "full": (
        ("root-p5", "mmcblk0p5"),
        ("local-fs", "Reached target Local File Systems"),
        ("multi-user", "Reached target Multi-User System"),
        ("weston", "weston-headless"),
        ("apphome-bus", "com.lge.hkmc.avn.appfunc.AppHomeScreen"),
    ),
}


def read_text(path: Path) -> str:
    if path.is_file():
        return path.read_text(errors="replace")
    return ""


def check_run(run_dir: Path, level: str) -> dict:
    if level not in LEVELS:
        raise ValueError(f"unknown level: {level}")
    serial = read_text(run_dir / "serial.log")
    checks = {name: marker in serial for name, marker in LEVELS[level]}
    return {
        "run_dir": str(run_dir),
        "level": level,
        "checks": checks,
        "pass": all(checks.values()),
        "limitation": "serial evidence only; not stock hardware equivalence",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("--level", choices=tuple(LEVELS), default="esinit")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    if not args.run_dir.is_dir():
        parser.exit(1, f"run directory not found: {args.run_dir}\n")
    try:
        summary = check_run(args.run_dir, args.level)
    except ValueError as exc:
        parser.exit(1, f"{exc}\n")
    if args.json:
        print(json.dumps(summary, indent=2))
    else:
        for name, ok in summary["checks"].items():
            print(f"{name}: {'PASS' if ok else 'FAIL'}")
        print("PASS" if summary["pass"] else "FAIL")
    return 0 if summary["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
