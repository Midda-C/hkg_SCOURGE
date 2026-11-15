/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * DHCP client release-gate tests: lease timers, client identity, AF_PACKET
 * parsing and owned default-route policy. No host network or firewall is
 * touched; namespace modes are run by test_carplay_dhcp.py.
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

static char g_route_defaults[2048];
static char g_route_after_add[2048];
static char g_route_after_del[2048];
static bool g_fail_route_add;
static bool g_fail_route_del;
static char g_cmds[64][512];
static int g_cmd_count;

static int fixture_command(char *out, size_t n, const char *cmd);

static void fixture_reset(void)
{
    g_route_defaults[0] = 0;
    g_route_after_add[0] = 0;
    g_route_after_del[0] = 0;
    g_fail_route_add = false;
    g_fail_route_del = false;
    g_cmd_count = 0;
    test_command = fixture_command;
}

static int fixture_command(char *out, size_t n, const char *cmd)
{
    if (g_cmd_count < 64)
        snprintf(g_cmds[g_cmd_count++], sizeof(g_cmds[0]), "%s", cmd);
    if (out && n) out[0] = 0;
    if (strstr(cmd, "ip -4 route show default")) {
        snprintf(out, n, "%s", g_route_defaults);
        return 0;
    }
    if (strstr(cmd, "route add default")) {
        if (g_fail_route_add) return 1;
        if (g_route_after_add[0]) snprintf(g_route_defaults, sizeof(g_route_defaults), "%s", g_route_after_add);
        return 0;
    }
    if (strstr(cmd, "route del default")) {
        if (g_fail_route_del) return 1;
        if (g_route_after_del[0]) snprintf(g_route_defaults, sizeof(g_route_defaults), "%s", g_route_after_del);
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

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    assert(fputs(text, f) >= 0);
    assert(fclose(f) == 0);
}

static void test_masks_and_options(void)
{
    uint8_t opts[312], mac[6] = {0x02, 0x00, 0x11, 0x22, 0x33, 0x44};
    size_t discover_len, request_len;
    uint32_t value = 0;
    struct w2_dhcp_packet packet;

    assert(w2_mask_to_prefix(0xffffff00U) == 24);
    assert(w2_mask_to_prefix(0xffffffffU) == 32);
    assert(w2_mask_to_prefix(0x80000000U) == 1);
    assert(w2_mask_to_prefix(0xfffffffeU) == 31);
    assert(w2_mask_to_prefix(0xffff00ffU) == -1);

    memset(opts, 0, sizeof(opts));
    discover_len = w2_dhcp_client_opts(opts, sizeof(opts), 1, mac, 0, 0, false);
    uint8_t *cid1 = w2_dhcp_opt(opts, discover_len, 61);
    assert(cid1 != NULL && cid1[0] == 1 && !memcmp(cid1 + 1, mac, 6));
    uint8_t cid1_copy[7];
    memcpy(cid1_copy, cid1, 7);
    memset(opts, 0, sizeof(opts));
    request_len = w2_dhcp_client_opts(opts, sizeof(opts), 3, mac,
                                      w2_ip4("10.42.0.10"),
                                      w2_ip4(WIFI1B_AP_IP), true);
    uint8_t *cid2 = w2_dhcp_opt(opts, request_len, 61);
    assert(cid2 != NULL && cid2[0] == 1 && !memcmp(cid2 + 1, mac, 6));
    assert(!memcmp(cid1_copy, cid2, 7));

    memset(&packet, 0, sizeof(packet));
    packet.options[0] = 99; packet.options[1] = 130;
    packet.options[2] = 83; packet.options[3] = 99;
    uint8_t type = 5;
    size_t off = 4;
    off += w2_dhcp_put(packet.options + off, sizeof(packet.options) - off, 53, 1, &type);
    uint32_t lease = htonl(7200);
    off += w2_dhcp_put(packet.options + off, sizeof(packet.options) - off, 51, 4, &lease);
    uint32_t t1 = htonl(3600);
    off += w2_dhcp_put(packet.options + off, sizeof(packet.options) - off, 58, 4, &t1);
    uint32_t t2 = htonl(6300);
    off += w2_dhcp_put(packet.options + off, sizeof(packet.options) - off, 59, 4, &t2);
    uint32_t router = htonl(w2_ip4(WIFI1B_AP_IP));
    off += w2_dhcp_put(packet.options + off, sizeof(packet.options) - off, 3, 4, &router);
    packet.options[off++] = 255;
    size_t n = offsetof(struct w2_dhcp_packet, options) + off;
    assert(w2_dhcp_opt_u32(&packet, n, 51, &value) && value == 7200);
    assert(w2_dhcp_opt_u32(&packet, n, 58, &value) && value == 3600);
    assert(w2_dhcp_opt_u32(&packet, n, 59, &value) && value == 6300);
    assert(w2_dhcp_opt_u32(&packet, n, 3, &value) && value == w2_ip4(WIFI1B_AP_IP));
    assert(!w2_dhcp_opt_u32(&packet, n, 54, &value));
}

static void test_lease_action(void)
{
    int64_t now = 100000;
    assert(dhcp_lease_action(false, now, 0, 0, 0) == DHCP_TICK_ACQUIRE);
    assert(dhcp_lease_action(true, now, 200000, 300000, 400000) == DHCP_TICK_NONE);
    assert(dhcp_lease_action(true, 200000, 200000, 300000, 400000) == DHCP_TICK_RENEW);
    assert(dhcp_lease_action(true, 300000, 200000, 300000, 400000) == DHCP_TICK_REBIND);
    assert(dhcp_lease_action(true, 400000, 200000, 300000, 400000) == DHCP_TICK_ACQUIRE);
    assert(dhcp_lease_action(true, now, 0, 0, 400000) == DHCP_TICK_NONE);
}

static void test_route_policy_units(void)
{
    unsigned int metric = 0;
    w2_owned_route_t route;
    char ledger[512];

    fixture_reset();
    g_route_defaults[0] = 0;
    assert(w2_route_select_metric(&metric) && metric == WIFI1B_CLIENT_DEFAULT_METRIC);

    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 192.0.2.1 dev modem0 proto static metric 100\n");
    assert(w2_route_select_metric(&metric) && metric == 101);
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 192.0.2.1 dev modem0 proto static metric 500\n");
    assert(w2_route_select_metric(&metric) && metric == 501);
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 192.0.2.1 dev modem0 proto static\n");
    assert(w2_route_select_metric(&metric) && metric == 1);
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 192.0.2.1 dev modem0 metric 4294967295\n");
    assert(!w2_route_select_metric(&metric));
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 192.0.2.1 dev modem0 metric 12x\n");
    assert(!w2_route_select_metric(&metric));

    assert(w2_route_line_matches(
        "default via 192.0.2.1 dev modem0 proto 4 metric 100",
        "192.0.2.1", "modem0", 100, 4));
    assert(!w2_route_line_matches(
        "default via 192.0.2.1 dev modem0 proto 4 metric 100",
        "192.0.2.1", "modem0", 101, 4));

    snprintf(ledger, sizeof(ledger), "v1 10.42.0.1 lo 101 66\n");
    assert(w2_owned_route_parse(ledger, &route));
    assert(!strcmp(route.gateway, "10.42.0.1"));
    assert(!strcmp(route.iface, "lo"));
    assert(route.metric == 101 && route.proto == 66);
    snprintf(ledger, sizeof(ledger), "v1 10.42.0.1 lo 101\n");
    assert(!w2_owned_route_parse(ledger, &route));
    snprintf(ledger, sizeof(ledger), "v1 10.42.0.1 lo 101 66 extra\n");
    assert(!w2_owned_route_parse(ledger, &route));
    snprintf(ledger, sizeof(ledger), "v1 10.42.0.1 bad/iface 101 66\n");
    assert(!w2_owned_route_parse(ledger, &route));
}

static void test_route_install_cleanup(void)
{
    fixture_reset();
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 192.0.2.1 dev modem0 proto static metric 100\n");
    snprintf(g_route_after_add, sizeof(g_route_after_add),
             "default via 192.0.2.1 dev modem0 proto static metric 100\n"
             "default via 10.42.0.1 dev lo proto 66 metric 101\n");
    snprintf(g_route_after_del, sizeof(g_route_after_del),
             "default via 192.0.2.1 dev modem0 proto static metric 100\n");
    assert(w2_client_route_install(w2_ip4("10.42.0.1"), "lo"));
    assert(g_dhcp_lease.route_ok);
    assert(g_dhcp_lease.route_metric == 101);
    assert(g_dhcp_lease.route_proto == WIFI1B_CLIENT_ROUTE_PROTO);
    assert(command_seen("route add default via 10.42.0.1 dev lo metric 101 proto 66"));
    char ledger[1024];
    assert(read_file(WIFI1B_CLIENT_ROUTE_LEDGER, ledger, sizeof(ledger)) > 0);
    assert(strstr(ledger, "v1 10.42.0.1 lo 101 66"));
    g_cmd_count = 0;
    assert(w2_owned_route_cleanup());
    assert(command_seen("route del default via 10.42.0.1 dev lo metric 101 proto 66"));
    assert(access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) != 0);
    assert(!command_seen("route flush"));
    assert(!command_seen("modem0"));

    /* Installation/readback failure must be false and leave no ledger. */
    fixture_reset();
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 192.0.2.1 dev modem0 proto static metric 100\n");
    g_fail_route_add = true;
    assert(!w2_client_route_install(w2_ip4("10.42.0.1"), "lo"));
    assert(access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) != 0);

    fixture_reset();
    snprintf(g_route_defaults, sizeof(g_route_defaults),
             "default via 192.0.2.1 dev modem0 proto static metric 100\n");
    snprintf(g_route_after_add, sizeof(g_route_after_add),
             "default via 192.0.2.1 dev modem0 proto static metric 100\n");
    assert(!w2_client_route_install(w2_ip4("10.42.0.1"), "lo"));
    assert(access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) != 0);

    /* A corrupt ledger is never acted on. */
    fixture_reset();
    write_text(WIFI1B_CLIENT_ROUTE_LEDGER, "v1 bad ledger\n");
    g_cmd_count = 0;
    assert(!w2_owned_route_cleanup());
    assert(g_cmd_count == 0);
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
}

