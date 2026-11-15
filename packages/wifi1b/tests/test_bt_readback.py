#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Gen5W_L EU 260128
"""Run actual boolean/pairing functions with mocked IPC; no OEM code/radio."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/wifi1b_daemon.c').read_text()
functions = source[source.index('static int bt_get_prop_bool('):source.index('\n#if 0', source.index('static int bt_get_prop_bool('))]
assert '"/lge1"' in source
assert '"org.bluez.Adapter.Adapter.v2_0_lge1"' in source
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
static int vendor = 1, standard = -1, disc = 1, pair = 1, power = 1;
static int vendor_calls, property_calls;
static bool g_bt_discoverable, g_bt_pairable, g_pair_stop_confirmed;
static char g_pair_msg[512];
static struct { char cp_btaddr[64]; } g_cfg;
static bool bt_call(const char *method, const char *arg, char *out, size_t n) {
    vendor_calls++;
    assert(!arg);
    assert(strncmp(method, "get", 3) == 0 && strstr(method, "Attribute"));
    if (vendor < 0) return false;
    int value = strstr(method,"Discoverable") ? disc : strstr(method,"Pairable") ? pair : power;
    snprintf(out,n,"method return\n boolean %s\n",value ? "true" : "false");
    return true;
}
static bool bt_props_get(const char *name, char *out, size_t n) {
    property_calls++;
    if (standard < 0) return false;
    snprintf(out,n,"variant boolean %s",standard ? "true" : "false");
    return true;
}
static bool bt_set_prop(const char *name, bool on) { return true; }
static void bt_fetch_address(void) {}
static void bt_state_save(void) {}
static void set_msg(const char *fmt, const char *msg) {}
static void usleep(int us) {}
'''
checks = r'''
int main(void) {
    assert(bt_get_prop_bool("Discoverable") == 1);
    assert(vendor_calls == 1 && property_calls == 0);
    disc = 0;
    assert(bt_get_prop_bool("Discoverable") == 0);
    vendor = -1; standard = 1;
    assert(bt_get_prop_bool("Discoverable") == 1);
    standard = 0;
    assert(bt_get_prop_bool("Discoverable") == 0);
    standard = -1;
    assert(bt_get_prop_bool("Discoverable") == -1);
    assert(!bt_set_pairing(true));
    assert(!bt_set_pairing(false) && !g_pair_stop_confirmed);
    vendor = 1; disc = pair = power = 1;
    assert(bt_set_pairing(true));
    pair = 0;
    assert(!bt_set_pairing(true));
    pair = 1; power = 0;
    assert(!bt_set_pairing(true));
    disc = pair = 0;
    assert(bt_set_pairing(false) && g_pair_stop_confirmed);
    puts("PASS: vendor getters, BlueZ fallback, unknown state and both pairing directions");
}
'''
with tempfile.TemporaryDirectory(prefix='wifi1b-bt-', dir='/tmp/opencode') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(prefix + functions + checks)
    subprocess.run(['cc', '-Wall', '-Werror', '-Wno-unused-parameter', '-o', str(path / 'test'), str(path / 'test.c')], check=True)
    subprocess.run([str(path / 'test')], check=True)
