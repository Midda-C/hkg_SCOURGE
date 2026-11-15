/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Bounded, read-only parsers for the Wifi1b CarPlay AP network readback.
 *
 * These helpers deliberately parse command output and /proc text only. They
 * never execute a command, write a sysctl or change an interface. Keeping the
 * parsing separate makes the exact-address/readback rules testable without a
 * radio.
 */
#ifndef WIFI1B_CP_NETWORK_H
#define WIFI1B_CP_NETWORK_H

#include <arpa/inet.h>
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CP_NET_IFNAME_MAX 16
#define CP_NET_ADDR_MAX 64
#define CP_NET_SCOPED_MAX (CP_NET_ADDR_MAX + CP_NET_IFNAME_MAX + 8)

/* IFA_F_TENTATIVE / IFA_F_DADFAILED from linux/if_addr.h. */
#define CP_NET_IFA_TENTATIVE 0x40u
#define CP_NET_IFA_DADFAILED 0x08u
#define CP_NET_SCOPE_LINK 0x20u

typedef struct {
    bool usable;
    bool any_linklocal;
    bool tentative;
    bool dad_failed;
    char address[CP_NET_ADDR_MAX];          /* compressed, no scope */
    char scoped[CP_NET_SCOPED_MAX];         /* fe80::...%wlan0 or %<ifindex> */
    unsigned int ifindex;
    unsigned int prefix_len;
    unsigned int scope;
    unsigned int flags;
} cp_net_linklocal;

typedef struct {
    bool parsed;
    bool is_ap;
    bool is_managed;
    bool have_ssid;
    char ssid[33];
    unsigned int ifindex;
    int channel;
    int frequency;
} cp_net_iw_info;

typedef struct {
    bool parsed;
    bool ap_mode;
    bool station_mode;
    bool completed;
    bool have_ssid;
    char ssid[33];
    char key_mgmt[32];
    int frequency;
} cp_net_wpa_status;

static int cp_net_hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool cp_net_parse_hex(const char *text, unsigned int *out)
{
    unsigned int value = 0;
    size_t i;

    if (text == NULL || text[0] == '\0' || out == NULL) return false;
    for (i = 0; text[i] != '\0'; i++) {
        int digit;
        if (i >= 8) return false;
        digit = cp_net_hex_digit(text[i]);
        if (digit < 0) return false;
        value = (value << 4) | (unsigned int)digit;
    }
    *out = value;
    return true;
}

