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
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
MAGIC = b"\x63\x82\x53\x63"
SERVER = "10.42.0.1"


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, check=True, capture_output=True, text=True,
                          **kwargs)


def nsenter(pid: int, command: list[str], **kwargs) -> subprocess.CompletedProcess:
    return run(["nsenter", "-t", str(pid), "-n", "--", *command], **kwargs)


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
    rp_all = Path("/proc/sys/net/ipv4/conf/all/rp_filter")
    rp_lo = Path("/proc/sys/net/ipv4/conf/lo/rp_filter")
    before_rp = (rp_all.read_text() if rp_all.exists() else "",
                 rp_lo.read_text() if rp_lo.exists() else "")
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
        after_rp = (rp_all.read_text() if rp_all.exists() else "",
                    rp_lo.read_text() if rp_lo.exists() else "")
        assert before_rp == after_rp, (before_rp, after_rp)
    finally:
        if client.poll() is None:
            client.kill()
            client.wait()
        sock.close()
    print("PASS: DHCP client sends the same option 61 in DISCOVER and REQUEST")


def route_lines() -> list[str]:
    return [line for line in subprocess.run(
        ["ip", "-4", "route", "show"], capture_output=True, text=True).stdout.splitlines()
        if line.startswith("default")]


def setup_modem_default(metric: int) -> None:
    subprocess.run(["ip", "link", "add", "modem0", "type", "dummy"],
                   check=False, capture_output=True)
    subprocess.run(["ip", "link", "set", "modem0", "up"], check=False,
                   capture_output=True)
    subprocess.run(["ip", "-4", "addr", "replace", "192.0.2.1/24",
                    "dev", "modem0"], check=False, capture_output=True)
    subprocess.run(["ip", "-4", "route", "del", "default", "dev", "modem0"],
                   check=False, capture_output=True)
    run(["ip", "-4", "route", "add", "default", "via", "192.0.2.2",
         "dev", "modem0", "metric", str(metric)])


def owned_route_lines() -> list[str]:
    return [line for line in route_lines() if " proto 66 " in line]


def ctest_json(binary: Path, *args: str, check: bool = True) -> dict:
    result = subprocess.run([str(binary), *args], capture_output=True,
                            text=True)
    if check:
        assert result.returncode == 0, (result.returncode, result.stdout,
                                        result.stderr)
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError:
        return {"ok": False, "raw": result.stdout, "stderr": result.stderr}


def test_client_apply(binary: Path, ctest: Path, env: dict) -> None:
    apply_env = dict(env)
    apply_env.pop("WIFI1B_DHCP_TEST_NOAPPLY", None)
    setup_modem_default(100)
    # The client verifies the connected route. A loopback address does not
    # create one, so seed the isolated namespace with the equivalent route.
    subprocess.run(["ip", "-4", "route", "replace", "10.42.0.0/24", "dev", "lo"],
                   check=True, capture_output=True)
    before_defaults = route_lines()
    assert any("metric 100" in line and "modem0" in line
               for line in before_defaults), before_defaults
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
        assert parse(request)[2].get(61) == b"\x01" + mac
        sock.sendto(reply(xid, mac, 5, "10.42.0.10"), client_addr)
        out, err = client.communicate(timeout=10)
        assert client.returncode == 0, (out, err)
        assert json.loads(out)["ok"] is True
        addr = subprocess.run(["ip", "-o", "-4", "addr", "show", "dev", "lo"],
                              capture_output=True, text=True).stdout
        assert "10.42.0.10/24" in addr, addr
        connected = subprocess.run(["ip", "-4", "route", "show", "dev", "lo"],
                                   capture_output=True, text=True).stdout
        assert "10.42.0.0/24" in connected, connected
        after_defaults = route_lines()
        assert all(line in after_defaults for line in before_defaults), (
            before_defaults, after_defaults)
        owned = owned_route_lines()
        assert len(owned) == 1, owned
        assert "via 10.42.0.1" in owned[0] and "dev lo" in owned[0], owned
        assert "metric 101" in owned[0], owned
        assert "metric 100" in " ".join(before_defaults)
    finally:
        if client.poll() is None:
            client.kill()
            client.wait()
        sock.close()
    cleaned = ctest_json(ctest, "--route-cleanup")
    assert cleaned["ok"] is True
    assert not owned_route_lines()
    assert all(line in route_lines() for line in before_defaults)
    print("PASS: DHCP client verifies address/readback and installs only an "
          "owned lower-priority default route")