static uint16_t checksum_finish(uint32_t sum)
{
    while (sum >> 16) sum = (sum & 0xffffU) + (sum >> 16);
    return (uint16_t)~sum;
}

static size_t build_raw_reply(uint8_t *packet, size_t cap, uint32_t xid,
                              const uint8_t mac[6], bool bad_ip,
                              bool bad_udp, bool fragmented, bool zero_udp)
{
    struct iphdr *ip = (struct iphdr *)packet;
    struct udphdr *udp = (struct udphdr *)(packet + sizeof(*ip));
    struct w2_dhcp_packet *dhcp = (struct w2_dhcp_packet *)(packet + sizeof(*ip) + sizeof(*udp));
    uint8_t opts[128];
    size_t off = 0;
    uint8_t type = 2, cid[7];
    uint32_t sid = htonl(w2_ip4(WIFI1B_AP_IP));
    uint32_t mask = htonl(0xffffff00U);
    uint32_t router = htonl(w2_ip4(WIFI1B_AP_IP));
    uint32_t lease = htonl(3600);

    memset(packet, 0, cap);
    memset(dhcp, 0, sizeof(*dhcp));
    dhcp->op = 2; dhcp->htype = 1; dhcp->hlen = 6; dhcp->xid = xid;
    dhcp->yiaddr = htonl(w2_ip4("10.42.0.10"));
    memcpy(dhcp->chaddr, mac, 6);
    dhcp->options[0] = 99; dhcp->options[1] = 130;
    dhcp->options[2] = 83; dhcp->options[3] = 99;
    cid[0] = 1; memcpy(cid + 1, mac, 6);
    off += w2_dhcp_put(opts + off, sizeof(opts) - off, 53, 1, &type);
    off += w2_dhcp_put(opts + off, sizeof(opts) - off, 54, 4, &sid);
    off += w2_dhcp_put(opts + off, sizeof(opts) - off, 1, 4, &mask);
    off += w2_dhcp_put(opts + off, sizeof(opts) - off, 3, 4, &router);
    off += w2_dhcp_put(opts + off, sizeof(opts) - off, 51, 4, &lease);
    off += w2_dhcp_put(opts + off, sizeof(opts) - off, 61, 7, cid);
    opts[off++] = 255;
    memcpy(dhcp->options + 4, opts, off);
    size_t dhcp_len = offsetof(struct w2_dhcp_packet, options) + 4 + off;
    size_t udp_len = sizeof(*udp) + dhcp_len;
    size_t total = sizeof(*ip) + udp_len;

    ip->version = 4; ip->ihl = 5; ip->tot_len = htons((uint16_t)total);
    ip->ttl = 64; ip->protocol = IPPROTO_UDP;
    ip->saddr = htonl(w2_ip4(WIFI1B_AP_IP));
    ip->daddr = htonl(w2_ip4("10.42.0.10"));
    ip->frag_off = htons(fragmented ? 0x2000 : 0);
    ip->check = 0;
    ip->check = htons(checksum_finish(w2_sum_bytes((uint8_t *)ip, sizeof(*ip), 0)));
    if (bad_ip) ip->check ^= 0xffffU;

    udp->source = htons(w2_dhcp_port());
    udp->dest = htons(w2_dhcp_client_port());
    udp->len = htons((uint16_t)udp_len);
    udp->check = 0;
    uint32_t sum = 0;
    sum = w2_sum_bytes((uint8_t *)&ip->saddr, 4, sum);
    sum = w2_sum_bytes((uint8_t *)&ip->daddr, 4, sum);
    sum += ip->protocol;
    sum += (uint32_t)udp_len;
    sum = w2_sum_bytes((uint8_t *)udp, udp_len, sum);
    udp->check = htons(checksum_finish(sum));
    if (zero_udp) udp->check = 0;
    if (bad_udp) udp->check ^= 0xffffU;
    return total;
}

