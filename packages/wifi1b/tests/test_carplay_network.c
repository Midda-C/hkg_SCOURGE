/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Offline and fixture-command tests for the C2 Wifi1b AP network
 * prerequisites: IPv6 link-local retention, exact IPv4/IPv6 readiness,
 * live AP readback, split configured/observed IE state, owned firewall
 * rules and ownership-safe CarPlay control.
 *
 * This file never executes a real network/firewall command: every command
 * goes through the WIFI_TEST hook. The namespace checks live in the Python
 * driver and run inside an unshared network namespace.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fast_usleep(useconds_t);
#define usleep fast_usleep
#define WIFI_TEST 1
#define main daemon_main
#include "../src/wifi1b_daemon.c"
#undef main
#undef usleep

static int fast_usleep(useconds_t us) { (void)us; return 0; }

#define CMD_MAX 512
static char g_cmds[CMD_MAX][768];
static int g_cmd_count;
static const char *g_iw_fixture;
static const char *g_wpa_fixture;
static const char *g_ip4_fixture;
static const char *g_route4_fixture;
static const char *g_ifconfig_fixture;
static const char *g_iwconfig_fixture;
static const char *g_fail_substring;

static int fixture_command(char *out, size_t n, const char *cmd);

static void fixture_reset(void)
{
    g_cmd_count = 0;
    g_iw_fixture = NULL;
    g_wpa_fixture = NULL;
    g_ip4_fixture = NULL;
    g_route4_fixture = NULL;
    g_ifconfig_fixture = NULL;
    g_iwconfig_fixture = NULL;
    g_fail_substring = NULL;
    test_command = fixture_command;
    test_iface = 1;
    test_owned_wpa = 1;
    test_dhcpd_start = -1;
}

static void record_command(const char *cmd)
{
    if (g_cmd_count < CMD_MAX)
        snprintf(g_cmds[g_cmd_count++], sizeof(g_cmds[0]), "%s", cmd);
}

static int fixture_command(char *out, size_t n, const char *cmd)
{
    record_command(cmd);
    if (out && n) out[0] = '\0';
    if (g_fail_substring && strstr(cmd, g_fail_substring)) return 1;
    if (strstr(cmd, " -C ")) return 1; /* force idempotent inserts */
    if (strstr(cmd, "iw dev wlan0 info")) {
        if (!g_iw_fixture) return 1;
        snprintf(out, n, "%s", g_iw_fixture);
        return 0;
    }
    if (strstr(cmd, "wpa_cli") && strstr(cmd, "status")) {
        if (!g_wpa_fixture) return 1;
        snprintf(out, n, "%s", g_wpa_fixture);
        return 0;
    }
    if (strstr(cmd, "iwconfig wlan0")) {
        if (!g_iwconfig_fixture) return 1;
        snprintf(out, n, "%s", g_iwconfig_fixture);
        return 0;
    }
    if (strstr(cmd, "ip -o -4 addr show dev wlan0")) {
        if (!g_ip4_fixture) return 1;
        snprintf(out, n, "%s", g_ip4_fixture);
        return 0;
    }
    if (strstr(cmd, "ip -4 route show dev wlan0")) {
        if (!g_route4_fixture) return 1;
        snprintf(out, n, "%s", g_route4_fixture);
        return 0;
    }
    if (strstr(cmd, "ifconfig wlan0")) {
        if (!g_ifconfig_fixture) return 1;
        snprintf(out, n, "%s", g_ifconfig_fixture);
        return 0;
    }
    return 0;
}

static bool command_seen(const char *needle)
{
    for (int i = 0; i < g_cmd_count; i++)
        if (strstr(g_cmds[i], needle)) return true;
    return false;
}

static void assert_no_command(const char *needle)
{
    for (int i = 0; i < g_cmd_count; i++) {
        if (strstr(g_cmds[i], needle)) {
            fprintf(stderr, "FAIL: unexpected command containing '%s': %s\n",
                    needle, g_cmds[i]);
            exit(1);
        }
    }
}

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    assert(fputs(text, f) >= 0);
    assert(fclose(f) == 0);
}

