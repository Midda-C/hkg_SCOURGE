#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""DHCP release-gate tests for Wifi1b.

Covers the client identifier in DISCOVER *and* REQUEST, address/mask helpers,
and DHCP server lease persistence/renewal. All sockets are loopback-only and
the whole test re-executes in a disposable network namespace when available.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
MAGIC = b"\x63\x82\x53\x63"
SERVER = "10.42.0.1"


def packet(xid: int, mac: bytes, msgtype: int, requested: str | None = None,
           client_id: bytes | None = None) -> bytes:
    fixed = struct.pack("!BBBBIHHIIII16s64s128s", 1, 1, 6, 0, xid, 0,
                        0x8000, 0, 0, 0, 0, mac + b"\0" * 10,
                        b"\0" * 64, b"\0" * 128)
    opts = MAGIC + bytes([53, 1, msgtype, 55, 3, 1, 3, 6])
    if requested:
        opts += bytes([50, 4]) + socket.inet_aton(requested)
    if client_id:
        opts += bytes([61, len(client_id)]) + client_id
    opts += b"\xff"
    return fixed + opts


def parse(data: bytes):
    op, htype, hlen, hops, xid, secs, flags, ci, yi, si, gi = struct.unpack(
        "!BBBBIHHIIII", data[:28])
    opts = data[236:]
    assert opts[:4] == MAGIC
    out = {}
    i = 4
    while i < len(opts):
        code = opts[i]
        i += 1
        if code == 0:
            continue
        if code == 255:
            break
        length = opts[i]
        i += 1
        out[code] = opts[i:i + length]
        i += length
    return op, socket.inet_ntoa(struct.pack("!I", yi)), out


def reply(xid: int, mac: bytes, msgtype: int, yiaddr: str) -> bytes:
    fixed = struct.pack("!BBBBIHHIIII16s64s128s", 2, 1, 6, 0, xid, 0,
                        0x8000, 0, int.from_bytes(socket.inet_aton(yiaddr), "big"),
                        int.from_bytes(socket.inet_aton(SERVER), "big"), 0,
                        mac + b"\0" * 10, b"\0" * 64, b"\0" * 128)
    opts = MAGIC + bytes([53, 1, msgtype, 54, 4]) + \
        socket.inet_aton(SERVER) + bytes([51, 4]) + struct.pack("!I", 3600)
    opts += bytes([1, 4]) + socket.inet_aton("255.255.255.0")
    opts += bytes([3, 4]) + socket.inet_aton(SERVER) + b"\xff"
    return fixed + opts


def test_client_id(binary: Path, env: dict) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", 6870))
    sock.settimeout(8)
    client = subprocess.Popen([str(binary), "--dhcp-client"], env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True)
    try:
        discover, client_addr = sock.recvfrom(1500)
        op, _yi, opts1 = parse(discover)
        assert op == 1 and opts1[53] == b"\x01", opts1
        assert 61 in opts1, "DISCOVER omitted client identifier"
        xid = struct.unpack("!I", discover[4:8])[0]
        mac = discover[28:34]
        sock.sendto(reply(xid, mac, 2, "10.42.0.10"), client_addr)

        request, client_addr = sock.recvfrom(1500)
        op, _yi, opts2 = parse(request)
        assert op == 1 and opts2[53] == b"\x03", opts2
        assert 61 in opts2, "REQUEST omitted client identifier"
        assert opts1[61] == opts2[61], (opts1[61], opts2[61])
        assert opts2[61][0] == 1 and opts2[61][1:] == mac
        assert opts2.get(50) == socket.inet_aton("10.42.0.10")
        sock.sendto(reply(xid, mac, 5, "10.42.0.10"), client_addr)
        out, err = client.communicate(timeout=10)
        assert client.returncode == 0, (out, err)
        parsed = json.loads(out)
        assert parsed["ok"] is True and parsed["ip"] == "10.42.0.10", parsed
    finally:
        if client.poll() is None:
            client.kill()
            client.wait()
        sock.close()
    print("PASS: DHCP client sends the same option 61 in DISCOVER and REQUEST")


