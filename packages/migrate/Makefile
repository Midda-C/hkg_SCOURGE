# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
.PHONY: all payload test clean

all: payload

payload:
	sh scripts/build_payload.sh

test: payload
	python3 tests/test_migrate.py

clean:
	rm -rf out/usb
