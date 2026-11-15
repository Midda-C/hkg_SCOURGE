#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Test the staged C1 snapshot, never substitute unstaged protocol fixes.

This source-only gate extracts an explicit allowlist of first-party index
blobs into a private temporary tree. It neither changes the index/worktree nor
executes OEM code. Run after staging C1 and before requesting review/committing.
"""
from pathlib import Path
import argparse
import json
import os
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[3]
BASE = Path("packages/wifi1b")
FILES = tuple(BASE / path for path in (
    "src/carplay/cp_wire.h", "src/carplay/cp_wire.c",
    "src/carplay/cp_bootstrap.h", "src/carplay/cp_bootstrap.c",
    "src/carplay/cp_link.h", "src/carplay/cp_link.c",
    "tests/test_carplay_bootstrap.c", "tests/test_carplay_bootstrap.py",
    "tests/test_carplay_link.c", "tests/test_carplay_link.py",
))
TESTS = ("test_carplay_bootstrap.py", "test_carplay_link.py")


def git(*args: str) -> bytes:
    return subprocess.check_output(["git", "-C", str(REPO), *args])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--expect-tree", help="refuse any other staged tree")
    args = parser.parse_args()
    tree = git("write-tree").decode("ascii").strip()
    if args.expect_tree is not None and tree != args.expect_tree:
        parser.error(f"index tree {tree} differs from expected {args.expect_tree}")
    # Read from the frozen tree, not :path, so a concurrent index edit cannot
    # silently mix two snapshots. A changed index still invalidates the gate.
    with tempfile.TemporaryDirectory(prefix="carplay-index-", dir="/tmp/opencode") as tmp:
        root = Path(tmp)
        for path in FILES:
            destination = root / path
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(git("show", f"{tree}:{path.as_posix()}"))
        environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        results = []
        for test in TESTS:
            result = subprocess.run(
                ["python3", str(root / BASE / "tests" / test)],
                cwd=root, env=environment, check=False, timeout=180,
            )
            results.append({"test": test, "returncode": result.returncode})
            if result.returncode != 0:
                print(json.dumps({"tree": tree, "status": "FAIL", "tests": results}))
                return 1
        current = git("write-tree").decode("ascii").strip()
        if current != tree:
            print(json.dumps({"tree": tree, "status": "INDEX_CHANGED", "current": current}))
            return 1
        print(json.dumps({"tree": tree, "status": "PASS", "tests": results}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