static void write_owner(const char *text)
{
    write_text(RADIO_OWNER_PATH, text);
}

static void remove_owner(void)
{
    unlink(RADIO_OWNER_PATH);
}

static const char *g_if_inet6_path;
static const char *g_ipv6_conf_dir;
static char g_if_inet6_path_buf[512];
static char g_ipv6_conf_dir_buf[512];

static void prepare_ipv6_fixture(const char *inet6_text, int disabled)
{
    snprintf(g_if_inet6_path_buf, sizeof(g_if_inet6_path_buf),
             "%s/if_inet6", APP_DIR);
    write_text(g_if_inet6_path_buf, inet6_text);
    g_if_inet6_path = g_if_inet6_path_buf;
    test_if_inet6_path = g_if_inet6_path;

    snprintf(g_ipv6_conf_dir_buf, sizeof(g_ipv6_conf_dir_buf),
             "%s/ipv6conf", APP_DIR);
    mkdir(g_ipv6_conf_dir_buf, 0700);
    const char *levels[] = {"wlan0", "all", "default"};
    for (size_t i = 0; i < sizeof(levels)/sizeof(levels[0]); i++) {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s/ipv6conf/%s", APP_DIR, levels[i]);
        mkdir(dir, 0700);
        char file[512];
        snprintf(file, sizeof(file), "%s/disable_ipv6", dir);
        write_text(file, disabled ? "1\n" : "0\n");
    }
    g_ipv6_conf_dir = g_ipv6_conf_dir_buf;
    test_ipv6_conf_dir = g_ipv6_conf_dir;
}

static void set_ap_fixtures(void)
{
    g_iw_fixture =
        "Interface wlan0\n"
        "\tifindex 3\n"
        "\twdev 0x1\n"
        "\tssid IONIQ5-CarPlay-B\n"
        "\ttype AP\n"
        "\twiphy 0\n"
        "\tchannel 36 (5180 MHz), width: 20 MHz, center1: 5180 MHz\n";
    g_wpa_fixture =
        "ssid=IONIQ5-CarPlay-B\n"
        "mode=AP\n"
        "key_mgmt=WPA2-PSK\n"
        "wpa_state=COMPLETED\n";
    g_ip4_fixture =
        "3: wlan0    inet 10.42.0.1/24 brd 10.42.0.255 scope global wlan0\\       "
        "valid_lft forever preferred_lft forever\n";
    g_ifconfig_fixture = NULL;
    g_iwconfig_fixture = NULL;
}

static void test_linklocal_parser(void)
{
    cp_net_linklocal ll;
    const char *text =
        "00000000000000000000000000000001 01 80 10 80       lo\n"
        "fe800000000000000000000000000001 03 40 20 40    wlan0\n"
        "fe800000000000000000000000000002 03 40 20 08    wlan0\n"
        "20010db8000000000000000000000001 03 40 00 80    wlan0\n"
        "fe800000000000000000000000000009 04 40 20 80    eth9\n"
        "fe800000000000000000000000000003 03 40 20 80    wlan0\n";

    assert(cp_net_parse_if_inet6(text, "wlan0", 0, &ll));
    assert(ll.usable);
    assert(ll.any_linklocal && ll.tentative && ll.dad_failed);
    assert(ll.ifindex == 3 && ll.prefix_len == 0x40 && ll.scope == 0x20);
    assert(!strcmp(ll.address, "fe80::3"));
    assert(!strcmp(ll.scoped, "fe80::3%wlan0"));

    /* A mismatched ifindex must not accept another interface's address. */
    assert(!cp_net_parse_if_inet6(text, "wlan0", 4, &ll));

    const char *tentative_only =
        "fe800000000000000000000000000001 03 40 20 40    wlan0\n";
    assert(!cp_net_parse_if_inet6(tentative_only, "wlan0", 0, &ll));
    assert(ll.any_linklocal && ll.tentative && !ll.usable);

    const char *dad_failed_only =
        "fe800000000000000000000000000002 03 40 20 08    wlan0\n";
    assert(!cp_net_parse_if_inet6(dad_failed_only, "wlan0", 0, &ll));
    assert(ll.any_linklocal && ll.dad_failed && !ll.usable);

    const char *global_only =
        "20010db8000000000000000000000001 03 40 00 80    wlan0\n";
    assert(!cp_net_parse_if_inet6(global_only, "wlan0", 0, &ll));

    /* Truncated/garbage lines are ignored, never partially accepted. */
    assert(!cp_net_parse_if_inet6("fe80 03 40 20 80 wlan0\n", "wlan0", 0, &ll));
    assert(!cp_net_parse_if_inet6("", "wlan0", 0, &ll));
}

