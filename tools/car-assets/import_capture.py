#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Ioniq 5 Gen5W_L, firmware NE_E.EUR.S5W_L.001.001.260128 (EU).
"""Verify and copy a car capture into a local, gitignored directory.

The capture is expected to contain meta/SHA256SUMS. The output may contain
private and OEM data and must never be committed. This tool only copies; it
does not extract assets or read target data into the repository.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
from pathlib import Path


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_sums(path: Path) -> list[tuple[str, str]]:
    entries = []
    for line in path.read_text(errors="replace").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 1)
        if len(parts) != 2:
            raise ValueError(f"bad SHA256SUMS line: {line!r}")
        digest, name = parts
        if len(digest) != 64:
            raise ValueError(f"bad digest in SHA256SUMS: {digest!r}")
        entries.append((digest, name))
    if not entries:
        raise ValueError("SHA256SUMS has no entries")
    return entries


def safe_relative(name: str) -> Path:
    if not name or name.strip() in (".", "./"):
        raise ValueError(f"unsafe capture path: {name!r}")
    p = Path(name)
    if p.is_absolute() or any(part == ".." for part in p.parts):
        raise ValueError(f"unsafe capture path: {name}")
    if not p.parts:
        raise ValueError(f"unsafe capture path: {name!r}")
    return p


def copy_capture(source: Path, dest: Path) -> dict:
    if not source.is_dir():
        raise ValueError(f"capture source is not a directory: {source}")
    sums = source / "meta" / "SHA256SUMS"
    if not sums.is_file():
        raise ValueError(f"missing capture manifest: {sums}")
    if dest.exists():
        raise ValueError(f"destination already exists: {dest}")

    verified = 0
    allowed: set[str] = set()
    for digest, name in parse_sums(sums):
        rel = safe_relative(name)
        target = source / rel
        if not target.is_file():
            raise ValueError(f"manifest target missing: {name}")
        actual = sha256_file(target)
        if actual != digest:
            raise ValueError(f"hash mismatch: {name}")
        allowed.add(rel.as_posix())
        verified += 1

    os.umask(0o077)
    dest.mkdir(parents=True, mode=0o700)

    for entry in source.rglob("*"):
        if entry.is_symlink():
            raise ValueError(f"refusing symlink in capture: {entry}")
        if entry.is_dir():
            continue
        if not entry.is_file():
            raise ValueError(f"refusing non-regular capture entry: {entry}")
        rel = entry.relative_to(source)
        rel_name = rel.as_posix()
        manifest_exception = (
            (rel.parent == Path("meta") and rel.name.startswith("SHA256SUMS"))
            or rel_name == "meta/capture.log")
        if rel_name not in allowed and not manifest_exception:
            raise ValueError(f"unlisted capture file: {rel_name}")
        target = dest / rel
        target.parent.mkdir(parents=True, mode=0o700, exist_ok=True)
        shutil.copy2(entry, target)
        target.chmod(0o600)
    for directory in [dest, *[p for p in dest.rglob("*") if p.is_dir()]]:
        directory.chmod(0o700)

    total = sum(1 for _ in dest.rglob("*") if _.is_file())
    return {"source": str(source), "dest": str(dest), "files": total,
            "verified": verified, "status": "local-only; never commit"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("dest", type=Path)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    try:
        result = copy_capture(args.source, args.dest)
        if args.json:
            print(json.dumps(result, indent=2))
        else:
            print(f"imported {result['files']} files -> {result['dest']}")
            print("local-only; never commit")
    except (ValueError, OSError) as exc:
        parser.exit(1, f"capture import failed: {exc}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
