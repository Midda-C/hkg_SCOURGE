# VM build scripts — fidelity and usable profiles

Status: **WIP — unit-tested build path; full image builds need loop/root and
are not yet VM- or car-validated.** These scripts build local, gitignored
workspaces from the owner's own decrypted rootfs. They do not download, decrypt
or redistribute firmware.

## What each profile is

- **fidelity** — sparse GPT eMMC projection. `p5` is the active root; `p8`/`p9`/
  `p10`/`p11`/`p12`/`p13` carry the documented roles. Boot with the vendor
  kernel and `init=/lib/systemd/esinit`.
- **usable** — flat ext4 rootfs for the compat kernel (Mode C). UI assets and
  QML come from the car-assets lane; this profile is not policy evidence.

## Build a fidelity workspace

```sh
sh tools/vm/build_guest_tools.sh   # provides guest/micom-dbus-sim for --micom-standin
python3 -B tools/vm/build_vm.py fidelity \
  --rootfs-tree work/rootfs \
  --output-dir work/vm/fidelity \
  --layout captured \
  --rw-data work/rootfs/rw_data \
  --navi work/rootfs/navi \
  --fill-inactive \
  --weston-wrapper \
  --micom-standin
```

`02_decrypt_rootfs.sh` produces `work/stock-plain.tar.gz`; extract it to
`work/rootfs` before pointing `--rootfs-tree` at it. `build_guest_tools.sh`
needs static libdbus development files (`DBUS_DEV_ROOT`); skip
`--micom-standin` or pass `--micom-helper` if unavailable.

`--layout projection` uses the smaller documented projection. `--layout-json`
accepts a validated `layout.json`. Use the **captured** layout for the closest
boot-path fidelity; use `projection` for a lightweight lab image.

The output contains `rootfs.img`, `bzImage`, `profile.json`, `manifest.json`,
and an optional `assets/` tree. The partition image is sparse; the active root
is `/dev/mmcblk0p5`. The partition builder uses `losetup` and may need
root/sudo; keep outputs in a disposable local workspace.

The helper wrappers are bash scripts. Run them with `bash`, not `sh`.

`--weston-wrapper` only installs a wrapper that supplies software-GL paths when
that runtime exists; it does not build Mesa/EGL. `--micom-standin` needs the
prebuilt `guest/micom-dbus-sim` helper and is a lab substitute for the SPI MICOM
handshake.

## Build a usable workspace

```sh
python3 -B tools/vm/build_vm.py usable \
  --rootfs-tree work/rootfs \
  --output-dir work/vm/usable \
  --assets car-assets/ui
```

This calls `tools/vm/build_rootfs.sh`, producing a flat 4 GB ext4 with the
research `/init`. The copied `bzImage` is the vendor kernel for reference; the
compat boot uses the separate pinned kernel.

## Tools and compat kernel

```sh
sh tools/vm/build_tools.sh tools.$(date +%Y%m%d).img
sh tools/vm/compat/pin-kernel.sh
sh tools/vm/compat/build-initramfs.sh
python3 -B tools/vm/launch.py doctor --graphics
```

The tools image embeds a manifest; stale images are refused. Kernel and
initramfs bytes stay gitignored and pinned.

## Fidelity boot checks

```sh
python3 -B tools/vm/launch.py run --profile conformance \
  --scenario vendor-esinit-spi \
  --rootfs work/vm/fidelity/rootfs.img \
  --root-partition /dev/mmcblk0p5 \
  --kernel-image work/vm/fidelity/bzImage --timeout 300
python3 -B tools/vm/fidelity/check-stock-boot.py \
  tools/vm/runs/<run-id> --level esinit
```

Required at `--level esinit`: `mmcblk0p5`, `FDE SUPPORT: No`, and
`use fstab for non FDE`. This is not a full Weston/HMI claim. For a Multi-User
fixture use `vendor-esinit-fixture` with `--level systemd`; AppHomeScreen/Weston
evidence remains WIP.

## Usable boot

```sh
python3 -B tools/vm/launch.py run --kernel compat \
  --rootfs work/vm/usable/rootfs.img \
  --profile research --scenario ui-smoke --qmp-screenshot
```

## Limits and status

- The exact vendor kernel has no QEMU DRM/FB path; Weston uses screenshots or a
  documented rebuilt-kernel/DRM delta.
- MICOM SPI, GPU/EGL and `lge_usb_adapter` are substitutions; label them.
- Partition sizes are from the owner's capture where measured; `p4`/`p16` roles
  are unknown placeholders.
- A car-snapshot build from captured raw partitions is owner-local and WIP.
- Never commit build outputs, OEM bytes, keys or personal data.
