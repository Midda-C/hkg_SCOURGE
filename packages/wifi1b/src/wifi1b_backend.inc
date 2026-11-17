/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 * Wifi1b backend: static wpa_supplicant + our own DHCP + dropbear.
 *
 * This file is included by wifi1b_daemon.c after the vendor/Bluetooth helpers.
 * It deliberately does not use ConnMan.  The OEM supplicant manager is only
 * evicted while a Wifi1b mode is active.
 */

#ifndef SO_BINDTODEVICE
#define SO_BINDTODEVICE 25
#endif

#define WIFI1B_AP_IP        "10.42.0.1"
#define WIFI1B_AP_CIDR      "10.42.0.1/24"
#define WIFI1B_AP_NETMASK   "255.255.255.0"
#define WIFI1B_AP_BROADCAST "10.42.0.255"
#define WIFI1B_DHCP_POOL_FIRST 10
#define WIFI1B_DHCP_POOL_LAST  200
#define WIFI1B_DHCP_LEASE_FILE APP_DIR "/dhcp.leases"
#define WIFI1B_DHCP_PID        APP_DIR "/dhcpd.pid"
#define WIFI1B_DHCP_LOG        APP_DIR "/dhcpd.log"
#define WIFI1B_INTENT          "/run/wifi1b/active"
#define WIFI1B_AP_FW_CHAIN     "WIFI1B_APP_AP"
#define WIFI1B_AP_FW6_CHAIN    "WIFI1B_APP_AP6"
#define WIFI1B_CLIENT_ROUTE_LEDGER APP_DIR "/client.route"
#define WIFI1B_CLIENT_ROUTE_PROTO  66U
#define WIFI1B_CLIENT_DEFAULT_METRIC 1000U
#define WIFI1B_USB0_IFACE          "usb0"
#define WIFI1B_USB0_IP             "10.55.0.2"
#define WIFI1B_USB0_NETMASK        "255.255.255.0"

/* Definitions that appear later in the original daemon. */
static bool client_associated(void);
static const char *mode_name(void);
static void write_status(void);
static bool fw_open(int port);
static void fw_close(int port);
static bool fw_port_add(const char *proto, int port);
static void fw_chain_close(const char *tool, const char *chain);
static void fw_tool(char *out, size_t n);
static void fw6_tool(char *out, size_t n);
static bool fw_rule_add(const char *tool, const char *chain, const char *rule);
static bool carplay_fw_open(void);
static void carplay_fw_close(void);
static int choose_ssh_port(int desired);
static bool ssh_running(void);
static void ssh_stop(void);
static bool ensure_car_user(void);
static bool ensure_line(const char *path, const char *prefix, const char *line);
static bool fw_chain_ensure(const char *tool, const char *chain);
static bool ap_stop(void);
static void usb0_rule_write(int port);
static bool g_client_modem_ipv4;
static int g_client_legacy_routes_removed;
static int g_usb0_fw_port;

/* ------------------------------------------------------------------ */
/* small process / command helpers                                     */
/* ------------------------------------------------------------------ */

static bool w2_proc_alive(int pid)
{
    char path[64], state[1024];
    if (pid <= 1) return false;
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    if (read_file(path, state, sizeof(state)) <= 0) return false;
    char *end = strrchr(state, ')');
    return end && end[1] == ' ' && end[2] != 'Z' && end[2] != 'X';
}

static void close_child_fds(int keep)
{
    DIR *d = opendir("/proc/self/fd");
    if (!d) {
        for (int fd = 3; fd < 1024; fd++) if (fd != keep) close(fd);
        return;
    }
    struct dirent *de;
    int own = dirfd(d);
    while ((de = readdir(d))) {
        int fd = atoi(de->d_name);
        if (fd >= 3 && fd != own && fd != keep) close(fd);
    }
    closedir(d);
}

static int w2_pid_by_comm(const char *name)
{
    DIR *d = opendir("/proc");
    if (!d) return -1;
    struct dirent *de;
    int found = -1;
    while ((de = readdir(d))) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        char path[64], comm[128];
        snprintf(path, sizeof(path), "/proc/%s/comm", de->d_name);
        if (read_file(path, comm, sizeof(comm)) <= 0) continue;
        char *nl = strchr(comm, '\n');
        if (nl) *nl = 0;
        if (!strcmp(comm, name)) { found = atoi(de->d_name); break; }
    }
    closedir(d);
    return found;
}

static bool w2_ip_command(const char *fmt, ...)
{
    char args[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(args, sizeof(args), fmt, ap);
    va_end(ap);
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "ip %s 2>/dev/null", args);
    if (sh("%s", cmd) == 0) return true;
    return false;
}

static void w2_iface_flush(void)
{
    /* Family-specific on purpose: a bare `ip addr flush` removes the IPv6
     * link-local address CarPlay needs. Only IPv4 changes here. */
    if (sh("ip -4 addr flush dev wlan0 2>/dev/null") != 0) {
        sh("ifconfig wlan0 0.0.0.0 2>/dev/null");
    }
}

static bool w2_iface_set_static(void)
{
    if (w2_ip_command("addr add " WIFI1B_AP_CIDR " dev wlan0")) {
        w2_ip_command("link set wlan0 up");
        return true;
    }
    return sh("ifconfig wlan0 " WIFI1B_AP_IP " netmask " WIFI1B_AP_NETMASK " up 2>/dev/null") == 0;
}

static void w2_iface_down_clean(void)
{
    w2_iface_flush();
    w2_ip_command("link set wlan0 down");
    sh("ifconfig wlan0 down 2>/dev/null");
}