def test_renew_rebind(ctest: Path, env: dict) -> None:
    mac = "02:00:11:22:33:44"
    for step in ("renew", "rebind"):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("127.0.0.1", 6870))
        sock.settimeout(8)
        client = subprocess.Popen(
            [str(ctest), "--renew-client", step, "10.42.0.10",
             "127.0.0.1", mac], env=env,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            request, client_addr = sock.recvfrom(1500)
            xid = struct.unpack("!I", request[4:8])[0]
            opts = parse(request)[2]
            assert socket.inet_ntoa(request[12:16]) == "10.42.0.10", (
                step, request[12:16])
            assert opts[53] == b"\x03", opts
            assert opts.get(61) == b"\x01" + bytes.fromhex(mac.replace(":", "")), (
                step, opts)
            assert 50 not in opts and 54 not in opts, (step, opts)
            sock.sendto(reply(xid, bytes.fromhex(mac.replace(":", "")), 5,
                              "10.42.0.10"), client_addr)
            out, err = client.communicate(timeout=10)
            assert client.returncode == 0, (step, out, err)
            assert json.loads(out)["ok"] is True, (step, out)
        finally:
            if client.poll() is None:
                client.kill()
                client.wait()
            sock.close()
    print("PASS: RENEW/REBIND requests carry ciaddr and the same client id")


def test_route_policy(ctest: Path) -> None:
    # Existing lower metric -> owned route is strictly higher.
    result = ctest_json(ctest, "--route-install", "10.42.0.1", "lo")
    assert result["ok"] is True and result["metric"] == 101, result
    assert any("via 10.42.0.1" in l and "metric 101" in l
               and "proto 66" in l for l in route_lines())
    assert any("via 192.0.2.2" in l and "metric 100" in l
               for l in route_lines())

    # Lease gateway change removes the old owned route and installs the new.
    result = ctest_json(ctest, "--route-install", "10.42.0.2", "lo")
    assert result["ok"] is True and result["metric"] == 101, result
    owned = owned_route_lines()
    assert len(owned) == 1 and "via 10.42.0.2" in owned[0], owned
    assert "via 10.42.0.1" not in " ".join(owned)

    # Explicit cleanup removes only our route; foreign stays unchanged.
    assert ctest_json(ctest, "--route-cleanup")["ok"] is True
    assert not owned_route_lines()
    assert any("via 192.0.2.2" in l and "metric 100" in l
               for l in route_lines())

    # Existing higher metric -> owned route is still above it.
    subprocess.run(["ip", "-4", "route", "del", "default", "dev", "modem0"],
                   check=False, capture_output=True)
    run(["ip", "-4", "route", "add", "default", "via", "192.0.2.2",
         "dev", "modem0", "metric", "500"])
    result = ctest_json(ctest, "--route-install", "10.42.0.1", "lo")
    assert result["ok"] is True and result["metric"] == 501, result
    assert ctest_json(ctest, "--route-cleanup")["ok"] is True
    assert any("metric 500" in l and "modem0" in l for l in route_lines())

    # No existing default -> bounded high fallback metric, never 0.
    subprocess.run(["ip", "-4", "route", "del", "default", "dev", "modem0"],
                   check=False, capture_output=True)
    assert not route_lines()
    result = ctest_json(ctest, "--route-install", "10.42.0.1", "lo")
    assert result["ok"] is True and result["metric"] == 1000, result
    assert ctest_json(ctest, "--route-cleanup")["ok"] is True

    # Installation failure is false and leaves no owned ledger/route.
    failure = ctest_json(ctest, "--route-install", "203.0.113.1", "lo",
                         check=False)
    assert failure["ok"] is False, failure
    assert not owned_route_lines()

    # Restore the lower foreign default for the raw-client route assertion.
    setup_modem_default(100)
    print("PASS: owned default-route metric, gateway change and cleanup policy")


def _checksum(data: bytes) -> int:
    if len(data) % 2:
        data += b"\x00"
    total = sum(struct.unpack(f"!{len(data)//2}H", data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def raw_dhcp_reply(xid: int, mac: bytes, msgtype: int, yiaddr: str,
                   server: str, client_port: int, server_port: int) -> bytes:
    fixed = struct.pack("!BBBBIHHIIII16s64s128s", 2, 1, 6, 0, xid, 0,
                        0x8000, 0, int.from_bytes(socket.inet_aton(yiaddr), "big"),
                        int.from_bytes(socket.inet_aton(server), "big"), 0,
                        mac + b"\0" * 10, b"\0" * 64, b"\0" * 128)
    opts = MAGIC + bytes([53, 1, msgtype, 54, 4]) + socket.inet_aton(server)
    opts += bytes([1, 4]) + socket.inet_aton("255.255.255.0")
    opts += bytes([3, 4]) + socket.inet_aton(server)
    opts += bytes([51, 4]) + struct.pack("!I", 3600)
    opts += bytes([58, 4]) + struct.pack("!I", 1800)
    opts += bytes([59, 4]) + struct.pack("!I", 3150)
    opts += bytes([61, 7, 1]) + mac + b"\xff"
    dhcp = fixed + opts
    udp_len = 8 + len(dhcp)
    udp = struct.pack("!HHHH", server_port, client_port, udp_len, 0) + dhcp
    pseudo = (socket.inet_aton(server) + socket.inet_aton("255.255.255.255") +
              b"\0\x11" + struct.pack("!H", udp_len))
    udp_sum = _checksum(pseudo + udp)
    udp = struct.pack("!HHHH", server_port, client_port, udp_len, udp_sum) + dhcp
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 1, 0x4000,
                     64, 17, 0, socket.inet_aton(server),
                     socket.inet_aton("255.255.255.255"))
    ip = ip[:10] + struct.pack("!H", _checksum(ip)) + ip[12:]
    return ip + udp


def test_raw_client(ctest: Path, env: dict, tmp_path: Path) -> None:
    if shutil.which("nsenter") is None or shutil.which("unshare") is None:
        print("SKIP raw AF_PACKET client: nsenter/unshare unavailable")
        return
    run(["ip", "link", "add", "wlan0", "type", "veth", "peer", "name", "cpeer0"])
    peer = subprocess.Popen(
        ["unshare", "-n", "sh", "-c", "ip link set lo up; exec sleep 300"],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    try:
        own_netns = os.readlink("/proc/self/ns/net")
        deadline = time.monotonic() + 5
        while True:
            if peer.poll() is not None:
                raise AssertionError(f"peer namespace exited: {peer.stderr.read()}")
            try:
                if os.readlink(f"/proc/{peer.pid}/ns/net") != own_netns:
                    break
            except OSError:
                pass
            if time.monotonic() >= deadline:
                raise AssertionError("peer network namespace did not appear")
            time.sleep(0.05)
        run(["ip", "link", "set", "cpeer0", "netns", str(peer.pid)])
        run(["ip", "link", "set", "lo", "up"])
        run(["ip", "link", "set", "wlan0", "up"])
        nsenter(peer.pid, ["ip", "link", "set", "lo", "up"])
        deadline = time.monotonic() + 5
        while True:
            if peer.poll() is not None:
                raise AssertionError(f"peer namespace exited: {peer.stderr.read()}")
            try:
                nsenter(peer.pid, ["ip", "link", "set", "cpeer0", "up"])
                link = nsenter(peer.pid, ["ip", "link", "show", "cpeer0"]).stdout
                break
            except subprocess.CalledProcessError as error:
                if time.monotonic() >= deadline:
                    print("nsenter failed:", error.stderr)
                    print("peer poll:", peer.poll(),
                          "stderr:", peer.stderr.read() if peer.poll() is not None else "")
                    raise
                time.sleep(0.05)
        peer_mac = next(line.split()[1] for line in link.splitlines()
                        if "link/ether" in line)
        subprocess.run(["ip", "-4", "route", "del", "10.42.0.0/24", "dev", "lo"],
                       check=False, capture_output=True)
        mac = "02:00:11:22:33:44"
        raw_env = dict(env)
        raw_env.pop("WIFI1B_DHCP_TEST_NOAPPLY", None)
        raw_env.update({
            "WIFI1B_DHCP_TEST_LOOPBACK": "1",
            "WIFI1B_DHCP_TEST_IFACE": "wlan0",
            "WIFI1B_DHCP_TEST_FORCE_RAW": "1",
            "WIFI1B_DHCP_TEST_PORT": "6870",
            "WIFI1B_DHCP_TEST_CLIENT_PORT": "6869",
        })
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("127.0.0.1", 6870))
        sock.settimeout(10)
        client = subprocess.Popen([str(ctest), "--raw-client", mac],
                                  env=raw_env, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True)
        try:
            discover, client_addr = sock.recvfrom(1500)
            xid = struct.unpack("!I", discover[4:8])[0]
            chaddr = discover[28:34]
            assert chaddr == bytes.fromhex(mac.replace(":", ""))
            offer = raw_dhcp_reply(xid, chaddr, 2, "10.42.0.10",
                                   "10.42.0.1", 6869, 6870)
            frame = (b"\xff" * 6 + bytes.fromhex(peer_mac.replace(":", "")) +
                     b"\x08\x00" + offer)
            frame_file = tmp_path / "raw-offer.bin"
            frame_file.write_bytes(frame)
            nsenter(peer.pid, [sys.executable, "-c", (
                "import socket,sys\n"
                "frame=open(sys.argv[1],'rb').read()\n"
                "s=socket.socket(socket.AF_PACKET,socket.SOCK_RAW,socket.htons(0x0800))\n"
                "s.bind(('cpeer0',0))\n"
                "s.send(frame)\n"), str(frame_file)])
            request, client_addr = sock.recvfrom(1500)
            opts = parse(request)[2]
            assert opts.get(61) == b"\x01" + chaddr
            ack = raw_dhcp_reply(xid, chaddr, 5, "10.42.0.10",
                                 "10.42.0.1", 6869, 6870)
            frame = (b"\xff" * 6 + bytes.fromhex(peer_mac.replace(":", "")) +
                     b"\x08\x00" + ack)
            frame_file.write_bytes(frame)
            nsenter(peer.pid, [sys.executable, "-c", (
                "import socket,sys\n"
                "frame=open(sys.argv[1],'rb').read()\n"
                "s=socket.socket(socket.AF_PACKET,socket.SOCK_RAW,socket.htons(0x0800))\n"
                "s.bind(('cpeer0',0))\n"
                "s.send(frame)\n"), str(frame_file)])
            out, err = client.communicate(timeout=15)
            assert client.returncode == 0, (out, err)
            parsed = json.loads(out)
            assert parsed["ok"] is True and parsed["route_ok"] is True, parsed
            addr = subprocess.run(["ip", "-o", "-4", "addr", "show", "dev", "wlan0"],
                                  capture_output=True, text=True).stdout
            assert "10.42.0.10/24" in addr, addr
            owned = owned_route_lines()
            assert len(owned) == 1 and "via 10.42.0.1" in owned[0], owned
            assert "metric 101" in owned[0], owned
            assert ctest_json(ctest, "--route-cleanup")["ok"] is True
            assert not owned_route_lines()
            assert any("metric 100" in l and "modem0" in l
                       for l in route_lines())
        finally:
            if client.poll() is None:
                client.kill()
                client.wait()
            sock.close()
    finally:
        peer.terminate()
        try:
            peer.wait(timeout=3)
        except subprocess.TimeoutExpired:
            peer.kill()
    print("PASS: AF_PACKET raw DHCP fallback drives a full lease without sysctls")


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

        def expiry(text: str, mac: str) -> int:
            for line in text.splitlines():
                if line.startswith(mac):
                    return int(line.split()[2])
            raise AssertionError(f"missing lease {mac}")

        # The server persists immediately after sending the ACK; allow that
        # short window before asserting the renewal hit the file.
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            after = leases.read_text()
            try:
                if expiry(after, "02:00:00:00:00:0a") > expiry(
                        before, "02:00:00:00:00:0a") + 3000:
                    break
            except AssertionError:
                pass
            time.sleep(0.05)
        else:
            raise AssertionError((before, after))
        assert "02:00:00:00:00:0a 10.42.0.10" in after, after
        assert "02:00:00:00:00:0b 10.42.0.11" in after, after
        assert "10.42.0.250" not in after, after
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
        ctest = tmp_path / "test_carplay_dhcp"
        subprocess.run([
            "gcc", "-O2", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-parameter", "-Wno-format-truncation",
            "-Wno-unused-function", "-Wno-unused-variable",
            "-Wno-unused-but-set-variable",
            "-DWIFI_TEST", f'-DAPP_DIR="{app}"', '-DWIFI_BUILD_ID="test"',
            "-static", "-o", str(binary),
            str(ROOT / "src/wifi1b_daemon.c")], check=True, timeout=90)
        subprocess.run([
            "gcc", "-O2", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-parameter", "-Wno-format-truncation",
            "-Wno-unused-function", "-Wno-unused-variable",
            "-Wno-unused-but-set-variable",
            "-DWIFI_TEST", f'-DAPP_DIR="{app}"', '-DWIFI_BUILD_ID="test"',
            "-static", "-o", str(ctest),
            str(ROOT / "tests/test_carplay_dhcp.c")], check=True, timeout=90)
        run([str(ctest)])
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
        test_renew_rebind(ctest, env)
        if isolated:
            test_client_apply(binary, ctest, env)
            test_route_policy(ctest)
            test_raw_client(ctest, env, tmp_path)
        else:
            print("SKIP apply/readback/route/raw checks: requires the network namespace")

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
