#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Content identity for Wifi1b first-party inputs."""
import hashlib
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def build_id():
    paths = [ROOT / "src/Makefile", ROOT.parent / "common/radio_owner.h",
             ROOT.parent / "common/install_io.sh",
             ROOT.parent / "common/runtime_cleanup.sh",
             ROOT / "artifacts/accepted-qml-legacy.md5",
             ROOT / "artifacts/systemd/60-connman-nodevice.conf"]
    for directory, suffixes in [("src", {".c", ".h", ".inc"}),
                                ("scripts", {".py", ".sh"}),
                                ("artifacts/systemd", {".service"})]:
        paths.extend(p for p in (ROOT / directory).rglob("*") if p.suffix in suffixes)
    qml = ROOT / "artifacts/qml/AppHomeScreen/qml_eco3.0"
    paths.append(qml / "HomeScreen/Wifi1bPage.qml")
    paths.extend((qml / "component").glob("Wifi1b*.qml"))
    digest = hashlib.sha256()
    for path in sorted(set(paths)):
        digest.update(os.path.relpath(path, ROOT).encode() + b"\0" + path.read_bytes() + b"\0")
    return digest.hexdigest()


if __name__ == "__main__":
    print(build_id())
