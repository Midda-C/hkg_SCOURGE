#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Ioniq 5 Gen5W_L, firmware NE_E.EUR.S5W_L.001.001.260128 (EU).
"""Small manifest schema for extracted car assets.

The manifest is metadata only: relative path, class, size, sha256 and a
sensitive flag. It does not contain file contents or device identifiers.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

SCHEMA_VERSION = 1
ALLOWED_CLASSES = {"ui", "qml", "navi", "runtime", "update", "logs", "maps",
                   "partitions", "critical"}


def validate_manifest(data: dict) -> dict:
    if not isinstance(data, dict):
        raise ValueError("manifest must be an object")
    if data.get("schema_version") != SCHEMA_VERSION:
        raise ValueError("manifest schema_version must be 1")
    assets = data.get("assets")
    if not isinstance(assets, list):
        raise ValueError("manifest assets must be a list")
    for asset in assets:
        if not isinstance(asset, dict):
            raise ValueError("manifest asset must be an object")
        if asset.get("class") not in ALLOWED_CLASSES:
            raise ValueError(f"bad asset class: {asset.get('class')!r}")
        path = Path(asset.get("path", ""))
        if (not str(asset.get("path", "")).strip()
                or str(asset.get("path", "")).strip() in (".", "./")
                or path.is_absolute()
                or any(p == ".." for p in path.parts)
                or not path.parts):
            raise ValueError(f"unsafe asset path: {asset.get('path')!r}")
        if asset.get("sensitive") not in (True, False):
            raise ValueError("asset sensitive flag must be boolean")
        if not isinstance(asset.get("size"), int) or asset["size"] < 0:
            raise ValueError("asset size must be a nonnegative integer")
        digest = asset.get("sha256")
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ValueError("asset sha256 must be 64 lowercase hex characters")
    return data


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    args = parser.parse_args()
    try:
        data = json.loads(args.manifest.read_text())
        validate_manifest(data)
        print(f"manifest ok: {len(data['assets'])} assets")
    except (ValueError, OSError, json.JSONDecodeError) as exc:
        parser.exit(1, f"manifest failed: {exc}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