static bool w2_read_wlan_ip(char *out, size_t n)
{
    if (out && n) out[0] = 0;
    char buf[4096];
    if (sh_out(buf, sizeof(buf), "ifconfig wlan0 2>/dev/null") == 0) {
        const char *p = strstr(buf, "inet addr:");
        if (p) p += 10;
        else {
            p = strstr(buf, "inet ");
            if (p) p += 5;
        }
        if (p) {
            size_t i = 0;
            while (*p && *p != ' ' && *p != '\n' && *p != '\t' && i + 1 < n)
                out[i++] = *p++;
            out[i] = 0;
            if (i && strcmp(out, "0.0.0.0") && strcmp(out, "127.0.0.1")) return true;
        }
    }
    if (sh_out(buf, sizeof(buf), "ip -o -4 addr show dev wlan0 2>/dev/null") == 0) {
        char *p = strstr(buf, " inet ");
        if (p) {
            p += 6;
            size_t i = 0;
            while (*p && *p != '/' && *p != ' ' && *p != '\n' && i + 1 < n)
                out[i++] = *p++;
            out[i] = 0;
            if (i && strcmp(out, "0.0.0.0")) return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* IPv6 link-local readback                                           */
/* ------------------------------------------------------------------ */

static bool w2_read_if_inet6(char *buf, size_t n)
{
#ifdef WIFI_TEST
    if (test_if_inet6_path) return read_file(test_if_inet6_path, buf, n) > 0;
#endif
    return read_file("/proc/net/if_inet6", buf, n) > 0;
}

static bool w2_ipv6_conf_read(const char *iface, const char *leaf,
                              char *out, size_t n)
{
    char path[256];
    const char *base = "/proc/sys/net/ipv6/conf";
#ifdef WIFI_TEST
    if (test_ipv6_conf_dir) base = test_ipv6_conf_dir;
#endif
    snprintf(path, sizeof(path), "%s/%s/%s", base, iface, leaf);
    return read_file(path, out, n) > 0;
}

/* Read-only diagnosis. Never writes a sysctl: turning IPv6 on must be an
 * explicit owner/system decision, not a side effect of starting CarPlay. */
static bool w2_ipv6_disabled(const char *iface, char *reason, size_t n)
{
    const char *levels[] = {iface, "all", "default"};
    for (size_t i = 0; i < sizeof(levels)/sizeof(levels[0]); i++) {
        char value[32];
        if (!w2_ipv6_conf_read(levels[i], "disable_ipv6", value, sizeof(value))) continue;
        if (!strcmp(trim(value), "1")) {
            snprintf(reason, n,
                     "IPv6 is disabled (net.ipv6.conf.%s.disable_ipv6=1); "
                     "enable IPv6 on wlan0, no global setting was changed",
                     levels[i]);
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* supplicant lifecycle                                                */
/* ------------------------------------------------------------------ */

static bool wpa_process_alive(void)
{
#ifdef WIFI_TEST
    if (test_owned_wpa >= 0) return test_owned_wpa != 0;
#endif
    DIR *d = opendir("/proc");
    if (!d) return false;
    struct dirent *de;
    bool found = false;
    while ((de = readdir(d))) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        int pid = atoi(de->d_name);
        char comm[64];
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/comm", pid);
        if (read_file(path, comm, sizeof(comm)) <= 0) continue;
        if (strcmp(trim(comm), "wpa_supplicant")) continue;
        if (pid_has_argument(pid, "-c" WPA_CONF) &&
            pid_has_argument(pid, "-O" WPA_CTRL_DIR)) {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

static int w2_owned_wpa_pid(void)
{
    DIR *d = opendir("/proc");
    if (!d) return -1;
    struct dirent *de;
    int found = -1;
    while ((de = readdir(d))) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        int pid = atoi(de->d_name);
        char path[80], comm[64];
        snprintf(path, sizeof(path), "/proc/%d/comm", pid);
        if (read_file(path, comm, sizeof(comm)) > 0 &&
            !strcmp(trim(comm), "wpa_supplicant") &&
            pid_has_argument(pid, "-c" WPA_CONF) &&
            pid_has_argument(pid, "-O" WPA_CTRL_DIR)) {
            found = pid;
            break;
        }
    }
    closedir(d);
    return found;
}

static void stop_wpa_supplicant(void)
{
    int pid = -1;
    char buf[64];
    if (read_file(WPA_PIDFILE, buf, sizeof(buf)) > 0) {
        int p = atoi(trim(buf));
        if (p > 1 && w2_proc_alive(p) &&
            pid_has_argument(p, "-c" WPA_CONF)) pid = p;
    }
    if (pid <= 0) pid = w2_owned_wpa_pid();
    if (pid > 0 && pid_has_argument(pid, "-c" WPA_CONF)) {
        kill(pid, SIGTERM);
        for (int i = 0; i < 20 && w2_proc_alive(pid); i++) usleep(100000);
        if (w2_proc_alive(pid)) kill(pid, SIGKILL);
        for (int i = 0; i < 10 && w2_proc_alive(pid); i++) usleep(100000);
    }
    if (pid > 0) waitpid(pid, NULL, WNOHANG);
    sh("rm -f " WPA_CTRL_DIR "/wlan0 2>/dev/null");
    sh("rmdir " WPA_CTRL_DIR " 2>/dev/null");
    unlink(WPA_PIDFILE);
}

static bool wpa_ping(void)
{
    return sh("wpa_cli -p " WPA_CTRL_DIR " -i wlan0 ping 2>/dev/null | grep -q PONG") == 0;
}

static bool stop_oem_wifi(void)
{
    /* The public owner recipe's field-tested eviction, in its exact order:
     * first clear any tether ConnMan still holds (the source of the
     * car's "Error enabling wifi tethering: Permission denied" residue),
     * then evict the managers.  Keep our own supplicant safe by matching
     * only the OEM -u/-e forms. */
    sh("connmanctl tether wifi off 2>/dev/null");
    sh("touch /tmp/no_unece 2>/dev/null");
    sh("rfkill unblock all 2>/dev/null");
    sh("systemctl stop supplicantmanager.path supplicantmanager.service 2>/dev/null");
    sh("pkill -f '[s]upplicantmanager' 2>/dev/null");
    sh("pkill -f '[w]pa_supplicant -u' 2>/dev/null");
    sh("pkill -f '[w]pa_supplicant .* -e /rw_data/wifi' 2>/dev/null");
    return true;
}

static bool load_driver(void)
{
    if (iface_present()) return true;
    sh("modprobe cfg80211 2>/dev/null");
    sh("modprobe bcmdhd firmware_path=/lib/firmware/bcmdhd/rtecdc.bin "
       "nvram_path=/lib/firmware/bcmdhd/nvram.txt iface_name=wlan0 2>/dev/null");
    for (int i = 0; i < 10; i++) {
        if (iface_present()) return true;
        usleep(300000);
    }
    sh("insmod " BCMDHD_KO " firmware_path=/lib/firmware/bcmdhd/rtecdc.bin "
       "nvram_path=/lib/firmware/bcmdhd/nvram.txt iface_name=wlan0 2>/dev/null");
    for (int i = 0; i < 10; i++) {
        if (iface_present()) return true;
        usleep(300000);
    }
    return iface_present();
}

static bool ensure_wifi_ready(void)
{
    if (!iface_present() && !load_driver()) {
        set_msg("bcmdhd/wlan0 not available");
        return false;
    }
    stop_oem_wifi();
    iface_up();
    return iface_present();
}

static bool write_supplicant_config(const char *data)
{
    mkdir(APP_DIR, 0755);
    return write_file_atomic(WPA_CONF, data, 0600) == 0;
}

static void conf_quote(char *dst, size_t n, const char *src)
{
    size_t j = 0;
    if (!n) return;
    for (const char *p = src ? src : ""; *p && j + 3 < n; p++) {
        if (*p == '\\' || *p == '"') dst[j++] = '\\';
        dst[j++] = *p;
    }
    dst[j] = 0;
}

static bool start_wpa_supplicant(void)
{
    if (g_stop) return false;
    stop_wpa_supplicant();
    cap_log_file(WPA_LOG, 1024 * 1024);
    sh("mkdir -p " WPA_CTRL_DIR " 2>/dev/null");
    sh("chmod 755 " WPA_CTRL_DIR " 2>/dev/null");
    pid_t pid = fork();
    if (pid == 0) {
        if (setsid() < 0) _exit(126);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGHUP, SIG_IGN);
        int logfd = open(WPA_LOG, O_WRONLY | O_CREAT | O_APPEND, 0600);
        int nul = open("/dev/null", O_RDONLY);
        if (logfd < 0 || nul < 0) _exit(126);
        fchmod(logfd, 0600);
        dup2(nul, STDIN_FILENO);
        dup2(logfd, STDOUT_FILENO);
        dup2(logfd, STDERR_FILENO);
        close(logfd); close(nul);
        execl("/usr/sbin/wpa_supplicant", "wpa_supplicant",
              "-Dnl80211,wext", "-iwlan0", "-c" WPA_CONF,
              "-O" WPA_CTRL_DIR, (char *)NULL);
        _exit(127);
    }
    if (pid <= 0) return false;
    char pidbuf[64];
    snprintf(pidbuf, sizeof(pidbuf), "%d\n", (int)pid);
    write_file_atomic(WPA_PIDFILE, pidbuf, 0644);
    log_msg("wifi1b: started wpa_supplicant pid=%d ctrl=%s", (int)pid, WPA_CTRL_DIR);
    for (int i = 0; i < 40; i++) {
        if (wpa_ping()) return true;
        if (!w2_proc_alive(pid)) break;
        usleep(250000);
    }
    char tail[512];
    log_tail(WPA_LOG, tail, sizeof(tail));
    if (tail[0]) log_msg("wifi1b: wpa_supplicant startup tail: %s", tail);
    return false;
}

static bool write_station_conf(const char *ssid, const char *psk, bool sae)
{
    char s[512], p[512], buf[8192];
    size_t used = snprintf(buf, sizeof(buf), "ctrl_interface=" WPA_CTRL_DIR "\n"
                           "ctrl_interface_group=0\nupdate_config=0\nap_scan=1\n");
    for (int i = 0; i < 4; i++) {
        const char *name = i ? g_cfg.extra_ssid[i-1] : ssid;
        const char *key = i ? g_cfg.extra_psk[i-1] : psk;
        if (!name[0]) continue;
        size_t len = strlen(key);
        bool hex = len == 64 && strspn(key, "0123456789abcdefABCDEF") == 64;
        if (strlen(name) > 32 || (len && (len < 8 || len > 64 || (len == 64 && !hex)))) return false;
        conf_quote(s, sizeof(s), name);
        conf_quote(p, sizeof(p), key);
        char security[1024];
        if (!len) snprintf(security, sizeof(security), "    key_mgmt=NONE\n");
        else snprintf(security, sizeof(security), "    psk=%s%s%s\n    key_mgmt=%s\n"
                      "    proto=RSN\n    pairwise=CCMP\n    group=CCMP\n%s",
                      hex ? "" : "\"", p, hex ? "" : "\"", sae && !hex ? "SAE" : "WPA-PSK",
                      sae && !hex ? "    ieee80211w=2\n" : "");
        int n = snprintf(buf + used, sizeof(buf) - used,
                         "network={\n    ssid=\"%s\"\n%s    scan_ssid=1\n    priority=%d\n}\n",
                         s, security, 100-i);
        if (n < 0 || (size_t)n >= sizeof(buf)-used) return false;
        used += n;
    }
    return write_supplicant_config(buf);
}

static bool parse_mac6(const char *s, unsigned char out[6])
{
    int idx = 0;
    const char *p = s;
    while (*p && idx < 6) {
        while (*p == ':' || *p == '-') p++;
        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) return false;
        char h[3] = {p[0], p[1], 0};
        out[idx++] = (unsigned char)strtol(h, NULL, 16);
        p += 2;
    }
    while (*p == ':' || *p == '-') p++;
    return idx == 6 && *p == 0;
}

static void format_mac6(const unsigned char in[6], char *out, size_t n)
{
    snprintf(out, n, "%02x:%02x:%02x:%02x:%02x:%02x",
             in[0], in[1], in[2], in[3], in[4], in[5]);
}

static void fetch_bt_address_if_needed(void)
{
    if (g_cfg.cp_btaddr[0]) return;
    char buf[256];
    if (read_file("/sys/class/bluetooth/hci0/address", buf, sizeof(buf)) > 0) {
        char *nl = strchr(buf, '\n');
        if (nl) *nl = 0;
        str_copy(g_cfg.cp_btaddr, sizeof(g_cfg.cp_btaddr), trim(buf));
    } else if (json_get_string_value("/app/data/vbt/VBTConfig.json",
                                     "DeviceAddress", buf, sizeof(buf)) ||
               json_get_string_value("/rw_data/app_data/vbt/VBTConfig.json",
                                     "DeviceAddress", buf, sizeof(buf))) {
        str_copy(g_cfg.cp_btaddr, sizeof(g_cfg.cp_btaddr), buf);
    } else {
        char out[4096];
        if (sh_out(out, sizeof(out), "hcitool dev 2>/dev/null") == 0) {
            char *p = strstr(out, "hci0");
            if (p) {
                p += 4;
                while (*p && isspace((unsigned char)*p)) p++;
                char *e = p;
                while (*e && !isspace((unsigned char)*e)) e++;
                if (e > p) {
                    *e = 0;
                    str_copy(g_cfg.cp_btaddr, sizeof(g_cfg.cp_btaddr), p);
                }
            }
        }
    }
}

static void prepare_carplay_identity(void)
{
    unsigned char mac[6];
    char formatted[32];
    fetch_bt_address_if_needed();
    if (parse_mac6(g_cfg.cp_btaddr, mac)) {
        format_mac6(mac, formatted, sizeof(formatted));
        str_copy(g_cfg.cp_id, sizeof(g_cfg.cp_id), formatted);
        return;
    }
    if (parse_mac6(g_cfg.cp_id, mac)) {
        format_mac6(mac, formatted, sizeof(formatted));
        str_copy(g_cfg.cp_id, sizeof(g_cfg.cp_id), formatted);
        return;
    }
    if (!g_cfg.cp_id[0]) {
        unsigned char rnd[6];
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) { (void)read(fd, rnd, sizeof(rnd)); close(fd); }
        else memset(rnd, 0x42, sizeof(rnd));
        rnd[0] = (unsigned char)((rnd[0] & 0xfc) | 0x02);
        format_mac6(rnd, g_cfg.cp_id, sizeof(g_cfg.cp_id));
        config_save();
    }
    log_msg("CarPlay identity: bt=%s id=%s", g_cfg.cp_btaddr, g_cfg.cp_id);
}

static bool write_ap_conf(void)
{
    char ssid[512], psk[512], name[256];
    conf_quote(ssid, sizeof(ssid), g_cfg.cp_ssid);
    conf_quote(psk, sizeof(psk), g_cfg.cp_psk);
    conf_quote(name, sizeof(name), g_cfg.cp_name);
    int freq = channel_to_freq();
    char buf[8192];
    snprintf(buf, sizeof(buf),
             "# Wifi1b static AP (vendor wpa_supplicant AP mode)\n"
             "ctrl_interface=" WPA_CTRL_DIR "\n"
             "ctrl_interface_group=0\n"
             "update_config=0\n"
             "ap_scan=2\n"
             "beacon_int=100\n"
             "max_num_sta=%d\n"
             "app_info=1\n"
             "carplay_name=\"%s\"\n"
             "carplay_id=%s\n"
             "manufacturer=\"Hyundai\"\n"
             "model_name=\"Ioniq 5\"\n"
             "model_number=\"260128\"\n"
             "device_name=\"%s\"\n"
             "network={\n"
             "    ssid=\"%s\"\n"
             "    mode=2\n"
             "    frequency=%d\n"
             "    key_mgmt=WPA-PSK\n"
             "    proto=RSN\n"
             "    pairwise=CCMP\n"
             "    group=CCMP\n"
             "    psk=\"%s\"\n"
             "    auth_alg=OPEN\n"
             "    ignore_broadcast_ssid=%d\n"
             "}\n",
             g_cfg.cp_max_sta, name, g_cfg.cp_id, name,
             ssid, freq, psk, g_cfg.cp_hidden ? 1 : 0);
    if (write_supplicant_config(buf)) return true;
    set_msg("cannot write AP supplicant config");
    return false;
}

static bool write_scan_conf(void)
{
    static const char conf[] =
        "ctrl_interface=" WPA_CTRL_DIR "\n"
        "ctrl_interface_group=0\n"
        "update_config=0\n"
        "ap_scan=1\n";
    return write_supplicant_config(conf);
}

/* ------------------------------------------------------------------ */
/* AP/DHCP                                                             */
/* ------------------------------------------------------------------ */

struct w2_dhcp_packet {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint8_t options[312];
} __attribute__((packed));

typedef struct {
    bool used;
    uint8_t mac[6];
    uint32_t ip;
    int64_t expires_ms;
} w2_lease_t;

static w2_lease_t g_w2_leases[64];

static int w2_dhcp_port(void)
{
    const char *e = getenv("WIFI1B_DHCP_TEST_PORT");
    int p = e ? atoi(e) : 67;
    return p > 0 && p < 65536 ? p : 67;
}

static int w2_dhcp_client_port(void)
{
    const char *e = getenv("WIFI1B_DHCP_TEST_CLIENT_PORT");
    int p = e ? atoi(e) : 68;
    return p > 0 && p < 65536 ? p : 68;
}

static uint32_t w2_ip4(const char *s)
{
    struct in_addr a;
    if (inet_pton(AF_INET, s, &a) != 1) return 0;
    return ntohl(a.s_addr);
}

static void w2_ip4_str(uint32_t ip, char *out, size_t n)
{
    struct in_addr a;
    a.s_addr = htonl(ip);
    inet_ntop(AF_INET, &a, out, n);
}

static int w2_dhcpd_pid(void)
{
    char buf[64];
    if (read_file(WIFI1B_DHCP_PID, buf, sizeof(buf)) > 0) {
        int p = atoi(trim(buf));
        if (p > 1 && w2_proc_alive(p)) {
            char path[64], comm[64];
            snprintf(path, sizeof(path), "/proc/%d/comm", p);
            if (read_file(path, comm, sizeof(comm)) > 0 &&
                !strcmp(trim(comm), "wifi1b_dhcpd")) return p;
        }
    }
    return w2_pid_by_comm("wifi1b_dhcpd");
}

static bool dhcpd_running(void)
{
    return w2_dhcpd_pid() > 0;
}

static int dhcp_leases_count(void)
{
    FILE *f = fopen(WIFI1B_DHCP_LEASE_FILE, "r");
    if (!f) return 0;
    int n = 0;
    char line[256];
    while (fgets(line, sizeof(line), f))
        if (line[0] && line[0] != '#') n++;
    fclose(f);
    return n;
}

static void w2_lease_write(void)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", WIFI1B_DHCP_LEASE_FILE, (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    int64_t now = now_ms();
    fprintf(f, "# mac ip expires_unix\n");
    for (size_t i = 0; i < sizeof(g_w2_leases)/sizeof(g_w2_leases[0]); i++) {
        if (!g_w2_leases[i].used || g_w2_leases[i].expires_ms <= now) continue;
        char ip[32];
        w2_ip4_str(g_w2_leases[i].ip, ip, sizeof(ip));
        fprintf(f, "%02x:%02x:%02x:%02x:%02x:%02x %s %lld\n",
                g_w2_leases[i].mac[0], g_w2_leases[i].mac[1],
                g_w2_leases[i].mac[2], g_w2_leases[i].mac[3],
                g_w2_leases[i].mac[4], g_w2_leases[i].mac[5],
                ip, (long long)(time(NULL) + (g_w2_leases[i].expires_ms - now)/1000));
    }
    fclose(f);
    chmod(tmp, 0600);
    rename(tmp, WIFI1B_DHCP_LEASE_FILE);
}

/* Load unexpired leases after a daemon restart. Without this a restarted
 * server forgot every lease and could hand the same address to a second
 * client. Malformed, expired, out-of-pool and duplicate entries are ignored;
 * the file is bounded by the in-memory lease table. */
static void w2_lease_load(void)
{
    char buf[8192];
    if (read_file(WIFI1B_DHCP_LEASE_FILE, buf, sizeof(buf)) <= 0) return;
    int64_t now = now_ms();
    time_t wall = time(NULL);
    uint32_t base = w2_ip4(WIFI1B_AP_IP) & 0xffffff00U;
    uint32_t first = base + WIFI1B_DHCP_POOL_FIRST;
    uint32_t last = base + WIFI1B_DHCP_POOL_LAST;
    char *save = NULL;

    for (char *line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *p = trim(line);
        char mac_text[32], ip_text[32];
        long long expiry = 0;
        uint8_t mac[6];
        struct in_addr address;
        uint32_t ip;

        if (!*p || *p == '#') continue;
        if (sscanf(p, "%31s %31s %lld", mac_text, ip_text, &expiry) != 3) continue;
        if (expiry <= (long long)wall) continue;
        if (!parse_mac6(mac_text, mac)) continue;
        if (inet_pton(AF_INET, ip_text, &address) != 1) continue;
        ip = ntohl(address.s_addr);
        if (ip < first || ip > last) continue;
        /* A corrupt far-future timestamp must not pin an address forever. */
        if (expiry > (long long)wall + 86400) expiry = (long long)wall + 86400;
        bool duplicate = false;
        size_t free_slot = sizeof(g_w2_leases)/sizeof(g_w2_leases[0]);
        for (size_t i = 0; i < sizeof(g_w2_leases)/sizeof(g_w2_leases[0]); i++) {
            if (!g_w2_leases[i].used) {
                if (free_slot == sizeof(g_w2_leases)/sizeof(g_w2_leases[0]))
                    free_slot = i;
                continue;
            }
            if (!memcmp(g_w2_leases[i].mac, mac, 6) || g_w2_leases[i].ip == ip)
                duplicate = true;
        }
        if (duplicate || free_slot >= sizeof(g_w2_leases)/sizeof(g_w2_leases[0]))
            continue;
        g_w2_leases[free_slot].used = true;
        memcpy(g_w2_leases[free_slot].mac, mac, 6);
        g_w2_leases[free_slot].ip = ip;
        g_w2_leases[free_slot].expires_ms = now + (int64_t)(expiry - (long long)wall) * 1000;
    }
    log_msg("dhcpd: loaded persisted leases from %s", WIFI1B_DHCP_LEASE_FILE);
}

static bool w2_lease_get(const uint8_t mac[6], int64_t now)
{
    for (size_t i = 0; i < sizeof(g_w2_leases)/sizeof(g_w2_leases[0]); i++) {
        if (g_w2_leases[i].used && !memcmp(g_w2_leases[i].mac, mac, 6) &&
            g_w2_leases[i].expires_ms > now) return true;
    }
    return false;
}

static uint32_t w2_lease_for_mac(const uint8_t mac[6], int64_t now)
{
    for (size_t i = 0; i < sizeof(g_w2_leases)/sizeof(g_w2_leases[0]); i++) {
        if (g_w2_leases[i].used && !memcmp(g_w2_leases[i].mac, mac, 6)) {
            /* A renewal must refresh the lifetime even when the old lease
             * has not yet expired. */
            g_w2_leases[i].expires_ms = now + 3600000;
            return g_w2_leases[i].ip;
        }
    }
    return 0;
}

static bool w2_ip_in_use(uint32_t ip, int64_t now)
{
    for (size_t i = 0; i < sizeof(g_w2_leases)/sizeof(g_w2_leases[0]); i++)
        if (g_w2_leases[i].used && g_w2_leases[i].ip == ip &&
            g_w2_leases[i].expires_ms > now) return true;
    return false;
}

static uint32_t w2_lease_alloc(const uint8_t mac[6], uint32_t requested, int64_t now)
{
    uint32_t base = w2_ip4(WIFI1B_AP_IP) & 0xffffff00U;
    uint32_t first = base + WIFI1B_DHCP_POOL_FIRST;
    uint32_t last = base + WIFI1B_DHCP_POOL_LAST;
    uint32_t existing = w2_lease_for_mac(mac, now);
    if (existing) {
        w2_lease_write();
        return existing;
    }
    if (requested >= first && requested <= last && !w2_ip_in_use(requested, now))
        goto assign;
    for (uint32_t ip = first; ip <= last; ip++)
        if (!w2_ip_in_use(ip, now)) { requested = ip; goto assign; }
    return 0;
assign:
    for (size_t i = 0; i < sizeof(g_w2_leases)/sizeof(g_w2_leases[0]); i++) {
        if (!g_w2_leases[i].used || g_w2_leases[i].expires_ms <= now) {
            g_w2_leases[i].used = true;
            memcpy(g_w2_leases[i].mac, mac, 6);
            g_w2_leases[i].ip = requested;
            g_w2_leases[i].expires_ms = now + 3600000;
            w2_lease_write();
            return requested;
        }
    }
    return 0;
}

static void w2_lease_release(const uint8_t mac[6])
{
    for (size_t i = 0; i < sizeof(g_w2_leases)/sizeof(g_w2_leases[0]); i++) {
        if (g_w2_leases[i].used && !memcmp(g_w2_leases[i].mac, mac, 6)) {
            g_w2_leases[i].used = false;
            w2_lease_write();
            return;
        }
    }
}

static uint8_t *w2_dhcp_opt_n(uint8_t *opts, size_t len, uint8_t code,
                              uint8_t *out_len)
{
    size_t i = 0;
    if (out_len) *out_len = 0;
    while (i + 1 < len) {
        uint8_t c = opts[i++];
        if (c == 0) continue;
        if (c == 255) return NULL;
        uint8_t l = opts[i++];
        if (i + l > len) return NULL;
        if (c == code) {
            if (out_len) *out_len = l;
            return opts + i;
        }
        i += l;
    }
    return NULL;
}

static uint8_t *w2_dhcp_opt(uint8_t *opts, size_t len, uint8_t code)
{
    return w2_dhcp_opt_n(opts, len, code, NULL);
}

static bool w2_dhcp_opt_u32(const struct w2_dhcp_packet *p, size_t n,
                            uint8_t code, uint32_t *out)
{
    size_t off = offsetof(struct w2_dhcp_packet, options) + 4;
    uint8_t len = 0;
    if (p == NULL || out == NULL || n <= off) return false;
    uint8_t *v = w2_dhcp_opt_n((uint8_t *)p->options + 4, n - off, code, &len);
    if (!v || len != 4) return false;
    uint32_t x;
    memcpy(&x, v, 4);
    *out = ntohl(x);
    return true;
}

static uint8_t w2_dhcp_msgtype(const struct w2_dhcp_packet *p, size_t n)
{
    size_t off = offsetof(struct w2_dhcp_packet, options) + 4;
    if (n <= off) return 0;
    uint8_t len = 0;
    uint8_t *v = w2_dhcp_opt_n((uint8_t *)p->options + 4, n - off, 53, &len);
    return v && len == 1 ? *v : 0;
}

static uint32_t w2_dhcp_requested(const struct w2_dhcp_packet *p, size_t n)
{
    uint32_t value = 0;
    if (w2_dhcp_opt_u32(p, n, 50, &value)) return value;
    return 0;
}

static bool w2_dhcp_server_id(const struct w2_dhcp_packet *p, size_t n, uint32_t *out)
{
    return w2_dhcp_opt_u32(p, n, 54, out);
}

static size_t w2_dhcp_put(uint8_t *dst, size_t cap, uint8_t code, uint8_t len, const void *data)
{
    if (cap < (size_t)len + 2) return 0;
    dst[0] = code;
    dst[1] = len;
    if (len) memcpy(dst + 2, data, len);
    return (size_t)len + 2;
}

static void w2_dhcp_send(int fd, const struct w2_dhcp_packet *req, size_t reqlen,
                         uint8_t type, uint32_t yiaddr, bool ack)
{
    (void)reqlen;
    struct w2_dhcp_packet rep;
    memset(&rep, 0, sizeof(rep));
    rep.op = 2;
    rep.htype = 1;
    rep.hlen = 6;
    rep.xid = req->xid;
    rep.flags = req->flags;
    rep.ciaddr = req->ciaddr;
    rep.yiaddr = htonl(yiaddr);
    rep.siaddr = htonl(w2_ip4(WIFI1B_AP_IP));
    memcpy(rep.chaddr, req->chaddr, 6);
    rep.options[0] = 99; rep.options[1] = 130; rep.options[2] = 83; rep.options[3] = 99;
    size_t off = 4;
    off += w2_dhcp_put(rep.options + off, sizeof(rep.options) - off, 53, 1, &type);
    uint32_t server = htonl(w2_ip4(WIFI1B_AP_IP));
    off += w2_dhcp_put(rep.options + off, sizeof(rep.options) - off, 54, 4, &server);
    uint32_t lease = htonl(ack ? 3600 : 3600);
    off += w2_dhcp_put(rep.options + off, sizeof(rep.options) - off, 51, 4, &lease);
    uint32_t renew = htonl(1800);
    off += w2_dhcp_put(rep.options + off, sizeof(rep.options) - off, 58, 4, &renew);
    uint32_t rebind = htonl(3150);
    off += w2_dhcp_put(rep.options + off, sizeof(rep.options) - off, 59, 4, &rebind);
    uint32_t mask = htonl(w2_ip4(WIFI1B_AP_NETMASK));
    off += w2_dhcp_put(rep.options + off, sizeof(rep.options) - off, 1, 4, &mask);
    uint32_t router = htonl(w2_ip4(WIFI1B_AP_IP));
    off += w2_dhcp_put(rep.options + off, sizeof(rep.options) - off, 3, 4, &router);
    off += w2_dhcp_put(rep.options + off, sizeof(rep.options) - off, 6, 4, &router);
    uint32_t bcast = htonl(w2_ip4(WIFI1B_AP_BROADCAST));
    off += w2_dhcp_put(rep.options + off, sizeof(rep.options) - off, 28, 4, &bcast);
    rep.options[off++] = 255;

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(w2_dhcp_client_port());
    if (getenv("WIFI1B_DHCP_TEST_LOOPBACK")) to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    else if (req->ciaddr) to.sin_addr.s_addr = req->ciaddr;
    else to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(fd, &rep, offsetof(struct w2_dhcp_packet, options) + off, 0,
           (struct sockaddr *)&to, sizeof(to));
}

static bool w2_dhcpd_run(void)
{
    prctl(PR_SET_NAME, "wifi1b_dhcpd", 0, 0, 0);
    w2_lease_load();
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, "wlan0", 6) != 0)
        log_msg("dhcpd: SO_BINDTODEVICE failed: %s", strerror(errno));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(w2_dhcp_port());
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        log_msg("dhcpd: bind udp/67 failed: %s", strerror(errno));
        close(fd);
        return false;
    }
    log_msg("dhcpd: serving %s pool .%d-.%d on wlan0",
            WIFI1B_AP_IP, WIFI1B_DHCP_POOL_FIRST, WIFI1B_DHCP_POOL_LAST);
    int64_t last_clean = now_ms();
    while (!g_stop) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, 1000);
        int64_t now = now_ms();
        if (now - last_clean > 60000) {
            last_clean = now;
            w2_lease_write();
        }
        if (pr <= 0) continue;
        uint8_t buf[1500];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n < (ssize_t)(offsetof(struct w2_dhcp_packet, options) + 4)) continue;
        struct w2_dhcp_packet *p = (struct w2_dhcp_packet *)buf;
        if (p->op != 1 || p->htype != 1 || p->hlen != 6) continue;
        uint8_t type = w2_dhcp_msgtype(p, (size_t)n);
        if (!type) continue;
        uint32_t requested = w2_dhcp_requested(p, (size_t)n);
        uint32_t sid = 0;
        bool has_sid = w2_dhcp_server_id(p, (size_t)n, &sid);
        char mac[32], ips[32];
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 p->chaddr[0], p->chaddr[1], p->chaddr[2],
                 p->chaddr[3], p->chaddr[4], p->chaddr[5]);
        if (type == 1) { /* DISCOVER */
            uint32_t ip = w2_lease_alloc(p->chaddr, requested, now);
            if (!ip) { log_msg("dhcpd: pool exhausted for %s", mac); continue; }
            w2_ip4_str(ip, ips, sizeof(ips));
            log_msg("dhcpd: DISCOVER %s -> OFFER %s", mac, ips);
            w2_dhcp_send(fd, p, (size_t)n, 2, ip, false);
        } else if (type == 3) { /* REQUEST */
            if (has_sid && sid != w2_ip4(WIFI1B_AP_IP)) continue;
            uint32_t ip = w2_lease_for_mac(p->chaddr, now);
            if (!ip && requested) ip = w2_lease_alloc(p->chaddr, requested, now);
            if (!ip) {
                log_msg("dhcpd: REQUEST %s but no lease; NAK", mac);
                w2_dhcp_send(fd, p, (size_t)n, 6, 0, false);
                continue;
            }
            w2_ip4_str(ip, ips, sizeof(ips));
            log_msg("dhcpd: REQUEST %s -> ACK %s", mac, ips);
            w2_dhcp_send(fd, p, (size_t)n, 5, ip, true);
            w2_lease_write();
        } else if (type == 7) { /* RELEASE */
            log_msg("dhcpd: RELEASE %s", mac);
            w2_lease_release(p->chaddr);
        } else if (type == 8) { /* INFORM */
            uint32_t ip = w2_ip4(WIFI1B_AP_IP);
            log_msg("dhcpd: INFORM %s -> ACK", mac);
            w2_dhcp_send(fd, p, (size_t)n, 5, ip, true);
        }
    }
    close(fd);
    return true;
}

