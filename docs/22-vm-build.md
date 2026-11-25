# 22 — Building the two VM profiles

TARGET: Ioniq 5 Gen5W_L (Linux), firmware `NE_E.EUR.S5W_L.001.001.260128`
(EU). Status: **WIP — the build path is unit-tested, but the full fidelity
image requires loop/root and is not yet VM- or car-validated.**

SCOURGE builds two local VM workspaces from the owner's own decrypted rootfs:

- **fidelity** — the vendor kernel and an eMMC-like GPT projection with the
  vendor `esinit`/systemd boot path. This is the closest available boot/policy
  environment, not stock hardware equivalence.
- **usable** — a flat rootfs for the compat kernel (Mode C). It is the fast,
  graphical/package workbench; it is not policy evidence.

No firmware, OEM binaries or assets are committed. Inputs are staged under
`input/`/`work/` and outputs under `work/vm/`, all gitignored.

## Prerequisites

```sh
bash scripts/00_fetch_third_party.sh
bash scripts/00_env.sh
bash scripts/01_stage_inputs.sh
bash scripts/02_decrypt_rootfs.sh
```

You need the owner's validated package and a local decrypted rootfs tree.
`02_decrypt_rootfs.sh` produces `work/stock-plain.tar.gz`, not an extracted
tree; extract it before building:

```sh
mkdir -p work/rootfs
tar -xzf work/stock-plain.tar.gz -C work/rootfs
```

Build tools: `qemu-system-x86_64`, `/dev/kvm` (or TCG), `parted`, `mkfs.ext4`,
`debugfs`, `fakeroot`, `losetup`, `truncate`, `sha256sum`, `tar`. The partition
loop setup may require root/sudo; use a disposable local workspace and do not
point outputs at shared backing images.

The helper wrappers are bash scripts. Run them with `bash`, not `sh`.

## Fidelity build

```sh
bash scripts/22_build_vm.sh fidelity \
  --rootfs-tree work/rootfs \
  --output-dir work/vm/fidelity \
  --layout captured \
  --rw-data work/rootfs/rw_data \
  --navi work/rootfs/navi \
  --fill-inactive \
  --weston-wrapper \
  --micom-standin
```

`--micom-standin` needs a prebuilt `tools/vm/guest/micom-dbus-sim`; build it
with `sh tools/vm/build_guest_tools.sh`. That build needs static libdbus
development files (`DBUS_DEV_ROOT`, see `tools/vm/build_research_tools.sh`); if
you do not have them, skip `--micom-standin` or pass `--micom-helper <path>`.
`--weston-wrapper` installs a wrapper but does not install the software-GL
runtime; full Weston/AppHomeScreen rendering remains WIP.

Outputs:

```text
work/vm/fidelity/rootfs.img     sparse GPT image, active root p5
work/vm/fidelity/bzImage        vendor 4.1.27-abl kernel
work/vm/fidelity/profile.json   boot facts and substitutions
work/vm/fidelity/manifest.json  hashes and sizes
```

Layout choices:

- `captured` uses the owner's observed EU partition sizes (`p5` active root,
  `p8` log, `p9` update, `p11` rw_data, `p14`/`p15` navi). Use this for the
  closest boot-path fidelity. `p4`/`p16` are unknown placeholders.
- `projection` uses the smaller public projection for a lightweight lab image.
- `--layout-json` accepts a validated layout file if you derive it from a car
  capture.

Boot facts:

- `root=/dev/mmcblk0p5`; `init=/lib/systemd/esinit`; `p1..p17` GPT.
- MICOM SPI gate is held by `/init-esinit-spi.sh`; `lge_usb_adapter` is
  neutralised by a projection-only modprobe rule.
- `--weston-wrapper` preserves the vendor binary and installs a wrapper that
  only supplies software-GL paths when that runtime exists. It does not build
  or install Mesa/EGL; app rendering remains WIP.
- `--micom-standin` installs `/micom-dbus-sim` and a projection-only service;
  it is a lab substitute for the SPI MICOM handshake.
- These are substitutions, not stock hardware. The exact vendor kernel is
  headless; a rebuilt kernel with `bochs-drm` is a documented fidelity delta.

Check a recorded run:

```sh
python3 -B tools/vm/launch.py run --profile conformance \
  --scenario vendor-esinit-spi \
  --rootfs work/vm/fidelity/rootfs.img \
  --root-partition /dev/mmcblk0p5 \
  --kernel-image work/vm/fidelity/bzImage --timeout 300
python3 -B tools/vm/fidelity/check-stock-boot.py \
  tools/vm/runs/<run-id> --level esinit
```

`vendor-esinit-spi` runs the built `/init-esinit-spi.sh`, which holds the SPI
parameter and execs the real esinit. At `--level esinit` the checker expects the
`p5` root, `FDE SUPPORT: No` and the non-FDE fstab selection. It does **not**
claim a full Weston/HMI boot. Use `vendor-esinit-fixture` for a Multi-User
fixture (`--level systemd`); AppHomeScreen/Weston evidence is still WIP.

## Usable build

```sh
bash scripts/22_build_vm.sh usable \
  --rootfs-tree work/rootfs \
  --output-dir work/vm/usable \
  --assets car-assets/ui
```

This builds a flat 4 GB ext4 with the research `/init` via
`tools/vm/build_rootfs.sh`. The `bzImage` it copies is the vendor kernel for
reference; the compat boot uses the separate pinned kernel. Boot it with
`--kernel compat` and the built rootfs:

```sh
python3 -B tools/vm/launch.py run --kernel compat \
  --rootfs work/vm/usable/rootfs.img \
  --profile research --scenario ui-smoke --qmp-screenshot
```

Car-extracted UI assets are installed through the [car-assets lane](23-car-assets.md);
they are not copied into git.

## Tools and compat prerequisites

```sh
sh tools/vm/build_tools.sh tools.$(date +%Y%m%d).img
sh tools/vm/compat/pin-kernel.sh
sh tools/vm/compat/build-initramfs.sh
python3 -B tools/vm/launch.py doctor --graphics
```

Stale tools images are refused. Compat kernel/initramfs bytes are pinned and
gitignored.

## Status and limits

- `WIP`: the build scripts are unit-tested, but the full fidelity image has not
  been built and booted in this checkout (loop/root are required).
- `VM-validated`: only after a real `launch.py run` records PASS evidence.
- `WIP`: authentic AppHomeScreen rendering, car-snapshot fidelity, maps/audio.
- Do not claim stock equivalence from the projection. MICOM, GPU/EGL, credential
  state and exact partition roles remain limited.
- Never commit build outputs, OEM bytes, keys or personal data.

## Troubleshooting

- `parted`/`mkfs.ext4` failure: check `sgdisk`/`parted` availability and free disk.
- `debugfs` helper injection: paths with spaces are rejected; keep inputs under
  space-free local paths.
- `esinit` stops before systemd: confirm `p8`–`p12` exist and the SPI wrapper ran.
- `weston`/AppHomeScreen missing: install the software-GL path and ensure
  `LIBGL_DRIVERS_PATH` reaches the child; the vendor launcher strips `LIBGL_*`.
- Stale tools image: rebuild with `build_tools.sh`; do not bypass the manifest.
