# Car asset capture and extraction

Status: **WIP — offline checks only; car execution requires explicit owner
approval and is not yet car-validated.** These tools are for the owner's own
parked, off-road Ioniq 5. They do not write vehicle data except a dump to a
mounted USB, and they do not commit OEM/private assets.

The assets not available from nudl/decryption/decompilation are runtime state:
the materialised `new_gui`/theme/QML trees, settings databases, update metadata,
partition geometry and logs. The firmware package gives the static rootfs; the
car capture gives the live runtime layer.

## Tools

```text
capture_inventory.sh   read-only inventory over SSH
capture_dump.sh        on-car dump to a mounted USB
import_capture.py      host-side hash verification and import
extract_assets.py      bounded asset extraction plus manifest
install_assets.py      install extracted assets into a VM host share
manifest.py            validate the metadata-only asset manifest
```

## Access

Use an owner-approved root channel, in order:

1. SCOURGE WiFi/SSH package (`packages/wifi`) — host-tested; car validation pending.
2. CloudPapi Owner Tools/dev-loader concept — reference only, not shipped.
3. Serial console/root — physical-access fallback.

Set `SCOURGE_CAR_CONFIRM=1` only after explicit approval. The SSH target is
not hardcoded; use the owner's own host/port and key. The `car`/`electric`
defaults are deliberate product defaults; do not publish personal credentials.

## Inventory

```sh
ssh <car-host> 'SCOURGE_CAR_CONFIRM=1 bash -s' < tools/car-assets/capture_inventory.sh
```

Read-only: identity, mounts, partitions, `tune2fs`, systemd, D-Bus, SMACK,
PCI/USB/DRM/audio, firmware and hashes. Output can contain personal details.

## Dump

```sh
SCOURGE_CAPTURE_CONFIRM=1 bash tools/car-assets/capture_dump.sh /media/usb
```

Requires a mounted writable removable USB under `/media` or `/run/media` with at
least 15 GiB free. Default skips secret-looking paths, logs, maps, critical
archives, raw partitions, `/voicememo` and `/Data`. Raw partitions and critical
archives require `SCOURGE_CAPTURE_INCLUDE_SENSITIVE=1`. Mounted read-write
partitions are skipped. The dump sets `umask 077`.

Opt-in:

```sh
SCOURGE_CAPTURE_INCLUDE_SENSITIVE=1
SCOURGE_CAPTURE_INCLUDE_LOGS=1
SCOURGE_CAPTURE_INCLUDE_NAVI2=1   # non-map new_gui asset tree
SCOURGE_CAPTURE_INCLUDE_MAPS=1    # large map data
```

## Import

```sh
python3 tools/car-assets/import_capture.py \
  captures/car-dump-YYYYMMDDTHHMMSSZ captures/car-local
```

Always verifies `meta/SHA256SUMS`, copies to a gitignored local directory with
`0700`/`0600` permissions, and never commits the capture.

## Extract

```sh
python3 tools/car-assets/extract_assets.py \
  captures/car-local --out car-assets/ui --class ui --class qml --class navi
```

Every archive and raw partition is verified against `meta/SHA256SUMS` before
extraction. Missing optional archives (for example a navi2 tree that was not
captured) are skipped with a warning; a missing required rootfs archive is an
error. Safe classes: `ui`, `qml`, `navi`, `update`. Sensitive classes (`runtime`,
`logs`, `maps`, `partitions`, `critical`) require `--allow-sensitive` and are
local-only. Secret-looking paths are skipped even in sensitive classes. The
output has `manifest.json` with path/size/hash metadata only.

## Install into a VM

```sh
python3 tools/car-assets/install_assets.py car-assets/ui work/vm/assets-share
python3 -B tools/vm/launch.py run --kernel compat \
  --rootfs work/vm/usable/rootfs.img \
  --share work/vm/assets-share \
  --cmd 'mkdir -p /tmp/new_gui; cp -a /media/host/. /tmp/new_gui; ...'
```

The installer verifies every manifest size/hash before copying. Use 9p for
iteration; copy archives to guest `/tmp` before extraction. Do not commit the
asset tree or build it into a public image.
