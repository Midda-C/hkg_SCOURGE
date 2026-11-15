/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Offline tests for the nonblocking MFi helper client.  All network fixtures
 * are private first-party AF_UNIX sockets under /tmp/opencode.  No OEM code,
 * helper binary, I2C, radio or real network is used.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "cp_oem_auth_async.h"

static int failures;
static int checks;

#define CHECK(expr)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(expr)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

#define CHECK_EQ(actual, expected)                                           \
    do {                                                                     \
        long long a_ = (long long)(actual);                                  \
        long long e_ = (long long)(expected);                                \
        checks++;                                                            \
        if (a_ != e_) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s=%lld, expected %lld\n",         \
                    __FILE__, __LINE__, #actual, a_, e_);                    \
            failures++;                                                      \
        }                                                                    \
    } while (0)

typedef struct {
    char dir[80];
    char path[CP_OEM_AUTH_SOCKET_PATH_MAX];
    int fd;
} fixture;

static bool fixture_open(fixture *fixture_out, const char *suffix)
{
    static const char prefix[] = "/tmp/opencode/cp-async-XXXXXX";
    struct sockaddr_un addr;
    size_t dir_len;
    size_t suffix_len;

    memset(fixture_out, 0, sizeof(*fixture_out));
    fixture_out->fd = -1;
    if (strlen(prefix) + 1u > sizeof(fixture_out->dir)) {
        return false;
    }
    memcpy(fixture_out->dir, prefix, sizeof(prefix));
    if (mkdtemp(fixture_out->dir) == NULL) {
        return false;
    }
    dir_len = strlen(fixture_out->dir);
    suffix_len = strlen(suffix);
    if (dir_len + 1u + suffix_len + 1u > sizeof(fixture_out->path)) {
        return false;
    }
    memcpy(fixture_out->path, fixture_out->dir, dir_len);
    fixture_out->path[dir_len] = '/';
    memcpy(fixture_out->path + dir_len + 1u, suffix, suffix_len + 1u);

    fixture_out->fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fixture_out->fd < 0) {
        return false;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, fixture_out->path, strlen(fixture_out->path) + 1u);
    if (bind(fixture_out->fd, (const struct sockaddr *)&addr, sizeof(addr)) !=
            0 ||
        chmod(fixture_out->path, 0600) != 0 ||
        listen(fixture_out->fd, 16) != 0) {
        return false;
    }
    return true;
}

static void fixture_close(fixture *fixture_out)
{
    if (fixture_out->fd >= 0) {
        (void)close(fixture_out->fd);
        fixture_out->fd = -1;
    }
    if (fixture_out->path[0] != '\0') {
        (void)unlink(fixture_out->path);
    }
    if (fixture_out->dir[0] != '\0') {
        (void)rmdir(fixture_out->dir);
    }
    memset(fixture_out, 0, sizeof(*fixture_out));
    fixture_out->fd = -1;
}

static cp_oem_auth_async_config fixture_config(const fixture *fixture_out)
{
    cp_oem_auth_async_config config = {0};

    memset(&config, 0, sizeof(config));
    config.experimental = true;
    config.socket_path = fixture_out->path;
    config.expected_peer_uid = (uint32_t)geteuid();
    return config;
}

static bool bytes_all_zero(const uint8_t *bytes, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (bytes[i] != 0u) {
            return false;
        }
    }
    return true;
}

