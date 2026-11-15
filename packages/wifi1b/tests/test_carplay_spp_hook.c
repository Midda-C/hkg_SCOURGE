/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Offline fake-event tests for the C3 build-only cp_spp_rx_hook.
 *
 * The test compiles cp_spp_rx_hook.c and this harness with a fixture-only
 * CP_SPP_RX_SOCKET_PATH.  It creates a receiver in the caller-provided temp
 * directory, sends fake T_VBT_SPP_RECEIVED_DATA_CFM objects, and checks the
 * exact datagram contract.  It never loads, executes, or patches an OEM
 * binary.  Payloads and events are synthetic.
 */
#define _GNU_SOURCE

#include "../src/carplay/cp_spp_rx.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef CP_SPP_RX_SOCKET_PATH
#error "test_carplay_spp_hook.c requires a fixture CP_SPP_RX_SOCKET_PATH"
#endif

/* The public header intentionally does not expose the private OEM ABI.  The
 * harness declares the fixed-address hook entry itself. */
extern void cp_spp_rx_hook(void *event);

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define EVENT_SIZE 0x60u
#define PAYLOAD_CAP CP_SPP_RX_MAX_PAYLOAD
#define PEER_TEXT "AA:BB:CC:DD:EE:FF"

struct fake_event {
    _Alignas(8) unsigned char raw[EVENT_SIZE];
};

static int g_rx = -1;
static volatile sig_atomic_t g_sigpipe;
static volatile sig_atomic_t g_alarm;

static void put_u64(unsigned char *p, uint64_t value)
{
    memcpy(p, &value, sizeof(value));
}

static void put_u32(unsigned char *p, uint32_t value)
{
    memcpy(p, &value, sizeof(value));
}

static void put_ptr(unsigned char *p, const void *value)
{
    uintptr_t converted = (uintptr_t)value;

    memcpy(p, &converted, sizeof(converted));
}

static void fake_event_init(struct fake_event *event, const char *peer,
                            uint64_t peer_len, const char *uuid,
                            uint64_t uuid_len, const void *payload,
                            uint32_t payload_len)
{
    memset(event, 0, sizeof(*event));
    put_ptr(event->raw + 0x08u, peer);
    put_u64(event->raw + 0x10u, peer_len);
    put_ptr(event->raw + 0x30u, uuid);
    put_u64(event->raw + 0x38u, uuid_len);
    put_ptr(event->raw + 0x50u, payload);
    put_u32(event->raw + 0x58u, payload_len);
}

static void fake_event_valid(struct fake_event *event, const char *peer,
                             const char *uuid, const void *payload,
                             uint32_t payload_len)
{
    fake_event_init(event, peer, CP_SPP_RX_PEER_LEN, uuid,
                    CP_SPP_RX_UUID_LEN, payload, payload_len);
}

static void on_sigpipe(int signo)
{
    (void)signo;
    g_sigpipe = 1;
}

static void on_alarm(int signo)
{
    (void)signo;
    g_alarm = 1;
}

static void install_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = on_sigpipe;
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGPIPE, &action, NULL) == 0);

    memset(&action, 0, sizeof(action));
    action.sa_handler = on_alarm;
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGALRM, &action, NULL) == 0);
}

static int make_receiver(void)
{
    int fd;
    struct sockaddr_un addr;
    struct stat st;
    char parent[sizeof(((struct sockaddr_un *)0)->sun_path)];
    const char *slash;
    size_t parent_len;

    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    CHECK(fd >= 0);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    CHECK(strlen(CP_SPP_RX_SOCKET_PATH) < sizeof(addr.sun_path));
    strcpy(addr.sun_path, CP_SPP_RX_SOCKET_PATH);

    unlink(CP_SPP_RX_SOCKET_PATH);
    CHECK(bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) == 0);
    CHECK(chmod(CP_SPP_RX_SOCKET_PATH, 0600) == 0);

    CHECK(stat(CP_SPP_RX_SOCKET_PATH, &st) == 0);
    CHECK(S_ISSOCK(st.st_mode));
    CHECK((st.st_mode & 0777) == 0600);
    if (geteuid() == 0) {
        CHECK(st.st_uid == 0);
    } else {
        CHECK(st.st_uid == geteuid());
    }

    slash = strrchr(CP_SPP_RX_SOCKET_PATH, '/');
    CHECK(slash != NULL);
    parent_len = (size_t)(slash - CP_SPP_RX_SOCKET_PATH);
    CHECK(parent_len > 0u && parent_len < sizeof(parent));
    memcpy(parent, CP_SPP_RX_SOCKET_PATH, parent_len);
    parent[parent_len] = '\0';
    CHECK(stat(parent, &st) == 0);
    CHECK(S_ISDIR(st.st_mode));
    CHECK((st.st_mode & 0777) == 0700);
    if (geteuid() == 0) {
        CHECK(st.st_uid == 0);
    } else {
        CHECK(st.st_uid == geteuid());
    }

    return fd;
}