static void test_command_parsers(void)
{
    cp_net_iw_info iw;
    cp_net_wpa_status wpa;
    char ip[64];

    assert(cp_net_parse_iw_info(
        "\tifindex 3\n"
        "\tssid IONIQ5\\x2dCarPlay\n"
        "\ttype AP\n"
        "\tchannel 36 (5180 MHz), width: 20 MHz\n", &iw));
    assert(iw.ifindex == 3 && iw.is_ap && !iw.is_managed);
    assert(iw.have_ssid && !strcmp(iw.ssid, "IONIQ5-CarPlay"));
    assert(iw.channel == 36 && iw.frequency == 5180);

    assert(cp_net_parse_wpa_status(
        "ssid=IONIQ5-CarPlay\n"
        "mode=AP\n"
        "key_mgmt=WPA2-PSK\n"
        "wpa_state=COMPLETED\n", &wpa));
    assert(wpa.ap_mode && wpa.completed && !wpa.station_mode);
    assert(!strcmp(wpa.ssid, "IONIQ5-CarPlay"));
    assert(!strcmp(wpa.key_mgmt, "WPA2-PSK"));

    assert(cp_net_parse_ip4_addr(
        "3: wlan0    inet 10.42.0.1/24 brd 10.42.0.255 scope global wlan0\n",
        "wlan0", ip, sizeof(ip)));
    assert(!strcmp(ip, "10.42.0.1"));
    assert(!cp_net_parse_ip4_addr(
        "4: wlan9    inet 10.42.0.1/24 brd 10.42.0.255 scope global wlan9\n",
        "wlan0", ip, sizeof(ip)));
    assert(cp_net_freq_to_channel(5180) == 36);
    assert(cp_net_freq_to_channel(2437) == 6);
}