static bool start_dhcpd(void)
{
#ifdef WIFI_TEST
    if (test_dhcpd_start >= 0) return test_dhcpd_start != 0;
#endif
    if (dhcpd_running()) return true;
    cap_log_file(WIFI1B_DHCP_LOG, 1024 * 1024);
    pid_t pid = fork();
    if (pid == 0) {
        if (setsid() < 0) _exit(126);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGHUP, SIG_IGN);
        close_child_fds(-1);
        int logfd = open(WIFI1B_DHCP_LOG, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (logfd >= 0) {
            fchmod(logfd, 0600);
            dup2(logfd, STDOUT_FILENO);
            dup2(logfd, STDERR_FILENO);
            close(logfd);
        }
        w2_dhcpd_run();
        _exit(0);
    }
    if (pid <= 0) {
        set_msg("dhcpd: fork failed");
        return false;
    }
    char pidbuf[64];
    snprintf(pidbuf, sizeof(pidbuf), "%d\n", (int)pid);
    write_file_atomic(WIFI1B_DHCP_PID, pidbuf, 0600);
    for (int i = 0; i < 20; i++) {
        if (dhcpd_running()) return true;
        if (!w2_proc_alive(pid)) break;
        usleep(100000);
    }
    set_msg("dhcpd: did not start");
    return false;
}

static void stop_dhcpd(void)
{
    int pid = w2_dhcpd_pid();
    if (pid > 1) {
        kill(pid, SIGTERM);
        for (int i = 0; i < 10 && w2_proc_alive(pid); i++) usleep(100000);
        if (w2_proc_alive(pid)) kill(pid, SIGKILL);
        waitpid(pid, NULL, WNOHANG);
    }
    unlink(WIFI1B_DHCP_PID);
}

static void ap_fw6_close(void)
{
    char tool[256];
    fw6_tool(tool, sizeof(tool));
    fw_chain_close(tool, WIFI1B_AP_FW6_CHAIN);
}

static void ap_fw_close(void)
{
    char tool[256];
    fw_tool(tool, sizeof(tool));
    fw_chain_close(tool, WIFI1B_AP_FW_CHAIN);
    ap_fw6_close();
}

static bool ap_fw6_open(void)
{
    char tool[256];
    fw6_tool(tool, sizeof(tool));
    if (!fw_chain_ensure(tool, WIFI1B_AP_FW6_CHAIN)) return false;
    /* NDP, MLD, echo and link-local multicast are required before the
     * CarPlay port chain exists. All accepts are inside our owned wlan0
     * chain; no other interface or global chain is touched. */
    if (!fw_rule_add(tool, WIFI1B_AP_FW6_CHAIN, "-p ipv6-icmp -j ACCEPT")) return false;
    if (!fw_rule_add(tool, WIFI1B_AP_FW6_CHAIN, "-d ff00::/8 -j ACCEPT")) return false;
    if (!fw_rule_add(tool, WIFI1B_AP_FW6_CHAIN, "-p udp --dport 547 -j ACCEPT")) return false;
    return true;
}

static bool ap_fw_open(void)
{
    char tool[256];
    fw_tool(tool, sizeof(tool));
    if (!fw_chain_ensure(tool, WIFI1B_AP_FW_CHAIN)) return false;
    if (sh("%s -C " WIFI1B_AP_FW_CHAIN " -j ACCEPT 2>/dev/null", tool) != 0 &&
        sh("%s -I " WIFI1B_AP_FW_CHAIN " -j ACCEPT 2>/dev/null", tool) != 0)
        return false;
    return ap_fw6_open();
}

/* Read-only AP observation. Every value below is read back from the live
 * interface/supplicant/kernel. Configuration is used only as the expected
 * SSID/channel comparison; it never fills an observed field. */
static void ap_probe(ap_probe_t *p)
{
    memset(p, 0, sizeof(*p));
    p->sampled_ms = now_ms();
    p->role = -1;
    p->ifindex = -1;

    char buf[4096], iw_ssid[80] = "", wpa_ssid[80] = "", wpa_key_mgmt[32] = "";
    int iw_role = -1, wpa_role = -1;
    bool completed = false;
    int iw_freq = 0, wpa_freq = 0;
    bool present = iface_present();

    if (!present) {
        p->role = 0;
    } else {
        cp_net_iw_info iw;
        if (sh_out(buf, sizeof(buf), "iw dev wlan0 info 2>/dev/null") == 0 &&
            cp_net_parse_iw_info(buf, &iw)) {
            if (iw.is_ap) iw_role = 1;
            else if (iw.is_managed) iw_role = 0;
            if (iw.ifindex > 0) p->ifindex = (int)iw.ifindex;
            if (iw.frequency > 0) iw_freq = iw.frequency;
            if (iw.channel > 0) p->channel = iw.channel;
            if (iw.have_ssid) str_copy(iw_ssid, sizeof(iw_ssid), iw.ssid);
        }
        cp_net_wpa_status wpa;
        if (sh_out(buf, sizeof(buf),
                   "wpa_cli -p " WPA_CTRL_DIR " -i wlan0 status 2>/dev/null") == 0 &&
            cp_net_parse_wpa_status(buf, &wpa)) {
            if (wpa.ap_mode) wpa_role = 1;
            else if (wpa.station_mode) wpa_role = 0;
            completed = wpa.completed;
            if (wpa.have_ssid) str_copy(wpa_ssid, sizeof(wpa_ssid), wpa.ssid);
            if (wpa.frequency > 0) wpa_freq = wpa.frequency;
            str_copy(wpa_key_mgmt, sizeof(wpa_key_mgmt), wpa.key_mgmt);
        }
        if ((iw_role < 0 || (iw_role == 1 && !iw_ssid[0] && wpa_role != 1)) &&
            sh_out(buf, sizeof(buf), "iwconfig wlan0 2>/dev/null") == 0) {
            int role = strstr(buf, "Mode:Master") ? 1 : strstr(buf, "Mode:Managed") ? 0 : -1;
            if (iw_role < 0) iw_role = role;
            else if (role >= 0 && role != iw_role) p->conflict = true;
            char *ssid = strstr(buf, "ESSID:\"");
            if (role == 1 && ssid) {
                ssid += 7;
                char *end = strchr(ssid, '"');
                if (end) {
                    *end = 0;
                    if (!decode_control_ssid(iw_ssid, sizeof(iw_ssid), ssid)) iw_ssid[0] = 0;
                }
            }
            if (iw_freq == 0 && wpa_freq == 0) {
                char *freq = strstr(buf, "Frequency:");
                if (freq) {
                    double ghz = strtod(freq + 10, NULL);
                    if (ghz > 0.0 && ghz < 100.0) iw_freq = (int)(ghz * 1000.0 + 0.5);
                }
            }
        }
    }

    p->station = wpa_role == 0 && completed;
    if (present) {
        p->role = iw_role;
        if (p->role < 0 && wpa_role == 1) p->role = 1;
    } else {
        p->role = 0;
    }
    if (iw_role >= 0 && wpa_role >= 0 && iw_role != wpa_role) p->conflict = true;
    if ((p->role == 1 && p->station) || (p->role == 0 && wpa_role == 1)) p->conflict = true;
    if (iw_freq > 0 && wpa_freq > 0 && iw_freq != wpa_freq) p->conflict = true;
    p->frequency = iw_freq > 0 ? iw_freq : wpa_freq;
    if (p->frequency > 0) {
        int channel = cp_net_freq_to_channel(p->frequency);
        if (channel > 0) p->channel = channel;
    }
    str_copy(p->configured_ssid, sizeof(p->configured_ssid), g_cfg.cp_ssid);
    if (p->role == 1) {
        str_copy(p->ssid, sizeof(p->ssid), iw_ssid[0] ? iw_ssid : wpa_role == 1 ? wpa_ssid : "");
        if (iw_ssid[0] && wpa_role == 1 && wpa_ssid[0] && strcmp(iw_ssid, wpa_ssid)) p->conflict = true;
    }
    str_copy(p->security, sizeof(p->security), wpa_key_mgmt);

    /* Exact AP IPv4. A generic wlan0 address is not enough. */
    char ipbuf[4096];
    p->ip[0] = 0;
    if (present) {
        bool ip_cmd_ok = sh_out(ipbuf, sizeof(ipbuf),
                                "ip -o -4 addr show dev wlan0 2>/dev/null") == 0;
        if (ip_cmd_ok && cp_net_parse_ip4_addr(ipbuf, "wlan0", p->ip, sizeof(p->ip))) {
            p->ipv4_ready = !strcmp(p->ip, WIFI1B_AP_IP);
        }
        if (ip_cmd_ok && !p->ipv4_ready) {
            char needle[64];
            snprintf(needle, sizeof(needle), "inet " WIFI1B_AP_IP "/");
            if (strstr(ipbuf, needle)) {
                str_copy(p->ip, sizeof(p->ip), WIFI1B_AP_IP);
                p->ipv4_ready = true;
            }
        }
        if (!ip_cmd_ok && wlan_ip(p->ip, sizeof(p->ip))) {
            p->ipv4_ready = !strcmp(p->ip, WIFI1B_AP_IP);
        }

        /* Exact usable IPv6 link-local, excluding tentative/DAD-failed entries. */
        char inet6[8192];
        cp_net_linklocal ll;
        if (w2_read_if_inet6(inet6, sizeof(inet6))) {
            cp_net_parse_if_inet6(inet6, "wlan0",
                                  p->ifindex > 0 ? (unsigned int)p->ifindex : 0, &ll);
            if (ll.ifindex > 0 && p->ifindex < 0) p->ifindex = (int)ll.ifindex;
            if (ll.usable) {
                p->ipv6_ready = true;
                str_copy(p->linklocal, sizeof(p->linklocal), ll.scoped);
            } else if (ll.dad_failed) {
                p->ipv6_unavailable = true;
                str_copy(p->ipv6_error, sizeof(p->ipv6_error),
                         "IPv6 link-local DAD failed on wlan0; check ICMPv6/NDP firewall on wlan0");
            } else if (ll.tentative) {
                str_copy(p->ipv6_error, sizeof(p->ipv6_error),
                         "IPv6 link-local on wlan0 is tentative (DAD in progress)");
            } else if (w2_ipv6_disabled("wlan0", p->ipv6_error, sizeof(p->ipv6_error))) {
                p->ipv6_unavailable = true;
            } else {
                str_copy(p->ipv6_error, sizeof(p->ipv6_error),
                         "no usable IPv6 link-local on wlan0 yet; readiness requires one");
            }
        } else {
            p->ipv6_unavailable = true;
            str_copy(p->ipv6_error, sizeof(p->ipv6_error),
                     "kernel IPv6 address table is unavailable (/proc/net/if_inet6 missing)");
        }
    }

    int configured_channel = g_cfg.cp_channel > 0
        ? g_cfg.cp_channel
        : cp_net_freq_to_channel(channel_to_freq());
    p->channel_matches_config =
        p->channel > 0 && configured_channel > 0 && p->channel == configured_channel;

    p->tether = p->role;
    p->state = AP_UNKNOWN;
    if (!p->conflict) {
        if (p->role == 0) {
            p->state = AP_ABSENT;
        } else if (p->role == 1) {
            p->state = AP_INTERFACE_UP;
            if (!strcmp(p->ssid, g_cfg.cp_ssid) && p->ipv4_ready &&
                p->ipv6_ready && p->frequency > 0)
                p->state = AP_READY;
        }
    }
}