static void close_receiver(void)
{
    if (g_rx >= 0) {
        close(g_rx);
        g_rx = -1;
    }
    unlink(CP_SPP_RX_SOCKET_PATH);
}

static ssize_t recv_one(unsigned char *buffer, size_t cap)
{
    ssize_t got;

    do {
        got = recv(g_rx, buffer, cap, MSG_DONTWAIT);
    } while (got < 0 && errno == EINTR);
    return got;
}

static void expect_none(void)
{
    unsigned char buffer[64];
    ssize_t got = recv_one(buffer, sizeof(buffer));

    CHECK(got < 0);
    CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
}

static void drain_receiver(void)
{
    unsigned char buffer[CP_SPP_RX_HEADER_SIZE + PAYLOAD_CAP];

    while (recv_one(buffer, sizeof(buffer)) >= 0) {
        /* discard */
    }
}

static void expect_packet(const char *peer, const void *payload,
                          uint32_t payload_len, unsigned int kind)
{
    unsigned char buffer[CP_SPP_RX_HEADER_SIZE + PAYLOAD_CAP];
    ssize_t got = recv_one(buffer, sizeof(buffer));
    uint32_t encoded_len;

    CHECK(got >= (ssize_t)CP_SPP_RX_HEADER_SIZE);
    CHECK(memcmp(buffer, CP_SPP_RX_MAGIC, CP_SPP_RX_MAGIC_LEN) == 0);
    memcpy(&encoded_len, buffer + CP_SPP_RX_OFF_DATALEN, sizeof(encoded_len));
    CHECK(encoded_len == payload_len);
    CHECK(memcmp(buffer + CP_SPP_RX_OFF_PEER, peer, CP_SPP_RX_PEER_LEN) == 0);
    CHECK(buffer[CP_SPP_RX_OFF_UUID_KIND] == (unsigned char)kind);
    CHECK(buffer[CP_SPP_RX_OFF_RESERVED] == 0u);
    CHECK(buffer[CP_SPP_RX_OFF_RESERVED + 1u] == 0u);
    CHECK((size_t)got == CP_SPP_RX_HEADER_SIZE + payload_len);
    if (payload_len > 0u) {
        CHECK(memcmp(buffer + CP_SPP_RX_HEADER_SIZE, payload, payload_len) == 0);
    }
}

static void lower_copy(char *out, const char *in)
{
    size_t i;

    for (i = 0u; i <= CP_SPP_RX_UUID_LEN; i++) {
        char c = in[i];

        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - ('A' - 'a'));
        }
        out[i] = c;
    }
}

