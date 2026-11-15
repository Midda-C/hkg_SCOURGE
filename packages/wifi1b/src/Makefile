# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
LDFLAGS ?= -static
BUILD_ID = $(shell python3 ../scripts/build_id.py)

all: wifi1b_daemon wifi1b_qmlpatch

wifi1b_daemon: wifi1b_daemon.c wifi1b_backend.inc ../../common/radio_owner.h FORCE
	$(CC) $(CFLAGS) -DWIFI_BUILD_ID='"$(BUILD_ID)"' $(LDFLAGS) -o $@ wifi1b_daemon.c

wifi1b_qmlpatch: wifi1b_qmlpatch.c FORCE
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ wifi1b_qmlpatch.c

clean:
	rm -f wifi1b_daemon wifi1b_qmlpatch

FORCE:
.PHONY: all clean FORCE