static void ap_evidence(const char *stage, const ap_probe_t *p)
{
    char actual[512], configured[512], security[64], linklocal[128], ipv6_error[256];
    json_escape(actual, sizeof(actual), p->ssid);
    json_escape(configured, sizeof(configured), p->configured_ssid);
    json_escape(security, sizeof(security), p->security);
    json_escape(linklocal, sizeof(linklocal), p->linklocal);
    json_escape(ipv6_error, sizeof(ipv6_error), p->ipv6_error);
    log_msg("AP stage=%s state=%d role=%d actual_ssid=%s configured_ssid=%s ip=%s "
            "ipv4_ready=%d ipv6=%s ifindex=%d freq=%d channel=%d security=%s "
            "station=%d conflict=%d ipv6_error=%s",
            stage, p->state, p->role, actual, configured, p->ip,
            p->ipv4_ready ? 1 : 0, linklocal, p->ifindex, p->frequency,
            p->channel, security, p->station, p->conflict, ipv6_error);
}

static ap_wait_t ap_wait_ready(bool seen, ap_probe_t *last)
{
    (void)seen;
    int ready = 0;
    int64_t deadline = now_ms() + 30000;
    for (int i = 0; i < 62 && !g_stop && now_ms() < deadline; i++) {
        ap_probe(last);
        /* IPv6 is disabled or DAD already failed: waiting cannot help, and a
         * silent band fallback would hide an actionable owner setting. */
        if (last->ipv6_unavailable && last->role == 1 && !last->conflict &&
            last->ipv4_ready && last->ssid[0] &&
            !strcmp(last->ssid, g_cfg.cp_ssid))
            return AP_WAIT_IPV6_BLOCKED;
        if (last->state == AP_READY) {
            if (++ready >= 3) return AP_WAIT_READY;
        } else ready = 0;
        if (!wpa_process_alive() && i > 4) return AP_WAIT_LOST;
        usleep(500000);
    }
    if (g_stop) return AP_WAIT_UNKNOWN;
    if (last->ipv6_unavailable) return AP_WAIT_IPV6_BLOCKED;
    return AP_WAIT_PENDING;
}

static bool ap_disable(bool explicit_stop)
{
    (void)explicit_stop;
    stop_dhcpd();
    ap_fw_close();
    stop_wpa_supplicant();
    w2_iface_down_clean();
    g_carplay_ie = false;
    return true;
}

static bool ap_open_services(void)
{
    if (!start_dhcpd()) {
        set_msg("CarPlay AP: DHCP server failed to start; AP not reported ready");
        return false;
    }
    if (!ap_fw_open()) {
        set_msg("CarPlay AP: wlan0 IPv4/IPv6 firewall setup failed; AP not reported ready");
        return false;
    }
    return true;
}

static bool ap_start(void)
{
    if (!g_cfg.cp_ssid[0] || strlen(g_cfg.cp_ssid) > 32 ||
        strlen(g_cfg.cp_psk) < 8 || strlen(g_cfg.cp_psk) > 63) {
        set_msg("CarPlay AP requires a 1-32 byte SSID and an 8-63 character key");
        return false;
    }
    if (!ensure_wifi_ready()) return false;

    ap_probe_t p;
    ap_probe(&p);
    if (p.state == AP_READY && !strcmp(p.ssid, g_cfg.cp_ssid) && wpa_process_alive()) {
        if (!ap_open_services()) {
            ap_stop();
            return false;
        }
        g_carplay_ie = true;
        set_msg("CarPlay AP running (SSID=%s, %d MHz, DHCP %s/24)",
                p.ssid, p.frequency, WIFI1B_AP_IP);
        return true;
    }

    stop_dhcpd();
    stop_wpa_supplicant();
    w2_iface_flush();
    /* GB 5 GHz regulatory domain, matching the rest of the system config.
     * The vendor wpa_supplicant config parser does not expose a country=
     * field, so set it through nl80211. */
    if (sh("iw reg set GB 2>/dev/null") != 0)
        log_msg("CarPlay AP: iw reg set GB failed; 5 GHz may be rejected");
    else
        log_msg("CarPlay AP: regulatory domain GB");
    prepare_carplay_identity();

    char failure[256] = "";
    for (int attempt = 0; attempt < 2 && !g_stop; attempt++) {
        if (attempt) {
            g_cfg.cp_band = 1;
            g_cfg.cp_channel = 6;
            config_save();
            log_msg("CarPlay AP: retrying on 2.4 GHz channel 6");
        }
        if (!write_ap_conf()) return false;
        iface_up();
        if (!start_wpa_supplicant()) {
            set_msg("CarPlay AP: supplicant failed to start");
            stop_wpa_supplicant();
            break;
        }
        if (!w2_iface_set_static()) {
            set_msg("CarPlay AP: cannot assign " WIFI1B_AP_IP);
            stop_wpa_supplicant();
            return false;
        }
        ap_wait_t w = ap_wait_ready(false, &p);
        if (w == AP_WAIT_READY) {
            if (!ap_open_services()) {
                ap_stop();
                return false;
            }
            g_carplay_ie = true;
            set_msg("CarPlay AP running (SSID=%s, %d MHz, DHCP %s/24)",
                    p.ssid, p.frequency, WIFI1B_AP_IP);
            return true;
        }
        ap_evidence("start-attempt", &p);
        if (w == AP_WAIT_IPV6_BLOCKED) {
            snprintf(failure, sizeof(failure),
                     "CarPlay AP cannot be ready (SSID=%s): %s",
                     g_cfg.cp_ssid,
                     p.ipv6_error[0] ? p.ipv6_error : "IPv6 link-local unavailable");
            stop_dhcpd();
            stop_wpa_supplicant();
            w2_iface_flush();
            break; /* a band fallback cannot create IPv6 */
        }
        if (!failure[0] && p.ipv6_error[0])
            snprintf(failure, sizeof(failure),
                     "CarPlay AP did not become network-ready (SSID=%s): %s",
                     g_cfg.cp_ssid, p.ipv6_error);
        stop_dhcpd();
        stop_wpa_supplicant();
        w2_iface_flush();
    }
    if (failure[0])
        set_msg("%s", failure);
    else
        set_msg("CarPlay AP failed to start (SSID=%s)", g_cfg.cp_ssid);
    return false;
}

static bool ap_stop(void)
{
    stop_dhcpd();
    ap_fw_close();
    carplay_fw_close();
    stop_wpa_supplicant();
    w2_iface_down_clean();
    g_carplay_ie = false;
    return true;
}

/* ------------------------------------------------------------------ */
/* client mode                                                         */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* built-in DHCP client                                                */
/*                                                                     */
/* The 260128 rootfs ships no usable DHCP client: the vendor busybox    */
/* has no udhcpc applet ("udhcpc: applet not found", verified in the    */
/* firmware VM) and there is no dhcpcd binary.  Client mode therefore   */
/* runs its own DISCOVER/OFFER/REQUEST/ACK exchange and applies the     */
/* lease with the stock ip utility.  Test hooks mirror the server.      */
/* ------------------------------------------------------------------ */

static bool w2_read_iface_mac(const char *iface, uint8_t out[6])
{
#ifdef WIFI_TEST
    if (test_mac_override) return parse_mac6(test_mac_override, out);
#endif
    char path[128], buf[64];
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", iface);
    if (read_file(path, buf, sizeof(buf)) <= 0) return false;
    return parse_mac6(trim(buf), out);
}

static int w2_mask_to_prefix(uint32_t mask)
{
    if (mask == 0) return 0;
    uint32_t inverse = ~mask;
    if (inverse & (inverse + 1U)) return -1; /* non-contiguous mask */
    int prefix = 0;
    while (mask & 0x80000000U) {
        prefix++;
        mask <<= 1;
    }
    return prefix;
}

/* Build the common DHCP client option block, including the Ethernet client
 * identifier. DISCOVER and REQUEST must carry the same option 61; a renewal
 * built through this helper keeps it consistent too. */
static size_t w2_dhcp_client_opts(uint8_t *opts, size_t cap, uint8_t type,
                                  const uint8_t mac[6], uint32_t requested,
                                  uint32_t server, bool have_server)
{
    static const uint8_t prl[] = {1, 3, 6, 15, 28};
    size_t off = 0;
    uint8_t cid[7];

    if (opts == NULL || cap < 16) return 0;
    off += w2_dhcp_put(opts + off, cap - off, 53, 1, &type);
    if (requested) {
        uint32_t value = htonl(requested);
        off += w2_dhcp_put(opts + off, cap - off, 50, 4, &value);
    }
    if (have_server) {
        uint32_t value = htonl(server);
        off += w2_dhcp_put(opts + off, cap - off, 54, 4, &value);
    }
    cid[0] = 1; /* Ethernet */
    memcpy(cid + 1, mac, 6);
    off += w2_dhcp_put(opts + off, cap - off, 61, 7, cid);
    off += w2_dhcp_put(opts + off, cap - off, 55, sizeof(prl), prl);
    return off;
}

static bool w2_addr_present(const char *iface, const char *ip, int prefix)
{
    char buf[4096], needle[128];
    if (sh_out(buf, sizeof(buf), "ip -o -4 addr show dev %s 2>/dev/null", iface) != 0)
        return false;
    snprintf(needle, sizeof(needle), "inet %s/%d", ip, prefix);
    char *hit = strstr(buf, needle);
    if (!hit) return false;
    char next = hit[strlen(needle)];
    return next == ' ' || next == '\0' || next == '\\' || next == '\n';
}

/* Only the connected route is verified. The modem/default route is never
 * replaced or removed by client mode. */
static bool w2_connected_route_present(const char *iface, const char *ip,
                                       uint32_t mask, int prefix)
{
    char buf[4096], net[32], needle[96];
    uint32_t network = w2_ip4(ip) & mask;

    w2_ip4_str(network, net, sizeof(net));
    snprintf(needle, sizeof(needle), "%s/%d", net, prefix);
    if (sh_out(buf, sizeof(buf), "ip -4 route show dev %s 2>/dev/null", iface) != 0)
        return false;
    char *hit = strstr(buf, needle);
    if (!hit) return false;
    char next = hit[strlen(needle)];
    return next == ' ' || next == '\0' || next == '\\' || next == '\n';
}

/* ------------------------------------------------------------------ */
/* modem and legacy wlan0 route state                                  */
/* ------------------------------------------------------------------ */

/* The vendor keeps a carrier-data IPv4 address on rmnet_usb0 (BlueLink)
 * and/or rmnet_usb1 (navigation). While either has an IPv4 address, client
 * mode must not install a WiFi default: the modem default and the vendor's
 * per-server host routes must remain authoritative. */
static bool w2_iface_has_ipv4(const char *iface)
{
    char buf[4096];
    if (iface == NULL || !*iface) return false;
    if (sh_out(buf, sizeof(buf), "ip -o -4 addr show dev %s 2>/dev/null", iface) == 0) {
        if (strstr(buf, " inet ") != NULL) return true;
        return !strncmp(trim(buf), "inet ", 5);
    }
    if (sh_out(buf, sizeof(buf), "ifconfig %s 2>/dev/null", iface) != 0)
        return false;
    return strstr(buf, "inet addr:") != NULL || strstr(buf, "inet ") != NULL;
}

static bool w2_modem_data_up(void)
{
    return w2_iface_has_ipv4("rmnet_usb0") ||
           w2_iface_has_ipv4("rmnet_usb1");
}

/* The target's old iproute2 omits the `dev` token when the query is already
 * filtered with `dev wlan0`.  Query the unfiltered table and keep only exact
 * wlan0 default lines, so deletes and owned-route matching carry the device
 * qualifier. */
static bool w2_wlan_defaults(char *out, size_t n)
{
    char all[8192];
    if (out == NULL || n == 0) return false;
    out[0] = 0;
    if (sh_out(all, sizeof(all), "ip -4 route show default 2>/dev/null") != 0)
        return false;
    size_t used = 0;
    char *save = NULL;
    for (char *line = strtok_r(all, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *p = trim(line);
        char copy[512], *tokens[64] = {0};
        int count = 0;
        char *tsave = NULL;
        bool wlan = false;
        if (strncmp(p, "default", 7) != 0) continue;
        snprintf(copy, sizeof(copy), "%s", p);
        for (char *tok = strtok_r(copy, " \t", &tsave); tok && count < 64;
             tok = strtok_r(NULL, " \t", &tsave))
            tokens[count++] = tok;
        for (int i = 0; i + 1 < count; i++) {
            if (!strcmp(tokens[i], "dev") && !strcmp(tokens[i + 1], "wlan0")) {
                wlan = true;
                break;
            }
        }
        if (!wlan) continue;
        int wrote = snprintf(out + used, n - used, "%s\n", p);
        if (wrote < 0 || (size_t)wrote >= n - used) return false;
        used += (size_t)wrote;
    }
    return true;
}

/* Strictly a route line from the kernel.  It is still treated as data: a
 * line with shell metacharacters is never passed back to /bin/sh. */
static bool w2_route_line_safe(const char *line)
{
    if (line == NULL) return false;
    for (const unsigned char *p = (const unsigned char *)line; *p; p++) {
        if (isalnum(*p) || *p == ' ' || *p == '\t' || *p == '.' || *p == ':' ||
            *p == '/' || *p == '-' || *p == '_')
            continue;
        return false;
    }
    return true;
}

static bool w2_legacy_delete_line(const char *line)
{
    char copy[512], *tokens[32] = {0};
    int count = 0;
    char *save = NULL;
    if (line == NULL || !w2_route_line_safe(line)) return false;
    snprintf(copy, sizeof(copy), "%s", line);
    for (char *tok = strtok_r(copy, " \t", &save); tok && count < 32;
         tok = strtok_r(NULL, " \t", &save))
        tokens[count++] = tok;
    if (count == 0 || strcmp(tokens[0], "default") != 0) return false;
    /* w2_owned_route_cleanup() has already removed the exact ledged route.
     * Any default still via wlan0 is stale/orphaned and must not be allowed
     * to outrank the modem default. */
    return sh("ip -4 route del %s 2>/dev/null", line) == 0;
}

/* The V21 Wifi1b build used `route replace default via <wifi-gw> dev wlan0`
 * and never removed it. On an over-the-top V22 install that route survives
 * the daemon restart and would still outrank the modem. Client mode owns
 * wlan0 at this point, so any remaining default via wlan0 is stale. Remove
 * only exact lines, never a modem/other-interface route. */
static bool w2_legacy_wlan_default_cleanup(void)
{
    char buf[4096];
    int removed = 0;
    for (int attempt = 0; attempt < 8; attempt++) {
        if (!w2_wlan_defaults(buf, sizeof(buf))) return true;
        bool deleted = false;
        char *save = NULL;
        for (char *line = strtok_r(buf, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            char *p = trim(line);
            if (strncmp(p, "default", 7) != 0) continue;
            if (w2_legacy_delete_line(p)) {
                removed++;
                deleted = true;
                break;
            }
        }
        if (!deleted) break;
    }
    if (removed) {
        g_client_legacy_routes_removed += removed;
        log_msg("dhcp client: removed %d stale wlan0 default route(s)", removed);
    }
    if (!w2_wlan_defaults(buf, sizeof(buf))) return true;
    return !strstr(buf, "default");
}

/* ------------------------------------------------------------------ */
/* client default-route policy                                         */
/*                                                                     */
/* modem_safe (default): while a modem data interface has IPv4, install  */
/* no WiFi default at all. WiFi remains a connected-route SSH path.     */
/* fallback: install one owned default strictly below every foreign      */
/* default. It is added/deleted by exact spec from a ledger; foreign     */
/* defaults and policy rules are never flushed.                         */
/* ------------------------------------------------------------------ */

typedef struct {
    char gateway[32];
    char iface[16];
    unsigned int metric;
    unsigned int proto;
} w2_owned_route_t;

typedef struct {
    bool valid;
    bool updated;
    bool route_ok;
    bool wifi_default;
    uint32_t ip;
    uint32_t server;
    uint32_t mask;
    uint32_t gateway;
    int prefix;
    int lease_s;
    int t1_s;
    int t2_s;
    unsigned int route_metric;
    unsigned int route_proto;
    char iface[16];
} w2_dhcp_lease_t;

static w2_dhcp_lease_t g_dhcp_lease;

static bool w2_route_token_u32(const char *text, unsigned int *out)
{
    char *end = NULL;
    unsigned long value;
    if (text == NULL || *text == '\0' || out == NULL) return false;
    if (*text == '-') return false;
    for (const char *p = text; *p; p++) if (*p < '0' || *p > '9') return false;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > 0xffffffffUL)
        return false;
    *out = (unsigned int)value;
    return true;
}

static bool w2_route_token_proto(const char *text, unsigned int *out)
{
    char *end = NULL;
    unsigned long value;
    if (text == NULL || *text == '\0' || out == NULL) return false;
    if (*text == '-') return false;
    errno = 0;
    value = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || value > 0xffffffffUL)
        return false;
    *out = (unsigned int)value;
    return true;
}

