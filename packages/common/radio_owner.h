/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 * TARGET: Gen5W_L EU 260128, shared wlan0 ownership.
 * A kernel lock serializes mutations; the record ("comm:pid") reserves the
 * radio BETWEEN workers, including after a crash. Only successful owner-Off
 * clears it. A stuck reservation is TAKE-ABLE when the recorded owner is
 * verifiably gone, or is alive but reports Off while wlan0 is absent/down.
 * A live active owner, or a busy interface, always blocks. Never unlink the
 * record file: that would create two independently locked inodes.
 */
#include <sys/file.h>
#include <ctype.h>

#ifndef RADIO_OWNER_PATH
#define RADIO_OWNER_PATH "/run/scourge-wlan.owner"
#endif
#ifndef RADIO_IFACE_FLAGS
#define RADIO_IFACE_FLAGS "/sys/class/net/wlan0/flags"
#endif
static int radio_fd = -1;
static const char *radio_peer_comm = "";
static const char *radio_peer_status = "";
static const char *radio_iface_flags = RADIO_IFACE_FLAGS;

/* Once per process start, before any worker forks. */
static void radio_configure(const char *peer_comm, const char *peer_status)
{
    radio_peer_comm = peer_comm ? peer_comm : "";
    radio_peer_status = peer_status ? peer_status : "";
}

/* Test hook: point the interface check at a fixture flags file. */
#if defined(__GNUC__)
__attribute__((unused))
#endif
static void radio_set_iface(const char *path)
{
    if (path) radio_iface_flags = path;
}

static bool radio_slurp(const char *path, char *out, size_t n)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || n < 2) { if (fd >= 0) close(fd); return false; }
    ssize_t r = read(fd, out, n - 1);
    close(fd);
    if (r < 0) return false;
    out[r] = 0;
    return true;
}

/* Any process with this comm keeps the owner alive (daemon or worker). */
static bool radio_comm_alive(const char *name)
{
    DIR *d = opendir("/proc");
    if (!d) return true;                    /* unknown: fail closed */
    struct dirent *de;
    bool found = false;
    while (!found && (de = readdir(d))) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        char path[64], comm[64];
        snprintf(path, sizeof(path), "/proc/%s/comm", de->d_name);
        if (!radio_slurp(path, comm, sizeof(comm))) continue;
        char *nl = strchr(comm, '\n');
        if (nl) *nl = 0;
        if (!strcmp(comm, name)) found = true;
    }
    closedir(d);
    return found;
}

/* The peer's persisted status is authoritative for its DESIRE. A missing or
 * unreadable status file means unknown, never idle. */
static bool radio_peer_idle(void)
{
    if (!radio_peer_status[0]) return false;
    char buf[4096];
    if (!radio_slurp(radio_peer_status, buf, sizeof(buf))) return false;
    return strstr(buf, "\"wlan_on\":false") != NULL ||
           strstr(buf, "\"mode\":\"off\"") != NULL;
}

/* Nobody can be using the radio unless it exists and is administratively
 * up. An absent interface or an unreadable-but-present flags file fails
 * closed (not idle) except for plain absence. */
static bool radio_iface_idle(void)
{
    struct stat st;
    if (stat(radio_iface_flags, &st) != 0) return true;    /* interface absent */
    char flags[64];
    if (!radio_slurp(radio_iface_flags, flags, sizeof(flags))) return false;
    char *end = NULL;
    unsigned long value = strtoul(flags, &end, 0);
    while (end && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) end++;
    return end != flags && !*end && !(value & 1);           /* IFF_UP clear */
}

static bool radio_write_owner(int fd, const char *identity)
{
    char line[64];
    int len = snprintf(line, sizeof(line), "%s:%d", identity, (int)getpid());
    return len > 0 && (size_t)len < sizeof(line) &&
           pwrite(fd, line, (size_t)len, 0) == len &&
           ftruncate(fd, len) == 0 && fsync(fd) == 0;
}

/* 1 = owned; 0 = idle Off/no mutation; -1 = blocked/failure. */
static int radio_begin(const char *identity, bool active)
{
    if (radio_fd >= 0) return 1;
    int fd = open(RADIO_OWNER_PATH, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) { close(fd); return -1; }
    char record[64] = {0};
    ssize_t n = pread(fd, record, sizeof(record) - 1, 0);
    if (n < 0) { close(fd); return -1; }
    record[n] = 0;

    char name[32] = {0};
    char *colon = strchr(record, ':');
    if (colon) {
        size_t l = (size_t)(colon - record);
        if (l == 0 || l >= sizeof(name)) { close(fd); return -1; }
        memcpy(name, record, l);
    } else if (record[0]) {
        /* Legacy V8 record without a pid. */
        if (strlen(record) >= sizeof(name)) { close(fd); return -1; }
        strcpy(name, record);
    }

    if (!name[0]) {
        /* Idle radio. */
        if (!active) { close(fd); return 0; }
        if (!radio_write_owner(fd, identity)) { close(fd); return -1; }
        radio_fd = fd;
        return 1;
    }
    if (!strcmp(name, identity)) {
        /* Ours: this daemon or one of its workers already reserved it. */
        radio_fd = fd;
        return 1;
    }
    /* Stale-owner takeover. The recorded owner must be verifiably gone, or
     * be our known peer, alive, reporting Off, with an idle interface.
     * A live active owner, an unknown third app, or a busy interface
     * always blocks: never two owners on one radio. */
    bool alive = radio_comm_alive(name);
    bool takeable = (!alive || (!strcmp(name, radio_peer_comm) && radio_peer_idle())) &&
                    radio_iface_idle();
    if (!takeable) { close(fd); return active ? -1 : 0; }
    if (ftruncate(fd, 0) != 0 || fsync(fd) != 0) { close(fd); return -1; }
    if (!active) { close(fd); return 0; }   /* healed; nothing to reserve */
    if (!radio_write_owner(fd, identity)) { close(fd); return -1; }
    radio_fd = fd;
    return 1;
}

static bool radio_release(void)
{
    if (radio_fd < 0) return false;
    bool ok = ftruncate(radio_fd, 0) == 0 && fsync(radio_fd) == 0;
    close(radio_fd);
    radio_fd = -1;
    return ok;
}

/* Advisory read of the record name (no lock: diagnostics and self-heal only;
 * correctness is enforced by the flock in radio_begin/release). */
static bool radio_record_name(char *out, size_t n)
{
    if (n < 2) return false;
    int fd = open(RADIO_OWNER_PATH, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    ssize_t r = pread(fd, out, n - 1, 0);
    close(fd);
    if (r < 0) return false;
    out[r] = 0;
    char *colon = strchr(out, ':');
    if (colon) *colon = 0;
    return out[0] != 0;
}

static bool radio_owner_is(const char *identity)
{
    char name[32];
    return radio_record_name(name, sizeof(name)) && !strcmp(name, identity);
}

/* A single service OR manual daemon, never both. Kernel releases on exit. */
static int radio_instance_lock(const char *path)
{
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) return fd;
    close(fd);
    return -1;
}
