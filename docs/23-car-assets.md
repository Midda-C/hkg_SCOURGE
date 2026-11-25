# 23 — Car asset capture and extraction

TARGET: Ioniq 5 Gen5W_L, firmware `NE_E.EUR.S5W_L.001.001.260128` (EU).
Status: **WIP — offline checks only; car validation pending.** This lane is for
the owner's own parked, off-road car.

Some runtime assets are not in the nudl package, decrypted rootfs or Ghidra
export: the materialised `new_gui`/theme/QML trees, settings databases, update
metadata, partition geometry and logs. Capture them from the car; keep them
local and gitignored.

## Access

Use an owner-approved root channel:

1. SCOURGE WiFi/SSH package (`packages/wifi`) — host-tested; on-car retest pending.
2. CloudPapi Owner Tools/dev-loader concept — reference only, not shipped.
3. Serial console/root — physical-access fallback.

The `car`/`electric` defaults are deliberate product defaults. Do not put personal
credentials or LAN details in the repo. Set the confirmation flag only after
explicit approval:

```sh
SCOURGE_CAR_CONFIRM=1
SCOURGE_CAPTURE_CONFIRM=1
```

## Inventory over SSH

```sh
ssh <car-host> 'SCOURGE_CAR_CONFIRM=1 bash -s' \
  < tools/car-assets/capture_inventory.sh
```

Read-only: identity, mounts, block devices, `tune2fs`, systemd units, processes,
D-Bus, SMACK, PCI/USB/DRM/audio, firmware list and hashes. Output may contain
private filenames and device identifiers; review before sharing.

## Dump to USB

```sh
SCOURGE_CAPTURE_CONFIRM=1 bash tools/car-assets/capture_dump.sh /media/usb
```

The script resolves the mountpoint, requires it under `/media` or `/run/media`,
requires a mounted writable removable block device, and requires at least
15 GiB free. It refuses `/`, `/rw_data`, `/log_data`, `/update` and other
non-media paths even when they are subpaths.

Default capture is the rootfs archive (with secret-looking paths excluded),
the writable-data/update/etc. archives without personal directories, runtime
metadata and hashes. `/voicememo` and `/Data` are skipped by default as
personal/app data. **Critical archives and raw partitions are opt-in only**
and require `SCOURGE_CAPTURE_INCLUDE_SENSITIVE=1`. Raw partitions that are
mounted read-write are skipped; a read-only mount may be dumped with a note.

Opt-in flags:

```sh
SCOURGE_CAPTURE_INCLUDE_SENSITIVE=1
SCOURGE_CAPTURE_INCLUDE_LOGS=1
SCOURGE_CAPTURE_INCLUDE_NAVI2=1   # non-map new_gui asset tree
SCOURGE_CAPTURE_INCLUDE_MAPS=1    # large map data
```

The dump sets `umask 077`; keep the USB private. The dump is local-only and must
be gitignored.

## Import

```sh
python3 tools/car-assets/import_capture.py \
  captures/car-dump-YYYYMMDDTHHMMSSZ captures/car-local
```

Always verifies `meta/SHA256SUMS`, copies to a gitignored local directory with
`0700`/`0600` permissions, and never commits the capture. `captures/` and
`car-local/` are ignored.

## Extract assets

```sh
python3 tools/car-assets/extract_assets.py \
  captures/car-local --out car-assets/ui \
  --class ui --class qml --class navi
```

Every archive and raw partition is verified against `meta/SHA256SUMS` before
extraction. Missing optional archives (for example `navi2-nonmap.tar` when
`SCOURGE_CAPTURE_INCLUDE_NAVI2=1` was not set) are skipped with a warning; a
missing required rootfs archive is an error.

Safe classes: `ui`, `qml`, `navi`, `update`. Sensitive classes (`runtime`,
`logs`, `maps`, `partitions`, `critical`) require `--allow-sensitive` and stay
local-only. Secret-looking paths (shadow files, SSH/keys, DM config,
credentials) are skipped even in sensitive classes. The `manifest.json` contains
only path/class/size/hash metadata and is written `0600`.

## Install into a VM

```sh
python3 tools/car-assets/install_assets.py car-assets/ui work/vm/assets-share
python3 -B tools/vm/launch.py run --kernel compat \
  --rootfs work/vm/usable/rootfs.img \
  --share work/vm/assets-share \
  --cmd 'mkdir -p /tmp/new_gui; cp -a /media/host/. /tmp/new_gui; ...'
```

The installer verifies every manifest size/hash before copying. Use 9p for
iteration; copy archives to guest `/tmp` before extraction. The assets are
OEM-derived and not committed.

## Status and limits

- `WIP`: offline checks only; no car run has validated the full path.
- `car-validated`: only after the owner runs it on the car and records evidence.
- `WIP`: full map capture, runtime-state reconstruction, and AppHomeScreen
  stock-boot fidelity.
- Raw captures may contain personal/OEM/credential data. Never publish, commit
  or feed them into a public image.
- Map data is large and optional; do not capture it without explicit consent.

## Troubleshooting

- SSH fails: check the actual port (the WiFi package may fall back to 2222+),
  host key, and owner-approved access.
- Dump refuses: mountpoint is not under `/media`/`/run/media`, is not removable,
  is read-only, or has less than 15 GiB free.
- `tar` failures: target filesystem may be full or read-only; retry with a clean
  USB and check `meta/capture.log`.
- Hash mismatch: stop; do not import or extract from an inconsistent dump.
- `extract_assets` missing archive: enable the corresponding
  `SCOURGE_CAPTURE_INCLUDE_NAVI2=1`/`SCOURGE_CAPTURE_INCLUDE_MAPS=1` at capture
  time, or accept the optional-class skip.