static void test_init_and_destroy(void)
{
    cp_oem_auth_async async = {0};
    cp_oem_auth_async_config config = {0};
    fixture fixture_out;

    memset(&async, 0, sizeof(async));
    memset(&config, 0, sizeof(config));
    CHECK_EQ(cp_oem_auth_async_init(&async, &config),
             CP_OEM_AUTH_ASYNC_ERR_DISABLED);
    CHECK(!async.initialized);
    CHECK_EQ(cp_oem_auth_async_get_fd(&async), -1);
    CHECK_EQ(cp_oem_auth_async_get_poll_events(&async), 0);
    CHECK(!cp_oem_auth_async_completion_ready(&async));
    cp_oem_auth_async_destroy(&async);
    CHECK(bytes_all_zero((const uint8_t *)&async, sizeof(async)));
    cp_oem_auth_async_destroy(&async); /* zero state is safe */

    CHECK(fixture_open(&fixture_out, "auth.sock"));
    config = fixture_config(&fixture_out);
    CHECK_EQ(cp_oem_auth_async_init(&async, &config), CP_OEM_AUTH_ASYNC_OK);
    CHECK(async.initialized && async.experimental);
    CHECK_EQ(cp_oem_auth_async_get_state(&async),
             CP_OEM_AUTH_ASYNC_STATE_IDLE);
    CHECK_EQ(cp_oem_auth_async_get_fd(&async), -1);
    CHECK_EQ(cp_oem_auth_async_init(&async, &config),
             CP_OEM_AUTH_ASYNC_ERR_STATE);
    cp_oem_auth_async_destroy(&async);
    fixture_close(&fixture_out);

    memset(&async, 0, sizeof(async));
    config = (cp_oem_auth_async_config){0};
    config.experimental = true;
    config.socket_path = "relative.sock";
    config.expected_peer_uid = (uint32_t)geteuid();
    CHECK_EQ(cp_oem_auth_async_init(&async, &config),
             CP_OEM_AUTH_ASYNC_ERR_ARGUMENT);
    CHECK(!async.initialized);
    cp_oem_auth_async_destroy(&async);
}

