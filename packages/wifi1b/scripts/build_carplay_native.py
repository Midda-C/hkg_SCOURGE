#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Gen5W / IONIQ 5, x86-64, target rootfs glibc 2.25
"""Build the root-only CarPlay MFi auth helper against the target glibc.

The script reads (never writes) the owner's rootfs and the pinned
libCoreUtils.so, refuses hash/symbol/interpreter mismatches, compiles the tiny
helper with a custom _start and the rootfs libc-2.25/libdl-2.25, verifies that
every imported GLIBC symbol is <= 2.25, and writes only into an ignored output
directory.  It never executes OEM code.

Usage:
  python3 packages/wifi1b/scripts/build_carplay_native.py \
      --rootfs /path/to/rootfs \
      --out packages/wifi1b/out/carplay-native

  ... --check-only    verify only; print metadata JSON
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

SCRIPT = Path(__file__).resolve()
WIFI1B = SCRIPT.parents[1]
REPO = SCRIPT.parents[3]
HEADER = WIFI1B / "src" / "carplay" / "cp_oem_auth.h"
NATIVE_C = WIFI1B / "src" / "carplay" / "cp_native_auth.c"
NATIVE_START = WIFI1B / "src" / "carplay" / "cp_native_start.S"
DEFAULT_OUT = WIFI1B / "out" / "carplay-native"
TARGET_INTERPRETER = "/lib/ld-linux-x86-64.so.2"
MAX_GLIBC = (2, 25)


class BuildError(RuntimeError):
    pass


def run(command: list[str], cwd: Path | None = None) -> str:
    try:
        result = subprocess.run(
            command,
            cwd=str(cwd) if cwd is not None else None,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=120,
            check=False,
        )
    except OSError as error:
        raise BuildError(f"cannot run {' '.join(command)}: {error}") from error
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        raise BuildError(f"command failed ({result.returncode}): "
                         f"{' '.join(command)}\n{detail}")
    return result.stdout


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def header_define(name: str) -> str:
    text = HEADER.read_text(encoding="utf-8").replace("\\\n", " ")
    match = re.search(r"^#define\s+" + re.escape(name) + r"\s+(\S+)",
                      text, re.MULTILINE)
    if match is None:
        raise BuildError(f"missing {name} in {HEADER}")
    return match.group(1).strip().strip('"')


def parse_int_literal(text: str) -> int:
    cleaned = re.sub(r"(?i)(ull|llu|ul|lu|ll|u|l)$", "", text)
    return int(cleaned, 0)


def parse_expected_symbols() -> dict[str, int]:
    return {
        header_define("CP_OEM_AUTH_MFI_INITIALIZE_SYMBOL"):
            parse_int_literal(header_define(
                "CP_OEM_AUTH_MFI_INITIALIZE_ADDR")),
        header_define("CP_OEM_AUTH_MFI_CERTIFICATE_SYMBOL"):
            parse_int_literal(header_define(
                "CP_OEM_AUTH_MFI_CERTIFICATE_ADDR")),
        header_define("CP_OEM_AUTH_MFI_SIGNATURE_SYMBOL"):
            parse_int_literal(header_define(
                "CP_OEM_AUTH_MFI_SIGNATURE_ADDR")),
    }


def elf_symbols(path: Path, dynamic: bool = True) -> dict[str, dict[str, object]]:
    command = ["readelf", "--wide"]
    command.append("--dyn-syms" if dynamic else "--syms")
    command.append(str(path))
    output = run(command)
    symbols: dict[str, dict[str, object]] = {}
    pattern = re.compile(
        r"^\s*\d+:\s+([0-9a-fA-F]+)\s+(\d+)\s+(\S+)\s+(\S+)\s+(\S+)\s+"
        r"(\S+)\s+(\S+)\s*$"
    )
    for line in output.splitlines():
        match = pattern.match(line)
        if match is None:
            continue
        value, size, kind, bind, visibility, ndx, name = match.groups()
        name = name.split("@", 1)[0]
        if name == "":
            continue
        symbols[name] = {
            "value": int(value, 16),
            "size": int(size, 10),
            "type": kind,
            "bind": bind,
            "visibility": visibility,
            "ndx": ndx,
        }
    return symbols


def verify_coreutils(path: Path, expected_sha: str,
                     expected_symbols: dict[str, int]) -> dict[str, object]:
    if not path.is_file() or path.is_symlink():
        raise BuildError(f"coreutils not found as a regular file: {path}")
    actual_sha = sha256_file(path)
    if actual_sha != expected_sha:
        raise BuildError(
            f"coreutils SHA-256 mismatch: expected {expected_sha}, "
            f"got {actual_sha}")
    header = run(["readelf", "-h", str(path)])
    if re.search(r"Class:\s+ELF64", header) is None:
        raise BuildError("coreutils is not ELF64")
    if re.search(r"Machine:\s+Advanced Micro Devices X86-64", header) is None:
        raise BuildError("coreutils is not x86-64")
    symbols = elf_symbols(path, dynamic=True)
    found: dict[str, int] = {}
    for name, address in expected_symbols.items():
        info = symbols.get(name)
        if info is None:
            raise BuildError(f"coreutils is missing exported symbol {name}")
        if info["type"] != "FUNC" or info["bind"] != "GLOBAL" or \
                info["visibility"] != "DEFAULT" or info["ndx"] == "UND":
            raise BuildError(f"coreutils symbol {name} is not a defined "
                             "global default function")
        if info["value"] != address:
            raise BuildError(
                f"coreutils symbol {name} address 0x{info['value']:x} "
                f"does not match pinned 0x{address:x}")
        found[name] = int(info["value"])
    return {
        "path": str(path),
        "sha256": actual_sha,
        "size": path.stat().st_size,
        "symbols": found,
    }


def elf_soname(path: Path) -> str | None:
    output = run(["readelf", "-d", str(path)])
    match = re.search(r"\(SONAME\)\s+Library soname: \[([^\]]+)\]", output)
    return match.group(1) if match else None


def verify_target_libs(rootfs: Path) -> dict[str, object]:
    expected = {
        "libc": (rootfs / "lib" / "libc-2.25.so", "libc.so.6"),
        "libdl": (rootfs / "lib" / "libdl-2.25.so", "libdl.so.2"),
        "libpthread": (rootfs / "lib" / "libpthread-2.25.so",
                       "libpthread.so.0"),
        "ld": (rootfs / "lib" / "ld-2.25.so", "ld-linux-x86-64.so.2"),
    }
    result: dict[str, object] = {}
    for name, (path, soname) in expected.items():
        if not path.is_file():
            raise BuildError(f"target {name} not found: {path}")
        actual_soname = elf_soname(path)
        if actual_soname != soname:
            raise BuildError(
                f"target {name} SONAME {actual_soname!r}, expected {soname!r}")
        result[name] = {
            "path": str(path),
            "sha256": sha256_file(path),
            "size": path.stat().st_size,
            "soname": actual_soname,
        }
    return result


def parse_glibc_version(value: str) -> tuple[int, int, int]:
    match = re.fullmatch(r"GLIBC_(\d+)\.(\d+)(?:\.(\d+))?", value)
    if match is None:
        raise BuildError(f"non-GLIBC symbol version {value}")
    return (int(match.group(1)), int(match.group(2)),
            int(match.group(3) or 0))


def verify_helper(path: Path) -> dict[str, object]:
    if not path.is_file():
        raise BuildError(f"helper not built: {path}")
    header = run(["readelf", "-h", str(path)])
    if re.search(r"Class:\s+ELF64", header) is None:
        raise BuildError("helper is not ELF64")
    if re.search(r"Machine:\s+Advanced Micro Devices X86-64", header) is None:
        raise BuildError("helper is not x86-64")
    program = run(["readelf", "-l", str(path)])
    interpreters = re.findall(
        r"\[Requesting program interpreter: ([^\]]+)\]", program)
    if interpreters != [TARGET_INTERPRETER]:
        raise BuildError(
            f"helper interpreter {interpreters!r}, expected "
            f"{TARGET_INTERPRETER!r}")
    dynamic = run(["readelf", "-d", str(path)])
    if "RPATH" in dynamic or "RUNPATH" in dynamic:
        raise BuildError("helper must not contain RPATH/RUNPATH")
    needed = re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", dynamic)
    allowed_needed = {"libc.so.6", "libdl.so.2", "libpthread.so.0"}
    unexpected = [name for name in needed if name not in allowed_needed]
    if unexpected:
        raise BuildError(f"helper has unexpected NEEDED entries: {unexpected}")
    if "libc.so.6" not in needed:
        raise BuildError("helper does not depend on libc.so.6")
    symbols = run(["objdump", "-T", str(path)])
    versions = sorted(set(re.findall(r"GLIBC_[0-9.]+", symbols)))
    if "GLIBC_PRIVATE" in versions:
        raise BuildError("helper imports GLIBC_PRIVATE")
    max_version = (0, 0, 0)
    for version in versions:
        parsed = parse_glibc_version(version)
        if parsed > max_version:
            max_version = parsed
    if max_version > MAX_GLIBC:
        raise BuildError(
            f"helper imports GLIBC {max_version[0]}.{max_version[1]} "
            f"(limit {MAX_GLIBC[0]}.{MAX_GLIBC[1]})")
    for forbidden in ("OPENSSL_", "GLIBCXX_", "CXXABI_"):
        if forbidden in symbols:
            raise BuildError(f"helper imports forbidden ABI {forbidden}")
    return {
        "path": str(path),
        "sha256": sha256_file(path),
        "size": path.stat().st_size,
        "interpreter": TARGET_INTERPRETER,
        "needed": needed,
        "glibc_versions": versions,
        "max_glibc": ".".join(str(part) for part in max_version),
    }


def ensure_output_dir(out: Path) -> None:
    resolved = out.resolve()
    if repo_relative(resolved) is not None:
        result = subprocess.run(
            ["git", "-C", str(REPO), "check-ignore", "-q", str(resolved)],
            check=False,
        )
        if result.returncode != 0:
            raise BuildError(
                f"output directory is not git-ignored: {resolved}")
        return
    try:
        resolved.relative_to(Path("/tmp/opencode").resolve())
    except ValueError:
        raise BuildError(
            f"output directory must be git-ignored or under /tmp/opencode: "
            f"{resolved}") from None


def repo_relative(path: Path) -> Path | None:
    try:
        return path.relative_to(REPO)
    except ValueError:
        return None


def compile_helper(rootfs: Path, out: Path, cc: str) -> dict[str, object]:
    out.mkdir(parents=True, exist_ok=True)
    object_file = out / "cp_native_auth.o"
    start_file = out / "cp_native_start.o"
    helper = out / "carplay-native-auth"
    source_dir = NATIVE_C.parent
    compile_flags = [
        "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
        "-fno-pie", "-fstack-protector-strong", "-march=x86-64",
        "-mtune=generic", "-fcf-protection=none",
    ]
    run([cc, *compile_flags, "-I", str(source_dir), "-c", str(NATIVE_C),
         "-o", str(object_file)])
    run([cc, "-c", "-fno-pie", str(NATIVE_START), "-o", str(start_file)])
    link_flags = [
        "-nostdlib", "-no-pie", "-Wl,-e,_start",
        f"-Wl,--dynamic-linker={TARGET_INTERPRETER}",
        "-Wl,-z,relro,-z,now,-z,noexecstack", "-Wl,--fatal-warnings",
    ]
    lib_dir = rootfs / "lib"
    run([
        cc, *link_flags, "-o", str(helper), str(start_file),
        str(object_file), "-L", str(lib_dir), "-Wl,--no-as-needed",
        "-l:libdl-2.25.so", "-l:libc-2.25.so",
    ])
    return {
        "path": str(helper),
        "compile_flags": compile_flags,
        "link_flags": link_flags + [
            "-L", str(lib_dir), "-Wl,--no-as-needed",
            "-l:libdl-2.25.so", "-l:libc-2.25.so",
        ],
    }


def metadata(rootfs: Path, coreutils: Path, out: Path | None,
             check_only: bool) -> dict[str, object]:
    expected_sha = header_define("CP_OEM_AUTH_COREUTILS_SHA256")
    expected_symbols = parse_expected_symbols()
    core = verify_coreutils(coreutils, expected_sha, expected_symbols)
    libs = verify_target_libs(rootfs)
    result: dict[str, object] = {
        "schema": 1,
        "check_only": check_only,
        "rootfs": str(rootfs),
        "coreutils": core,
        "target_libs": libs,
        "target_interpreter": TARGET_INTERPRETER,
        "max_glibc": ".".join(str(part) for part in MAX_GLIBC),
    }
    if out is not None and not check_only:
        helper = compile_helper(rootfs, out, os.environ.get("CC", "gcc"))
        result["helper"] = verify_helper(Path(helper["path"]))
        result["build"] = {
            "cc": os.environ.get("CC", "gcc"),
            "compile_flags": helper["compile_flags"],
            "link_flags": helper["link_flags"],
        }
        (out / "build-metadata.json").write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rootfs", required=True, type=Path)
    parser.add_argument("--coreutils", type=Path)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()

    rootfs = args.rootfs.resolve()
    coreutils = (args.coreutils if args.coreutils is not None
                 else rootfs / "app" / "lib" / "libCoreUtils.so").resolve()
    out = args.out.resolve()
    try:
        if not args.check_only:
            ensure_output_dir(out)
        result = metadata(rootfs, coreutils, out, args.check_only)
    except BuildError as error:
        print(f"build_carplay_native: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
