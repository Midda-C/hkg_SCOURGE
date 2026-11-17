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

    /* Regression: in WiFi client mode the WiFi health branches return early
     * when the association is down; the USB ensure must run before them. */
    g_cfg.mode = MODE_CLIENT;
    g_cfg.wlan_on = true;
    g_cfg.ssh_enabled = true;
    g_work_kind = WORK_MODE;
    g_pending_kind = WORK_MODE;
    g_work_pending = false;
    g_apply_pid = 0;
    g_last_watchdog_ms = -60000;
    g_network_failed = false;
    g_boot_dumped = true;
    watchdog();
    if (g_work_kind != WORK_SSH) return 10;
    if (g_apply_pid > 0) {
        waitpid(g_apply_pid, NULL, 0);
        g_apply_pid = 0;
    }
    ssh_stop();

    if (!g_cfg.ssh_enabled) return 8;
    g_cfg.ssh_enabled = false;
    if (ssh_start() != false) return 9;
    unlink(WIFI1B_USB0_SYSFS);
    return 0;
}
