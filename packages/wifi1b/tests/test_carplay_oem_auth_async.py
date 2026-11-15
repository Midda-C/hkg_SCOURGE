#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""First-party mock-helper tests for the nonblocking MFi async client.

The mock server speaks the published helper wire directly in Python.  No OEM
library, native helper, I2C, radio, real network or deployment is used.  All
outputs live under /tmp/opencode.
"""

from __future__ import annotations

import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src" / "carplay"
TEST_C = ROOT / "tests" / "test_carplay_oem_auth_async.c"
HEADER = SRC / "cp_oem_auth_async.h"
SOURCE = SRC / "cp_oem_auth_async.c"
PY_TEST = Path(__file__).resolve()
OUTPUT_ROOT = Path("/tmp/opencode")

MAGIC = 0x53434131
VERSION = 1
OP_CERT = 1
OP_SIGN = 2
REQ = ">IHHIII"
RESP = ">IHHIIII"


def checked(command: list[str], **kwargs) -> subprocess.CompletedProcess:
    result = subprocess.run(command, check=False, **kwargs)
    if result.returncode != 0:
        raise AssertionError(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return result


def run(command: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(command, check=False, **kwargs)


def parse_fields(output: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for line in output.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            fields[key] = value
    return fields


def response(op: int, request_id: int, payload: bytes = b"", *,
             status: int = 0, magic: int = MAGIC, version: int = VERSION,
             reserved: int = 0, payload_len: int | None = None,
             request_id_override: int | None = None,
             op_override: int | None = None) -> bytes:
    if payload_len is None:
        payload_len = len(payload)
    if request_id_override is not None:
        request_id = request_id_override
    if op_override is not None:
        op = op_override
    return struct.pack(RESP, magic, version, op, request_id, status,
                       payload_len, reserved) + payload


class MockServer(threading.Thread):
    def __init__(self, directory: Path, handler, connections: int = 1):
        super().__init__(daemon=True)
        self.directory = directory
        self.directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        os.chmod(self.directory, 0o700)
        self.path = directory / "a.sock"
        self.handler = handler
        self.connections = connections
        self.error: BaseException | None = None
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.server.bind(str(self.path))
        os.chmod(self.path, 0o600)
        self.server.listen(4)

    def run(self) -> None:
        try:
            for _ in range(self.connections):
                connection, _ = self.server.accept()
                connection.settimeout(2.0)
                try:
                    self.handler(connection)
                finally:
                    connection.close()
        except BaseException as error:  # reported in the main thread
            self.error = error
        finally:
            self.server.close()

    def close(self) -> None:
        try:
            self.server.close()
        except OSError:
            pass
        try:
            self.path.unlink()
        except OSError:
            pass


def read_request(connection: socket.socket) -> tuple[int, int, int, bytes]:
    data = connection.recv(4096)
    if len(data) < 20:
        raise AssertionError(f"short request: {data!r}")
    magic, version, op, request_id, payload_len, reserved = \
        struct.unpack(REQ, data[:20])
    if magic != MAGIC or version != VERSION or reserved != 0:
        raise AssertionError("bad request header")
    if op == OP_CERT and payload_len != 0:
        raise AssertionError("certificate request had payload")
    if op == OP_SIGN and payload_len not in (20, 32):
        raise AssertionError("signature request length")
    if len(data) != 20 + payload_len:
        raise AssertionError("request length mismatch")
    return op, request_id, payload_len, data[20:]


def run_case(binary: Path, root: Path, name: str, handler,
             *, mode: str = "cert", timeout_ms: int = 2000,
             connections: int = 1) -> dict[str, str]:
    server = MockServer(root / name, handler, connections=connections)
    server.start()
    result = run([str(binary), "--async-run", str(server.path), mode,
                  str(timeout_ms)], text=True, capture_output=True,
                 timeout=20)
    server.join(timeout=5)
    server.close()
    if server.error is not None:
        raise AssertionError(f"{name}: server error {server.error!r}")
    if result.returncode not in (0,) and "error_name=" not in result.stdout:
        raise AssertionError(f"{name}: runner failed\n{result.stdout}\n"
                             f"{result.stderr}")
    return parse_fields(result.stdout)


def test_mock_rpc(binary: Path, root: Path) -> None:
    def valid_cert(connection: socket.socket) -> None:
        op, request_id, _, _ = read_request(connection)
        assert op == OP_CERT
        connection.send(response(op, request_id, bytes(range(64))))

    fields = run_case(binary, root, "cert64", valid_cert)
    assert fields["error_name"] == "ok"
    assert fields["op"] == "1"
    assert fields["length"] == "64"
    assert fields["hex"] == bytes(range(64)).hex()
    assert fields["generation"] == str(0x1122334455667788)
    assert int(fields["request_id"]) != 0
    assert fields["fd"] == "-1"
    assert fields["poll_events"] == "0"
    assert fields["taken"] == "1"
    assert int(fields["iterations"]) > 0

    def valid_sign(connection: socket.socket) -> None:
        op, request_id, payload_len, _ = read_request(connection)
        assert op == OP_SIGN and payload_len == 20
        connection.send(response(op, request_id, bytes(range(32))))

    fields = run_case(binary, root, "sign20", valid_sign, mode="sign")
    assert fields["error_name"] == "ok"
    assert fields["op"] == "2"
    assert fields["length"] == "32"
    assert fields["hex"] == bytes(range(32)).hex()

    def valid_cert_2048(connection: socket.socket) -> None:
        op, request_id, _, _ = read_request(connection)
        connection.send(response(op, request_id, b"a" * 2048))

    fields = run_case(binary, root, "cert2048", valid_cert_2048)
    assert fields["error_name"] == "ok"
    assert fields["length"] == "2048"

    for size, name in ((2049, "cert2049"), (4096, "cert4096")):
        def oversized_cert(connection: socket.socket, size=size) -> None:
            op, request_id, _, _ = read_request(connection)
            connection.send(response(op, request_id, b"b" * size))

        fields = run_case(binary, root, name, oversized_cert)
        assert fields["error_name"] == "too-large", fields
        assert fields["length"] == "0"
        assert fields["hex"] == ""
        assert fields["fd"] == "-1"

    def oversized_sign(connection: socket.socket) -> None:
        op, request_id, _, _ = read_request(connection)
        connection.send(response(op, request_id, b"c" * 1025))

    fields = run_case(binary, root, "sign1025", oversized_sign, mode="sign")
    assert fields["error_name"] == "too-large"
    assert fields["length"] == "0"

    def helper_error(connection: socket.socket) -> None:
        op, request_id, _, _ = read_request(connection)
        connection.send(response(op, request_id, status=8))

    fields = run_case(binary, root, "helper8", helper_error)
    assert fields["error_name"] == "helper"
    assert fields["helper_status"] == "8"
    assert fields["length"] == "0"

    forged = {
        "magic": dict(magic=0x12345678),
        "version": dict(version=2),
        "op": dict(op_override=OP_SIGN),
        "id": dict(request_id_override=0xDEADBEEF),
        "reserved": dict(reserved=1),
        "status99": dict(status=99),
        "empty": dict(payload=b""),
    }
    for name, overrides in forged.items():
        def forged_handler(connection: socket.socket, overrides=overrides):
            op, request_id, _, _ = read_request(connection)
            payload = overrides.pop("payload", b"ok")
            connection.send(response(op, request_id, payload, **overrides))

        fields = run_case(binary, root, "forged-" + name, forged_handler)
        assert fields["error_name"] == "protocol", (name, fields)
        assert fields["length"] == "0"

    def status_payload(connection: socket.socket) -> None:
        op, request_id, _, _ = read_request(connection)
        connection.send(response(op, request_id, b"bad", status=8))

    fields = run_case(binary, root, "status-payload", status_payload)
    assert fields["error_name"] == "protocol"

    def extra_bytes(connection: socket.socket) -> None:
        op, request_id, _, _ = read_request(connection)
        connection.send(response(op, request_id, b"abcd", payload_len=4) +
                        b"extra")

    fields = run_case(binary, root, "extra", extra_bytes)
    assert fields["error_name"] == "protocol"

    def truncated(connection: socket.socket) -> None:
        _ = read_request(connection)
        connection.send(b"\x53\x43\x41\x31")

    fields = run_case(binary, root, "truncated", truncated)
    assert fields["error_name"] == "truncated"

    def oversized_record(connection: socket.socket) -> None:
        _ = read_request(connection)
        connection.send(b"x" * 5000)

    fields = run_case(binary, root, "oversize-record", oversized_record)
    assert fields["error_name"] == "truncated"

    def rights(connection: socket.socket) -> None:
        op, request_id, _, _ = read_request(connection)
        fd = os.open("/dev/null", os.O_RDONLY)
        try:
            control = [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                        struct.pack("i", fd))]
            connection.sendmsg([response(op, request_id, b"cert")], control)
        finally:
            os.close(fd)

    fields = run_case(binary, root, "rights", rights)
    assert fields["error_name"] == "rights"
    assert int(fields["rights_received"]) >= 1
    assert fields["rights_received"] == fields["rights_closed"]
    assert fields["fd"] == "-1"

    def close_early(connection: socket.socket) -> None:
        _ = read_request(connection)

    fields = run_case(binary, root, "close", close_early)
    assert fields["error_name"] == "recv"
    assert fields["fd"] == "-1"

    def delayed(connection: socket.socket) -> None:
        op, request_id, _, _ = read_request(connection)
        time.sleep(0.2)
        try:
            connection.send(response(op, request_id, b"delayed" * 8))
        except OSError:
            pass

    fields = run_case(binary, root, "delayed", delayed, timeout_ms=2000)
    assert fields["error_name"] == "ok"
    assert int(fields["iterations"]) > 5, fields
    assert fields["saw_receiving"] == "1"

    def timeout_handler(connection: socket.socket) -> None:
        _ = read_request(connection)
        time.sleep(0.5)

    fields = run_case(binary, root, "timeout", timeout_handler,
                      timeout_ms=100)
    assert fields["error_name"] == "timeout"
    assert fields["fd"] == "-1"
    assert int(fields["iterations"]) > 0


def test_reconnect_wrong_token(binary: Path, root: Path) -> None:
    first_seen = threading.Event()

    def handler(connection: socket.socket) -> None:
        op, request_id, _, _ = read_request(connection)
        assert op == OP_CERT
        if not first_seen.is_set():
            first_seen.set()
            try:
                connection.settimeout(2.0)
                connection.recv(1)
            except OSError:
                pass
        else:
            connection.send(response(op, request_id, b"second"))

    server = MockServer(root / "two", handler, connections=2)
    server.start()
    result = run([str(binary), "--async-two", str(server.path)], text=True,
                 capture_output=True, timeout=20)
    server.join(timeout=5)
    server.close()
    if server.error is not None:
        raise AssertionError(f"two-connection server: {server.error!r}")
    fields = parse_fields(result.stdout)
    assert fields["first_taken"] == "1"
    assert fields["first_stale_cancel"] == "generation"
    assert fields["first_cancel"] == "ok"
    assert fields["first_error"] == "cancelled"
    assert fields["first_generation"] == "10"
    assert fields["second_taken"] == "1"
    assert fields["second_error"] == "ok"
    assert fields["second_generation"] == "11"
    assert fields["second_length"] == "6"
    assert fields["second_hex"] == b"second".hex()
    assert fields["first_request"] != fields["second_request"]


def static_checks() -> None:
    files = [HEADER, SOURCE, TEST_C, PY_TEST]
    spdx = "SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors"
    license_line = "SPDX-License-Identifier: AGPL-3.0-only"
    for path in files:
        assert path.is_file(), path
        head = path.read_text(encoding="utf-8")[:500]
        assert spdx in head and license_line in head, path

    source = SOURCE.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")
    for forbidden in ("malloc(", "calloc(", "realloc(", "free(",
                      "pthread_", "printf(", "fprintf(", "syslog(",
                      "perror(", "#include <stdio.h>", "cp_oem_auth_open_rpc",
                      "cp_oem_auth_boot_provider", "CP_BOOT_AUTH_SUCCEEDED"):
        assert forbidden not in source, forbidden
    for forbidden in ("malloc(", "pthread_", "printf(", "fprintf(",
                      "syslog(", "perror(", "#include <stdio.h>",
                      "CP_BOOT_AUTH_SUCCEEDED",
                      "cp_boot_auth_provider ("):
        assert forbidden not in header, forbidden
    import re
    assert re.search(r"(?<![\w])poll\s*\(", source) is None
    assert "poll_events" in source and "poll_events" in header
    assert source.count("#ifdef CP_OEM_AUTH_ASYNC_TEST") >= 1
    assert header.count("#ifdef CP_OEM_AUTH_ASYNC_TEST") >= 1
    assert "experimental" in header and "socket_path" in header
    assert "expected_peer_uid" in header
    assert "CP_OEM_AUTH_ASYNC_MAX_CERTIFICATE 2048u" in header
    assert "CP_WIRE_MAX_AUTH_BLOB" in source or "CP_WIRE_MAX_AUTH_BLOB" in header
    forbidden_home = "/" + "home/"
    assert forbidden_home not in "".join(
        path.read_text(encoding="utf-8") for path in files)


def main() -> int:
    static_checks()
    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="cas-", dir=str(OUTPUT_ROOT)) as tmp_name:
        tmp = Path(tmp_name)
        binary = tmp / "test_carplay_oem_auth_async"
        checked(["gcc", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-pedantic", "-DCP_OEM_AUTH_ASYNC_TEST", "-I", str(SRC),
                 str(TEST_C), str(SOURCE), "-o", str(binary)],
                text=True, capture_output=True, timeout=120)
        unit = checked([str(binary)], text=True, capture_output=True,
                       timeout=120)
        assert "checks passed" in unit.stdout, unit.stdout
        assert "FAIL" not in unit.stdout + unit.stderr

        production = tmp / "cp_oem_auth_async_prod.o"
        checked(["gcc", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-pedantic", "-I", str(SRC), "-c", str(SOURCE), "-o",
                 str(production)], text=True, capture_output=True, timeout=120)
        test_object = tmp / "cp_oem_auth_async_test.o"
        checked(["gcc", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-pedantic", "-DCP_OEM_AUTH_ASYNC_TEST", "-I", str(SRC),
                 "-c", str(SOURCE), "-o", str(test_object)], text=True,
                capture_output=True, timeout=120)
        undefined = checked(["nm", "-u", str(production)], text=True,
                            capture_output=True, timeout=30).stdout
        for forbidden in ("poll", "pthread_", "malloc", "calloc", "realloc",
                          "printf", "fprintf", "cp_oem_auth_open_rpc",
                          "cp_oem_auth_boot_provider"):
            assert forbidden not in undefined, (forbidden, undefined)

        root = tmp / "rpc"
        root.mkdir(mode=0o700)
        test_mock_rpc(binary, root)
        test_reconnect_wrong_token(binary, root)

    print("carplay c3 async: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
