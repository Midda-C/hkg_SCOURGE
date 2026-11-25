#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Ioniq 5 Gen5W_L, firmware NE_E.EUR.S5W_L.001.001.260128 (EU).
#
# Projection-only init wrapper: mount the interfaces esinit needs, force the
# MICOM SPI validation parameter to 1, and hand over to the vendor esinit.
# The vendor driver can overwrite a command-line value during probe, so this
# helper holds the parameter across the poll window. It is a lab substitute,
# not the shipped MICOM SPI handshake.
set -u
export LC_ALL=C

mount -t proc proc /proc 2>/dev/null || true
mount -t sysfs sysfs /sys 2>/dev/null || true
mkdir -p /run /tmp 2>/dev/null || true

# Hold the gate open while esinit polls. If the module is not present, the
# real gate stays shut and esinit follows its NOK branch.
(
    _i=0
    while [ "$_i" -lt 120 ]; do
        if [ -e /sys/module/micom/parameters/spi_validation ]; then
            printf '1\n' > /sys/module/micom/parameters/spi_validation \
                2>/dev/null || true
        fi
        _i=$((_i + 1))
        sleep 0.1
    done
) &

exec /lib/systemd/esinit
