#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Private fixture sockets only; never bind the production endpoint."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src/carplay"
with tempfile.TemporaryDirectory(prefix="cp-spp-socket-", dir="/tmp/opencode") as tmp:
    directory = Path(tmp)
    fixture = directory / "fixture"
    fixture.mkdir(mode=0o700)
    hook_prototype = directory / "hook-prototype.h"
    hook_prototype.write_text("void cp_spp_rx_hook(void *event);\n")
    common = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
              "-pedantic", "-Wshadow", "-Wstrict-prototypes", "-Wmissing-prototypes",
              "-I", str(SRC)]
    # Production compilation is separate, with no test-only path/uid override.
    subprocess.run(common + ["-O2", "-c", str(SRC / "cp_spp_socket.c"),
                             "-o", str(directory / "production.o")], check=True)
    for name, flags in [("normal", ["-O2"]),
                        ("sanitizer", ["-O1", "-g", "-fsanitize=address,undefined",
                                       "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"])]:
        binary = directory / name
        subprocess.run(common + flags + ["-DCP_SPP_SOCKET_TESTING",
                       f'-DCP_SPP_RX_SOCKET_PATH="{fixture / "rx.sock"}"',
                       "-include", str(hook_prototype),
                       str(SRC / "cp_spp_socket.c"),
                       str(SRC / "cp_spp_rx_hook.c"),
                       str(ROOT / "tests/test_carplay_spp_socket.c"),
                       "-o", str(binary)], check=True)
        subprocess.run([str(binary), str(fixture)], check=True)