static void test_valid_encoding(void)
{
    struct fake_event event;
    const unsigned char payload[] = {0x00u, 0x01u, 0x7fu, 0xfeu, 0xffu};
    char lower[CP_SPP_RX_UUID_LEN + 1u];

    fake_event_valid(&event, PEER_TEXT, CP_SPP_RX_UUID_IAP2_ACCESSORY_CAFF,
                     payload, (uint32_t)sizeof(payload));
    cp_spp_rx_hook(&event);
    expect_packet(PEER_TEXT, payload, (uint32_t)sizeof(payload),
                  CP_SPP_RX_UUID_KIND_IAP2_ACCESSORY_CAFF);

    lower_copy(lower, CP_SPP_RX_UUID_IAP2_ACCESSORY_CAFF);
    fake_event_valid(&event, PEER_TEXT, lower, payload,
                     (uint32_t)sizeof(payload));
    cp_spp_rx_hook(&event);
    expect_packet(PEER_TEXT, payload, (uint32_t)sizeof(payload),
                  CP_SPP_RX_UUID_KIND_IAP2_ACCESSORY_CAFF);

    fake_event_valid(&event, PEER_TEXT, CP_SPP_RX_UUID_IAP2_PHONE_CAFE,
                     payload, (uint32_t)sizeof(payload));
    cp_spp_rx_hook(&event);
    expect_packet(PEER_TEXT, payload, (uint32_t)sizeof(payload),
                  CP_SPP_RX_UUID_KIND_IAP2_PHONE_CAFE);

    fake_event_valid(&event, PEER_TEXT, CP_SPP_RX_UUID_CARPLAY, payload,
                     (uint32_t)sizeof(payload));
    cp_spp_rx_hook(&event);
    expect_packet(PEER_TEXT, payload, (uint32_t)sizeof(payload),
                  CP_SPP_RX_UUID_KIND_CARPLAY);

    lower_copy(lower, CP_SPP_RX_UUID_CARPLAY);
    fake_event_valid(&event, PEER_TEXT, lower, payload,
                     (uint32_t)sizeof(payload));
    cp_spp_rx_hook(&event);
    expect_packet(PEER_TEXT, payload, (uint32_t)sizeof(payload),
                  CP_SPP_RX_UUID_KIND_CARPLAY);
}

static void test_invalid_events(void)
{
    struct fake_event event;
    const unsigned char payload[] = {0x11u, 0x22u, 0x33u};
    char peer[CP_SPP_RX_PEER_LEN + 1u];
    char uuid[CP_SPP_RX_UUID_LEN + 1u];

    cp_spp_rx_hook(NULL);
    expect_none();

    memcpy(peer, PEER_TEXT, sizeof(peer));
    memcpy(uuid, CP_SPP_RX_UUID_IAP2_ACCESSORY_CAFF, sizeof(uuid));

    fake_event_init(&event, NULL, CP_SPP_RX_PEER_LEN, uuid,
                    CP_SPP_RX_UUID_LEN, payload, (uint32_t)sizeof(payload));
    cp_spp_rx_hook(&event);
    expect_none();

    fake_event_init(&event, peer, CP_SPP_RX_PEER_LEN - 1u, uuid,
                    CP_SPP_RX_UUID_LEN, payload, (uint32_t)sizeof(payload));
    cp_spp_rx_hook(&event);
    expect_none();

    fake_event_init(&event, peer, CP_SPP_RX_PEER_LEN + 1u, uuid,
                    CP_SPP_RX_UUID_LEN, payload, (uint32_t)sizeof(payload));
    cp_spp_rx_hook(&event);
    expect_none();

    peer[0] = 0x01;
    fake_event_valid(&event, peer, uuid, payload, (uint32_t)sizeof(payload));
    cp_spp_rx_hook(&event);
    expect_none();
    memcpy(peer, PEER_TEXT, sizeof(peer));

    fake_event_init(&event, peer, CP_SPP_RX_PEER_LEN, NULL,
                    CP_SPP_RX_UUID_LEN, payload, (uint32_t)sizeof(payload));
    cp_spp_rx_hook(&event);
    expect_none();

    fake_event_init(&event, peer, CP_SPP_RX_PEER_LEN, uuid,
                    CP_SPP_RX_UUID_LEN - 1u, payload,
                    (uint32_t)sizeof(payload));
    cp_spp_rx_hook(&event);
    expect_none();

    fake_event_init(&event, peer, CP_SPP_RX_PEER_LEN, uuid,
                    CP_SPP_RX_UUID_LEN + 1u, payload,
                    (uint32_t)sizeof(payload));
    cp_spp_rx_hook(&event);
    expect_none();

    memcpy(uuid, "00000000-DECA-FADE-DECA-DEAFDECACAF0", sizeof(uuid));
    fake_event_valid(&event, peer, uuid, payload, (uint32_t)sizeof(payload));
    cp_spp_rx_hook(&event);
    expect_none();

    memcpy(uuid, "EC884348-CD41-40A2-9727-575D50BF1FD4", sizeof(uuid));
    fake_event_valid(&event, peer, uuid, payload, (uint32_t)sizeof(payload));
    cp_spp_rx_hook(&event);
    expect_none();

    memcpy(uuid, CP_SPP_RX_UUID_IAP2_ACCESSORY_CAFF, sizeof(uuid));
    fake_event_init(&event, peer, CP_SPP_RX_PEER_LEN, uuid,
                    CP_SPP_RX_UUID_LEN, NULL, (uint32_t)sizeof(payload));
    cp_spp_rx_hook(&event);
    expect_none();

    fake_event_valid(&event, peer, uuid, NULL, 1u);
    cp_spp_rx_hook(&event);
    expect_none();

    fake_event_valid(&event, peer, uuid, payload, 0u);
    cp_spp_rx_hook(&event);
    expect_none();

    fake_event_valid(&event, peer, uuid, payload,
                     CP_SPP_RX_MAX_PAYLOAD + 1u);
    cp_spp_rx_hook(&event);
    expect_none();
}