static void test_probe_readiness_and_status(void)
{
    char status[16384];
    const char *usable =
        "fe800000000000000000000000000003 03 40 20 80    wlan0\n";

    fixture_reset();
    test_command = fixture_command;
    test_iface = 1;
    test_owned_wpa = 1;
    set_ap_fixtures();
    prepare_ipv6_fixture(usable, 0);

    config_defaults();
    str_copy(g_cfg.cp_ssid, sizeof(g_cfg.cp_ssid), "IONIQ5-CarPlay-B");
    str_copy(g_cfg.cp_psk, sizeof(g_cfg.cp_psk), "fixture-secret-passphrase");
    g_cfg.cp_band = 2;
    g_cfg.cp_channel = 36;

    ap_probe_t p;
    ap_probe(&p);
    assert(p.state == AP_READY);
    assert(p.role == 1 && !p.conflict);
    assert(p.ipv4_ready && !strcmp(p.ip, WIFI1B_AP_IP));
    assert(p.ipv6_ready && !strcmp(p.linklocal, "fe80::3%wlan0"));
    assert(p.ifindex == 3);
    assert(p.frequency == 5180 && p.channel == 36);
    assert(p.channel_matches_config);
    assert(!strcmp(p.security, "WPA2-PSK"));
    assert(!strcmp(p.ssid, "IONIQ5-CarPlay-B"));
    assert(strstr(p.security, "fixture-secret-passphrase") == NULL);
    assert(strstr(p.linklocal, "fixture-secret-passphrase") == NULL);
    assert(strstr(p.ssid, "fixture-secret-passphrase") == NULL);

    /* A stale configured channel must not replace the observed 5 GHz one. */
    g_cfg.cp_channel = 149;
    ap_probe(&p);
    assert(p.frequency == 5180 && p.channel == 36);
    assert(!p.channel_matches_config);
    assert(p.state == AP_READY); /* actual channel is known and valid */
    g_cfg.cp_channel = 36;

    /* wpa and iw disagreeing about frequency is a conflict, not readiness. */
    g_wpa_fixture =
        "ssid=IONIQ5-CarPlay-B\n"
        "mode=AP\n"
        "key_mgmt=WPA2-PSK\n"
        "freq=2437\n";
    ap_probe(&p);
    assert(p.conflict && p.state != AP_READY);

    /* A non-exact IPv4 address is not AP readiness. */
    set_ap_fixtures();
    g_ip4_fixture =
        "3: wlan0    inet 10.42.0.2/24 brd 10.42.0.255 scope global wlan0\n";
    ap_probe(&p);
    assert(!p.ipv4_ready && p.state == AP_INTERFACE_UP);

    /* No readable frequency means the on-air channel cannot be verified. */
    g_ip4_fixture =
        "3: wlan0    inet 10.42.0.1/24 brd 10.42.0.255 scope global wlan0\n";
    g_iw_fixture =
        "Interface wlan0\n\tifindex 3\n\tssid IONIQ5-CarPlay-B\n\ttype AP\n";
    ap_probe(&p);
    assert(p.frequency == 0 && p.channel == 0 && p.state == AP_INTERFACE_UP);

    /* Observed status readback, with split configured vs observed IE. */
    set_ap_fixtures();
    ap_probe(&p);
    assert(p.state == AP_READY);
    memset(&g_observed, 0, sizeof(g_observed));
    g_observed.gen = g_apply_gen;
    g_observed.ap = p;
    g_observed.ap_on = p.state == AP_READY;
    str_copy(g_observed.ap_ssid, sizeof(g_observed.ap_ssid), p.ssid);
    g_observed_ms = now_ms();
    g_carplay_ie = true;
    build_status(status, sizeof(status));
    assert(strstr(status, "\"ap_ssid\":\"IONIQ5-CarPlay-B\""));
    assert(strstr(status, "\"ap_channel\":36"));
    assert(strstr(status, "\"ap_frequency\":5180"));
    assert(strstr(status, "\"ap_security\":\"WPA2-PSK\""));
    assert(strstr(status, "\"ap_linklocal\":\"fe80::3%wlan0\""));
    assert(strstr(status, "\"ap_ifindex\":3"));
    assert(strstr(status, "\"ap_ipv4_ready\":true"));
    assert(strstr(status, "\"ap_ipv6_ready\":true"));
    assert(strstr(status, "\"ap_network_ready\":true"));
    assert(strstr(status, "\"carplay_ie_configured\":true"));
    assert(strstr(status, "\"carplay_ie_observed\":false"));
    assert(strstr(status, "\"carplay_ie_observation\":\"unverified\""));
    assert(strstr(status, "\"ap_security\":\"fixture-secret-passphrase\"") == NULL);
}