static void test_begin_validation(void)
{
    static const uint8_t digest[32] = {0};
    cp_oem_auth_async async = {0};
    cp_oem_auth_async_config config = {0};
    cp_oem_auth_async_completion completion = {0};
    fixture fixture_out;
    size_t i;
    static const size_t bad_lengths[] = {0u, 1u, 19u, 21u, 31u, 33u, 64u};

    CHECK(fixture_open(&fixture_out, "auth.sock"));
    config = fixture_config(&fixture_out);
    CHECK_EQ(cp_oem_auth_async_init(&async, &config), CP_OEM_AUTH_ASYNC_OK);
    CHECK_EQ(cp_oem_auth_async_service(&async, 0, 1u),
             CP_OEM_AUTH_ASYNC_ERR_STATE);

    CHECK_EQ(cp_oem_auth_async_begin_certificate(&async, 1u, 10u, 10u),
             CP_OEM_AUTH_ASYNC_ERR_DEADLINE);
    CHECK_EQ(cp_oem_auth_async_begin_certificate(&async, 1u, 10u, 9u),
             CP_OEM_AUTH_ASYNC_ERR_DEADLINE);
    CHECK_EQ(cp_oem_auth_async_begin_signature(&async, 1u, NULL, 20u, 10u,
                                               20u),
             CP_OEM_AUTH_ASYNC_ERR_ARGUMENT);
    for (i = 0; i < sizeof(bad_lengths) / sizeof(bad_lengths[0]); i++) {
        CHECK_EQ(cp_oem_auth_async_begin_signature(
                     &async, 1u, digest, bad_lengths[i], 10u, 20u),
                 CP_OEM_AUTH_ASYNC_ERR_CHALLENGE_LENGTH);
    }
    CHECK_EQ(cp_oem_auth_async_take_completion(&async, &completion), false);
    CHECK_EQ(cp_oem_auth_async_cancel(&async, 0u), CP_OEM_AUTH_ASYNC_OK);

    CHECK_EQ(cp_oem_auth_async_begin_certificate(&async, 7u, 10u, 100u),
             CP_OEM_AUTH_ASYNC_OK);
    CHECK_EQ(cp_oem_auth_async_get_state(&async),
             CP_OEM_AUTH_ASYNC_STATE_CONNECTING);
    CHECK_EQ(cp_oem_auth_async_get_fd(&async), -1);
    CHECK_EQ(cp_oem_auth_async_begin_certificate(&async, 8u, 11u, 100u),
             CP_OEM_AUTH_ASYNC_ERR_BUSY);
    CHECK_EQ(cp_oem_auth_async_cancel(&async, 6u),
             CP_OEM_AUTH_ASYNC_ERR_GENERATION);
    CHECK_EQ(cp_oem_auth_async_get_state(&async),
             CP_OEM_AUTH_ASYNC_STATE_CONNECTING);

    CHECK_EQ(cp_oem_auth_async_service(&async, 0, 10u),
             CP_OEM_AUTH_ASYNC_OK);
    CHECK(async.fd >= 0);
    CHECK(async.poll_events != 0);
    CHECK_EQ(cp_oem_auth_async_cancel(&async, 6u),
             CP_OEM_AUTH_ASYNC_ERR_GENERATION);
    CHECK_EQ(cp_oem_auth_async_cancel(&async, 7u), CP_OEM_AUTH_ASYNC_OK);
    CHECK(cp_oem_auth_async_completion_ready(&async));
    CHECK_EQ(cp_oem_auth_async_get_fd(&async), -1);
    CHECK_EQ(cp_oem_auth_async_get_poll_events(&async), 0);
    CHECK_EQ(cp_oem_auth_async_get_state(&async), CP_OEM_AUTH_ASYNC_STATE_DONE);
    CHECK_EQ(cp_oem_auth_async_service(&async, 0, 11u),
             CP_OEM_AUTH_ASYNC_ERR_STATE);

    CHECK(cp_oem_auth_async_take_completion(&async, &completion));
    CHECK_EQ(completion.op, CP_OEM_AUTH_ASYNC_OP_CERTIFICATE);
    CHECK_EQ(completion.generation, 7u);
    CHECK(completion.request_id != 0u);
    CHECK_EQ(completion.error, CP_OEM_AUTH_ASYNC_ERR_CANCELLED);
    CHECK_EQ(completion.length, 0u);
    CHECK(bytes_all_zero(completion.bytes, sizeof(completion.bytes)));
    CHECK(bytes_all_zero(async.completion.bytes,
                         sizeof(async.completion.bytes)));
    CHECK(!cp_oem_auth_async_take_completion(&async, &completion));
    CHECK_EQ(cp_oem_auth_async_get_state(&async),
             CP_OEM_AUTH_ASYNC_STATE_IDLE);
    CHECK_EQ(cp_oem_auth_async_begin_certificate(&async, 7u, 9u, 100u),
             CP_OEM_AUTH_ASYNC_ERR_CLOCK);
    CHECK_EQ(cp_oem_auth_async_begin_signature(&async, 8u, digest, 20u,
                                               12u, 100u),
             CP_OEM_AUTH_ASYNC_OK);
    CHECK_EQ(cp_oem_auth_async_service(&async, 0, 12u),
             CP_OEM_AUTH_ASYNC_OK);
    CHECK(async.fd >= 0);
    CHECK_EQ(cp_oem_auth_async_cancel(&async, 8u), CP_OEM_AUTH_ASYNC_OK);
    CHECK(cp_oem_auth_async_take_completion(&async, &completion));
    CHECK_EQ(completion.op, CP_OEM_AUTH_ASYNC_OP_SIGNATURE);
    CHECK_EQ(completion.generation, 8u);
    CHECK_EQ(completion.error, CP_OEM_AUTH_ASYNC_ERR_CANCELLED);
    CHECK_EQ(cp_oem_auth_async_begin_signature(&async, 9u, digest, 32u,
                                               13u, 100u),
             CP_OEM_AUTH_ASYNC_OK);
    CHECK_EQ(cp_oem_auth_async_service(&async, 0, 13u),
             CP_OEM_AUTH_ASYNC_OK);
    CHECK_EQ(cp_oem_auth_async_cancel(&async, 9u), CP_OEM_AUTH_ASYNC_OK);
    CHECK(cp_oem_auth_async_take_completion(&async, &completion));
    CHECK_EQ(completion.error, CP_OEM_AUTH_ASYNC_ERR_CANCELLED);
    CHECK_EQ(completion.generation, 9u);
    cp_oem_auth_async_destroy(&async);
    fixture_close(&fixture_out);
}

