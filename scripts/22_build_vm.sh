#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Ioniq 5 Gen5W_L, firmware NE_E.EUR.S5W_L.001.001.260128 (EU).
# 22_build_vm.sh — build the fidelity or usable VM workspace from local inputs.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/lib.sh"

usage() {
    cat >&2 <<'EOF'
usage: bash scripts/22_build_vm.sh fidelity|usable [options]

  fidelity: GPT eMMC projection, vendor kernel, esinit boot path
  usable:   flat rootfs for the compat kernel (Mode C)

Pass the build_vm.py options after the profile:
  --rootfs-tree work/rootfs --output-dir work/vm/fidelity
  --layout captured|projection --layout-json <file>
  --rw-data <dir> --navi <dir> --navi2 <dir>
  --assets <dir> --fill-inactive --weston-wrapper --micom-standin

Build inputs must come from the owner's own staged/decrypted firmware.
EOF
}

if [ "${1:-}" = "fidelity" ] || [ "${1:-}" = "usable" ]; then
    VM_PY="${VM_PY:-python3}"
    exec "$VM_PY" -B "$SCOURGE_ROOT/tools/vm/build_vm.py" "$@"
else
    usage
    exit 2
fi