static void test_raw_parser(void)
{
    uint8_t packet[1500], payload[1500];
    size_t payload_len = 0;
    uint32_t xid = 0x12345678;
    uint8_t mac[6] = {0x02, 0x00, 0x11, 0x22, 0x33, 0x44};
    size_t n = build_raw_reply(packet, sizeof(packet), xid, mac, false, false, false, false);

    assert(w2_dhcp_raw_parse(packet, n, xid, mac, payload, sizeof(payload), &payload_len));
    assert(payload_len >= offsetof(struct w2_dhcp_packet, options) + 4);
    assert(w2_dhcp_msgtype((struct w2_dhcp_packet *)payload, payload_len) == 2);

    n = build_raw_reply(packet, sizeof(packet), xid, mac, true, false, false, false);
    assert(!w2_dhcp_raw_parse(packet, n, xid, mac, payload, sizeof(payload), &payload_len));
    n = build_raw_reply(packet, sizeof(packet), xid, mac, false, true, false, false);
    assert(!w2_dhcp_raw_parse(packet, n, xid, mac, payload, sizeof(payload), &payload_len));
    n = build_raw_reply(packet, sizeof(packet), xid, mac, false, false, false, true);
    assert(w2_dhcp_raw_parse(packet, n, xid, mac, payload, sizeof(payload), &payload_len));
    n = build_raw_reply(packet, sizeof(packet), xid, mac, false, false, true, false);
    assert(!w2_dhcp_raw_parse(packet, n, xid, mac, payload, sizeof(payload), &payload_len));
    n = build_raw_reply(packet, sizeof(packet), xid, mac, false, false, false, false);
    assert(!w2_dhcp_raw_parse(packet, n - 1, xid, mac, payload, sizeof(payload), &payload_len));
    assert(!w2_dhcp_raw_parse(packet, n, xid + 1, mac, payload, sizeof(payload), &payload_len));
    mac[0] ^= 1;
    assert(!w2_dhcp_raw_parse(packet, n, xid, mac, payload, sizeof(payload), &payload_len));
    mac[0] ^= 1;

    struct w2_dhcp_packet *dhcp = (struct w2_dhcp_packet *)(packet + sizeof(struct iphdr) + sizeof(struct udphdr));
    uint8_t *cid = w2_dhcp_opt(dhcp->options + 4,
                               n - sizeof(struct iphdr) - sizeof(struct udphdr) -
                               offsetof(struct w2_dhcp_packet, options) - 4, 61);
    assert(cid != NULL);
    cid[1] ^= 1;
    struct udphdr *mut_udp = (struct udphdr *)(packet + sizeof(struct iphdr));
    mut_udp->check = 0;
    assert(!w2_dhcp_raw_parse(packet, n, xid, mac, payload, sizeof(payload), &payload_len));

    /* Wrong UDP destination port. */
    n = build_raw_reply(packet, sizeof(packet), xid, mac, false, false, false, false);
    struct udphdr *udp = (struct udphdr *)(packet + sizeof(struct iphdr));
    udp->dest = htons(9999);
    assert(!w2_dhcp_raw_parse(packet, n, xid, mac, payload, sizeof(payload), &payload_len));
}