/* Parse a route spec line without trusting it: the ledger is local, but a
 * corrupt/partial file must never make us delete an unrelated route. */
static bool w2_owned_route_parse(const char *text, w2_owned_route_t *route)
{
    char line[512], copy[512], *tokens[8] = {0};
    int count = 0;
    char *save = NULL;

    if (text == NULL || route == NULL) return false;
    memset(route, 0, sizeof(*route));
    snprintf(line, sizeof(line), "%s", text);
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                       line[len - 1] == ' ' || line[len - 1] == '\t'))
        line[--len] = '\0';
    snprintf(copy, sizeof(copy), "%s", line);
    for (char *tok = strtok_r(copy, " \t", &save); tok && count < 8;
         tok = strtok_r(NULL, " \t", &save))
        tokens[count++] = tok;
    if (count != 5 || strcmp(tokens[0], "v1") != 0) return false;
    if (inet_pton(AF_INET, tokens[1], &(struct in_addr){0}) != 1) return false;
    size_t iface_len = strlen(tokens[2]);
    if (iface_len == 0 || iface_len >= sizeof(route->iface)) return false;
    for (size_t i = 0; i < iface_len; i++) {
        unsigned char c = (unsigned char)tokens[2][i];
        if (!(isalnum(c) || c == '_' || c == '-' || c == '.')) return false;
    }
    if (!w2_route_token_u32(tokens[3], &route->metric) ||
        !w2_route_token_u32(tokens[4], &route->proto))
        return false;
    snprintf(route->gateway, sizeof(route->gateway), "%s", tokens[1]);
    snprintf(route->iface, sizeof(route->iface), "%s", tokens[2]);
    return true;
}

static bool w2_route_line_matches(const char *line, const char *gateway,
                                  const char *iface, unsigned int metric,
                                  unsigned int proto)
{
    char copy[512], *tokens[64] = {0};
    int count = 0;
    char *save = NULL;
    char metric_text[16];

    if (line == NULL || gateway == NULL || iface == NULL) return false;
    snprintf(copy, sizeof(copy), "%s", line);
    for (char *tok = strtok_r(copy, " \t", &save); tok && count < 64;
         tok = strtok_r(NULL, " \t", &save))
        tokens[count++] = tok;
    if (count == 0 || strcmp(tokens[0], "default") != 0) return false;
    snprintf(metric_text, sizeof(metric_text), "%u", metric);
    bool via = false, dev = false, met = false, pro = false;
    for (int i = 0; i + 1 < count; i++) {
        if (!strcmp(tokens[i], "via") && !strcmp(tokens[i + 1], gateway)) via = true;
        else if (!strcmp(tokens[i], "dev") && !strcmp(tokens[i + 1], iface)) dev = true;
        else if (!strcmp(tokens[i], "metric") && !strcmp(tokens[i + 1], metric_text)) met = true;
        else if (!strcmp(tokens[i], "proto")) {
            unsigned int parsed = 0;
            if (w2_route_token_proto(tokens[i + 1], &parsed) && parsed == proto)
                pro = true;
        }
    }
    return via && dev && met && pro;
}

static bool w2_default_routes(char *out, size_t n)
{
    if (sh_out(out, n, "ip -4 route show default 2>/dev/null") != 0) {
        out[0] = '\0';
        return false;
    }
    return true;
}

static bool w2_route_exact_present(const char *gateway, const char *iface,
                                   unsigned int metric, unsigned int proto,
                                   bool *present)
{
    char buf[4096];
    if (present == NULL || !w2_default_routes(buf, sizeof(buf))) return false;
    *present = false;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *p = trim(line);
        if (w2_route_line_matches(p, gateway, iface, metric, proto)) {
            *present = true;
            break;
        }
    }
    return true;
}

/* Strictly higher metric than every existing default. No existing default
 * uses WIFI1B_CLIENT_DEFAULT_METRIC rather than metric 0. Malformed or
 * overflowing metrics fail closed instead of risking preference inversion. */
static bool w2_route_select_metric(unsigned int *metric)
{
    char buf[4096], *save = NULL;
    bool saw_default = false;
    unsigned int max_metric = 0;

    if (metric == NULL || !w2_default_routes(buf, sizeof(buf))) return false;
    for (char *line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *p = trim(line);
        char copy[512], *tokens[64] = {0};
        int count = 0;
        char *tsave = NULL;
        if (strncmp(p, "default", 7) != 0) continue;
        saw_default = true;
        snprintf(copy, sizeof(copy), "%s", p);
        for (char *tok = strtok_r(copy, " \t", &tsave); tok && count < 64;
             tok = strtok_r(NULL, " \t", &tsave))
            tokens[count++] = tok;
        unsigned int value = 0;
        bool have_metric = false;
        for (int i = 0; i < count; i++) {
            if (!strcmp(tokens[i], "metric")) {
                if (have_metric || i + 1 >= count ||
                    !w2_route_token_u32(tokens[i + 1], &value)) {
                    log_msg("dhcp client: refusing malformed default metric: %s", p);
                    return false;
                }
                have_metric = true;
            }
        }
        if (!have_metric) value = 0; /* kernel omits metric 0 */
        if (value > max_metric) max_metric = value;
    }
    if (!saw_default) {
        *metric = WIFI1B_CLIENT_DEFAULT_METRIC;
        return true;
    }
    if (max_metric == 0xffffffffU) {
        log_msg("dhcp client: no route metric above existing default %u", max_metric);
        return false;
    }
    *metric = max_metric + 1;
    return true;
}

static bool w2_owned_route_cleanup(void)
{
    char ledger[1024];
    if (access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) != 0) return true;
    int n = read_file(WIFI1B_CLIENT_ROUTE_LEDGER, ledger, sizeof(ledger));
    if (n < 0) return false;
    if (n == 0) return true;
    w2_owned_route_t route;
    if (!w2_owned_route_parse(ledger, &route)) {
        log_msg("dhcp client: refusing to act on malformed route ledger");
        return false;
    }
    for (int attempt = 0; attempt < 4; attempt++) {
        bool present = false;
        if (!w2_route_exact_present(route.gateway, route.iface, route.metric,
                                    route.proto, &present))
            return false;
        if (!present) break;
        if (!w2_ip_command("route del default via %s dev %s metric %u proto %u",
                           route.gateway, route.iface, route.metric, route.proto))
            return false;
    }
    /* Re-read: never unlink the ledger while our exact route is still there. */
    bool present = false;
    if (!w2_route_exact_present(route.gateway, route.iface, route.metric,
                                route.proto, &present))
        return false;
    if (present) return false;
    unlink(WIFI1B_CLIENT_ROUTE_LEDGER);
    return true;
}

static bool w2_owned_route_install(const char *gateway, const char *iface,
                                   unsigned int metric, unsigned int proto)
{
    char ledger[512];
    if (gateway == NULL || iface == NULL) return false;
    if (!w2_ip_command("route add default via %s dev %s metric %u proto %u",
                       gateway, iface, metric, proto))
        return false;
    bool present = false;
    if (!w2_route_exact_present(gateway, iface, metric, proto, &present) || !present) {
        w2_ip_command("route del default via %s dev %s metric %u proto %u",
                      gateway, iface, metric, proto);
        return false;
    }
    snprintf(ledger, sizeof(ledger), "v1 %s %s %u %u\n",
             gateway, iface, metric, proto);
    if (write_file_atomic(WIFI1B_CLIENT_ROUTE_LEDGER, ledger, 0600) != 0) {
        w2_ip_command("route del default via %s dev %s metric %u proto %u",
                      gateway, iface, metric, proto);
        return false;
    }
    return true;
}

/* Install or suppress the WiFi route according to the active policy.  The
 * gateway comes from the lease, never from configuration.  Import/upgrade
 * cleanup runs first so a V21 `route replace default via wlan0` route cannot
 * survive an over-the-top install. */
static bool w2_client_route_install(uint32_t gateway, const char *iface)
{
    unsigned int metric = 0;
    char gw[32];
    bool modem_up = w2_modem_data_up();

    g_client_modem_ipv4 = modem_up;
    if (!w2_owned_route_cleanup()) {
        snprintf(g_dhcp_error, sizeof(g_dhcp_error),
                 "cannot remove previous owned default route");
        return false;
    }
    if (!w2_legacy_wlan_default_cleanup()) {
        snprintf(g_dhcp_error, sizeof(g_dhcp_error),
                 "cannot remove stale wlan0 default route");
        return false;
    }

    /* The requested design: WiFi is an SSH-only link while the modem has
     * data.  Do not put any WiFi default in the main table in that state. */
    if (g_cfg.client_route_policy == CLIENT_ROUTE_MODEM_SAFE && modem_up) {
        g_dhcp_lease.route_ok = true;
        g_dhcp_lease.wifi_default = false;
        g_dhcp_lease.route_metric = 0;
        g_dhcp_lease.route_proto = 0;
        log_msg("dhcp client: modem IPv4 present; WiFi default suppressed");
        return true;
    }

    if (gateway == 0) {
        snprintf(g_dhcp_error, sizeof(g_dhcp_error),
                 "DHCP lease has no router/gateway option");
        return false;
    }
    if (gateway == 0xffffffffU || (gateway >> 28) >= 14 ||
        (gateway >> 24) == 127) {
        snprintf(g_dhcp_error, sizeof(g_dhcp_error),
                 "DHCP gateway is not a usable unicast address");
        return false;
    }
    w2_ip4_str(gateway, gw, sizeof(gw));
    if (!w2_route_select_metric(&metric)) {
        snprintf(g_dhcp_error, sizeof(g_dhcp_error),
                 "cannot select a safe WiFi default-route metric");
        return false;
    }
    if (!w2_owned_route_install(gw, iface, metric, WIFI1B_CLIENT_ROUTE_PROTO)) {
        snprintf(g_dhcp_error, sizeof(g_dhcp_error),
                 "cannot install/read back the owned WiFi default route");
        return false;
    }
    g_dhcp_lease.route_ok = true;
    g_dhcp_lease.wifi_default = true;
    g_dhcp_lease.route_metric = metric;
    g_dhcp_lease.route_proto = WIFI1B_CLIENT_ROUTE_PROTO;
    return true;
}

static bool w2_default_metric_value(const char *line, unsigned int *value)
{
    char copy[512], *tokens[64] = {0};
    int count = 0;
    char *save = NULL;
    bool have_metric = false;
    unsigned int parsed = 0;

    if (line == NULL || value == NULL) return false;
    snprintf(copy, sizeof(copy), "%s", line);
    for (char *tok = strtok_r(copy, " \t", &save); tok && count < 64;
         tok = strtok_r(NULL, " \t", &save))
        tokens[count++] = tok;
    if (count == 0 || strcmp(tokens[0], "default") != 0) return false;
    for (int i = 0; i < count; i++) {
        if (strcmp(tokens[i], "metric")) continue;
        if (have_metric || i + 1 >= count ||
            !w2_route_token_u32(tokens[i + 1], &parsed))
            return false;
        have_metric = true;
    }
    *value = have_metric ? parsed : 0;
    return true;
}

/* Highest metric among defaults that are not our exact ledged route. */
static bool w2_foreign_default_max_metric(unsigned int *max_metric, bool *saw)
{
    char buf[4096];
    w2_owned_route_t own;
    bool have_own = false;
    char ledger[1024];

    if (max_metric == NULL || saw == NULL) return false;
    *max_metric = 0;
    *saw = false;
    if (!w2_default_routes(buf, sizeof(buf))) return false;
    if (read_file(WIFI1B_CLIENT_ROUTE_LEDGER, ledger, sizeof(ledger)) > 0 &&
        w2_owned_route_parse(ledger, &own))
        have_own = true;

    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *p = trim(line);
        if (strncmp(p, "default", 7) != 0) continue;
        if (have_own && w2_route_line_matches(p, own.gateway, own.iface,
                                              own.metric, own.proto))
            continue;
        unsigned int value = 0;
        if (!w2_default_metric_value(p, &value)) {
            log_msg("route audit: refusing malformed default metric: %s", p);
            return false;
        }
        *saw = true;
        if (value > *max_metric) *max_metric = value;
    }
    return true;
}

/* True when a default via wlan0 exists that is not our exact ledged route.
 * Such a route can steal replies from the modem interface, so fallback mode
 * reinstalls the owned route (which first removes every stale wlan0 default). */
static bool w2_wlan_foreign_default_present(bool *present)
{
    char buf[4096];
    w2_owned_route_t own;
    bool have_own = false;
    char ledger[1024];

    if (present == NULL) return false;
    *present = false;
    if (!w2_wlan_defaults(buf, sizeof(buf))) return false;
    if (read_file(WIFI1B_CLIENT_ROUTE_LEDGER, ledger, sizeof(ledger)) > 0 &&
        w2_owned_route_parse(ledger, &own))
        have_own = true;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *p = trim(line);
        if (strncmp(p, "default", 7) != 0) continue;
        if (have_own && w2_route_line_matches(p, own.gateway, own.iface,
                                              own.metric, own.proto))
            continue;
        *present = true;
        break;
    }
    return true;
}

/* Reconcile the route intent with live state.  In modem_safe mode the WiFi
 * default is removed as soon as a modem data IPv4 appears and reinstated
 * only if the modem data call goes away.  In fallback mode the owned route
 * is re-added if it vanished and moved above any higher foreign default. */
static bool w2_client_route_audit(void)
{
    if (!g_dhcp_lease.valid || g_dhcp_lease.ip == 0) return true;
    bool modem_up = w2_modem_data_up();
    g_client_modem_ipv4 = modem_up;

    if (g_cfg.client_route_policy == CLIENT_ROUTE_MODEM_SAFE && modem_up) {
        bool had = g_dhcp_lease.wifi_default;
        if (!w2_owned_route_cleanup()) return false;
        if (!w2_legacy_wlan_default_cleanup()) return false;
        g_dhcp_lease.route_ok = true;
        g_dhcp_lease.wifi_default = false;
        g_dhcp_lease.route_metric = 0;
        g_dhcp_lease.route_proto = 0;
        if (had) log_msg("route audit: modem data is up; WiFi default removed");
        return true;
    }

    bool own_present = false;
    char ledger[1024];
    if (read_file(WIFI1B_CLIENT_ROUTE_LEDGER, ledger, sizeof(ledger)) > 0) {
        w2_owned_route_t own;
        if (!w2_owned_route_parse(ledger, &own)) return false;
        if (!w2_route_exact_present(own.gateway, own.iface, own.metric,
                                    own.proto, &own_present))
            return false;
    }
    if (!own_present) {
        if (g_dhcp_lease.gateway == 0) {
            g_dhcp_lease.route_ok = true;
            g_dhcp_lease.wifi_default = false;
            g_dhcp_lease.route_metric = 0;
            g_dhcp_lease.route_proto = 0;
            return true;
        }
        log_msg("route audit: WiFi fallback missing; re-installing");
        return w2_client_route_install(g_dhcp_lease.gateway, g_dhcp_lease.iface);
    }

    bool foreign_wlan = false;
    if (!w2_wlan_foreign_default_present(&foreign_wlan)) return false;
    if (foreign_wlan) {
        log_msg("route audit: stale foreign wlan0 default present; re-installing");
        return w2_client_route_install(g_dhcp_lease.gateway, g_dhcp_lease.iface);
    }
    unsigned int max_metric = 0;
    bool saw = false;
    if (!w2_foreign_default_max_metric(&max_metric, &saw)) return false;
    if (saw && max_metric >= g_dhcp_lease.route_metric) {
        log_msg("route audit: foreign default metric %u >= owned %u; re-installing",
                max_metric, g_dhcp_lease.route_metric);
        return w2_client_route_install(g_dhcp_lease.gateway, g_dhcp_lease.iface);
    }
    g_dhcp_lease.route_ok = true;
    g_dhcp_lease.wifi_default = true;
    return true;
}

/* ------------------------------------------------------------------ */
/* raw DHCP receive fallback (AF_PACKET)                               */
/*                                                                     */
/* The stock stack may drop an OFFER before INPUT when rp_filter is on. */
/* This fallback never changes a sysctl: it observes the same IPv4 UDP   */
/* datagram at the packet socket, with strict bounds and identity checks.*/
/* ------------------------------------------------------------------ */