static void test_timeout_and_clock(void)
{
    cp_oem_auth_async async = {0};
    cp_oem_auth_async_config config = {0};
    cp_oem_auth_async_completion completion = {0};
    fixture fixture_out;

    CHECK(fixture_open(&fixture_out, "auth.sock"));
    config = fixture_config(&fixture_out);
    CHECK_EQ(cp_oem_auth_async_init(&async, &config), CP_OEM_AUTH_ASYNC_OK);

    CHECK_EQ(cp_oem_auth_async_begin_certificate(&async, 9u, 1000u, 1100u),
             CP_OEM_AUTH_ASYNC_OK);
    CHECK_EQ(cp_oem_auth_async_service(&async, 0, 1000u),
             CP_OEM_AUTH_ASYNC_OK);
    CHECK(async.fd >= 0);
    CHECK_EQ(cp_oem_auth_async_service(&async, 0, 1100u),
             CP_OEM_AUTH_ASYNC_ERR_TIMEOUT);
    CHECK(cp_oem_auth_async_completion_ready(&async));
    CHECK_EQ(cp_oem_auth_async_get_fd(&async), -1);
    CHECK(cp_oem_auth_async_take_completion(&async, &completion));
    CHECK_EQ(completion.error, CP_OEM_AUTH_ASYNC_ERR_TIMEOUT);
    CHECK_EQ(completion.generation, 9u);
    CHECK_EQ(completion.length, 0u);

    CHECK_EQ(cp_oem_auth_async_begin_certificate(&async, 10u, 2000u, 2200u),
             CP_OEM_AUTH_ASYNC_OK);
    CHECK_EQ(cp_oem_auth_async_service(&async, 0, 2000u),
             CP_OEM_AUTH_ASYNC_OK);
    CHECK(async.fd >= 0);
    CHECK_EQ(cp_oem_auth_async_service(&async, 0, 1999u),
             CP_OEM_AUTH_ASYNC_ERR_CLOCK);
    CHECK(cp_oem_auth_async_completion_ready(&async));
    CHECK_EQ(cp_oem_auth_async_get_fd(&async), -1);
    CHECK(cp_oem_auth_async_take_completion(&async, &completion));
    CHECK_EQ(completion.error, CP_OEM_AUTH_ASYNC_ERR_CLOCK);
    CHECK_EQ(completion.generation, 10u);
    CHECK_EQ(completion.length, 0u);
    cp_oem_auth_async_destroy(&async);
    fixture_close(&fixture_out);
}