static void test_max_payload(void)
{
    struct fake_event event;
    unsigned char *payload = malloc(CP_SPP_RX_MAX_PAYLOAD);
    size_t i;

    CHECK(payload != NULL);
    for (i = 0u; i < CP_SPP_RX_MAX_PAYLOAD; i++) {
        payload[i] = (unsigned char)(i * 31u + 7u);
    }
    fake_event_valid(&event, PEER_TEXT, CP_SPP_RX_UUID_CARPLAY, payload,
                     CP_SPP_RX_MAX_PAYLOAD);
    cp_spp_rx_hook(&event);
    expect_packet(PEER_TEXT, payload, CP_SPP_RX_MAX_PAYLOAD,
                  CP_SPP_RX_UUID_KIND_CARPLAY);
    free(payload);
}

static int count_fds(void)
{
    DIR *dir;
    struct dirent *entry;
    int count = 0;

    dir = opendir("/proc/self/fd");
    CHECK(dir != NULL);
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 &&
            strcmp(entry->d_name, "..") != 0) {
            count++;
        }
    }
    closedir(dir);
    return count;
}

static void test_fd_leak(void)
{
    struct fake_event event;
    const unsigned char payload[] = {0x5au, 0xa5u};
    int before = count_fds();
    int after;
    unsigned int i;

    for (i = 0u; i < 200u; i++) {
        fake_event_valid(&event, PEER_TEXT,
                         CP_SPP_RX_UUID_IAP2_ACCESSORY_CAFF, payload,
                         (uint32_t)sizeof(payload));
        cp_spp_rx_hook(&event);
        expect_packet(PEER_TEXT, payload, (uint32_t)sizeof(payload),
                      CP_SPP_RX_UUID_KIND_IAP2_ACCESSORY_CAFF);
    }
    after = count_fds();
    CHECK(before == after);
}

static void test_backpressure(void)
{
    struct fake_event event;
    unsigned char *payload = malloc(2048u);
    unsigned int i;

    CHECK(payload != NULL);
    memset(payload, 0xa5, 2048u);
    drain_receiver();
    g_alarm = 0;
    g_sigpipe = 0;
    alarm(5u);
    for (i = 0u; i < 2000u && g_alarm == 0; i++) {
        fake_event_valid(&event, PEER_TEXT,
                         CP_SPP_RX_UUID_IAP2_ACCESSORY_CAFF, payload, 2048u);
        cp_spp_rx_hook(&event);
    }
    alarm(0u);
    CHECK(g_alarm == 0);
    CHECK(g_sigpipe == 0);
    drain_receiver();
    free(payload);
}

static void test_missing_receiver(void)
{
    struct fake_event event;
    const unsigned char payload[] = {0x01u};

    close_receiver();
    g_alarm = 0;
    g_sigpipe = 0;
    alarm(2u);
    fake_event_valid(&event, PEER_TEXT, CP_SPP_RX_UUID_CARPLAY, payload,
                     (uint32_t)sizeof(payload));
    cp_spp_rx_hook(&event);
    alarm(0u);
    CHECK(g_alarm == 0);
    CHECK(g_sigpipe == 0);
}

int main(void)
{
    install_handlers();
    g_rx = make_receiver();
    test_valid_encoding();
    test_invalid_events();
    test_max_payload();
    test_fd_leak();
    test_backpressure();
    test_missing_receiver();
    close_receiver();
    printf("PASS: cp_spp_rx_hook fake-event, backpressure, SIGPIPE and FD tests\n");
    return 0;
}