static uint32_t w2_sum_bytes(const uint8_t *data, size_t len, uint32_t sum)
{
    size_t i;
    for (i = 0; i + 1 < len; i += 2)
        sum += ((uint32_t)data[i] << 8) | data[i + 1];
    if (i < len) sum += (uint32_t)data[i] << 8;
    return sum;
}

static uint16_t w2_fold_checksum(uint32_t sum)
{
    while (sum >> 16) sum = (sum & 0xffffU) + (sum >> 16);
    return (uint16_t)~sum;
}

static bool w2_udp_checksum_valid(const struct iphdr *ip,
                                  const struct udphdr *udp, size_t udp_len)
{
    uint32_t sum = 0;
    sum = w2_sum_bytes((const uint8_t *)&ip->saddr, 4, sum);
    sum = w2_sum_bytes((const uint8_t *)&ip->daddr, 4, sum);
    sum += ip->protocol;          /* zero byte + protocol */
    sum += (uint32_t)udp_len;     /* network-order 16-bit length */
    sum = w2_sum_bytes((const uint8_t *)udp, udp_len, sum);
    return w2_fold_checksum(sum) == 0;
}

/* Parse an IPv4/UDP DHCP reply received through AF_PACKET SOCK_DGRAM.
 * Everything is length-checked; fragments are rejected rather than
 * reassembled, and the DHCP magic/xid/chaddr/client-id identity must match. */
static bool w2_dhcp_raw_parse(const uint8_t *packet, size_t len,
                              uint32_t xid, const uint8_t mac[6],
                              uint8_t *payload, size_t payload_cap,
                              size_t *payload_len)
{
    static const uint8_t magic[4] = {99, 130, 83, 99};
    struct iphdr ip_copy;
    const struct iphdr *ip;
    const struct udphdr *udp;
    size_t ihl, dhcp_len, opt_len;
    unsigned int total, udp_len;
    uint16_t frag;
    uint8_t cid_len = 0;
    uint8_t *cid;
    struct w2_dhcp_packet *dhcp;

    if (packet == NULL || payload == NULL || payload_len == NULL || mac == NULL)
        return false;
    if (len < sizeof(struct iphdr) + sizeof(struct udphdr)) return false;
    memcpy(&ip_copy, packet, sizeof(ip_copy));
    ip = &ip_copy;
    ihl = (size_t)ip->ihl * 4;
    if (ip->version != 4 || ihl < sizeof(struct iphdr) || len < ihl + 8)
        return false;
    total = (unsigned int)ntohs(ip->tot_len);
    if (total < ihl + 8 || total > len) return false;
    if (w2_fold_checksum(w2_sum_bytes(packet, ihl, 0)) != 0) return false;
    frag = ntohs(ip->frag_off);
    if ((frag & (IP_MF | IP_OFFMASK)) != 0) return false;
    if (ip->protocol != IPPROTO_UDP) return false;

    udp = (const struct udphdr *)(packet + ihl);
    udp_len = (unsigned int)ntohs(udp->len);
    if (udp_len < 8 || ihl + udp_len > total) return false;
    if (ntohs(udp->source) != w2_dhcp_port() ||
        ntohs(udp->dest) != w2_dhcp_client_port())
        return false;
    if (udp->check != 0 && !w2_udp_checksum_valid(ip, udp, udp_len))
        return false;

    dhcp_len = udp_len - 8;
    if (dhcp_len > payload_cap ||
        dhcp_len < offsetof(struct w2_dhcp_packet, options) + 4)
        return false;
    memcpy(payload, packet + ihl + 8, dhcp_len);
    dhcp = (struct w2_dhcp_packet *)payload;
    if (dhcp->op != 2 || dhcp->htype != 1 || dhcp->hlen != 6 ||
        dhcp->xid != xid || memcmp(dhcp->chaddr, mac, 6) != 0)
        return false;
    if (memcmp(dhcp->options, magic, sizeof(magic)) != 0) return false;
    opt_len = dhcp_len - offsetof(struct w2_dhcp_packet, options) - 4;
    cid = w2_dhcp_opt_n(dhcp->options + 4, opt_len, 61, &cid_len);
    if (cid != NULL &&
        (cid_len != 7 || cid[0] != 1 || memcmp(cid + 1, mac, 6) != 0))
        return false;
    *payload_len = dhcp_len;
    return true;
}

static int w2_dhcp_raw_open(const char *iface, int *ifindex)
{
    struct sockaddr_ll sa;
    unsigned int index;
    int fd;

    if (ifindex) *ifindex = 0;
    if (iface == NULL || *iface == '\0') return -1;
    index = if_nametoindex(iface);
    if (index == 0) return -1;
    fd = socket(AF_PACKET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                htons(ETH_P_IP));
    if (fd < 0) return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sll_family = AF_PACKET;
    sa.sll_protocol = htons(ETH_P_IP);
    sa.sll_ifindex = (int)index;
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    if (ifindex) *ifindex = (int)index;
    return fd;
}

static bool w2_dhcp_raw_receive(int fd, int ifindex, uint32_t xid,
                                const uint8_t mac[6], uint8_t *payload,
                                size_t payload_cap, size_t *payload_len)
{
    for (int i = 0; i < 8; i++) {
        uint8_t packet[1600];
        struct sockaddr_ll from;
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(fd, packet, sizeof(packet), 0,
                             (struct sockaddr *)&from, &from_len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (from.sll_ifindex != ifindex) continue;
        if (w2_dhcp_raw_parse(packet, (size_t)n, xid, mac, payload,
                              payload_cap, payload_len))
            return true;
    }
    return false;
}

/* Poll the normal UDP socket and the raw fallback together. The test hook
 * can force raw-only RX so the namespace test proves that path. */
static int w2_dhcp_wait_reply(int fd, int raw_fd, int raw_ifindex,
                              uint32_t xid, const uint8_t mac[6],
                              uint8_t *reply, size_t reply_cap,
                              size_t *reply_len, int64_t deadline)
{
    bool use_udp = getenv("WIFI1B_DHCP_TEST_FORCE_RAW") == NULL;

    while (!g_stop && now_ms() < deadline) {
        struct pollfd pfd[2];
        int nfds = 0, udp_slot = -1, raw_slot = -1;
        if (use_udp) {
            udp_slot = nfds;
            pfd[nfds].fd = fd;
            pfd[nfds].events = POLLIN;
            pfd[nfds].revents = 0;
            nfds++;
        }
        if (raw_fd >= 0) {
            raw_slot = nfds;
            pfd[nfds].fd = raw_fd;
            pfd[nfds].events = POLLIN;
            pfd[nfds].revents = 0;
            nfds++;
        }
        if (nfds == 0) return 0;
        int pr = poll(pfd, (nfds_t)nfds, 250);
        if (pr <= 0) continue;
        if (udp_slot >= 0 && (pfd[udp_slot].revents & POLLIN)) {
            ssize_t n = recv(fd, reply, reply_cap, 0);
            if (n > 0 &&
                (size_t)n >= offsetof(struct w2_dhcp_packet, options) + 4) {
                *reply_len = (size_t)n;
                return 1;
            }
        }
        if (raw_slot >= 0 && (pfd[raw_slot].revents & POLLIN)) {
            if (w2_dhcp_raw_receive(raw_fd, raw_ifindex, xid, mac, reply,
                                    reply_cap, reply_len))
                return 2;
        }
    }
    return 0;
}

/* Read-only diagnosis. The vendor may have rp_filter enabled; changing
 * global/all/default or per-interface sysctls is not safe from this daemon.
 * Record the limitation instead, so a real OFFER loss is actionable. */
static void w2_dhcp_rp_filter_note(const char *iface)
{
    char all[16] = "", per[16] = "", path[128];
    read_file("/proc/sys/net/ipv4/conf/all/rp_filter", all, sizeof(all));
    snprintf(path, sizeof(path), "/proc/sys/net/ipv4/conf/%s/rp_filter", iface);
    read_file(path, per, sizeof(per));
    char *all_value = trim(all), *per_value = trim(per);
    if (!strcmp(all_value, "1") || !strcmp(per_value, "1"))
        log_msg("dhcp client: rp_filter enabled (all=%s %s=%s); sysctls left unchanged",
                all_value, iface, per_value);
}

static void w2_read_bssid(char *out, size_t n)
{
    if (out && n) out[0] = 0;
    char buf[2048];
    if (sh_out(buf, sizeof(buf),
               "wpa_cli -p " WPA_CTRL_DIR " -i wlan0 status 2>/dev/null") != 0)
        return;
    char *p = strstr(buf, "bssid=");
    if (!p) return;
    p += 6;
    size_t i = 0;
    while (p[i] && p[i] != '\n' && p[i] != '\r' && i + 1 < n) {
        out[i] = p[i];
        i++;
    }
    out[i] = 0;
}

/* One-shot DHCP client on the given interface. Returns the leased IPv4
 * address and applies it only after verifying the address and connected
 * route readback. The default/modem route is deliberately untouched. */
static size_t w2_dhcp_build_client(struct w2_dhcp_packet *req, uint32_t xid,
                                   const uint8_t mac[6], uint8_t type,
                                   uint32_t ciaddr, uint32_t requested,
                                   uint32_t server, bool have_server)
{
    memset(req, 0, sizeof(*req));
    req->op = 1;
    req->htype = 1;
    req->hlen = 6;
    req->xid = xid;
    req->ciaddr = htonl(ciaddr);
    memcpy(req->chaddr, mac, 6);
    req->flags = htons(0x8000);
    req->options[0] = 99; req->options[1] = 130;
    req->options[2] = 83; req->options[3] = 99;
    size_t off = 4;
    off += w2_dhcp_client_opts(req->options + off,
                               sizeof(req->options) - off, type, mac,
                               requested, server, have_server);
    req->options[off++] = 255;
    return offsetof(struct w2_dhcp_packet, options) + off;
}

static bool w2_dhcp_reply_identity(const struct w2_dhcp_packet *rep, size_t n,
                                   uint32_t xid, const uint8_t mac[6])
{
    return n >= offsetof(struct w2_dhcp_packet, options) + 4 &&
           rep->op == 2 && rep->htype == 1 && rep->hlen == 6 &&
           rep->xid == xid && !memcmp(rep->chaddr, mac, 6) &&
           rep->options[0] == 99 && rep->options[1] == 130 &&
           rep->options[2] == 83 && rep->options[3] == 99;
}

static void w2_dhcp_remove_own_address(const char *iface)
{
    char ip[32];
    if (g_dhcp_lease.ip == 0 || g_dhcp_lease.prefix <= 0 ||
        g_dhcp_lease.prefix > 32 || g_dhcp_lease.iface[0] == '\0' ||
        strcmp(iface, g_dhcp_lease.iface) != 0)
        return;
    w2_ip4_str(g_dhcp_lease.ip, ip, sizeof(ip));
    /* Exact owned address only; never a family flush. */
    w2_ip_command("addr del %s/%d dev %s", ip, g_dhcp_lease.prefix, iface);
}

static void w2_dhcp_invalidate(const char *iface, bool remove_address)
{
    g_dhcp_lease.valid = false;
    g_dhcp_lease.updated = true;
    g_dhcp_lease.route_ok = false;
    g_dhcp_lease.wifi_default = false;
    if (!w2_owned_route_cleanup())
        log_msg("dhcp client: owned default route cleanup failed");
    if (remove_address) w2_dhcp_remove_own_address(iface);
}

/* Apply an ACK and verify every claim before the lease is considered ready.
 * The gateway/metric come from the ACK and current route table, never from
 * configuration. */
static bool w2_dhcp_apply_ack(const struct w2_dhcp_packet *rep, size_t n,
                              const char *iface, uint32_t fallback_ip,
                              uint32_t fallback_server, char *out_ip,
                              size_t ip_n)
{
    uint32_t ip = ntohl(rep->yiaddr);
    uint32_t mask = 0, router = 0, server_id = 0;
    uint32_t lease_s = 3600, t1_s = 0, t2_s = 0;
    char ipbuf[32];

    if (ip == 0) ip = fallback_ip;
    if (ip == 0) {
        snprintf(g_dhcp_error, sizeof(g_dhcp_error), "ACK carried no usable address");
        return false;
    }
    if (!w2_dhcp_opt_u32(rep, n, 1, &mask) || mask == 0) mask = 0xffffff00U;
    int prefix = w2_mask_to_prefix(mask);
    if (prefix < 0 || prefix > 32) {
        snprintf(g_dhcp_error, sizeof(g_dhcp_error), "invalid subnet mask from DHCP server");
        return false;
    }
    if (!w2_dhcp_opt_u32(rep, n, 3, &router) || router == 0)
        router = fallback_server;
    if (!w2_dhcp_opt_u32(rep, n, 54, &server_id) || server_id == 0)
        server_id = fallback_server;
    if (!w2_dhcp_opt_u32(rep, n, 51, &lease_s) || lease_s == 0)
        lease_s = 3600;
    if (lease_s < 60) lease_s = 60;
    if (lease_s > 7U * 86400U) lease_s = 7U * 86400U;
    if (!w2_dhcp_opt_u32(rep, n, 58, &t1_s)) t1_s = 0;
    if (!w2_dhcp_opt_u32(rep, n, 59, &t2_s)) t2_s = 0;
    if (t1_s == 0 || t1_s >= lease_s) t1_s = lease_s / 2;
    if (t2_s == 0 || t2_s <= t1_s || t2_s >= lease_s) t2_s = lease_s * 7U / 8U;
    if (t1_s == 0) t1_s = 1;
    if (t2_s <= t1_s) t2_s = lease_s > t1_s + 1 ? lease_s - 1 : t1_s + 1;

    w2_ip4_str(ip, ipbuf, sizeof(ipbuf));
    if (!getenv("WIFI1B_DHCP_TEST_NOAPPLY")) {
        if (g_dhcp_lease.ip &&
            (g_dhcp_lease.ip != ip || g_dhcp_lease.prefix != prefix) &&
            !strcmp(g_dhcp_lease.iface, iface))
            w2_dhcp_remove_own_address(iface);
        if (!w2_ip_command("addr replace %s/%d dev %s", ipbuf, prefix, iface) &&
            !w2_ip_command("addr add %s/%d dev %s", ipbuf, prefix, iface)) {
            snprintf(g_dhcp_error, sizeof(g_dhcp_error),
                     "cannot apply %s/%d on %s", ipbuf, prefix, iface);
            return false;
        }
        if (!w2_ip_command("link set %s up", iface)) {
            snprintf(g_dhcp_error, sizeof(g_dhcp_error),
                     "cannot bring %s up", iface);
            return false;
        }
        if (!w2_addr_present(iface, ipbuf, prefix)) {
            snprintf(g_dhcp_error, sizeof(g_dhcp_error),
                     "address %s/%d is not visible on %s", ipbuf, prefix, iface);
            return false;
        }
        if (!w2_connected_route_present(iface, ipbuf, mask, prefix)) {
            snprintf(g_dhcp_error, sizeof(g_dhcp_error),
                     "connected route for %s/%d is not visible on %s",
                     ipbuf, prefix, iface);
            return false;
        }
        if (!w2_client_route_install(router, iface)) return false;
    } else {
        g_dhcp_lease.wifi_default = false;
    }

    g_dhcp_lease.valid = true;
    g_dhcp_lease.updated = true;
    g_dhcp_lease.route_ok = !getenv("WIFI1B_DHCP_TEST_NOAPPLY") ||
                            g_dhcp_lease.route_ok;
    g_dhcp_lease.ip = ip;
    g_dhcp_lease.server = server_id;
    g_dhcp_lease.mask = mask;
    g_dhcp_lease.gateway = router;
    g_dhcp_lease.prefix = prefix;
    g_dhcp_lease.lease_s = (int)lease_s;
    g_dhcp_lease.t1_s = (int)t1_s;
    g_dhcp_lease.t2_s = (int)t2_s;
    str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), iface);
    if (out_ip && ip_n) str_copy(out_ip, ip_n, ipbuf);
    return true;
}