static void test_path_failures(void)
{
    cp_oem_auth_async async = {0};
    cp_oem_auth_async_config config = {0};
    cp_oem_auth_async_completion completion = {0};
    fixture fixture_out;
    char missing[CP_OEM_AUTH_SOCKET_PATH_MAX];
    size_t dir_len;

    CHECK(fixture_open(&fixture_out, "auth.sock"));
    config = fixture_config(&fixture_out);
    CHECK_EQ(cp_oem_auth_async_init(&async, &config), CP_OEM_AUTH_ASYNC_OK);

    dir_len = strlen(fixture_out.dir);
    memset(missing, 0, sizeof(missing));
    memcpy(missing, fixture_out.dir, dir_len);
    memcpy(missing + dir_len, "/missing.sock", sizeof("/missing.sock"));
    config.socket_path = missing;
    cp_oem_auth_async_destroy(&async);
    CHECK_EQ(cp_oem_auth_async_init(&async, &config), CP_OEM_AUTH_ASYNC_OK);
    CHECK_EQ(cp_oem_auth_async_begin_certificate(&async, 11u, 100u, 200u),
             CP_OEM_AUTH_ASYNC_OK);
    CHECK_EQ(cp_oem_auth_async_service(&async, 0, 100u),
             CP_OEM_AUTH_ASYNC_ERR_PATH);
    CHECK(cp_oem_auth_async_completion_ready(&async));
    CHECK(cp_oem_auth_async_take_completion(&async, &completion));
    CHECK_EQ(completion.error, CP_OEM_AUTH_ASYNC_ERR_PATH);
    CHECK_EQ(completion.length, 0u);
    cp_oem_auth_async_destroy(&async);

    CHECK_EQ(chmod(fixture_out.dir, 0755), 0);
    config = fixture_config(&fixture_out);
    memset(&async, 0, sizeof(async));
    CHECK_EQ(cp_oem_auth_async_init(&async, &config), CP_OEM_AUTH_ASYNC_OK);
    CHECK_EQ(cp_oem_auth_async_begin_certificate(&async, 12u, 100u, 200u),
             CP_OEM_AUTH_ASYNC_OK);
    CHECK_EQ(cp_oem_auth_async_service(&async, 0, 100u),
             CP_OEM_AUTH_ASYNC_ERR_PATH);
    CHECK(cp_oem_auth_async_take_completion(&async, &completion));
    CHECK_EQ(completion.error, CP_OEM_AUTH_ASYNC_ERR_PATH);
    cp_oem_auth_async_destroy(&async);
    (void)chmod(fixture_out.dir, 0700);
    fixture_close(&fixture_out);
}

static void test_destroy_zeroization(void)
{
    cp_oem_auth_async async = {0};
    cp_oem_auth_async_config config = {0};
    fixture fixture_out;

    CHECK(fixture_open(&fixture_out, "auth.sock"));
    config = fixture_config(&fixture_out);
    CHECK_EQ(cp_oem_auth_async_init(&async, &config), CP_OEM_AUTH_ASYNC_OK);
    CHECK_EQ(cp_oem_auth_async_begin_certificate(&async, 13u, 100u, 200u),
             CP_OEM_AUTH_ASYNC_OK);
    CHECK_EQ(cp_oem_auth_async_service(&async, 0, 100u),
             CP_OEM_AUTH_ASYNC_OK);
    CHECK(async.fd >= 0);
    cp_oem_auth_async_destroy(&async);
    CHECK(bytes_all_zero((const uint8_t *)&async, sizeof(async)));
    CHECK_EQ(cp_oem_auth_async_get_fd(&async), -1);
    CHECK_EQ(cp_oem_auth_async_get_poll_events(&async), 0);
    cp_oem_auth_async_destroy(&async);
    fixture_close(&fixture_out);
}

/* ---------------------------------------------------------------- */
/* Poll-driven runner used by the Python mock-helper tests            */
/* ---------------------------------------------------------------- */

static void print_hex(const uint8_t *bytes, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        printf("%02x", bytes[i]);
    }
}