def test_client_apply(binary: Path, env: dict) -> None:
    apply_env = dict(env)
    apply_env.pop("WIFI1B_DHCP_TEST_NOAPPLY", None)
    before_routes = subprocess.run(["ip", "-4", "route", "show"],
                                   capture_output=True, text=True).stdout
    # The client verifies the connected route. A loopback address does not
    # create one, so seed the isolated namespace with the equivalent route.
    subprocess.run(["ip", "-4", "route", "add", "10.42.0.0/24", "dev", "lo"],
                   check=True, capture_output=True)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", 6870))
    sock.settimeout(8)
    client = subprocess.Popen([str(binary), "--dhcp-client"], env=apply_env,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True)
    try:
        discover, client_addr = sock.recvfrom(1500)
        xid = struct.unpack("!I", discover[4:8])[0]
        mac = discover[28:34]
        sock.sendto(reply(xid, mac, 2, "10.42.0.10"), client_addr)
        request, client_addr = sock.recvfrom(1500)
        sock.sendto(reply(xid, mac, 5, "10.42.0.10"), client_addr)
        _request = request
        out, err = client.communicate(timeout=10)
        assert client.returncode == 0, (out, err)
        assert json.loads(out)["ok"] is True
        addr = subprocess.run(["ip", "-o", "-4", "addr", "show", "dev", "lo"],
                              capture_output=True, text=True).stdout
        assert "10.42.0.10/24" in addr, addr
        route = subprocess.run(["ip", "-4", "route", "show", "dev", "lo"],
                               capture_output=True, text=True).stdout
        assert "10.42.0.0/24" in route, route
        after_routes = subprocess.run(["ip", "-4", "route", "show"],
                                      capture_output=True, text=True).stdout
        before_defaults = [l for l in before_routes.splitlines()
                           if l.startswith("default")]
        after_defaults = [l for l in after_routes.splitlines()
                          if l.startswith("default")]
        assert before_defaults == after_defaults, (before_defaults, after_defaults)
    finally:
        if client.poll() is None:
            client.kill()
            client.wait()
        sock.close()
    print("PASS: DHCP client verifies address/connected-route readback "
          "without touching default routes")