static bool cp_net_parse_in6_hex(const char *text, unsigned char out[16])
{
    size_t i;

    if (text == NULL || out == NULL || strlen(text) != 32) return false;
    for (i = 0; i < 16; i++) {
        int hi = cp_net_hex_digit(text[i * 2]);
        int lo = cp_net_hex_digit(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return true;
}

/* Decode the C-style escapes emitted by `iw`/`wpa_cli`. Rejects malformed
 * input rather than guessing an SSID. */
static bool cp_net_decode_ssid(char *out, size_t n, const char *value)
{
    size_t at = 0;

    if (out == NULL || n == 0) return false;
    out[0] = '\0';
    if (value == NULL) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; p++) {
        unsigned char c = *p;
        if (c == '\\') {
            p++;
            if (*p == '\0') return false;
            if (*p == 'x' && p[1] != '\0' && p[2] != '\0' &&
                cp_net_hex_digit((char)p[1]) >= 0 && cp_net_hex_digit((char)p[2]) >= 0) {
                char hex[3] = {(char)p[1], (char)p[2], '\0'};
                c = (unsigned char)strtol(hex, NULL, 16);
                p += 2;
            } else if (*p == 'n') {
                c = '\n';
            } else if (*p == 'r') {
                c = '\r';
            } else if (*p == 't') {
                c = '\t';
            } else if (*p == '\\' || *p == '"') {
                c = *p;
            } else {
                return false;
            }
        }
        if (c == '\0' || at + 1 >= n) return false;
        out[at++] = (char)c;
    }
    out[at] = '\0';
    return true;
}

/* Parse /proc/net/if_inet6. Only a usable fe80::/10 address for the requested
 * interface/ifindex is returned as usable. Tentative (0x40) and DAD-failed
 * (0x08) entries are reported for diagnostics but never accepted. */
static bool cp_net_parse_if_inet6(const char *text, const char *ifname,
                                  unsigned int want_ifindex,
                                  cp_net_linklocal *out)
{
    const char *line;

    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    if (text == NULL) return false;

    for (line = text; *line != '\0';) {
        const char *nl = strchr(line, '\n');
        size_t len = nl != NULL ? (size_t)(nl - line) : strlen(line);
        char buf[256];
        char addr_text[33], ifindex_text[9], plen_text[9], scope_text[9], flags_text[9];
        char name_text[CP_NET_IFNAME_MAX + 1];
        unsigned char addr[16];
        unsigned int ifindex, plen, scope, flags;
        bool is_linklocal;

        if (len >= sizeof(buf)) {
            line = nl != NULL ? nl + 1 : line + len;
            continue;
        }
        memcpy(buf, line, len);
        buf[len] = '\0';

        if (sscanf(buf, "%32s %8s %8s %8s %8s %15s",
                   addr_text, ifindex_text, plen_text, scope_text,
                   flags_text, name_text) != 6) {
            line = nl != NULL ? nl + 1 : line + len;
            continue;
        }
        if (!cp_net_parse_in6_hex(addr_text, addr) ||
            !cp_net_parse_hex(ifindex_text, &ifindex) ||
            !cp_net_parse_hex(plen_text, &plen) ||
            !cp_net_parse_hex(scope_text, &scope) ||
            !cp_net_parse_hex(flags_text, &flags)) {
            line = nl != NULL ? nl + 1 : line + len;
            continue;
        }
        if (ifname != NULL && ifname[0] != '\0' && strcmp(name_text, ifname) != 0) {
            line = nl != NULL ? nl + 1 : line + len;
            continue;
        }
        if (want_ifindex != 0 && ifindex != want_ifindex) {
            line = nl != NULL ? nl + 1 : line + len;
            continue;
        }
        is_linklocal = addr[0] == 0xfeU && (addr[1] & 0xc0U) == 0x80U;
        if (!is_linklocal) {
            line = nl != NULL ? nl + 1 : line + len;
            continue;
        }

        out->any_linklocal = true;
        if ((flags & CP_NET_IFA_TENTATIVE) != 0) out->tentative = true;
        if ((flags & CP_NET_IFA_DADFAILED) != 0) out->dad_failed = true;
        if (out->ifindex == 0) out->ifindex = ifindex;
        if (!out->usable && (flags & (CP_NET_IFA_TENTATIVE | CP_NET_IFA_DADFAILED)) == 0) {
            char formatted[INET6_ADDRSTRLEN];
            if (inet_ntop(AF_INET6, addr, formatted, sizeof(formatted)) != NULL) {
                snprintf(out->address, sizeof(out->address), "%s", formatted);
                if (name_text[0] != '\0') {
                    snprintf(out->scoped, sizeof(out->scoped), "%s%%%s", formatted, name_text);
                } else {
                    snprintf(out->scoped, sizeof(out->scoped), "%s%%%u", formatted, ifindex);
                }
                out->ifindex = ifindex;
                out->prefix_len = plen;
                out->scope = scope;
                out->flags = flags;
                out->usable = true;
            }
        }
        line = nl != NULL ? nl + 1 : line + len;
    }
    if (!out->usable) {
        out->address[0] = '\0';
        out->scoped[0] = '\0';
    }
    return out->usable;
}

static void cp_net_rtrim(char *s)
{
    size_t len;

    if (s == NULL) return;
    len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
                       s[len - 1] == ' ' || s[len - 1] == '\t')) {
        s[--len] = '\0';
    }
}

static char *cp_net_ltrim(char *s)
{
    while (s != NULL && (*s == ' ' || *s == '\t')) s++;
    return s;
}

/* Parse `iw dev wlan0 info`. Missing values stay zero/false; they are never
 * filled from configuration. */
static bool cp_net_parse_iw_info(const char *text, cp_net_iw_info *out)
{
    const char *line;

    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    out->parsed = true;
    if (text == NULL) return false;

    for (line = text; *line != '\0';) {
        const char *nl = strchr(line, '\n');
        size_t len = nl != NULL ? (size_t)(nl - line) : strlen(line);
        char buf[512];
        char *s;

        if (len >= sizeof(buf)) {
            line = nl != NULL ? nl + 1 : line + len;
            continue;
        }
        memcpy(buf, line, len);
        buf[len] = '\0';
        cp_net_rtrim(buf);
        s = cp_net_ltrim(buf);

        if (strncmp(s, "ifindex ", 8) == 0) {
            unsigned long value = strtoul(s + 8, NULL, 10);
            if (value > 0 && value <= 0xffffffffUL) out->ifindex = (unsigned int)value;
        } else if (strncmp(s, "type ", 5) == 0) {
            const char *type = cp_net_ltrim(s + 5);
            if (strcmp(type, "AP") == 0) out->is_ap = true;
            else if (strcmp(type, "managed") == 0) out->is_managed = true;
        } else if (strncmp(s, "ssid ", 5) == 0) {
            out->have_ssid = cp_net_decode_ssid(out->ssid, sizeof(out->ssid), s + 5);
        } else if (strncmp(s, "channel ", 8) == 0) {
            char *end = NULL;
            long ch = strtol(s + 8, &end, 10);
            if (end != s + 8 && ch > 0 && ch <= 1000) {
                out->channel = (int)ch;
                while (*end == ' ') end++;
                if (*end == '(') {
                    long mhz = strtol(end + 1, NULL, 10);
                    if (mhz > 0 && mhz <= 100000) out->frequency = (int)mhz;
                }
            }
        }
        line = nl != NULL ? nl + 1 : line + len;
    }
    return true;
}