static bool w2_dhcp_client_run(w2_dhcp_step_t step, const char *iface_in,
                               char *out_ip, size_t ip_n)
{
    const char *iface = iface_in;
    {
        const char *t = getenv("WIFI1B_DHCP_TEST_IFACE");
        if (t && *t) iface = t;
    }
    if (out_ip && ip_n) out_ip[0] = 0;
    if (step == W2_DHCP_ACQUIRE) {
        g_client_modem_ipv4 = false;
        /* Re-acquisition replaces only our old exact address/route. */
        if (!w2_owned_route_cleanup()) {
            snprintf(g_dhcp_error, sizeof(g_dhcp_error),
                     "cannot remove previous owned default route");
            w2_dhcp_remove_own_address(iface);
            g_dhcp_lease.valid = false;
            g_dhcp_lease.route_ok = false;
            g_dhcp_lease.wifi_default = false;
            g_dhcp_lease.updated = true;
            return false;
        }
        w2_dhcp_remove_own_address(iface);
        g_dhcp_lease.valid = false;
        g_dhcp_lease.route_ok = false;
        g_dhcp_lease.wifi_default = false;
    } else if (!g_dhcp_lease.valid || g_dhcp_lease.ip == 0) {
        snprintf(g_dhcp_error, sizeof(g_dhcp_error), "no active lease to renew");
        return false;
    }
    g_dhcp_lease.updated = step == W2_DHCP_ACQUIRE;

    uint8_t mac[6];
    if (!w2_read_iface_mac(iface, mac)) {
        set_msg("client: cannot read %s MAC", iface);
        return false;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    if (!getenv("WIFI1B_DHCP_TEST_LOOPBACK") &&
        setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, iface, strlen(iface)) != 0)
        log_msg("dhcp client: SO_BINDTODEVICE %s failed: %s", iface, strerror(errno));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(w2_dhcp_client_port());
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        log_msg("dhcp client: bind udp/%d failed: %s", w2_dhcp_client_port(), strerror(errno));
        close(fd);
        return false;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    w2_dhcp_rp_filter_note(iface);
    int raw_ifindex = 0;
    int raw_fd = w2_dhcp_raw_open(iface, &raw_ifindex);
    bool force_raw = getenv("WIFI1B_DHCP_TEST_FORCE_RAW") != NULL;
    if (raw_fd < 0)
        log_msg("dhcp client: AF_PACKET fallback unavailable: %s", strerror(errno));
    else
        log_msg("dhcp client: AF_PACKET fallback active on %s", iface);
    if (force_raw && raw_fd < 0) {
        snprintf(g_dhcp_error, sizeof(g_dhcp_error),
                 "raw DHCP fallback requested but unavailable");
        close(fd);
        return false;
    }

    bool ok = false;
    struct w2_dhcp_packet req;
    uint8_t reply[1500];
    size_t reply_len = 0;
    unsigned int seed = (unsigned int)(now_ms() ^ getpid());
    uint32_t xid = (seed * 1103515245u + 12345u) & 0x7fffffffu;
    g_dhcp_tx = g_dhcp_rx_offer = g_dhcp_rx_ack = 0;
    g_dhcp_attempts = 0;
    g_dhcp_bssid[0] = 0;
    g_dhcp_error[0] = 0;

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(w2_dhcp_port());
    if (getenv("WIFI1B_DHCP_TEST_LOOPBACK"))
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    else
        to.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    uint32_t offered = 0, server = 0;
    size_t reqlen = 0;
    if (step == W2_DHCP_ACQUIRE) {
        reqlen = w2_dhcp_build_client(&req, xid, mac, 1, 0, 0, 0, false);
        for (int attempt = 0; attempt < 4 && !offered && !g_stop; attempt++) {
            g_dhcp_attempts = attempt + 1;
            w2_read_bssid(g_dhcp_bssid, sizeof(g_dhcp_bssid));
            if (sendto(fd, &req, reqlen, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
                snprintf(g_dhcp_error, sizeof(g_dhcp_error), "sendto: %s", strerror(errno));
                break;
            }
            g_dhcp_tx++;
            int got = w2_dhcp_wait_reply(fd, raw_fd, raw_ifindex, xid, mac,
                                         reply, sizeof(reply), &reply_len,
                                         now_ms() + 3000);
            if (got > 0) {
                struct w2_dhcp_packet *rep = (struct w2_dhcp_packet *)reply;
                if (w2_dhcp_reply_identity(rep, reply_len, xid, mac) &&
                    w2_dhcp_msgtype(rep, reply_len) == 2) {
                    offered = ntohl(rep->yiaddr);
                    uint32_t sid = 0;
                    w2_dhcp_server_id(rep, reply_len, &sid);
                    server = sid;
                    g_dhcp_rx_offer++;
                    char ipbuf[32], srvbuf[32];
                    w2_ip4_str(offered, ipbuf, sizeof(ipbuf));
                    w2_ip4_str(server, srvbuf, sizeof(srvbuf));
                    log_msg("dhcp client: OFFER %s from %s (attempt %d, bssid %s, raw=%d)",
                            ipbuf, srvbuf, g_dhcp_attempts, g_dhcp_bssid, got == 2);
                }
            }
            if (!offered && !client_associated()) {
                snprintf(g_dhcp_error, sizeof(g_dhcp_error),
                         "link lost after DISCOVER attempt %d (bssid %s)",
                         g_dhcp_attempts, g_dhcp_bssid);
                break;
            }
        }
        if (!offered) {
            if (!g_dhcp_error[0])
                snprintf(g_dhcp_error, sizeof(g_dhcp_error),
                         "no OFFER after %d attempts (bssid %s)",
                         g_dhcp_attempts, g_dhcp_bssid);
            w2_dhcp_invalidate(iface, true);
            goto done;
        }
        reqlen = w2_dhcp_build_client(&req, xid, mac, 3, 0, offered,
                                      server, server != 0);
    } else {
        uint32_t dest = getenv("WIFI1B_DHCP_TEST_LOOPBACK")
            ? INADDR_LOOPBACK : INADDR_BROADCAST;
        if (step == W2_DHCP_RENEW && g_dhcp_lease.server)
            dest = g_dhcp_lease.server;
        to.sin_addr.s_addr = htonl(dest);
        /* RENEW/REBIND: ciaddr set, same client ID, no requested-IP option. */
        reqlen = w2_dhcp_build_client(&req, xid, mac, 3, g_dhcp_lease.ip,
                                      0, 0, false);
    }

    for (int attempt = 0; attempt < 3 && !ok && !g_stop; attempt++) {
        if (sendto(fd, &req, reqlen, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
            snprintf(g_dhcp_error, sizeof(g_dhcp_error), "REQUEST sendto: %s", strerror(errno));
            break;
        }
        g_dhcp_tx++;
        int got = w2_dhcp_wait_reply(fd, raw_fd, raw_ifindex, xid, mac,
                                     reply, sizeof(reply), &reply_len,
                                     now_ms() + 3000);
        if (got > 0) {
            struct w2_dhcp_packet *rep = (struct w2_dhcp_packet *)reply;
            if (w2_dhcp_reply_identity(rep, reply_len, xid, mac)) {
                uint8_t t = w2_dhcp_msgtype(rep, reply_len);
                if (t == 5) {
                    g_dhcp_rx_ack++;
                    uint32_t fallback_ip = step == W2_DHCP_ACQUIRE
                        ? offered : g_dhcp_lease.ip;
                    uint32_t fallback_server = step == W2_DHCP_ACQUIRE
                        ? server : g_dhcp_lease.server;
                    if (w2_dhcp_apply_ack(rep, reply_len, iface, fallback_ip,
                                          fallback_server, out_ip, ip_n)) {
                        ok = true;
                        break;
                    }
                    w2_dhcp_invalidate(iface, true);
                    break;
                }
                if (t == 6) {
                    snprintf(g_dhcp_error, sizeof(g_dhcp_error),
                             "NAK from server (bssid %s)", g_dhcp_bssid);
                    w2_dhcp_invalidate(iface, true);
                    break;
                }
            }
        }
        if (!ok && !client_associated()) {
            snprintf(g_dhcp_error, sizeof(g_dhcp_error),
                     "link lost after REQUEST attempt %d (bssid %s)",
                     attempt + 1, g_dhcp_bssid);
            break;
        }
    }
    if (!ok && !g_dhcp_error[0])
        snprintf(g_dhcp_error, sizeof(g_dhcp_error),
                 "no ACK after 3 attempts (bssid %s)", g_dhcp_bssid);
    if (!ok && step == W2_DHCP_ACQUIRE)
        w2_dhcp_invalidate(iface, true);

done:
    if (raw_fd >= 0) close(raw_fd);
    close(fd);
    return ok;
}

static bool w2_dhcp_client(const char *iface_in, char *out_ip, size_t ip_n)
{
    return w2_dhcp_client_run(W2_DHCP_ACQUIRE, iface_in, out_ip, ip_n);
}

static bool w2_dhcp_client_renew(w2_dhcp_step_t step)
{
    const char *iface = g_dhcp_lease.iface[0] ? g_dhcp_lease.iface : "wlan0";
    return w2_dhcp_client_run(step, iface, NULL, 0);
}

static bool client_disconnect(void)
{
    if (!w2_owned_route_cleanup())
        log_msg("client: owned default route cleanup failed during disconnect");
    w2_dhcp_remove_own_address("wlan0");
    g_dhcp_lease.valid = false;
    g_dhcp_lease.route_ok = false;
    g_dhcp_lease.wifi_default = false;
    g_dhcp_lease.updated = true;
    if (wpa_process_alive()) {
        sh("wpa_cli -p " WPA_CTRL_DIR " -i wlan0 disconnect 2>/dev/null");
        sh("wpa_cli -p " WPA_CTRL_DIR " -i wlan0 remove_network all 2>/dev/null");
    }
    w2_iface_flush();
    return true;
}

static bool client_service_ready(const char *service, const char *ip)
{
    (void)service;
    char current[64];
    return w2_read_wlan_ip(current, sizeof(current)) && !strcmp(current, ip);
}

static bool client_wait_ip(const char *service, int attempts)
{
    (void)service;
    for (int i = 0; i < attempts && !g_stop; i++) {
        char ip[64];
        if (client_associated() && w2_read_wlan_ip(ip, sizeof(ip))) {
            set_msg("client connected to '%s' (%s)", g_cfg.ssid, ip);
            return true;
        }
        if ((i % 4) == 3) write_status();
        usleep(500000);
    }
    return false;
}

static bool client_connect(void)
{
    g_client_legacy_routes_removed = 0;
    if (!g_cfg.ssid[0]) {
        set_msg("no client SSID imported (use wifi.txt or iphone_wifi.txt)");
        return false;
    }
    size_t keylen = strlen(g_cfg.psk);
    if (keylen && (keylen < 8 || keylen > 64 ||
        (keylen == 64 && strspn(g_cfg.psk, "0123456789abcdefABCDEF") != 64))) {
        set_msg("client: WPA key must be 8-63 characters or 64 hexadecimal digits");
        return false;
    }
    /* A new association must not inherit a stale default route/address. */
    if (!w2_owned_route_cleanup()) {
        set_msg("client: previous owned default route could not be removed");
        g_dhcp_lease.valid = false;
        g_dhcp_lease.route_ok = false;
        g_dhcp_lease.wifi_default = false;
        g_dhcp_lease.updated = true;
        return false;
    }
    if (!w2_legacy_wlan_default_cleanup()) {
        set_msg("client: stale wlan0 default could not be removed; refusing to disrupt the modem route");
        g_dhcp_lease.valid = false;
        g_dhcp_lease.route_ok = false;
        g_dhcp_lease.wifi_default = false;
        g_dhcp_lease.updated = true;
        return false;
    }
    w2_dhcp_remove_own_address("wlan0");
    g_dhcp_lease.valid = false;
    g_dhcp_lease.route_ok = false;
    g_dhcp_lease.wifi_default = false;
    g_dhcp_lease.updated = true;
    if (!ensure_wifi_ready()) return false;
    ap_stop();
    stop_wpa_supplicant();
    iface_up();
    /* The stock INPUT policy is DROP and the DHCP OFFER is a new broadcast,
     * not an established flow.  Open UDP/68 on wlan0 for the client-mode
     * lease; the AP chain owns UDP/67 in AP mode. */
    if (!fw_port_add("udp", 68))
        log_msg("client: DHCP client firewall rule failed (nonfatal)");

    bool sae = false;
    for (int attempt = 0; attempt < 2 && !g_stop; attempt++) {
        if (!write_station_conf(g_cfg.ssid, g_cfg.psk, sae)) return false;
        if (!start_wpa_supplicant()) {
            set_msg("client: supplicant failed to start");
            break;
        }
        for (int i = 0; i < 40 && !g_stop; i++) {
            if (client_associated()) break;
            if (!wpa_process_alive()) break;
            usleep(500000);
        }
        if (client_associated()) {
            char ip[64];
            if (!w2_dhcp_client("wlan0", ip, sizeof(ip))) {
                set_msg("client: DHCP failed: %s",
                        g_dhcp_error[0] ? g_dhcp_error : "unknown");
                return false;
            }
            char cur[64];
            if (w2_read_wlan_ip(cur, sizeof(cur)) && !strcmp(cur, ip)) {
                set_msg("client connected to '%s' (%s)", g_cfg.ssid, ip);
                return true;
            }
            if (client_wait_ip(NULL, 20)) return true;
            set_msg("client: DHCP leased %s but the address is not visible yet", ip);
            return false;
        }
        stop_wpa_supplicant();
        if (!sae && keylen) {
            sae = true;
            log_msg("client: WPA2 association failed; retrying with SAE");
            continue;
        }
        break;
    }
    set_msg("client: could not associate to '%s' (WPA2/SAE attempted)", g_cfg.ssid);
    return false;
}

/* ------------------------------------------------------------------ */
/* SSH                                                                 */
/* ------------------------------------------------------------------ */

#ifndef WIFI1B_USB0_SYSFS
#define WIFI1B_USB0_SYSFS "/sys/class/net/usb0"
#endif

/* The Orange Pi is an ECM gadget: the head unit sees usb0 and the daemon's
 * runtime udev rule gives it 10.55.0.2/24 with no default route.  That link
 * is the out-of-band SSH path, independent of WiFi state. */
static bool usb0_present(void)
{
    return file_exists(WIFI1B_USB0_SYSFS);
}

static bool ssh_wifi_active(void)
{
    return g_cfg.wlan_on && g_cfg.mode != MODE_OFF;
}

static bool ssh_transport_available(void)
{
    return ssh_wifi_active() || usb0_present();
}

static bool usb0_fw_open(int port)
{
    if (!usb0_present() || port <= 0) return true;
#ifdef WIFI_TEST
    g_usb0_fw_port = port;
    return true;
#endif
    char tool[256];
    fw_tool(tool, sizeof(tool));
    /* One owned chain means a port change or daemon restart cannot leave an
     * old ACCEPT behind.  The chain is hooked only on usb0. */
    sh("%s -N WIFI1B_USB0_SSH 2>/dev/null", tool);
    if (sh("%s -C INPUT -i %s -j WIFI1B_USB0_SSH 2>/dev/null",
           tool, WIFI1B_USB0_IFACE) != 0 &&
        sh("%s -I INPUT -i %s -j WIFI1B_USB0_SSH 2>/dev/null",
           tool, WIFI1B_USB0_IFACE) != 0)
        return false;
    if (g_usb0_fw_port == port &&
        sh("%s -C WIFI1B_USB0_SSH -p tcp --dport %d -j ACCEPT 2>/dev/null",
           tool, port) == 0) {
        return true;
    }
    sh("%s -F WIFI1B_USB0_SSH 2>/dev/null", tool);
    if (sh("%s -A WIFI1B_USB0_SSH -p tcp --dport %d -j ACCEPT 2>/dev/null",
           tool, port) != 0)
        return false;
    g_usb0_fw_port = port;
    return true;
}

static void usb0_fw_close(int port)
{
    (void)port;
#ifdef WIFI_TEST
    g_usb0_fw_port = 0;
    return;
#endif
    char tool[256];
    fw_tool(tool, sizeof(tool));
    sh("%s -F WIFI1B_USB0_SSH 2>/dev/null", tool);
    g_usb0_fw_port = 0;
}

static bool ensure_car_user(void)
{
#ifdef WIFI_TEST
    return true;
#endif
    bool ok = true;
    if (!ensure_line("/etc/passwd", "wifi1b:", WIFI1B_PASSWD_LINE)) ok = false;
    const char *pw_templates[] = {"/etc/passwd.legacy", "/etc/passwd.secure", NULL};
    for (int i = 0; pw_templates[i]; i++)
        if (file_exists(pw_templates[i]))
            (void)ensure_line(pw_templates[i], "wifi1b:", WIFI1B_PASSWD_LINE);
    const char *shadow = file_exists("/etc/shadow.Global") ? "/etc/shadow.Global" : "/etc/shadow";
    if (!ensure_line(shadow, "wifi1b:", WIFI1B_SHADOW_LINE)) ok = false;
    if (strcmp(shadow, "/etc/shadow") && file_exists("/etc/shadow"))
        (void)ensure_line("/etc/shadow", "wifi1b:", WIFI1B_SHADOW_LINE);
    if (file_exists("/tmp/shells"))
        (void)ensure_line("/tmp/shells", "/bin/sh", "/bin/sh");
    mkdir(SSH_HOME, 0700);
    mkdir(SSH_HOME "/.ssh", 0700);
    chmod(SSH_HOME, 0700);
    chmod(SSH_HOME "/.ssh", 0700);
    sh("chsmack -a _ " SSH_HOME " " SSH_HOME "/.ssh 2>/dev/null");
    if (!ok) log_msg("ensure_car_user: could not persist wifi1b user");
    else log_msg("ensure_car_user: wifi1b user present");
    return ok;
}

static bool ssh_start(void)
{
    if (!g_cfg.ssh_enabled || g_stop || g_shutdown_cleanup) {
        ssh_stop();
        return false;
    }
    /* WiFi off is not SSH off when the Orange Pi USB link is present: the
     * Pi's usb0 path is the out-of-band management channel. */
    if (!ssh_transport_available()) {
        ssh_stop();
        return true; /* deferred until a transport appears */
    }
    if (!file_exists(DROPBEAR_BIN) && !file_exists(DROPBEAR_MULTI)) {
        set_msg("dropbear binary missing");
        return false;
    }
    ensure_car_user();
    if (ssh_running() && g_ssh_active_port) {
        if (g_ssh_bound_ip[0]) {
            /* Dropbear may have outlived a firewall flush or a daemon
             * restart.  Never report SSH ready while its rule is missing. */
            if (ssh_wifi_active()) {
                g_ssh_fw_open = fw_open(g_ssh_active_port);
                if (!g_ssh_fw_open)
                    log_msg("SSH: firewall rule missing while SSH is running");
            } else {
                g_ssh_fw_open = false;
            }
            if (!usb0_fw_open(g_ssh_active_port))
                log_msg("SSH: usb0 firewall rule missing while SSH is running");
            return true;
        }
        ssh_stop();
    }
    int chosen = choose_ssh_port(g_cfg.ssh_port);
    if (!chosen) {
        set_msg("SSH failed: no free port");
        return false;
    }
    if (chosen != g_cfg.ssh_port) {
        set_msg("SSH port %d busy; using %d", g_cfg.ssh_port, chosen);
        g_cfg.ssh_port = chosen;
        g_ssh_port_fallback = true;
        config_save();
    }
    mkdir(SSH_DIR, 0700);
    mkdir(SSH_HOME, 0700);
    mkdir(SSH_HOME "/.ssh", 0700);
    char eff[512];
    snprintf(eff, sizeof(eff), SSH_HOME "/.ssh/authorized_keys");
    if (file_exists(SSH_KEYS)) {
        char keys[16384];
        if (read_file(SSH_KEYS, keys, sizeof(keys)) <= 0) {
            set_msg("SSH not started: imported keys are empty or unreadable");
            return false;
        }
        if (write_file_atomic(eff, keys, 0600) != 0) {
            set_msg("SSH not started: effective key replacement failed");
            return false;
        }
        chmod(eff, 0600);
        sh("chsmack -a _ %s 2>/dev/null", eff);
    } else {
        unlink(eff);
    }
    int fw_ok = 1;
    if (ssh_wifi_active()) {
        fw_ok = fw_open(chosen);
        g_ssh_fw_open = fw_ok != 0;
        if (!fw_ok)
            log_msg("SSH: owned firewall rule failed (nonfatal); dropbear will still bind");
    } else {
        g_ssh_fw_open = false;
    }
    usb0_rule_write(chosen);
    if (usb0_present() && !usb0_fw_open(chosen))
        log_msg("SSH: usb0 firewall rule failed (nonfatal); dropbear will still bind");

    const char *bin = file_exists(DROPBEAR_BIN) ? DROPBEAR_BIN : DROPBEAR_MULTI;
    const char *keygen = file_exists(DROPBEAR_KEY) ? DROPBEAR_KEY : NULL;
    char edkey[512], rsakey[512];
    snprintf(edkey, sizeof(edkey), SSH_DIR "/dropbear_ed25519_host_key");
    snprintf(rsakey, sizeof(rsakey), SSH_DIR "/dropbear_rsa_host_key");
    if (keygen) {
        if (!file_exists(edkey)) sh("%s -t ed25519 -f %s >> " APP_DIR "/ssh.log 2>&1", keygen, edkey);
        if (!file_exists(rsakey)) sh("%s -t rsa -s 2048 -f %s >> " APP_DIR "/ssh.log 2>&1", keygen, rsakey);
    }
    cap_log_file(APP_DIR "/ssh.log", 1024 * 1024);
    if (file_exists(edkey) && file_exists(rsakey)) {
        sh("%s -r %s -r %s -s -p %d -P %s >> " APP_DIR "/ssh.log 2>&1 &",
           bin, edkey, rsakey, g_cfg.ssh_port, SSH_PID);
    } else if (file_exists(edkey) || file_exists(rsakey)) {
        const char *one = file_exists(edkey) ? edkey : rsakey;
        sh("%s -r %s -s -p %d -P %s >> " APP_DIR "/ssh.log 2>&1 &",
           bin, one, g_cfg.ssh_port, SSH_PID);
    } else {
        sh("%s -R -s -p %d -P %s >> " APP_DIR "/ssh.log 2>&1 &",
           bin, g_cfg.ssh_port, SSH_PID);
    }
    usleep(500000);
    if (ssh_running()) {
        g_ssh_active_port = g_cfg.ssh_port;
        str_copy(g_ssh_bound_ip, sizeof(g_ssh_bound_ip), "0.0.0.0");
        write_file_atomic(SSH_BOUND, "0.0.0.0", 0600);
        set_msg("SSH started on port %d (user wifi1b, key-only)", g_cfg.ssh_port);
        return true;
    }
    fw_close(g_cfg.ssh_port);
    usb0_fw_close(g_cfg.ssh_port);
    usb0_rule_write(0);
    g_ssh_fw_open = false;
    set_msg("SSH failed to start on port %d", g_cfg.ssh_port);
    return false;
}

/* ------------------------------------------------------------------ */
/* CarPlay identity hooks                                              */
/* ------------------------------------------------------------------ */

static bool carplay_ie_on(void)
{
    prepare_carplay_identity();
    /* The AP config carries app_info/carplay_name/carplay_id.  The vendor
     * supplicant builds the Apple IE from those fields. */
    g_carplay_ie = true;
    set_msg("CarPlay IE fields present in AP config (id=%s)", g_cfg.cp_id);
    return true;
}

static void carplay_ie_off(void)
{
    g_carplay_ie = false;
}

/* ------------------------------------------------------------------ */
/* mode application                                                    */
/* ------------------------------------------------------------------ */

static void intent_write(bool active)
{
    mkdir("/run/wifi1b", 0755);
    if (active)
        write_file_atomic(WIFI1B_INTENT, mode_name(), 0600);
    else
        unlink(WIFI1B_INTENT);
}

static void stop_all(void)
{
    g_carplay_ie = false;
    ssh_stop();
    ap_stop();
    client_disconnect();
    stop_wpa_supplicant();
    w2_iface_down_clean();
}

/* A failed WiFi apply must not take the Orange Pi out-of-band SSH path with
 * it.  Keep usb0 SSH alive when it is present; otherwise stop WiFi SSH. */
static void ssh_after_wifi_failure(void)
{
    char saved[256];
    str_copy(saved, sizeof(saved), g_last_msg);
    if (g_cfg.ssh_enabled && usb0_present()) {
        if (!ssh_start())
            log_msg("SSH: usb0 recovery after WiFi failure failed");
    } else {
        ssh_stop();
    }
    /* Keep the WiFi failure reason visible; SSH start/stop has its own log. */
    str_copy(g_last_msg, sizeof(g_last_msg), saved);
}

/* Stock USB handoff is a mutation of the OEM WiFi manager. Only do it while
 * we hold the shared radio reservation, or when the record proves the radio
 * is idle. A peer-owned wlan0 is never evicted by a main-thread preference
 * change. */
static bool carplay_stock_handoff(bool owned)
{
    char owner[32] = "";

    if (!owned && radio_record_name(owner, sizeof(owner)) &&
        strcmp(owner, "wifi1b_daemon")) {
        log_msg("CarPlay stock handoff deferred: wlan0 reserved by %s", owner);
        return false;
    }
    int unmask_rc = sh("systemctl unmask supplicantmanager.path supplicantmanager.service 2>/dev/null");
    int start_rc = sh("systemctl start supplicantmanager.path 2>/dev/null");
    bool ok = start_rc == 0;
    if (unmask_rc != 0)
        log_msg("CarPlay stock handoff: supplicantmanager unmask returned %d", unmask_rc);
    log_msg("CarPlay stock handoff: OEM supplicant manager %s%s",
            ok ? "restored" : "could not be confirmed",
            owned ? "" : " (radio idle)");
    return ok;
}

static bool apply_mode_work(bool pair)
{
    /* Stock USB handoff is authoritative: never start the wireless AP. */
    if (g_cfg.carplay_control && g_cfg.mode == MODE_CARPLAY) {
        g_cfg.mode = MODE_OFF;
        g_cfg.wlan_on = false;
    }
    bool active = g_cfg.wlan_on && g_cfg.mode != MODE_OFF;
    int ownership = radio_begin("wifi1b_daemon", active);
    if (ownership <= 0) {
        /* No reservation means no wlan0 mutation. Stock handoff is only safe
         * when the owner record is empty (or ours, but radio_begin would
         * then have returned 1). */
        if (ownership == 0 && !active && g_cfg.carplay_control &&
            !wpa_process_alive() && !ssh_running() && !dhcpd_running())
            carplay_stock_handoff(false);
        if (ownership == 0 && !active) {
            /* No reservation means no interface mutation, but removing our
             * exact ledger route is safe and prevents a stale fallback. */
            (void)w2_owned_route_cleanup();
            g_dhcp_lease.valid = false;
            g_dhcp_lease.route_ok = false;
            g_dhcp_lease.wifi_default = false;
            g_dhcp_lease.updated = true;
        }
        set_msg("%s", ownership == 0 ? "WLAN off; radio left untouched" :
                "Radio busy/reserved; turn the owning WiFi app Off first");
        return ownership == 0;
    }
    if (!g_cfg.wlan_on || g_cfg.mode == MODE_OFF) {
        /* Reached only with our reservation, including interrupted scans.
         * The earlier ownership guard keeps idle/peer-Off hands-off. */
        stop_all();
        /* stop_all() intentionally tears down WiFi-owned SSH.  The Pi USB
         * link is a separate transport and must stay usable with WiFi Off.
         * Do not start it while the daemon is shutting down. */
        if (!g_stop && !g_shutdown_cleanup && g_cfg.ssh_enabled && usb0_present())
            ssh_start();
        bt_pair_stop();
        /* Wireless CarPlay is not active: clear the vendor flag so the stock
         * USB CarPlay path is not left disabled by a previous session. */
        bool vbt_off = false;
        vbt_set_wireless_carplay(false, &vbt_off);
        restart_vbtd_if_needed(vbt_off);
        ap_probe_t p;
        ap_probe(&p);
        char flags[64] = "", *end = NULL;
        bool down = !iface_present();
        if (!down && read_file("/sys/class/net/wlan0/flags", flags, sizeof(flags)) > 0) {
            unsigned long value = strtoul(flags, &end, 0);
            down = end != flags && !(*trim(end)) && !(value & 1); /* IFF_UP */
        }
        bool usb_ssh = g_cfg.ssh_enabled && usb0_present() && ssh_running();
        bool off = down && !wpa_process_alive() &&
                   (!ssh_running() || usb_ssh) && !dhcpd_running();
        bool handoff_ok = true;
        if (off && g_cfg.carplay_control)
            handoff_ok = carplay_stock_handoff(true);
        intent_write(false);
        set_msg("%s", off ? (g_cfg.carplay_control && !handoff_ok ?
                             "WLAN off; Stock USB handoff could not be confirmed" :
                             g_cfg.carplay_control ?
                             "WLAN off; Stock USB handoff applied" : "WLAN off") :
                "WLAN-off cleanup could not be confirmed; recovery will retry");
        if (off && usb_ssh)
            set_msg("WLAN off; USB SSH active on %s", WIFI1B_USB0_IP);
        return off && handoff_ok && radio_release();
    }
    /* The shared ownership contract above has spoken: a successful begin
     * means the peer is gone or verifiably Off. The legacy status-file peek
     * must not second-guess it, or a crashed peer's stale status would
     * block a legitimate takeover. Wifi2 is not part of the contract, so
     * its status file is still checked directly before an active start. */
    {
        char path[128], peer_status[8192];
        snprintf(path, sizeof(path), "/rw_data/wifi2/status.json");
        if (read_file(path, peer_status, sizeof(peer_status)) > 0 &&
            (strstr(peer_status, "\"wlan_on\":true") ||
             strstr(peer_status, "\"mode\":\"client\"") ||
             strstr(peer_status, "\"mode\":\"carplay\""))) {
            set_msg("Wifi2 is active; stop it before starting Wifi1b");
            return false;
        }
    }
    intent_write(true);
    if (g_cfg.mode == MODE_CLIENT) {
        bt_pair_stop();
        bool vbt_off = false;
        vbt_set_wireless_carplay(false, &vbt_off);
        restart_vbtd_if_needed(vbt_off);
        if (!ensure_wifi_ready()) {
            (void)w2_owned_route_cleanup();
            g_dhcp_lease.valid = false;
            g_dhcp_lease.route_ok = false;
            g_dhcp_lease.wifi_default = false;
            g_dhcp_lease.updated = true;
            ssh_after_wifi_failure();
            return false;
        }
        if (!client_connect()) {
            char why[256];
            str_copy(why, sizeof(why), g_last_msg);
            (void)w2_owned_route_cleanup();
            g_dhcp_lease.valid = false;
            g_dhcp_lease.route_ok = false;
            g_dhcp_lease.wifi_default = false;
            g_dhcp_lease.updated = true;
            ssh_after_wifi_failure();
            set_msg("client mode failed: %s", why);
            return false;
        }
        if (g_cfg.ssh_enabled) ssh_start();
        else ssh_stop();
        set_msg("client mode: %s", g_cfg.ssid);
        return true;
    }

    /* Leaving client mode: remove only the exact ledged default route. */
    if (!w2_owned_route_cleanup()) {
        g_dhcp_lease.valid = false;
        g_dhcp_lease.route_ok = false;
        g_dhcp_lease.wifi_default = false;
        g_dhcp_lease.updated = true;
        set_msg("client: owned default route cleanup failed; CarPlay not started");
        return false;
    }
    g_dhcp_lease.valid = false;
    g_dhcp_lease.route_ok = false;
    g_dhcp_lease.wifi_default = false;
    g_dhcp_lease.updated = true;

    if (g_ap_restart_requested) {
        ssh_stop();
        if (!ap_stop()) {
            ssh_after_wifi_failure();
            set_msg("CarPlay explicit restart blocked: AP teardown not confirmed");
            return false;
        }
    }
    if (pair) bt_pair_start();
    else if (g_pairing_stop_requested) bt_pair_stop();
    if (!ap_start()) {
        ssh_after_wifi_failure();
        bool vbt_off = false;
        vbt_set_wireless_carplay(false, &vbt_off);
        restart_vbtd_if_needed(vbt_off);
        return false;
    }
    if (!carplay_fw_open()) {
        ap_probe_t failed;
        ap_probe(&failed);
        ap_evidence("firewall-failed", &failed);
        ssh_after_wifi_failure();
        bool vbt_off = false;
        vbt_set_wireless_carplay(false, &vbt_off);
        restart_vbtd_if_needed(vbt_off);
        set_msg("CarPlay AP firewall setup failed; AP not reported ready");
        ap_stop();
        return false;
    }
    bool ie_ok = carplay_ie_on();
    ensure_carplay_services();
    bool vbt_changed = false;
    bool vbt_ok = vbt_set_wireless_carplay(true, &vbt_changed);
    restart_vbtd_if_needed(vbt_changed);
    if (g_cfg.ssh_enabled) ssh_start();
    else ssh_stop();
    ap_probe_t p;
    ap_probe(&p);
    if (p.state != AP_READY) {
        ssh_after_wifi_failure();
        bool vbt_off = false;
        vbt_set_wireless_carplay(false, &vbt_off);
        restart_vbtd_if_needed(vbt_off);
        ap_evidence("lost-during-service-setup", &p);
        set_msg("CarPlay AP lost readiness during service setup");
        return false;
    }
    set_msg("Wifi1b CarPlay AP running (SSID=%s, %d MHz, DHCP %s/24)%s",
            p.ssid, p.frequency, WIFI1B_AP_IP,
            ie_ok ? "" : " - CarPlay IE not configured");
    bool services_ok = sh("systemctl is-active --quiet carplay_iAP2.service 2>/dev/null") == 0 &&
                       sh("systemctl is-active --quiet carplay_mdnsd.service 2>/dev/null") == 0;
    if (!services_ok) log_msg("CarPlay AP up, transport services not ready");
    if (!vbt_ok) log_msg("CarPlay AP up, VBT wireless flag not confirmed");
    return true;
}
