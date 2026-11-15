#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Build and run the standalone cp_link C tests, plus source-policy checks.

Build artifacts live under /tmp/opencode only. The normal build treats
warnings as errors; the optional ASan/UBSan build re-runs the same binary
logic with runtime checking enabled.
"""
from __future__ import annotations

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src" / "carplay" / "cp_link.c"
HDR = ROOT / "src" / "carplay" / "cp_link.h"
TEST = ROOT / "tests" / "test_carplay_link.c"
BUILD_ROOT = Path("/tmp/opencode")

COMMON = [
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-pedantic",
    "-Wshadow",
    "-Wstrict-prototypes",
    "-Wmissing-prototypes",
]
INPUTS = [str(SRC), str(TEST)]


def compile_and_run(tmp: Path, name: str, extra: list[str], env_extra: dict[str, str]) -> None:
    binary = tmp / name
    cmd = [os.environ.get("CC", "cc"), *COMMON, *extra, "-o", str(binary), *INPUTS]
    subprocess.run(cmd, check=True, timeout=120)
    env = dict(os.environ)
    env.update(env_extra)
    subprocess.run([str(binary)], check=True, timeout=120, env=env)


def assert_source_policy() -> None:
    source = SRC.read_text()
    header = HDR.read_text()
    test = TEST.read_text()

    for path, text in ((SRC, source), (HDR, header), (TEST, test),
                       (Path(__file__), Path(__file__).read_text())):
        assert "SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors" in text, path
        assert "SPDX-License-Identifier: AGPL-3.0-only" in text, path

    # No dynamic allocation, sockets or threading in the engine.
    for forbidden in (
        r"\bmalloc\s*\(",
        r"\bcalloc\s*\(",
        r"\brealloc\s*\(",
        r"\bfree\s*\(",
        r"\bsocket\s*\(",
        r"\bconnect\s*\(",
        r"\bbind\s*\(",
        r"\blisten\s*\(",
        r"\baccept\s*\(",
        r"\bpthread_",
    ):
        assert not re.search(forbidden, source), forbidden
        assert not re.search(forbidden, header), forbidden

    # This module must not parse or synthesise CSM framing in code.
    assert "0x4040" not in source

    # Fixed wire facts and explicit bounds must be visible in the public API.
    for needle in (
        "0xFFu, 0x55u, 0x02u, 0x00u, 0xEEu, 0x10u",
        "CP_LINK_CONTROL_SYN 0x80u",
        "CP_LINK_CONTROL_ACK 0x40u",
        "CP_LINK_CONTROL_EAK 0x20u",
        "CP_LINK_CONTROL_RST 0x10u",
        "CP_LINK_MAX_PAYLOAD",
        "CP_LINK_MAX_OUTSTANDING",
        "CP_LINK_MAX_REORDER",
        "CP_LINK_OUTPUT_RESERVED",
    ):
        assert needle in source, needle

    for needle in (
        "transport-agnostic",
        "CP_LINK_EVENT_SESSION_DATA",
        "cp_link_output_peek",
        "cp_link_output_consume",
        "cp_link_sequence_distance",
        "cp_link_config_live_bluetooth",
        "CP_LINK_RESET_TX_BACKPRESSURE",
        "CP_LINK_ERR_BACKPRESSURE",
    ):
        assert needle in header, needle

    # The tests must contain fixed external vectors, not only engine round-trips.
    for vector in (
        "ff550200ee10",
        "ff5a001d80630000a7",
        "ff5a000940632a00d1",
        "ff5a000d40642a03c9010203fa",
        "0104100a0fa001f404030a00020b02010c01020d",
    ):
        assert vector in test, vector


def sanitizer_supported(compiler: str, tmp: Path) -> bool:
    probe = tmp / "sanprobe.c"
    probe.write_text("int main(void) { return 0; }\n")
    result = subprocess.run(
        [compiler, "-std=c11", "-fsanitize=address,undefined", "-o",
         str(tmp / "sanprobe"), str(probe)],
        capture_output=True,
        text=True,
        timeout=60,
    )
    return result.returncode == 0


def main() -> int:
    BUILD_ROOT.mkdir(parents=True, exist_ok=True)
    assert_source_policy()
    with tempfile.TemporaryDirectory(prefix="cp-link-", dir=str(BUILD_ROOT)) as d:
        tmp = Path(d)
        compile_and_run(tmp, "test_carplay_link", ["-O2"], {})
        print("PASS: cp_link normal strict build and tests")

        compiler = os.environ.get("CC", "cc")
        if sanitizer_supported(compiler, tmp):
            compile_and_run(
                tmp,
                "test_carplay_link_san",
                ["-O1", "-g", "-fno-omit-frame-pointer",
                 "-fsanitize=address,undefined"],
                {
                    "ASAN_OPTIONS": "detect_leaks=1:abort_on_error=1:strict_string_checks=1",
                    "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
                },
            )
            print("PASS: cp_link ASan/UBSan build and tests")
        else:
            print("SKIP: compiler sanitizer runtime unavailable")
    print("PASS: cp_link source policy and independent vectors")
    return 0


if __name__ == "__main__":
    sys.exit(main())
