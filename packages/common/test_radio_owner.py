#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Real flock races, crash reservations, stale-owner TAKEOVER and both daemons'
actual entry guards. Never touches the host radio: the interface is a fixture
file and every process runs against a private RADIO_OWNER_PATH."""
from pathlib import Path
import os
import subprocess
import tempfile
import time

packages = Path(__file__).resolve().parents[1]
HARNESS = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define WIFI_TEST 1
#define main daemon_main
#include DAEMON_SOURCE
#undef main

static int forbidden(char *out, size_t n, const char *cmd)
{
    (void)out; (void)n; (void)cmd;
    assert(!"blocked/idle mode executed a radio command");
    return 1;
}

/* argv: hold SECONDS | reserve SECONDS | release | (none) */
int main(int argc, char **argv)
{
    radio_configure(getenv("PEER_COMM") ? getenv("PEER_COMM") : "",
                    getenv("PEER_STATUS") ? getenv("PEER_STATUS") : "");
    radio_set_iface(getenv("IFACE"));
    const char *identity = getenv("IDENTITY");
    assert(identity && identity[0]);
    const char *mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "hold") || !strcmp(mode, "reserve")) {
        if (radio_begin(identity, true) != 1) return 2;
        if (!strcmp(mode, "reserve")) {
            /* Keep the record but drop the flock, then stay alive: this is
             * a live owner between workers (the stuck-record shape). */
            int fd = radio_fd;
            radio_fd = -1;
            close(fd);
        }
        sleep(argc > 2 ? atoi(argv[2]) : 2);
        return 0; /* exit without release: the record persists */
    }
    if (!strcmp(mode, "release")) {
        if (radio_begin(identity, true) != 1) return 2;
        return radio_release() ? 0 : 3;
    }
    /* No-arg guard: blocked active request and idle Off request must both
     * return WITHOUT running any radio command. */
    test_command = forbidden;
    g_cfg.wlan_on = true; g_cfg.mode = MODE_CARPLAY;
    assert(!apply_mode_work(false));
    g_cfg.wlan_on = false; g_cfg.mode = MODE_OFF;
    assert(apply_mode_work(false));
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='radio-test-', dir='/tmp/opencode') as d:
    tmp = Path(d)
    owner = tmp / 'owner'
    peer_status = tmp / 'peer-status.json'
    iface_flags = tmp / 'wlan0-flags'
    harness = tmp / 'harness.c'
    harness.write_text(HARNESS)
    binaries = {}
    for app, comm, peer_comm in (('wifi', 'wifi_daemon', 'wifi1b_daemon'),
                                  ('wifi1b', 'wifi1b_daemon', 'wifi_daemon')):
        exe = tmp / comm  # comm must match the on-car binary name for liveness checks
        subprocess.run(['gcc', '-O0', '-w',
                        f'-DDAEMON_SOURCE="{packages/app/"src"/(app+"_daemon.c")}"',
                        f'-DAPP_DIR="{tmp/app}"',
                        f'-DRADIO_OWNER_PATH="{owner}"',
                        str(harness), '-o', str(exe)], check=True)
        binaries[comm] = (exe, dict(os.environ, IDENTITY=comm, PEER_COMM=peer_comm,
                                    PEER_STATUS=str(peer_status), IFACE=str(iface_flags)))

    def run(comm, *args, timeout=10):
        exe, env = binaries[comm]
        return subprocess.run([str(exe), *map(str, args)], env=env,
                              capture_output=True, text=True, timeout=timeout).returncode

    def record_name():
        data = owner.read_bytes().decode()
        return data.split(':', 1)[0]

    def spawn(comm, *args):
        exe, env = binaries[comm]
        return subprocess.Popen([str(exe), *map(str, args)], env=env,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def set_peer(active):
        peer_status.write_text('{"wlan_on":%s,"mode":"%s"}\n'
                               % ('true' if active else 'false',
                                  'carplay' if active else 'off'))

    def set_iface(state):
        # absent | down | up
        if state == 'absent':
            iface_flags.unlink(missing_ok=True)
        else:
            iface_flags.write_text('0x100%d\n' % (3 if state == 'up' else 2))

    for iteration in range(3):
        owner.write_bytes(b'')
        set_peer(True)
        set_iface('absent')
        # 1. Real race: the flock holder blocks a concurrent contender.
        holder = spawn('wifi_daemon', 'hold', 2)
        for _ in range(100):
            if owner.exists() and record_name() == 'wifi_daemon':
                break
            time.sleep(0.02)
        assert record_name() == 'wifi_daemon', 'holder did not take the reservation'
        assert run('wifi1b_daemon', 'reserve', 1) == 2, 'contender not blocked by live lock holder'
        holder.wait(timeout=10)
        # 2. Holder crashed (record persists, owner process gone): takeover.
        assert record_name() == 'wifi_daemon'
        live = spawn('wifi1b_daemon', 'reserve', 4)
        time.sleep(0.5)
        assert record_name() == 'wifi1b_daemon', 'dead owner not take-able'
        # 3. Live owner, peer reports active: blocked even with idle iface.
        assert run('wifi_daemon', 'reserve', 1) == 2, 'live active peer not blocking'
        # 4. Live owner, peer reports Off, but the interface is busy: blocked.
        set_peer(False)
        set_iface('up')
        assert run('wifi_daemon', 'reserve', 1) == 2, 'busy interface must block takeover'
        # 5. Live owner, peer Off, interface down: takeover.
        set_iface('down')
        taker = spawn('wifi_daemon', 'reserve', 4)
        time.sleep(0.5)
        assert record_name() == 'wifi_daemon', 'live Off peer with idle iface not take-able'
        # 6. Verified release clears the record.
        assert run('wifi_daemon', 'release') == 0
        assert not owner.read_bytes(), 'release did not clear the record'
        live.wait(timeout=10)
        taker.wait(timeout=10)

    # 7. Entry guards: with a live active peer holding the record, both a
    #    blocked activation and an idle Off request run no radio commands.
    owner.write_bytes(b'')
    set_peer(True)
    set_iface('absent')
    peer = spawn('wifi1b_daemon', 'reserve', 3)
    time.sleep(0.5)
    assert record_name() == 'wifi1b_daemon'
    assert run('wifi_daemon') == 0, 'blocked/idle entry guards ran radio commands'
    peer.wait(timeout=10)
    # 8. After the peer exits, a release heals the stale record.
    assert run('wifi_daemon', 'release') == 0
    assert not owner.read_bytes()
print('PASS: races, crash reservations, dead/Off-peer takeover, busy-iface blocking, verified release, entry guards')