static int run_one(const char *path, const char *mode, uint32_t timeout_ms)
{
    cp_oem_auth_async async = {0};
    cp_oem_auth_async_config config = {0};
    cp_oem_auth_async_completion completion = {0};
    uint8_t digest[20];
    uint64_t now = 1000u;
    uint64_t deadline = now + timeout_ms;
    unsigned int iterations = 0u;
    bool saw_connecting = false;
    bool saw_sending = false;
    bool saw_receiving = false;
    bool taken;
    cp_oem_auth_async_error error;

    memset(digest, 0x42, sizeof(digest));
    memset(&completion, 0, sizeof(completion));
    memset(&config, 0, sizeof(config));
    config.experimental = true;
    config.socket_path = path;
    config.expected_peer_uid = (uint32_t)geteuid();
    if (cp_oem_auth_async_init(&async, &config) != CP_OEM_AUTH_ASYNC_OK) {
        printf("init_error=1\n");
        return 1;
    }
    if (strcmp(mode, "sign") == 0) {
        if (cp_oem_auth_async_begin_signature(&async, 0x1122334455667788ULL,
                                              digest, sizeof(digest), now,
                                              deadline) !=
            CP_OEM_AUTH_ASYNC_OK) {
            printf("begin_error=1\n");
            cp_oem_auth_async_destroy(&async);
            return 1;
        }
    } else {
        if (cp_oem_auth_async_begin_certificate(
                &async, 0x1122334455667788ULL, now, deadline) !=
            CP_OEM_AUTH_ASYNC_OK) {
            printf("begin_error=1\n");
            cp_oem_auth_async_destroy(&async);
            return 1;
        }
    }

    for (;;) {
        struct pollfd pfd;
        int poll_rc;
        short revents = 0;

        if (async.state == CP_OEM_AUTH_ASYNC_STATE_CONNECTING) {
            saw_connecting = true;
        } else if (async.state == CP_OEM_AUTH_ASYNC_STATE_SENDING) {
            saw_sending = true;
        } else if (async.state == CP_OEM_AUTH_ASYNC_STATE_RECEIVING) {
            saw_receiving = true;
        }
        if (async.completion_ready) {
            break;
        }
        pfd.fd = async.fd;
        pfd.events = async.poll_events;
        pfd.revents = 0;
        poll_rc = poll(&pfd, async.fd >= 0 ? 1u : 0u, 1);
        if (poll_rc > 0) {
            revents = pfd.revents;
        }
        error = cp_oem_auth_async_service(&async, revents, now);
        iterations++;
        now++;
        if (error != CP_OEM_AUTH_ASYNC_OK && !async.completion_ready) {
            break;
        }
        if (iterations > timeout_ms + 1000u) {
            break;
        }
    }

    taken = cp_oem_auth_async_take_completion(&async, &completion);
    printf("taken=%u\n", taken ? 1u : 0u);
    printf("error=%d\n", (int)completion.error);
    printf("error_name=%s\n",
           cp_oem_auth_async_error_name(completion.error));
    printf("helper_status=%d\n", (int)completion.helper_status);
    printf("op=%d\n", (int)completion.op);
    printf("generation=%llu\n",
           (unsigned long long)completion.generation);
    printf("request_id=%u\n", completion.request_id);
    printf("length=%u\n", completion.length);
    printf("hex=");
    print_hex(completion.bytes, completion.length);
    putchar('\n');
    printf("fd=%d\n", cp_oem_auth_async_get_fd(&async));
    printf("poll_events=%d\n",
           (int)cp_oem_auth_async_get_poll_events(&async));
    printf("iterations=%u\n", iterations);
    printf("saw_connecting=%u\n", saw_connecting ? 1u : 0u);
    printf("saw_sending=%u\n", saw_sending ? 1u : 0u);
    printf("saw_receiving=%u\n", saw_receiving ? 1u : 0u);
    printf("rights_received=%u\n", async.rights_fds_received);
    printf("rights_closed=%u\n", async.rights_fds_closed);
    cp_oem_auth_async_destroy(&async);
    return completion.error == CP_OEM_AUTH_ASYNC_OK ? 0 : (int)completion.error;
}

