#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Ioniq 5 Gen5W_L, firmware NE_E.EUR.S5W_L.001.001.260128 (EU).
"""Install a local extracted asset tree into a VM host share or workspace.

This copies extracted assets to a new local directory. It never modifies a
vehicle, a backing image, or committed source. For a running compat VM, pass
the target directory to launch.py --share and stage it inside the guest.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
from manifest import validate_manifest  # noqa: E402


def hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def install(assets: Path, target: Path, force: bool = False) -> dict:
    if not assets.is_dir():
        raise ValueError(f"assets directory not found: {assets}")
    if target.exists() and not force:
        raise ValueError(f"target already exists: {target}")
    if target.exists() and not target.is_dir():
        raise ValueError(f"target is not a directory: {target}")

    manifest = assets / "manifest.json"
    copies: list[tuple[Path, Path]] = []
    if manifest.is_file():
        data = json.loads(manifest.read_text())
        validate_manifest(data)
        for asset in data["assets"]:
            rel = Path(asset["path"])
            source = assets / rel
            if not source.is_file():
                raise ValueError(f"manifest asset missing: {rel}")
            if source.is_symlink():
                raise ValueError(f"refusing symlink asset: {rel}")
            if source.stat().st_size != asset["size"]:
                raise ValueError(f"manifest size mismatch: {rel}")
            if hash_file(source) != asset["sha256"]:
                raise ValueError(f"manifest hash mismatch: {rel}")
            copies.append((source, rel))
    else:
        for source in assets.rglob("*"):
            if source.is_symlink():
                raise ValueError(f"refusing symlink asset: {source}")
            if source.is_dir():
                continue
            if not source.is_file():
                raise ValueError(f"refusing non-regular asset: {source}")
            copies.append((source, source.relative_to(assets)))

    if target.exists():
        shutil.rmtree(target)
    os.umask(0o077)
    target.mkdir(parents=True, mode=0o700)
    for source, rel in copies:
        dest = target / rel
        dest.parent.mkdir(parents=True, mode=0o700, exist_ok=True)
        shutil.copy2(source, dest)
        dest.chmod(0o600)
    for directory in [target, *[p for p in target.rglob("*") if p.is_dir()]]:
        directory.chmod(0o700)

    count = sum(1 for entry in target.rglob("*") if entry.is_file())
    return {"assets": str(assets), "target": str(target), "files": count,
            "status": "local-only; not committed"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("assets", type=Path)
    parser.add_argument("target", type=Path)
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    try:
        result = install(args.assets, args.target, args.force)
        if args.json:
            print(json.dumps(result, indent=2))
        else:
            print(f"installed {result['files']} assets -> {result['target']}")
            print("local-only; pass this directory to launch.py --share")
    except (ValueError, OSError) as exc:
        parser.exit(1, f"asset install failed: {exc}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
