/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Orange Pi USB-SSH lifecycle: with WiFi off, SSH must stay deferred until
 * usb0 exists and must start/stop on that transport.  The fixture uses a
 * compile-time APP_DIR and a fake dropbear binary; no host network is touched.
 */
#define WIFI_TEST 1
#define main daemon_main
#include "../src/wifi1b_daemon.c"
#undef main

static int write_marker(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    close(fd);
    return 0;
}

int main(void)
{
    config_defaults();
    g_cfg.mode = MODE_OFF;
    g_cfg.wlan_on = false;
    g_cfg.ssh_enabled = true;
    g_cfg.ssh_port = 2224;

    unlink(WIFI1B_USB0_SYSFS);
    if (ssh_start() != true) return 2;
    if (owned_dropbear_pid() > 0) return 3;

    if (write_marker(WIFI1B_USB0_SYSFS) != 0) return 4;
    if (ssh_start() != true) return 5;
    int pid = owned_dropbear_pid();
    if (pid <= 0) return 6;
    ssh_stop();
    if (owned_dropbear_pid() > 0) return 7;

    if (!g_cfg.ssh_enabled) return 8;
    g_cfg.ssh_enabled = false;
    if (ssh_start() != false) return 9;
    unlink(WIFI1B_USB0_SYSFS);
    return 0;
}