static int run_two(const char *path)
{
    cp_oem_auth_async async = {0};
    cp_oem_auth_async_config config = {0};
    cp_oem_auth_async_completion first;
    cp_oem_auth_async_completion second;
    uint64_t now = 1000u;
    unsigned int iterations;
    cp_oem_auth_async_error stale;
    cp_oem_auth_async_error cancelled;
    bool taken;

    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    memset(&config, 0, sizeof(config));
    config.experimental = true;
    config.socket_path = path;
    config.expected_peer_uid = (uint32_t)geteuid();
    if (cp_oem_auth_async_init(&async, &config) != CP_OEM_AUTH_ASYNC_OK) {
        printf("init_error=1\n");
        return 1;
    }
    if (cp_oem_auth_async_begin_certificate(&async, 10u, now, now + 5000u) !=
        CP_OEM_AUTH_ASYNC_OK) {
        printf("first_begin_error=1\n");
        cp_oem_auth_async_destroy(&async);
        return 1;
    }
    for (iterations = 0u; iterations < 5000u; iterations++) {
        struct pollfd pfd;
        int poll_rc;
        short revents = 0;
        cp_oem_auth_async_service(&async,
                                  (short)(async.fd >= 0 ? POLLOUT : 0),
                                  now);
        if (async.fd >= 0 &&
            (async.state == CP_OEM_AUTH_ASYNC_STATE_SENDING ||
             async.state == CP_OEM_AUTH_ASYNC_STATE_RECEIVING)) {
            break;
        }
        pfd.fd = async.fd;
        pfd.events = async.poll_events;
        pfd.revents = 0;
        poll_rc = poll(&pfd, async.fd >= 0 ? 1u : 0u, 1);
        if (poll_rc > 0) {
            revents = pfd.revents;
        }
        if (async.fd >= 0) {
            (void)cp_oem_auth_async_service(&async, revents, now);
        }
        now++;
    }
    stale = cp_oem_auth_async_cancel(&async, 9u);
    cancelled = cp_oem_auth_async_cancel(&async, 10u);
    taken = cp_oem_auth_async_take_completion(&async, &first);
    printf("first_taken=%u\n", taken ? 1u : 0u);
    printf("first_stale_cancel=%s\n",
           cp_oem_auth_async_error_name(stale));
    printf("first_cancel=%s\n",
           cp_oem_auth_async_error_name(cancelled));
    printf("first_error=%s\n",
           cp_oem_auth_async_error_name(first.error));
    printf("first_generation=%llu\n",
           (unsigned long long)first.generation);
    printf("first_request=%u\n", first.request_id);

    if (cp_oem_auth_async_begin_certificate(&async, 11u, now, now + 5000u) !=
        CP_OEM_AUTH_ASYNC_OK) {
        printf("second_begin_error=1\n");
        cp_oem_auth_async_destroy(&async);
        return 1;
    }
    for (iterations = 0u; iterations < 5000u; iterations++) {
        struct pollfd pfd;
        int poll_rc;
        short revents = 0;

        if (async.completion_ready) {
            break;
        }
        pfd.fd = async.fd;
        pfd.events = async.poll_events;
        pfd.revents = 0;
        poll_rc = poll(&pfd, async.fd >= 0 ? 1u : 0u, 1);
        if (poll_rc > 0) {
            revents = pfd.revents;
        }
        (void)cp_oem_auth_async_service(&async, revents, now);
        now++;
    }
    taken = cp_oem_auth_async_take_completion(&async, &second);
    printf("second_taken=%u\n", taken ? 1u : 0u);
    printf("second_error=%s\n",
           cp_oem_auth_async_error_name(second.error));
    printf("second_generation=%llu\n",
           (unsigned long long)second.generation);
    printf("second_request=%u\n", second.request_id);
    printf("second_length=%u\n", second.length);
    printf("second_hex=");
    print_hex(second.bytes, second.length);
    putchar('\n');
    printf("fd=%d\n", cp_oem_auth_async_get_fd(&async));
    cp_oem_auth_async_destroy(&async);
    return second.error == CP_OEM_AUTH_ASYNC_OK ? 0 : (int)second.error;
}

int main(int argc, char **argv)
{
    if (argc == 5 && strcmp(argv[1], "--async-run") == 0) {
        char *end = NULL;
        unsigned long timeout = strtoul(argv[4], &end, 10);
        if (end == argv[4] || *end != '\0' || timeout > 60000u) {
            return 2;
        }
        return run_one(argv[2], argv[3], (uint32_t)timeout);
    }
    if (argc == 3 && strcmp(argv[1], "--async-two") == 0) {
        return run_two(argv[2]);
    }
    if (argc != 1) {
        return 2;
    }

    test_init_and_destroy();
    test_begin_validation();
    test_timeout_and_clock();
    test_path_failures();
    test_destroy_zeroization();

    if (failures != 0) {
        fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("carplay oem auth async: %d checks passed\n", checks);
    return 0;
}
