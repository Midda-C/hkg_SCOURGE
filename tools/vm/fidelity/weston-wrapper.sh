#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Ioniq 5 Gen5W_L, firmware NE_E.EUR.S5W_L.001.001.260128 (EU).
#
# Projection-only Weston wrapper. The vendor launcher strips LIBGL_* from its
# children, so AppHomeScreen cannot find swrast. This wrapper supplies the
# software-GL driver path and strips the unsupported --tty=1 option; it calls
# the preserved vendor binary at /usr/bin/weston.vendor.
set -u
export LC_ALL=C

REAL=/usr/bin/weston.vendor
if [ ! -x "$REAL" ]; then
    REAL=/usr/bin/weston.real
fi
if [ ! -x "$REAL" ]; then
    echo 'weston-wrapper: vendor weston binary not found' >&2
    exit 1
fi

export GALLIUM_DRIVER=softpipe
export LIBGL_ALWAYS_SOFTWARE=1
export LIBGL_DRIVERS_PATH=/opt/software-egl18/dri
export LD_LIBRARY_PATH=/opt/software-egl18/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export LD_PRELOAD=/usr/lib/libEGL.so.1
if [ ! -d /opt/software-egl18/dri ]; then
    echo 'weston-wrapper: software-GL runtime absent; Weston/AppHomeScreen rendering is WIP' >&2
fi
if [ -f /usr/lib/vm-pixman-solid-shim.so ]; then
    export LD_PRELOAD="/usr/lib/vm-pixman-solid-shim.so:$LD_PRELOAD"
fi

args=()
for arg in "$@"; do
    [ "$arg" = "--tty=1" ] || args+=("$arg")
done

exec "$REAL" "${args[@]}"