static void test_ipv6_failures(void)
{
    ap_probe_t p;
    const char *tentative =
        "fe800000000000000000000000000001 03 40 20 40    wlan0\n";
    const char *dad_failed =
        "fe800000000000000000000000000002 03 40 20 08    wlan0\n";
    const char *global_only =
        "20010db8000000000000000000000001 03 40 00 80    wlan0\n";

    fixture_reset();
    test_command = fixture_command;
    test_iface = 1;
    test_owned_wpa = 1;
    set_ap_fixtures();
    config_defaults();
    str_copy(g_cfg.cp_ssid, sizeof(g_cfg.cp_ssid), "IONIQ5-CarPlay-B");
    str_copy(g_cfg.cp_psk, sizeof(g_cfg.cp_psk), "fixture-secret-passphrase");
    g_cfg.cp_band = 2;
    g_cfg.cp_channel = 36;

    prepare_ipv6_fixture(tentative, 0);
    ap_probe(&p);
    assert(!p.ipv6_ready && !p.ipv6_unavailable);
    assert(strstr(p.ipv6_error, "tentative") != NULL);
    assert(p.state == AP_INTERFACE_UP);
    assert(ap_wait_ready(false, &p) == AP_WAIT_PENDING);

    prepare_ipv6_fixture(dad_failed, 0);
    ap_probe(&p);
    assert(!p.ipv6_ready && p.ipv6_unavailable);
    assert(strstr(p.ipv6_error, "DAD failed") != NULL);
    assert(ap_wait_ready(false, &p) == AP_WAIT_IPV6_BLOCKED);

    prepare_ipv6_fixture(global_only, 1);
    ap_probe(&p);
    assert(!p.ipv6_ready && p.ipv6_unavailable);
    assert(strstr(p.ipv6_error, "disable_ipv6=1") != NULL);
    assert(ap_wait_ready(false, &p) == AP_WAIT_IPV6_BLOCKED);
    /* Diagnosis is read-only: the fixture still says disabled. */
    char value[32];
    assert(w2_ipv6_conf_read("wlan0", "disable_ipv6", value, sizeof(value)));
    assert(!strcmp(trim(value), "1"));
}

static void test_firewall_rules(void)
{
    fixture_reset();
    test_command = fixture_command;
    test_iface = 1;
    config_defaults();

    assert(ap_fw6_open());
    assert(command_seen("-N WIFI1B_APP_AP6"));
    assert(command_seen("-C INPUT -i wlan0 -j WIFI1B_APP_AP6"));
    assert(command_seen("-I INPUT -i wlan0 -j WIFI1B_APP_AP6"));
    assert(command_seen("-p ipv6-icmp -j ACCEPT"));
    assert(command_seen("-d ff00::/8 -j ACCEPT"));
    assert(command_seen("--dport 547"));
    assert_no_command("-I INPUT -j ");
    assert_no_command("-A INPUT ");

    g_cmd_count = 0;
    assert(carplay_fw_open());
    assert(command_seen("-p udp --dport 5020 -j ACCEPT"));
    assert(command_seen("-p tcp --dport 6030 -j ACCEPT"));
    assert(command_seen("-p udp --dport 5353 -j ACCEPT"));
    assert(command_seen("-p tcp --dport 7000 -j ACCEPT"));
    assert(command_seen("-p ipv6-icmp -j ACCEPT"));
    assert(command_seen("-d ff00::/8 -j ACCEPT"));
    assert_no_command("-I INPUT -j ");
    assert_no_command("-I FORWARD");
    assert_no_command(" -A INPUT ");

    /* IPv4 AP chain keeps its existing blanket accept only inside the
     * owned interface chain; no new interface/global hook is added. */
    g_cmd_count = 0;
    assert(ap_fw_open());
    assert(command_seen("-C WIFI1B_APP_AP -j ACCEPT") ||
           command_seen("-I WIFI1B_APP_AP -j ACCEPT"));
    assert_no_command("-I INPUT -j ");
}

static void test_services_and_flush(void)
{
    fixture_reset();
    test_command = fixture_command;
    test_iface = 1;
    config_defaults();

    test_dhcpd_start = 0;
    assert(!ap_open_services());
    assert(strstr(g_last_msg, "DHCP server failed") != NULL);

    test_dhcpd_start = 1;
    g_fail_substring = "tables";
    assert(!ap_open_services());
    assert(strstr(g_last_msg, "firewall setup failed") != NULL);

    test_dhcpd_start = -1;
    g_fail_substring = NULL;

    /* IPv4 cleanup is explicitly family-specific. */
    g_cmd_count = 0;
    w2_iface_flush();
    assert(command_seen("ip -4 addr flush dev wlan0"));
    assert_no_command("ip addr flush dev wlan0 ");
}

