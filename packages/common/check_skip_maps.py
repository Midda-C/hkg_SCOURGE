#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Gen5W_L EU 260128
"""Validate every staged package's skip.map/skip.services against its manifest.

The combo master skips a stage only when its curated installed files still
match the packaged bytes. This gate catches a map that points at a payload
file the manifest does not cover (a typo would silently weaken the check).
"""
from pathlib import Path
import sys

combo = Path(sys.argv[1])
stages = sorted(d.name for d in combo.iterdir() if (d / "stage.sh").is_file())
assert stages, "no stages staged"
for stage in stages:
    d = combo / stage
    assert (d / "skip.map").is_file(), f"{stage}: missing skip.map"
    assert (d / "skip.services").is_file(), f"{stage}: missing skip.services"
    manifest = set()
    for line in (d / "manifest.md5").read_text().splitlines():
        if not line.strip():
            continue
        name = line.split(maxsplit=1)[1].lstrip("*").removeprefix("./")
        manifest.add(name)
    entries = 0
    for line in (d / "skip.map").read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("!"):
            assert line[1:].startswith("/"), f"{stage}: !path must be absolute: {line}"
            continue
        if line.startswith(("?", "*")):
            line = line[1:]
        assert "|" in line, f"{stage}: malformed map line: {line}"
        rel, dst = line.split("|", 1)
        assert dst.startswith("/"), f"{stage}: destination must be absolute: {dst}"
        assert rel in manifest, f"{stage}: map payload file not in manifest: {rel}"
        entries += 1
    units = [u.strip() for u in (d / "skip.services").read_text().splitlines()]
    units = [u for u in units if u and not u.startswith("#")]
    assert units, f"{stage}: skip.services lists no unit"
    for unit in units:
        assert (d / "payload/systemd" / unit).is_file(), f"{stage}: unknown unit {unit}"
    print(f"PASS {stage}: {entries} mapped files, units {', '.join(units)}")
print(f"PASS: {len(stages)} stage skip maps match manifests and packaged units")