def start_server(binary: Path, app: Path, env: dict) -> subprocess.Popen:
    proc = subprocess.Popen([str(binary), "--dhcpd"], env=env,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    deadline = time.monotonic() + 5
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind(("127.0.0.1", 6872))
    finally:
        sock.close()
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise AssertionError("DHCP server exited during startup")
        time.sleep(0.1)
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.bind(("127.0.0.1", 6871))
            sock.close()
            return proc
        except OSError:
            sock.close()
            time.sleep(0.2)
    proc.kill()
    raise AssertionError("DHCP server did not open the test port")


def send_recv(sock: socket.socket, data: bytes) -> bytes:
    for _ in range(10):
        sock.sendto(data, ("127.0.0.1", 6871))
        try:
            reply_data, _ = sock.recvfrom(1500)
            return reply_data
        except socket.timeout:
            time.sleep(0.1)
    raise AssertionError("no DHCP reply")


def test_server_leases(binary: Path, app: Path, env: dict) -> None:
    now = int(time.time())
    leases = app / "dhcp.leases"
    leases.write_text(
        "# mac ip expires_unix\n"
        f"02:00:00:00:00:0a 10.42.0.10 {now + 120}\n"
        f"02:00:00:00:00:0b 10.42.0.11 {now - 10}\n"
        "02:00:00:00:00:0c 10.42.0.250 " + f"{now + 120}\n")
    server = start_server(binary, app, env)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", 6872))
    sock.settimeout(3)
    try:
        # The persisted .10 lease must not be offered to a different client.
        mac_b = bytes.fromhex("02000000000b")
        discover_b = packet(0x1001, mac_b, 1,
                            client_id=b"\x01" + mac_b)
        op, yi, opts = parse(send_recv(sock, discover_b))
        assert op == 2 and opts[53] == b"\x02" and yi == "10.42.0.11", (op, yi, opts)
        request_b = packet(0x1001, mac_b, 3, requested=yi,
                           client_id=b"\x01" + mac_b)
        op, yi2, opts2 = parse(send_recv(sock, request_b))
        assert op == 2 and opts2[53] == b"\x05" and yi2 == "10.42.0.11"

        # Renewal of the loaded .10 lease must refresh its lifetime and persist.
        before = leases.read_text()
        mac_a = bytes.fromhex("02000000000a")
        renew_a = packet(0x1002, mac_a, 3, requested="10.42.0.10",
                         client_id=b"\x01" + mac_a)
        op, yi3, opts3 = parse(send_recv(sock, renew_a))
        assert op == 2 and opts3[53] == b"\x05" and yi3 == "10.42.0.10"
        after = leases.read_text()
        assert "02:00:00:00:00:0a 10.42.0.10" in after, after
        assert "02:00:00:00:00:0b 10.42.0.11" in after, after
        assert "10.42.0.250" not in after, after

        def expiry(text: str, mac: str) -> int:
            for line in text.splitlines():
                if line.startswith(mac):
                    return int(line.split()[2])
            raise AssertionError(f"missing lease {mac}")

        assert expiry(after, "02:00:00:00:00:0a") > expiry(
            before, "02:00:00:00:00:0a") + 3000, (before, after)
    finally:
        sock.close()
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
    print("PASS: DHCP server loads persisted leases and refreshes renewals")


def inner(isolated: bool) -> int:
    if os.geteuid() == 0:
        subprocess.run(["ip", "link", "set", "lo", "up"], check=False,
                       capture_output=True)
    with tempfile.TemporaryDirectory(prefix="wifi1b-dhcp-gates-",
                                     dir="/tmp/opencode") as tmp:
        tmp_path = Path(tmp)
        app = tmp_path / "app"
        app.mkdir()
        binary = tmp_path / "wifi1b_daemon_dhcp"
        subprocess.run([
            "gcc", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter",
            "-Wno-format-truncation", "-Wno-unused-function",
            "-Wno-unused-variable", "-Wno-unused-but-set-variable",
            "-DWIFI_TEST", f'-DAPP_DIR="{app}"', '-DWIFI_BUILD_ID="test"',
            "-static", "-o", str(binary),
            str(ROOT / "src/wifi1b_daemon.c")], check=True, timeout=90)
        stub = tmp_path / "bin"
        stub.mkdir()
        (stub / "wpa_cli").write_text("#!/bin/sh\nexit 1\n")
        (stub / "wpa_cli").chmod(0o755)
        env = dict(os.environ)
        env["PATH"] = f"{stub}:{env.get('PATH', '')}"
        env.update({
            "WIFI1B_DHCP_TEST_LOOPBACK": "1",
            "WIFI1B_DHCP_TEST_IFACE": "lo",
            "WIFI1B_DHCP_TEST_NOAPPLY": "1",
            "WIFI1B_DHCP_TEST_PORT": "6870",
            "WIFI1B_DHCP_TEST_CLIENT_PORT": "6869",
        })
        test_client_id(binary, env)
        if isolated:
            test_client_apply(binary, env)
        else:
            print("SKIP apply/readback check: requires the network namespace")

        server_env = dict(env)
        server_env["WIFI1B_DHCP_TEST_PORT"] = "6871"
        server_env["WIFI1B_DHCP_TEST_CLIENT_PORT"] = "6872"
        test_server_leases(binary, app, server_env)
    return 0


def main() -> int:
    if len(sys.argv) > 1 and sys.argv[1] == "--inner":
        return inner(True)
    probe = subprocess.run(["unshare", "-rn", "true"], capture_output=True)
    if probe.returncode == 0:
        return subprocess.run(
            ["unshare", "-rn", sys.executable, str(Path(__file__).resolve()),
             "--inner"], check=False).returncode
    print("SKIP namespace isolation unavailable; running loopback-only sockets")
    return inner(False)


if __name__ == "__main__":
    raise SystemExit(main())