/* Copy only a safe key-management token. This cannot expose the passphrase. */
static void cp_net_copy_key_mgmt(char *dst, size_t n, const char *src)
{
    size_t at = 0;

    if (dst == NULL || n == 0) return;
    dst[0] = '\0';
    if (src == NULL) return;
    for (const char *p = src; *p != '\0' && at + 1 < n; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '-' || c == '_') dst[at++] = (char)c;
        else if (at > 0) break;
    }
    dst[at] = '\0';
}

/* Parse `wpa_cli ... status`. */
static bool cp_net_parse_wpa_status(const char *text, cp_net_wpa_status *out)
{
    const char *line;

    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    out->parsed = true;
    if (text == NULL) return false;

    for (line = text; *line != '\0';) {
        const char *nl = strchr(line, '\n');
        size_t len = nl != NULL ? (size_t)(nl - line) : strlen(line);
        char buf[512];
        char *s;

        if (len >= sizeof(buf)) {
            line = nl != NULL ? nl + 1 : line + len;
            continue;
        }
        memcpy(buf, line, len);
        buf[len] = '\0';
        cp_net_rtrim(buf);
        s = cp_net_ltrim(buf);

        if (strcmp(s, "mode=AP") == 0) out->ap_mode = true;
        else if (strcmp(s, "mode=station") == 0) out->station_mode = true;
        else if (strcmp(s, "wpa_state=COMPLETED") == 0) out->completed = true;
        else if (strncmp(s, "ssid=", 5) == 0) {
            out->have_ssid = cp_net_decode_ssid(out->ssid, sizeof(out->ssid), s + 5);
        } else if (strncmp(s, "key_mgmt=", 9) == 0) {
            cp_net_copy_key_mgmt(out->key_mgmt, sizeof(out->key_mgmt), s + 9);
        } else if (strncmp(s, "freq=", 5) == 0) {
            long freq = strtol(s + 5, NULL, 10);
            if (freq > 0 && freq <= 100000) out->frequency = (int)freq;
        }
        line = nl != NULL ? nl + 1 : line + len;
    }
    return true;
}

/* Parse `ip -o -4 addr show dev wlan0`. Returns the first IPv4 address for the
 * requested interface. Exact-address comparisons are the caller's job. */
static bool cp_net_parse_ip4_addr(const char *text, const char *ifname,
                                  char *out, size_t out_len)
{
    const char *line;

    if (out == NULL || out_len == 0) return false;
    out[0] = '\0';
    if (text == NULL) return false;

    for (line = text; *line != '\0';) {
        const char *nl = strchr(line, '\n');
        size_t len = nl != NULL ? (size_t)(nl - line) : strlen(line);
        char buf[512];
        char name[CP_NET_IFNAME_MAX + 1], cidr[96];
        char address[INET6_ADDRSTRLEN];
        struct in_addr parsed;

        if (len >= sizeof(buf)) {
            line = nl != NULL ? nl + 1 : line + len;
            continue;
        }
        memcpy(buf, line, len);
        buf[len] = '\0';
        if (sscanf(buf, " %*d: %15s inet %95s", name, cidr) != 2) {
            line = nl != NULL ? nl + 1 : line + len;
            continue;
        }
        if (ifname != NULL && ifname[0] != '\0' && strcmp(name, ifname) != 0) {
            line = nl != NULL ? nl + 1 : line + len;
            continue;
        }
        size_t at = 0;
        while (cidr[at] != '\0' && cidr[at] != '/' &&
               at + 1 < sizeof(address)) {
            address[at] = cidr[at];
            at++;
        }
        address[at] = '\0';
        if (at == 0 || inet_pton(AF_INET, address, &parsed) != 1) {
            line = nl != NULL ? nl + 1 : line + len;
            continue;
        }
        snprintf(out, out_len, "%s", address);
        return true;
    }
    return false;
}

static int cp_net_freq_to_channel(int frequency)
{
    if (frequency == 2484) return 14;
    if (frequency >= 2412 && frequency <= 2472)
        return (frequency - 2407) / 5;
    if (frequency >= 5000 && frequency <= 5895)
        return (frequency - 5000) / 5;
    if (frequency >= 5955 && frequency <= 7115)
        return (frequency - 5950) / 5;
    return 0;
}

#endif /* WIFI1B_CP_NETWORK_H */