static void test_carplay_control_ownership(void)
{
    fixture_reset();
    test_command = fixture_command;
    test_iface = 1;
    config_defaults();

    write_owner("wifi_daemon:999999\n");
    str_copy(g_cfg.cp_ssid, sizeof(g_cfg.cp_ssid), "IONIQ5-CarPlay-B");
    g_cfg.carplay_control = 1;
    g_cfg.mode = MODE_CARPLAY;
    g_cfg.wlan_on = true;
    g_cmd_count = 0;
    g_last_msg[0] = 0;
    assert(apply_carplay_control(false));
    assert(g_cmd_count == 0);
    assert(g_cfg.mode == MODE_OFF && !g_cfg.wlan_on);
    assert(strstr(g_last_msg, "wifi_daemon") != NULL);

    g_cfg.carplay_control = 0;
    g_cfg.mode = MODE_CARPLAY;
    g_cfg.wlan_on = true;
    g_cmd_count = 0;
    g_last_msg[0] = 0;
    assert(apply_carplay_control(false));
    assert(g_cmd_count == 0);
    assert(strstr(g_last_msg, "wifi_daemon") != NULL);

    /* Stock handoff must not run any teardown/eviction command on a
     * peer-owned radio, and only the OEM manager commands when idle. */
    g_cmd_count = 0;
    carplay_stock_handoff(false);
    assert(g_cmd_count == 0);

    remove_owner();
    g_cmd_count = 0;
    carplay_stock_handoff(false);
    assert(command_seen("systemctl unmask supplicantmanager.path"));
    assert(command_seen("systemctl start supplicantmanager.path"));
    assert_no_command("pkill");
    assert_no_command("connmanctl tether");
    assert_no_command("ap_stop");
}

static void test_ap_probe_without_interface(void)
{
    fixture_reset();
    test_command = fixture_command;
    test_iface = 0;
    config_defaults();
    ap_probe_t p;
    ap_probe(&p);
    assert(p.role == 0);
    assert(p.state == AP_ABSENT);
    assert(p.ip[0] == 0 && p.linklocal[0] == 0);
}

static int lease_slot_for(const uint8_t mac[6])
{
    for (size_t i = 0; i < sizeof(g_w2_leases)/sizeof(g_w2_leases[0]); i++)
        if (g_w2_leases[i].used && !memcmp(g_w2_leases[i].mac, mac, 6))
            return (int)i;
    return -1;
}