static void test_accept_lease_result(void)
{
    int64_t before = now_ms();
    char buf[2048];
    g_apply_gen = 42;
    g_work_kind = WORK_DHCP;
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    snprintf(buf, sizeof(buf),
             "42 1\nRenewed\n"
             "client_lease_updated=1\nclient_lease_valid=1\n"
             "client_lease_ip=10.42.0.10\nclient_lease_server=10.42.0.1\n"
             "client_lease_gateway=10.42.0.1\nclient_lease_prefix=24\n"
             "client_lease_lease_s=3600\nclient_lease_t1_s=1800\n"
             "client_lease_t2_s=3150\nclient_lease_route_ok=1\n"
             "client_route_metric=101\nclient_route_proto=66\n"
             "client_lease_iface=wlan0\n");
    assert(accept_apply_result(buf));
    assert(g_dhcp_lease.valid && g_dhcp_lease.route_ok);
    assert(g_dhcp_lease.ip == w2_ip4("10.42.0.10"));
    assert(g_dhcp_lease.gateway == w2_ip4("10.42.0.1"));
    assert(g_client_t1_at_ms >= before + 1800000);
    assert(g_client_t2_at_ms >= before + 3150000);
    assert(g_client_expiry_at_ms >= before + 3600000);

    g_apply_gen = 43;
    snprintf(buf, sizeof(buf),
             "43 1\nbad lease\nclient_lease_updated=1\nclient_lease_valid=1\n");
    assert(accept_apply_result(buf));
    assert(!g_dhcp_lease.valid && !g_dhcp_lease.route_ok);
    assert(g_client_expiry_at_ms == 0);

    /* A timeout result (updated=0) leaves the current clock untouched. */
    g_apply_gen = 44;
    g_dhcp_lease.valid = true;
    g_client_t1_at_ms = 1234;
    g_client_t2_at_ms = 5678;
    g_client_expiry_at_ms = 9999;
    snprintf(buf, sizeof(buf),
             "44 0\ntimeout\nclient_lease_updated=0\n");
    assert(accept_apply_result(buf));
    assert(g_dhcp_lease.valid);
    assert(g_client_t1_at_ms == 1234 && g_client_t2_at_ms == 5678 &&
           g_client_expiry_at_ms == 9999);
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_client_t1_at_ms = g_client_t2_at_ms = g_client_expiry_at_ms = 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--route-install") && argc > 3) {
        config_defaults();
        bool ok = w2_client_route_install(w2_ip4(argv[2]), argv[3]);
        printf("{\"ok\":%s,\"metric\":%u,\"proto\":%u,\"error\":\"%s\"}\n",
               ok ? "true" : "false", g_dhcp_lease.route_metric,
               g_dhcp_lease.route_proto, g_dhcp_error);
        return ok ? 0 : 1;
    }
    if (argc > 1 && !strcmp(argv[1], "--route-cleanup")) {
        config_defaults();
        bool ok = w2_owned_route_cleanup();
        printf("{\"ok\":%s}\n", ok ? "true" : "false");
        return ok ? 0 : 1;
    }
    if (argc > 1 && !strcmp(argv[1], "--route-metric")) {
        unsigned int metric = 0;
        test_command = NULL;
        bool ok = w2_route_select_metric(&metric);
        printf("{\"ok\":%s,\"metric\":%u}\n", ok ? "true" : "false", metric);
        return ok ? 0 : 1;
    }
    if (argc > 5 && !strcmp(argv[1], "--renew-client")) {
        config_defaults();
        test_command = NULL;
        test_mac_override = argv[5];
        memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
        g_dhcp_lease.valid = true;
        g_dhcp_lease.updated = false;
        g_dhcp_lease.ip = w2_ip4(argv[3]);
        g_dhcp_lease.server = w2_ip4(argv[4]);
        g_dhcp_lease.prefix = 24;
        g_dhcp_lease.lease_s = 3600;
        g_dhcp_lease.t1_s = 1800;
        g_dhcp_lease.t2_s = 3150;
        str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), "wlan0");
        w2_dhcp_step_t step = !strcmp(argv[2], "rebind")
            ? W2_DHCP_REBIND : W2_DHCP_RENEW;
        bool ok = w2_dhcp_client_renew(step);
        printf("{\"ok\":%s,\"valid\":%s,\"error\":\"%s\"}\n",
               ok ? "true" : "false",
               g_dhcp_lease.valid ? "true" : "false", g_dhcp_error);
        return ok ? 0 : 1;
    }
    if (argc > 2 && !strcmp(argv[1], "--raw-client")) {
        config_defaults();
        test_command = NULL;
        test_mac_override = argv[2];
        char ip[64] = "";
        bool ok = w2_dhcp_client("wlan0", ip, sizeof(ip));
        printf("{\"ok\":%s,\"ip\":\"%s\",\"route_ok\":%s,\"metric\":%u,"
               "\"proto\":%u,\"error\":\"%s\"}\n",
               ok ? "true" : "false", ip,
               g_dhcp_lease.route_ok ? "true" : "false",
               g_dhcp_lease.route_metric, g_dhcp_lease.route_proto,
               g_dhcp_error);
        return ok ? 0 : 1;
    }
    if (argc > 3 && !strcmp(argv[1], "--raw-recv")) {
        uint32_t xid = (uint32_t)strtoul(argv[2], NULL, 0);
        uint8_t mac[6];
        uint8_t payload[1500];
        size_t payload_len = 0;
        if (!parse_mac6(argv[3], mac)) return 2;
        int index = 0;
        int fd = w2_dhcp_raw_open("wlan0", &index);
        if (fd < 0) {
            printf("{\"ok\":false,\"error\":\"raw open\"}\n");
            return 1;
        }
        int64_t deadline = now_ms() + 5000;
        bool got = false;
        while (!got && now_ms() < deadline) {
            struct pollfd pfd = {.fd = fd, .events = POLLIN};
            if (poll(&pfd, 1, 100) > 0 &&
                (pfd.revents & POLLIN))
                got = w2_dhcp_raw_receive(fd, index, xid, mac, payload,
                                          sizeof(payload), &payload_len);
        }
        close(fd);
        printf("{\"ok\":%s,\"len\":%zu}\n", got ? "true" : "false", payload_len);
        return got ? 0 : 1;
    }

    mkdir(APP_DIR, 0700);
    test_masks_and_options();
    test_lease_action();
    test_route_policy_units();
    test_route_install_cleanup();
    test_raw_parser();
    test_accept_lease_result();
    puts("PASS: DHCP client identity, timers, raw parsing and owned route policy");
    return 0;
}
