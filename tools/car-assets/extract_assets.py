#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Ioniq 5 Gen5W_L, firmware NE_E.EUR.S5W_L.001.001.260128 (EU).
"""Extract bounded UI/runtime assets from a local car capture.

The input is a local capture produced by capture_dump.sh. The output is a
gitignored local asset tree plus a metadata-only manifest. Default extraction
skips secret-looking paths. Runtime/log/map/partition/critical classes require
an explicit --allow-sensitive decision and remain local-only.

Every archive and raw partition read is verified against the capture's
meta/SHA256SUMS before extraction. Missing optional archives (for example a
navi2 tree that was not opted into at capture time) are skipped with a warning;
a missing required archive is an error.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import tarfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
from manifest import SCHEMA_VERSION, validate_manifest  # noqa: E402

# (archive path, required when this class is requested)
CLASSES = {
    "ui": {
        "archives": (("archives/rootfs.tar", True),
                     ("archives/navi2-nonmap.tar", False)),
        "prefixes": (
            "app/share/AppHomeScreen",
            "app/share/images/hmc",
            "app/share/images/new_gui",
            "system/system/new_gui",
            "navi/system/system/new_gui",
        ),
        "sensitive": False,
    },
    "qml": {
        "archives": (("archives/rootfs.tar", True),),
        "prefixes": (
            "app/share/AppHomeScreen",
            "app/share/AppSetup",
            "app/share/BluetoothHMI",
        ),
        "sensitive": False,
    },
    "navi": {
        "archives": (("archives/navi2-nonmap.tar", False),),
        "prefixes": (
            "Bin",
            "Config",
            "BIDI",
            "Text.Info",
            "navi_sw_version.txt",
            "system/system/new_gui",
        ),
        "sensitive": False,
    },
    "runtime": {
        "archives": (("archives/rw_data.tar", True),),
        "prefixes": ("app_data", "var/lib", "usr_data"),
        "sensitive": True,
    },
    "update": {
        "archives": (("archives/update.tar", False),),
        "prefixes": (
            "updateVersionInfo",
            "scheduledUpdateVersion",
            "FumoUsbUpdateTimeVersion",
            "scheduledUpdateDate",
            "whatsnew",
            "usb",
            "package.tar.sign",
            "update_modemtype",
        ),
        "sensitive": False,
    },
    "logs": {
        "archives": (("archives/log_data.tar", False),),
        "prefixes": ("",),
        "sensitive": True,
    },
    "maps": {
        "archives": (("archives/navi2-maps.tar", False),),
        "prefixes": ("",),
        "sensitive": True,
    },
    "critical": {
        "archives": (("critical/critical.tar", False),),
        "prefixes": ("",),
        "sensitive": True,
    },
    "partitions": {
        "archives": (),
        "prefixes": (),
        "sensitive": True,
    },
}

SECRET_MARKERS = (
    "etc/shadow", "etc/shadow.Global", "etc/shadow-", "etc/gshadow",
    "etc/gshadow-", "etc/passwd.secure", "etc/ssh",
    "etc/machine-id", "etc/hostname", "app/share/AppUpgrade/teruten5_pub.key",
    "app/share/dmengine", "authorized_keys", "wifi.txt", "keystore",
    "*.key", "*.psk", "*.pw", "mof", "/MOF", "credentials",
)


def normalize(name: str) -> str:
    name = name.replace("\\", "/")
    while name.startswith("./"):
        name = name[2:]
    if not name or name == ".":
        raise ValueError("empty archive member")
    if name.startswith("/"):
        raise ValueError(f"absolute archive member: {name}")
    if any(part == ".." for part in Path(name).parts):
        raise ValueError(f"unsafe archive member: {name}")
    return name


def is_secret(name: str) -> bool:
    for marker in SECRET_MARKERS:
        if marker.startswith("*."):
            if name.endswith(marker[1:]):
                return True
        elif marker in name:
            return True
    return False


def matches(name: str, prefixes: tuple[str, ...]) -> bool:
    if not prefixes or prefixes == ("",):
        return True
    for prefix in prefixes:
        if name == prefix or name.startswith(prefix + "/"):
            return True
    return False


def hash_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def parse_capture_sums(capture: Path) -> dict[str, str]:
    sums = capture / "meta" / "SHA256SUMS"
    if not sums.is_file():
        raise ValueError(f"missing capture manifest: {sums}")
    entries: dict[str, str] = {}
    for line in sums.read_text(errors="replace").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 1)
        if len(parts) != 2:
            raise ValueError(f"bad SHA256SUMS line: {line!r}")
        digest, name = parts
        if len(digest) != 64:
            raise ValueError(f"bad digest in SHA256SUMS: {digest!r}")
        key = normalize(name)
        if key in entries:
            raise ValueError(f"duplicate SHA256SUMS entry: {key}")
        entries[key] = digest
    if not entries:
        raise ValueError("SHA256SUMS has no entries")
    return entries


def verify_archive(capture: Path, archive: Path,
                   entries: dict[str, str] | None = None) -> None:
    entries = entries if entries is not None else parse_capture_sums(capture)
    rel = archive.relative_to(capture).as_posix()
    digest = entries.get(rel)
    if digest is None:
        raise ValueError(f"archive not listed in capture manifest: {rel}")
    actual = hashlib.sha256(archive.read_bytes()).hexdigest()
    if actual != digest:
        raise ValueError(f"archive hash mismatch: {rel}")


def extract_tar(capture: Path, out: Path, archive_name: str,
                prefixes: tuple[str, ...], class_name: str, sensitive: bool,
                max_bytes: int, assets: dict[str, dict],
                entries: dict[str, str]) -> int:
    archive = capture / archive_name
    if not archive.is_file():
        raise ValueError(f"missing required archive: {archive}")
    verify_archive(capture, archive, entries)
    total = 0
    with tarfile.open(archive, "r:*") as tar:
        for member in tar.getmembers():
            if member.isdir() or member.islnk() or member.issym() or not member.isfile():
                continue
            try:
                name = normalize(member.name)
            except ValueError as exc:
                print(f"skip: {exc}", file=sys.stderr)
                continue
            if not matches(name, prefixes) or is_secret(name):
                continue
            target = out / name
            if not target.resolve().is_relative_to(out.resolve()):
                raise ValueError(f"unsafe output path: {name}")
            if total + member.size > max_bytes:
                raise ValueError(f"asset extraction exceeds {max_bytes} bytes")
            target.parent.mkdir(parents=True, mode=0o700, exist_ok=True)
            with tar.extractfile(member) as handle:
                data = handle.read()
            target.write_bytes(data)
            target.chmod(0o600)
            total += len(data)
            assets.setdefault(name, {
                "class": class_name,
                "path": name,
                "size": len(data),
                "sha256": hash_bytes(data),
                "sensitive": sensitive,
            })
    return total


def extract_partitions(capture: Path, out: Path, max_bytes: int,
                       assets: dict[str, dict],
                       entries: dict[str, str]) -> int:
    part_dir = capture / "partitions"
    if not part_dir.is_dir():
        raise ValueError(f"missing partitions directory: {part_dir}")
    total = 0
    for source in sorted(part_dir.glob("*.img")):
        if source.is_symlink() or not source.is_file():
            raise ValueError(f"unsafe partition entry: {source}")
        verify_archive(capture, source, entries)
        if total + source.stat().st_size > max_bytes:
            raise ValueError(f"partition extraction exceeds {max_bytes} bytes")
        target = out / "partitions" / source.name
        target.parent.mkdir(parents=True, mode=0o700, exist_ok=True)
        target.write_bytes(source.read_bytes())
        target.chmod(0o600)
        total += source.stat().st_size
        assets[target.relative_to(out).as_posix()] = {
            "class": "partitions",
            "path": target.relative_to(out).as_posix(),
            "size": source.stat().st_size,
            "sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
            "sensitive": True,
        }
    return total


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--class", dest="classes", action="append",
                        choices=tuple(CLASSES) + ("all",), default=[])
    parser.add_argument("--allow-sensitive", action="store_true")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--max-bytes", type=int, default=4 * 1024 * 1024 * 1024)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    if not args.capture.is_dir():
        parser.exit(1, f"capture not found: {args.capture}\n")
    if args.out.exists():
        parser.exit(1, f"output already exists: {args.out}\n")
    if args.max_bytes <= 0:
        parser.exit(1, "max-bytes must be positive\n")

    requested = args.classes or ["all"]
    if "all" in requested:
        requested = ["ui", "qml", "navi", "update"]
    if any(CLASSES[name]["sensitive"] for name in requested) and not args.allow_sensitive:
        parser.exit(1, "sensitive asset classes require --allow-sensitive\n")

    assets: dict[str, dict] = {}
    total = 0
    try:
        entries = parse_capture_sums(args.capture)
        if args.list or args.dry_run:
            for name in requested:
                info = CLASSES[name]
                for archive_name, required in info["archives"]:
                    archive = args.capture / archive_name
                    if not archive.is_file():
                        level = "missing" if required else "missing optional"
                        print(f"{level}: {archive}", file=sys.stderr)
                        continue
                    verify_archive(args.capture, archive, entries)
                    with tarfile.open(archive, "r:*") as tar:
                        for member in tar.getmembers():
                            if not member.isfile():
                                continue
                            try:
                                member_name = normalize(member.name)
                            except ValueError as exc:
                                print(f"skip: {exc}", file=sys.stderr)
                                continue
                            if not matches(member_name, info["prefixes"]):
                                continue
                            if is_secret(member_name):
                                continue
                            print(f"{name}: {member_name} ({member.size} bytes)")
            return 0

        os.umask(0o077)
        args.out.mkdir(parents=True, mode=0o700)
        for name in requested:
            info = CLASSES[name]
            if name == "partitions":
                total += extract_partitions(args.capture, args.out,
                                           args.max_bytes, assets, entries)
                continue
            for archive_name, required in info["archives"]:
                archive = args.capture / archive_name
                if not archive.is_file():
                    if required:
                        raise ValueError(f"missing required archive: {archive}")
                    print(f"skip missing optional archive: {archive}",
                          file=sys.stderr)
                    continue
                total += extract_tar(args.capture, args.out, archive_name,
                                     info["prefixes"], name, info["sensitive"],
                                     args.max_bytes, assets, entries)
        for directory in [args.out, *[p for p in args.out.rglob("*")
                                       if p.is_dir()]]:
            directory.chmod(0o700)
        manifest = {
            "schema_version": SCHEMA_VERSION,
            "capture": str(args.capture),
            "classes": requested,
            "assets": list(assets.values()),
            "total_bytes": total,
            "limitation": "local-only; OEM/private assets are not committed",
        }
        validate_manifest(manifest)
        manifest_path = args.out / "manifest.json"
        manifest_path.write_text(json.dumps(manifest, indent=2))
        manifest_path.chmod(0o600)
        if args.json:
            print(json.dumps(manifest, indent=2))
        else:
            print(f"extracted {len(assets)} assets ({total} bytes) -> {args.out}")
            print("local-only; OEM/private assets are not committed")
    except (ValueError, OSError, tarfile.TarError) as exc:
        parser.exit(1, f"asset extraction failed: {exc}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
