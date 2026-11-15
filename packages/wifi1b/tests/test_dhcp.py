#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Protocol test for the Wifi1b DHCP server on high test ports."""
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
MAGIC = b"\x63\x82\x53\x63"
SERVER = "10.42.0.1"


def packet(xid, mac, msgtype, requested=None):
    fixed = struct.pack("!BBBBIHHIIII16s64s128s", 1, 1, 6, 0, xid, 0, 0x8000,
                        0, 0, 0, 0, mac + b"\0" * 10, b"\0" * 64, b"\0" * 128)
    opts = MAGIC + bytes([53, 1, msgtype, 55, 3, 1, 3, 6])
    if requested:
        opts += bytes([50, 4]) + socket.inet_aton(requested)
    opts += b"\xff"
    return fixed + opts


def parse(data):
    op, htype, hlen, hops, xid, secs, flags, ci, yi, si, gi = struct.unpack(
        "!BBBBIHHIIII", data[:28])
    opts = data[236:]
    assert opts[:4] == MAGIC
    out = {}
    i = 4
    while i < len(opts):
        c = opts[i]
        i += 1
        if c == 0:
            continue
        if c == 255:
            break
        l = opts[i]
        i += 1
        out[c] = opts[i:i+l]
        i += l
    return op, socket.inet_ntoa(struct.pack("!I", yi)), out


with tempfile.TemporaryDirectory(prefix="wifi1b-dhcp-", dir="/tmp/opencode") as d:
    tmp = Path(d)
    app = tmp / "app"
    app.mkdir()
    binary = tmp / "wifi1b_daemon_dhcp"
    subprocess.run(["gcc", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter",
                    "-Wno-format-truncation", "-Wno-unused-function",
                    "-Wno-unused-variable", "-Wno-unused-but-set-variable",
                    "-DWIFI_TEST", f'-DAPP_DIR="{app}"', '-DWIFI_BUILD_ID="test"',
                    "-static", "-o", str(binary), str(ROOT / "src/wifi1b_daemon.c")],
                   check=True, timeout=90)
    env = dict(os.environ, WIFI1B_DHCP_TEST_PORT="6767",
               WIFI1B_DHCP_TEST_CLIENT_PORT="6768", WIFI1B_DHCP_TEST_LOOPBACK="1")
    proc = subprocess.Popen([str(binary), "--dhcpd"], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("127.0.0.1", 6768))
        sock.settimeout(3)
        mac = bytes.fromhex("020011223344")
        xid = 0x12345678
        for _ in range(10):
            sock.sendto(packet(xid, mac, 1), ("127.0.0.1", 6767))
            try:
                data, _ = sock.recvfrom(1500)
                break
            except socket.timeout:
                time.sleep(0.2)
        else:
            raise AssertionError("no DHCP OFFER")
        op, yi, opts = parse(data)
        assert op == 2 and opts[53] == b"\x02", (op, opts)
        assert yi == "10.42.0.10", yi
        assert opts[54] == socket.inet_aton(SERVER)
        assert opts[3] == socket.inet_aton(SERVER)
        # REQUEST the offered address.
        req = packet(xid, mac, 3, requested=yi)
        sock.sendto(req, ("127.0.0.1", 6767))
        data, _ = sock.recvfrom(1500)
        op, yi2, opts2 = parse(data)
        assert op == 2 and opts2[53] == b"\x05" and yi2 == yi
        leases = (app / "dhcp.leases").read_text()
        assert "02:00:11:22:33:44" in leases and "10.42.0.10" in leases
        print("PASS: Wifi1b DHCP offers, ACKs and records a lease")
        sock.close()  # free the client port for the built-in client

        # Built-in client against the same server: DISCOVER/OFFER/REQUEST/ACK.
        # NOAPPLY keeps the client from running ip(8) on the host.
        env2 = dict(env, WIFI1B_DHCP_TEST_NOAPPLY="1", WIFI1B_DHCP_TEST_IFACE="lo")
        out = subprocess.run([str(binary), "--dhcp-client"], env=env2,
                             capture_output=True, text=True, timeout=30).stdout
        d = __import__("json").loads(out)
        # .10 is still leased to the first client's MAC, so the server hands
        # out the next free pool address; any pool address proves the exchange.
        assert d["ok"] is True and d["ip"].startswith("10.42.0."), d
        leases2 = (app / "dhcp.leases").read_text()
        assert "00:00:00:00:00:00" in leases2 and d["ip"] in leases2
        print("PASS: Wifi1b built-in DHCP client completes the exchange")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
