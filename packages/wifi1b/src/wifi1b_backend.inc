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

/* Definitions that appear later in the original daemon. */
static bool client_associated(void);
static const char *mode_name(void);
static void write_status(void);
static bool fw_open(int port);
static void fw_close(int port);
static bool fw_port_add(const char *proto, int port);
static void fw_chain_close(const char *tool, const char *chain);
static void fw_tool(char *out, size_t n);
static bool carplay_fw_open(void);
static void carplay_fw_close(void);
static int choose_ssh_port(int desired);
static bool ssh_running(void);
static void ssh_stop(void);
static bool ensure_car_user(void);
static bool ensure_line(const char *path, const char *prefix, const char *line);
static bool fw_chain_ensure(const char *tool, const char *chain);

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
    if (!w2_ip_command("addr flush dev wlan0")) {
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
            if (g_w2_leases[i].expires_ms <= now)
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
    if (existing) return existing;
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

static uint8_t *w2_dhcp_opt(uint8_t *opts, size_t len, uint8_t code)
{
    size_t i = 0;
    while (i + 1 < len) {
        uint8_t c = opts[i++];
        if (c == 0) continue;
        if (c == 255) return NULL;
        uint8_t l = opts[i++];
        if (i + l > len) return NULL;
        if (c == code) return opts + i;
        i += l;
    }
    return NULL;
}

static uint8_t w2_dhcp_msgtype(const struct w2_dhcp_packet *p, size_t n)
{
    size_t off = offsetof(struct w2_dhcp_packet, options) + 4;
    if (n <= off) return 0;
    uint8_t *v = w2_dhcp_opt((uint8_t *)p->options + 4, n - off, 53);
    return v ? *v : 0;
}

static uint32_t w2_dhcp_requested(const struct w2_dhcp_packet *p, size_t n)
{
    size_t off = offsetof(struct w2_dhcp_packet, options) + 4;
    if (n <= off) return 0;
    uint8_t *v = w2_dhcp_opt((uint8_t *)p->options + 4, n - off, 50);
    if (!v) return 0;
    uint32_t x;
    memcpy(&x, v, 4);
    return ntohl(x);
}

static bool w2_dhcp_server_id(const struct w2_dhcp_packet *p, size_t n, uint32_t *out)
{
    size_t off = offsetof(struct w2_dhcp_packet, options) + 4;
    if (n <= off) return false;
    uint8_t *v = w2_dhcp_opt((uint8_t *)p->options + 4, n - off, 54);
    if (!v) return false;
    uint32_t x;
    memcpy(&x, v, 4);
    *out = ntohl(x);
    return true;
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

static void ap_fw_close(void)
{
    char tool[256];
    fw_tool(tool, sizeof(tool));
    fw_chain_close(tool, WIFI1B_AP_FW_CHAIN);
}

static bool ap_fw_open(void)
{
    char tool[256];
    fw_tool(tool, sizeof(tool));
    if (!fw_chain_ensure(tool, WIFI1B_AP_FW_CHAIN)) return false;
    if (sh("%s -C " WIFI1B_AP_FW_CHAIN " -j ACCEPT 2>/dev/null", tool) != 0 &&
        sh("%s -I " WIFI1B_AP_FW_CHAIN " -j ACCEPT 2>/dev/null", tool) != 0)
        return false;
    return true;
}

static void ap_probe(ap_probe_t *p)
{
    memset(p, 0, sizeof(*p));
    p->sampled_ms = now_ms();
    p->role = -1;
    char buf[4096], iw_ssid[80] = "", wpa_ssid[80] = "";
    int wpa_role = -1;
    bool completed = false;
    if (!iface_present()) p->role = 0;
    else if (sh_out(buf, sizeof(buf), "iw dev wlan0 info 2>/dev/null") == 0) {
        char *save = NULL;
        for (char *line = strtok_r(buf, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            char *s = line;
            while (isspace((unsigned char)*s)) s++;
            if (!strncmp(s, "type ", 5)) {
                const char *type = trim(s + 5);
                p->role = !strcmp(type, "AP") ? 1 : !strcmp(type, "managed") ? 0 : -1;
            }
            if (!strncmp(s, "ssid ", 5) && !decode_control_ssid(iw_ssid, sizeof(iw_ssid), s + 5))
                iw_ssid[0] = 0;
        }
    }
    if (sh_out(buf, sizeof(buf), "wpa_cli -p " WPA_CTRL_DIR " -i wlan0 status 2>/dev/null") == 0) {
        char *save = NULL;
        for (char *line = strtok_r(buf, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            if (!strcmp(line, "mode=AP")) wpa_role = 1;
            else if (!strcmp(line, "mode=station")) wpa_role = 0;
            else if (!strcmp(line, "wpa_state=COMPLETED")) completed = true;
            else if (!strncmp(line, "ssid=", 5) && !decode_control_ssid(wpa_ssid, sizeof(wpa_ssid), line + 5))
                wpa_ssid[0] = 0;
        }
    }
    if (p->role < 0 && sh_out(buf, sizeof(buf), "iwconfig wlan0 2>/dev/null") == 0) {
        p->role = strstr(buf, "Mode:Master") ? 1 : strstr(buf, "Mode:Managed") ? 0 : -1;
        char *ssid = strstr(buf, "ESSID:\"");
        if (p->role == 1 && ssid) {
            ssid += 7;
            char *end = strchr(ssid, '"');
            if (end) {
                *end = 0;
                if (!decode_control_ssid(iw_ssid, sizeof(iw_ssid), ssid)) iw_ssid[0] = 0;
            }
        }
    }
    p->station = wpa_role == 0 && completed;
    if (p->role < 0 && wpa_role == 1) p->role = 1;
    p->conflict = (p->role == 1 && p->station) || (p->role == 0 && wpa_role == 1);
    str_copy(p->configured_ssid, sizeof(p->configured_ssid), g_cfg.cp_ssid);
    if (p->role == 1)
        str_copy(p->ssid, sizeof(p->ssid), iw_ssid[0] ? iw_ssid : wpa_ssid);
    if (wlan_ip(p->ip, sizeof(p->ip))) p->ip[0] = p->ip[0];
    p->tether = p->role;
    p->state = AP_UNKNOWN;
    if (!p->conflict) {
        if (p->role == 0) p->state = AP_ABSENT;
        else if (p->role == 1) {
            p->state = AP_INTERFACE_UP;
            if (!strcmp(p->ssid, g_cfg.cp_ssid) && p->ip[0])
                p->state = AP_READY;
        }
    }
}

static void ap_evidence(const char *stage, const ap_probe_t *p)
{
    char actual[512], configured[512];
    json_escape(actual, sizeof(actual), p->ssid);
    json_escape(configured, sizeof(configured), p->configured_ssid);
    log_msg("AP stage=%s state=%d role=%d actual_ssid=%s configured_ssid=%s ip=%s station=%d conflict=%d",
            stage, p->state, p->role, actual, configured, p->ip, p->station, p->conflict);
}

static ap_wait_t ap_wait_ready(bool seen, ap_probe_t *last)
{
    (void)seen;
    int ready = 0;
    int64_t deadline = now_ms() + 30000;
    for (int i = 0; i < 62 && !g_stop && now_ms() < deadline; i++) {
        ap_probe(last);
        if (last->state == AP_READY) {
            if (++ready >= 3) return AP_WAIT_READY;
        } else ready = 0;
        if (!wpa_process_alive() && i > 4) return AP_WAIT_LOST;
        usleep(500000);
    }
    return g_stop ? AP_WAIT_UNKNOWN : AP_WAIT_PENDING;
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
        if (!start_dhcpd()) log_msg("CarPlay AP: DHCP server did not start");
        if (!ap_fw_open()) log_msg("CarPlay AP: wlan0 firewall accept failed (nonfatal)");
        g_carplay_ie = true;
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
            if (!start_dhcpd()) log_msg("CarPlay AP: DHCP server did not start");
            if (!ap_fw_open()) log_msg("CarPlay AP: wlan0 firewall accept failed (nonfatal)");
            g_carplay_ie = true;
            set_msg("CarPlay AP running (SSID=%s, %d MHz, DHCP %s/24)",
                    p.ssid, channel_to_freq(), WIFI1B_AP_IP);
            return true;
        }
        ap_evidence("start-attempt", &p);
        stop_dhcpd();
        stop_wpa_supplicant();
        w2_iface_flush();
    }
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
    char path[128], buf[64];
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", iface);
    if (read_file(path, buf, sizeof(buf)) <= 0) return false;
    return parse_mac6(trim(buf), out);
}

static uint32_t w2_dhcp_opt_u32(const struct w2_dhcp_packet *p, size_t n, uint8_t code)
{
    size_t off = offsetof(struct w2_dhcp_packet, options) + 4;
    if (n <= off) return 0;
    uint8_t *v = w2_dhcp_opt((uint8_t *)p->options + 4, n - off, code);
    if (!v) return 0;
    if (code == 1) {  /* subnet mask */
        uint32_t x;
        memcpy(&x, v, 4);
        return ntohl(x);
    }
    return 0;
}

static void w2_sysctl_write(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return;
    (void)write(fd, value, strlen(value));
    close(fd);
}

static void w2_rp_filter_restore(const char *all_path, const char *if_path,
                                 const char *old_all, const char *old_if)
{
    char v[16];
    if (old_all && old_all[0]) {
        snprintf(v, sizeof(v), "%s", old_all);
        w2_sysctl_write(all_path, trim(v));
    }
    if (old_if && old_if[0]) {
        snprintf(v, sizeof(v), "%s", old_if);
        w2_sysctl_write(if_path, trim(v));
    }
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

/* One-shot DHCP client on the given interface.  Returns the leased IPv4
 * address and applies it plus the default route via ip(8). */
static bool w2_dhcp_client(const char *iface_in, char *out_ip, size_t ip_n)
{
    const char *iface = iface_in;
    {
        const char *t = getenv("WIFI1B_DHCP_TEST_IFACE");
        if (t && *t) iface = t;
    }
    if (out_ip && ip_n) out_ip[0] = 0;
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

    /* The vendor sets net.ipv4.conf.all.rp_filter=1.  With no route yet,
     * the kernel drops the DHCP OFFER before the INPUT chain, so the
     * firewall rule alone cannot make client mode work.  Relax rp_filter
     * only for the exchange and restore it afterwards. */
    char rp_all[16] = "", rp_if[16] = "", rp_path[128] = "";
    bool rp_saved = false;
    if (!getenv("WIFI1B_DHCP_TEST_IFACE") && !getenv("WIFI1B_DHCP_TEST_LOOPBACK")) {
        read_file("/proc/sys/net/ipv4/conf/all/rp_filter", rp_all, sizeof(rp_all));
        snprintf(rp_path, sizeof(rp_path),
                 "/proc/sys/net/ipv4/conf/%s/rp_filter", iface);
        read_file(rp_path, rp_if, sizeof(rp_if));
        if (rp_all[0])
            w2_sysctl_write("/proc/sys/net/ipv4/conf/all/rp_filter", "0");
        if (rp_if[0])
            w2_sysctl_write(rp_path, "0");
        rp_saved = true;
    }
    bool ok = false;

    struct w2_dhcp_packet req;
    memset(&req, 0, sizeof(req));
    req.op = 1;
    req.htype = 1;
    req.hlen = 6;
    unsigned int seed = (unsigned int)(now_ms() ^ getpid());
    req.xid = (seed * 1103515245u + 12345u) & 0x7fffffffu;
    memcpy(req.chaddr, mac, 6);
    req.flags = htons(0x8000);
    req.options[0] = 99; req.options[1] = 130; req.options[2] = 83; req.options[3] = 99;

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(w2_dhcp_port());
    if (getenv("WIFI1B_DHCP_TEST_LOOPBACK"))
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    else
        to.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    size_t off = 4;
    uint8_t mtype = 1; /* DISCOVER */
    off += w2_dhcp_put(req.options + off, sizeof(req.options) - off, 53, 1, &mtype);
    uint8_t prl[] = {1, 3, 6, 15, 28}; /* mask, router, dns, domain, broadcast */
    off += w2_dhcp_put(req.options + off, sizeof(req.options) - off, 55, sizeof(prl), prl);
    uint8_t cid[7];
    cid[0] = 1; /* Ethernet */
    memcpy(cid + 1, mac, 6);
    off += w2_dhcp_put(req.options + off, sizeof(req.options) - off, 61, 7, cid);
    req.options[off++] = 255;
    size_t reqlen = offsetof(struct w2_dhcp_packet, options) + off;

    uint32_t offered = 0, server = 0;
    g_dhcp_tx = g_dhcp_rx_offer = g_dhcp_rx_ack = 0;
    g_dhcp_attempts = 0;
    g_dhcp_bssid[0] = 0;
    g_dhcp_error[0] = 0;

    /* DISCOVER: retry across roams.  A mesh can move the station to a
     * different BSSID mid-exchange, so one send + a long wait is fragile. */
    for (int attempt = 0; attempt < 4 && !offered && !g_stop; attempt++) {
        g_dhcp_attempts = attempt + 1;
        w2_read_bssid(g_dhcp_bssid, sizeof(g_dhcp_bssid));
        if (sendto(fd, &req, reqlen, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
            snprintf(g_dhcp_error, sizeof(g_dhcp_error), "sendto: %s", strerror(errno));
            break;
        }
        g_dhcp_tx++;
        int64_t deadline = now_ms() + 3000;
        while (now_ms() < deadline && !g_stop) {
            struct pollfd pfd = {.fd = fd, .events = POLLIN};
            if (poll(&pfd, 1, 250) <= 0) continue;
            uint8_t buf[1500];
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n < (ssize_t)(offsetof(struct w2_dhcp_packet, options) + 4)) continue;
            struct w2_dhcp_packet *rep = (struct w2_dhcp_packet *)buf;
            if (rep->xid != req.xid || rep->op != 2) continue;
            if (w2_dhcp_msgtype(rep, (size_t)n) != 2) continue;
            offered = ntohl(rep->yiaddr);
            uint32_t sid = 0;
            w2_dhcp_server_id(rep, (size_t)n, &sid);
            server = sid;
            g_dhcp_rx_offer++;
            char ipbuf[32], srvbuf[32];
            w2_ip4_str(offered, ipbuf, sizeof(ipbuf));
            w2_ip4_str(server, srvbuf, sizeof(srvbuf));
            log_msg("dhcp client: OFFER %s from %s (attempt %d, bssid %s)",
                    ipbuf, srvbuf, g_dhcp_attempts, g_dhcp_bssid);
            break;
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
        goto done;
    }

    /* REQUEST/ACK: rebuild the packet with the offered address. */
    off = 4;
    mtype = 3;
    off += w2_dhcp_put(req.options + off, sizeof(req.options) - off, 53, 1, &mtype);
    uint32_t want = htonl(offered);
    off += w2_dhcp_put(req.options + off, sizeof(req.options) - off, 50, 4, &want);
    uint32_t sid = htonl(server);
    off += w2_dhcp_put(req.options + off, sizeof(req.options) - off, 54, 4, &sid);
    off += w2_dhcp_put(req.options + off, sizeof(req.options) - off, 55, sizeof(prl), prl);
    req.options[off++] = 255;
    reqlen = offsetof(struct w2_dhcp_packet, options) + off;

    for (int attempt = 0; attempt < 3 && !ok && !g_stop; attempt++) {
        if (sendto(fd, &req, reqlen, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
            snprintf(g_dhcp_error, sizeof(g_dhcp_error), "REQUEST sendto: %s", strerror(errno));
            break;
        }
        g_dhcp_tx++;
        int64_t deadline = now_ms() + 3000;
        while (now_ms() < deadline && !g_stop) {
            struct pollfd pfd = {.fd = fd, .events = POLLIN};
            if (poll(&pfd, 1, 250) <= 0) continue;
            uint8_t buf[1500];
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n < (ssize_t)(offsetof(struct w2_dhcp_packet, options) + 4)) continue;
            struct w2_dhcp_packet *rep = (struct w2_dhcp_packet *)buf;
            if (rep->xid != req.xid || rep->op != 2) continue;
            uint8_t t = w2_dhcp_msgtype(rep, (size_t)n);
            if (t == 5) { /* ACK */
                uint32_t mask = w2_dhcp_opt_u32(rep, (size_t)n, 1);
                char ipbuf[32];
                w2_ip4_str(offered, ipbuf, sizeof(ipbuf));
                g_dhcp_rx_ack++;
                log_msg("dhcp client: ACK %s mask=%s (bssid %s)", ipbuf,
                        mask ? "ok" : "absent", g_dhcp_bssid);
                if (!getenv("WIFI1B_DHCP_TEST_NOAPPLY")) {
                    char mb[32];
                    w2_ip4_str(mask ? mask : 0xffffff00U, mb, sizeof(mb));
                    w2_ip_command("addr add %s/%s dev %s 2>/dev/null || true", ipbuf, mb, iface);
                    w2_ip_command("link set %s up", iface);
                    if (server) {
                        char srv[32];
                        w2_ip4_str(server, srv, sizeof(srv));
                        w2_ip_command("route replace default via %s dev %s 2>/dev/null || true",
                                      srv, iface);
                    }
                }
                if (out_ip) str_copy(out_ip, ip_n, ipbuf);
                ok = true;
                break;
            }
            if (t == 6) { /* NAK */
                snprintf(g_dhcp_error, sizeof(g_dhcp_error),
                         "NAK from server (bssid %s)", g_dhcp_bssid);
                break;
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

done:
    if (rp_saved)
        w2_rp_filter_restore("/proc/sys/net/ipv4/conf/all/rp_filter",
                             rp_path, rp_all, rp_if);
    close(fd);
    return ok;
}

static bool client_disconnect(void)
{
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
    if (!g_cfg.wlan_on || g_cfg.mode == MODE_OFF) {
        ssh_stop();
        return false;
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
            g_ssh_fw_open = fw_open(g_ssh_active_port);
            if (!g_ssh_fw_open)
                log_msg("SSH: firewall rule missing while SSH is running");
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
    int fw_ok = fw_open(chosen);
    g_ssh_fw_open = fw_ok != 0;
    if (!fw_ok) log_msg("SSH: owned firewall rule failed (nonfatal); dropbear will still bind");

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
        set_msg("%s", ownership == 0 ? "WLAN off; radio left untouched" :
                "Radio busy/reserved; turn the owning WiFi app Off first");
        return ownership == 0;
    }
    if (!g_cfg.wlan_on || g_cfg.mode == MODE_OFF) {
        /* Reached only with our reservation, including interrupted scans.
         * The earlier ownership guard keeps idle/peer-Off hands-off. */
        stop_all();
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
        bool off = down && !wpa_process_alive() && !ssh_running() && !dhcpd_running();
        intent_write(false);
        set_msg("%s", off ? "WLAN off" : "WLAN-off cleanup could not be confirmed; recovery will retry");
        return off && radio_release();
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
        if (!ensure_wifi_ready()) { ssh_stop(); return false; }
        if (!client_connect()) {
            char why[256];
            str_copy(why, sizeof(why), g_last_msg);
            ssh_stop();
            set_msg("client mode failed: %s", why);
            return false;
        }
        if (g_cfg.ssh_enabled) ssh_start();
        else ssh_stop();
        set_msg("client mode: %s", g_cfg.ssid);
        return true;
    }

    if (g_ap_restart_requested) {
        ssh_stop();
        if (!ap_stop()) {
            set_msg("CarPlay explicit restart blocked: AP teardown not confirmed");
            return false;
        }
    }
    if (pair) bt_pair_start();
    else if (g_pairing_stop_requested) bt_pair_stop();
    if (!ap_start()) {
        ssh_stop();
        bool vbt_off = false;
        vbt_set_wireless_carplay(false, &vbt_off);
        restart_vbtd_if_needed(vbt_off);
        return false;
    }
    bool fw_ok = carplay_fw_open();
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
        ssh_stop();
        bool vbt_off = false;
        vbt_set_wireless_carplay(false, &vbt_off);
        restart_vbtd_if_needed(vbt_off);
        ap_evidence("lost-during-service-setup", &p);
        set_msg("CarPlay AP lost readiness during service setup");
        return false;
    }
    set_msg("Wifi1b CarPlay AP running (SSID=%s, %d MHz, DHCP %s/24)%s",
            p.ssid, channel_to_freq(), WIFI1B_AP_IP,
            ie_ok ? "" : " - CarPlay IE not set");
    bool services_ok = sh("systemctl is-active --quiet carplay_iAP2.service 2>/dev/null") == 0 &&
                       sh("systemctl is-active --quiet carplay_mdnsd.service 2>/dev/null") == 0;
    if (!services_ok) log_msg("CarPlay AP up, transport services not ready");
    if (!fw_ok) log_msg("CarPlay AP up, CarPlay firewall rules failed");
    if (!vbt_ok) log_msg("CarPlay AP up, VBT wireless flag not confirmed");
    return true;
}
