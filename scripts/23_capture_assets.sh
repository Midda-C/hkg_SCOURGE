#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Ioniq 5 Gen5W_L, firmware NE_E.EUR.S5W_L.001.001.260128 (EU).
# 23_capture_assets.sh — dispatcher for the car asset capture/extraction lane.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/lib.sh"

usage() {
    cat >&2 <<'EOF'
usage: bash scripts/23_capture_assets.sh inventory|dump|import|extract|install [options]

  inventory   read-only runtime inventory (run on the car or via ssh)
  dump        on-car dump to a mounted USB (explicit approval required)
  import      verify and copy a capture into a local gitignored directory
  extract     bounded asset extraction + metadata manifest
  install     copy extracted assets into a VM host share

Car actions require SCOURGE_CAR_CONFIRM=1 or SCOURGE_CAPTURE_CONFIRM=1.
EOF
}

case "${1:-}" in
    inventory)
        shift
        SCOURGE_CAR_CONFIRM="${SCOURGE_CAR_CONFIRM:-0}" \
            bash "$SCOURGE_ROOT/tools/car-assets/capture_inventory.sh" "$@"
        ;;
    dump)
        shift
        SCOURGE_CAPTURE_CONFIRM="${SCOURGE_CAPTURE_CONFIRM:-0}" \
            bash "$SCOURGE_ROOT/tools/car-assets/capture_dump.sh" "$@"
        ;;
    import)
        shift
        exec "${ASSETS_PY:-python3}" "$SCOURGE_ROOT/tools/car-assets/import_capture.py" "$@"
        ;;
    extract)
        shift
        exec "${ASSETS_PY:-python3}" "$SCOURGE_ROOT/tools/car-assets/extract_assets.py" "$@"
        ;;
    install)
        shift
        exec "${ASSETS_PY:-python3}" "$SCOURGE_ROOT/tools/car-assets/install_assets.py" "$@"
        ;;
    *)
        usage
        exit 2
        ;;
esac