static void test_dhcp_release_gates(void)
{
    uint8_t opts[312], mac[6] = {0x02, 0x00, 0x11, 0x22, 0x33, 0x44};
    uint8_t *cid;
    size_t discover_len, request_len;

    assert(w2_mask_to_prefix(0xffffff00U) == 24);
    assert(w2_mask_to_prefix(0xffffffffU) == 32);
    assert(w2_mask_to_prefix(0x80000000U) == 1);
    assert(w2_mask_to_prefix(0xffff00ffU) == -1);

    memset(opts, 0, sizeof(opts));
    discover_len = w2_dhcp_client_opts(opts, sizeof(opts), 1, mac, 0, 0, false);
    cid = w2_dhcp_opt(opts, discover_len, 61);
    assert(cid != NULL && cid[0] == 1 && !memcmp(cid + 1, mac, 6));
    assert(w2_dhcp_opt(opts, discover_len, 53) != NULL);
    assert(w2_dhcp_opt(opts, discover_len, 55) != NULL);

    memset(opts, 0, sizeof(opts));
    request_len = w2_dhcp_client_opts(opts, sizeof(opts), 3, mac,
                                      w2_ip4("10.42.0.10"),
                                      w2_ip4(WIFI1B_AP_IP), true);
    cid = w2_dhcp_opt(opts, request_len, 61);
    assert(cid != NULL && cid[0] == 1 && !memcmp(cid + 1, mac, 6));
    assert(w2_dhcp_opt(opts, request_len, 50) != NULL);
    assert(w2_dhcp_opt(opts, request_len, 54) != NULL);
    assert(w2_dhcp_opt(opts, request_len, 55) != NULL);

    fixture_reset();
    test_command = fixture_command;
    g_ip4_fixture =
        "3: wlan0    inet 10.42.0.10/24 brd 10.42.0.255 scope global wlan0\n";
    g_route4_fixture =
        "10.42.0.0/24 dev wlan0 proto kernel scope link src 10.42.0.10\n";
    assert(w2_addr_present("wlan0", "10.42.0.10", 24));
    assert(!w2_addr_present("wlan0", "10.42.0.11", 24));
    assert(w2_connected_route_present("wlan0", "10.42.0.10",
                                      0xffffff00U, 24));
    assert(!w2_connected_route_present("wlan0", "10.42.0.10",
                                       0xffffff00U, 25));

    memset(g_w2_leases, 0, sizeof(g_w2_leases));
    time_t wall = time(NULL);
    char leases[1024], path[512];
    snprintf(path, sizeof(path), "%s", WIFI1B_DHCP_LEASE_FILE);
    snprintf(leases, sizeof(leases),
             "# mac ip expires_unix\n"
             "02:00:11:22:33:44 10.42.0.10 %lld\n"
             "02:00:11:22:33:45 10.42.0.11 %lld\n"
             "02:00:11:22:33:46 10.42.0.250 %lld\n"
             "garbage line\n",
             (long long)wall + 3600, (long long)wall - 10,
             (long long)wall + 3600);
    write_text(path, leases);
    g_w2_leases[0].used = false;
    w2_lease_load();
    uint8_t mac1[6], mac2[6], mac3[6];
    assert(parse_mac6("02:00:11:22:33:44", mac1));
    assert(parse_mac6("02:00:11:22:33:45", mac2));
    assert(parse_mac6("02:00:11:22:33:46", mac3));
    assert(w2_lease_for_mac(mac1, now_ms()) == w2_ip4("10.42.0.10"));
    assert(w2_lease_for_mac(mac2, now_ms()) == 0);
    assert(w2_lease_for_mac(mac3, now_ms()) == 0);
    int slot = lease_slot_for(mac1);
    assert(slot >= 0);
    int64_t before = g_w2_leases[slot].expires_ms;
    assert(before > now_ms() + 3500000);
    g_w2_leases[slot].expires_ms = now_ms() + 1000;
    assert(w2_lease_for_mac(mac1, now_ms()) == w2_ip4("10.42.0.10"));
    assert(g_w2_leases[slot].expires_ms > now_ms() + 3500000);
    w2_lease_write();
    char saved[2048];
    assert(read_file(path, saved, sizeof(saved)) > 0);
    assert(strstr(saved, "02:00:11:22:33:44 10.42.0.10"));
    assert(strstr(saved, "02:00:11:22:33:45") == NULL);
    assert(strstr(saved, "10.42.0.250") == NULL);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--flush")) {
        w2_iface_flush();
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--fw-open")) {
        config_defaults();
        return carplay_fw_open() ? 0 : 1;
    }
    if (argc > 1 && !strcmp(argv[1], "--fw-open-ap")) {
        config_defaults();
        return ap_fw_open() ? 0 : 1;
    }
    if (argc > 1 && !strcmp(argv[1], "--query-status")) {
        fixture_reset();
        test_command = fixture_command;
        set_ap_fixtures();
        prepare_ipv6_fixture(
            "fe800000000000000000000000000003 03 40 20 80    wlan0\n", 0);
        config_defaults();
        char *args[] = {(char *)"wifi1b_daemon_test", (char *)"--status", NULL};
        return daemon_main(2, args);
    }
    if (argc > 1 && !strcmp(argv[1], "--query-port")) {
        fixture_reset();
        test_command = fixture_command;
        config_defaults();
        char *args[] = {(char *)"wifi1b_daemon_test", (char *)"--choose-port",
                        (char *)"2224", NULL};
        return daemon_main(3, args);
    }

    mkdir(APP_DIR, 0700);
    test_linklocal_parser();
    test_command_parsers();
    test_probe_readiness_and_status();
    test_ipv6_failures();
    test_firewall_rules();
    test_services_and_flush();
    test_carplay_control_ownership();
    test_dhcp_release_gates();
    test_ap_probe_without_interface();
    puts("PASS: WiFi1b C2 AP IPv4/IPv6 readiness, readback, firewall and ownership helpers");
    return 0;
}
