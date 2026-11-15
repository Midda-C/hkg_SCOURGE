/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Nonblocking, caller-driven MFi helper RPC.  No poll, sleep, allocation,
 * pthreads, stdio or logging.  See cp_oem_auth_async.h.
 */

#define _GNU_SOURCE
#include "cp_oem_auth_async.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

_Static_assert(CP_OEM_AUTH_ASYNC_MAX_CERTIFICATE <= CP_WIRE_MAX_AUTH_BLOB,
               "async certificate cap must fit cp_bootstrap auth blob");
_Static_assert(CP_OEM_AUTH_ASYNC_MAX_CERTIFICATE <=
                   CP_OEM_AUTH_MAX_CERTIFICATE,
               "async certificate cap exceeds helper maximum");
_Static_assert(CP_OEM_AUTH_ASYNC_MAX_SIGNATURE <=
                   CP_OEM_AUTH_MAX_SIGNATURE,
               "async signature cap exceeds helper maximum");
_Static_assert(CP_OEM_AUTH_ASYNC_RX_SIZE >=
                   CP_OEM_AUTH_RPC_RESPONSE_LEN +
                       CP_OEM_AUTH_ASYNC_MAX_CERTIFICATE,
               "async receive buffer too small");

static size_t async_strlen(const char *text, size_t limit)
{
    size_t n = 0u;

    if (text == NULL) {
        return limit + 1u;
    }
    while (n <= limit && text[n] != '\0') {
        n++;
    }
    return n;
}

static int async_lstat(const char *path, struct stat *st)
{
    return (int)syscall(SYS_newfstatat, AT_FDCWD, path, st,
                        AT_SYMLINK_NOFOLLOW);
}

static bool async_parent_dir(const char *path, char *out, size_t cap)
{
    const char *slash;
    size_t n;

    if (path == NULL || out == NULL || cap == 0u) {
        return false;
    }
    slash = strrchr(path, '/');
    if (slash == NULL) {
        return false;
    }
    n = (size_t)(slash - path);
    if (n == 0u) {
        n = 1u;
    }
    if (n + 1u > cap) {
        return false;
    }
    memcpy(out, path, n);
    out[n] = '\0';
    return true;
}

static uint32_t async_expected_uid(const cp_oem_auth_async *async)
{
#ifdef CP_OEM_AUTH_ASYNC_TEST
    return async->expected_peer_uid;
#else
    (void)async;
    return 0u;
#endif
}

static void async_close_fd(cp_oem_auth_async *async)
{
    if (async->fd >= 0) {
        (void)close(async->fd);
        async->fd = -1;
    }
    async->poll_events = 0;
}

static void async_wipe_transport(cp_oem_auth_async *async)
{
    cp_oem_auth_secure_zero(async->tx, sizeof(async->tx));
    cp_oem_auth_secure_zero(async->rx, sizeof(async->rx));
    async->tx_len = 0u;
}

static uint16_t async_wire_op(cp_oem_auth_async_op op)
{
    return op == CP_OEM_AUTH_ASYNC_OP_CERTIFICATE
               ? (uint16_t)CP_OEM_AUTH_RPC_OP_CERTIFICATE
               : (uint16_t)CP_OEM_AUTH_RPC_OP_SIGNATURE;
}

static cp_oem_auth_async_error async_fail(cp_oem_auth_async *async,
                                          cp_oem_auth_async_error error,
                                          cp_oem_auth_error helper_status)
{
    async_close_fd(async);
    memset(&async->completion, 0, sizeof(async->completion));
    async->completion.op = async->op;
    async->completion.generation = async->generation;
    async->completion.request_id = async->request_id;
    async->completion.error = error;
    async->completion.helper_status = helper_status;
    async->completion.length = 0u;
    async->completion_ready = true;
    async->state = CP_OEM_AUTH_ASYNC_STATE_DONE;
    async_wipe_transport(async);
    return error;
}

static cp_oem_auth_async_error async_succeed(cp_oem_auth_async *async,
                                             const uint8_t *payload,
                                             size_t length)
{
    if (payload == NULL || length == 0u ||
        length > sizeof(async->completion.bytes)) {
        return async_fail(async, CP_OEM_AUTH_ASYNC_ERR_TOO_LARGE,
                          CP_OEM_AUTH_OK);
    }
    async_close_fd(async);
    memset(&async->completion, 0, sizeof(async->completion));
    async->completion.op = async->op;
    async->completion.generation = async->generation;
    async->completion.request_id = async->request_id;
    async->completion.error = CP_OEM_AUTH_ASYNC_OK;
    async->completion.helper_status = CP_OEM_AUTH_OK;
    async->completion.length = (uint32_t)length;
    memcpy(async->completion.bytes, payload, length);
    async->completion_ready = true;
    async->state = CP_OEM_AUTH_ASYNC_STATE_DONE;
    async_wipe_transport(async);
    return CP_OEM_AUTH_ASYNC_OK;
}

static cp_oem_auth_async_error async_check_path(
    const cp_oem_auth_async *async)
{
    struct stat socket_stat;
    struct stat dir_stat;
    char parent[CP_OEM_AUTH_SOCKET_PATH_MAX];
    uint32_t uid = async_expected_uid(async);

    if (async_lstat(async->socket_path, &socket_stat) != 0) {
        return CP_OEM_AUTH_ASYNC_ERR_PATH;
    }
    if (!S_ISSOCK(socket_stat.st_mode) ||
        (uint32_t)socket_stat.st_uid != uid ||
        (socket_stat.st_mode & 0777u) != 0600u) {
        return CP_OEM_AUTH_ASYNC_ERR_PATH;
    }
    if (!async_parent_dir(async->socket_path, parent, sizeof(parent))) {
        return CP_OEM_AUTH_ASYNC_ERR_PATH;
    }
    if (async_lstat(parent, &dir_stat) != 0) {
        return CP_OEM_AUTH_ASYNC_ERR_PATH;
    }
    if (!S_ISDIR(dir_stat.st_mode) || (uint32_t)dir_stat.st_uid != uid ||
        (dir_stat.st_mode & 0777u) != 0700u) {
        return CP_OEM_AUTH_ASYNC_ERR_PATH;
    }
    return CP_OEM_AUTH_ASYNC_OK;
}

static cp_oem_auth_async_error async_peer_ok(
    const cp_oem_auth_async *async)
{
    struct ucred cred;
    socklen_t cred_len = (socklen_t)sizeof(cred);

    if (getsockopt(async->fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) != 0 ||
        cred_len != (socklen_t)sizeof(cred) ||
        (uint32_t)cred.uid != async_expected_uid(async)) {
        return CP_OEM_AUTH_ASYNC_ERR_PEER;
    }
    return CP_OEM_AUTH_ASYNC_OK;
}

/* Returns OK when connected or connect-pending; failure error otherwise. */
static cp_oem_auth_async_error async_connect_start(
    cp_oem_auth_async *async)
{
    struct sockaddr_un addr;
    cp_oem_auth_async_error path_error;
    int fd;
    int flags;
    int rc;

    path_error = async_check_path(async);
    if (path_error != CP_OEM_AUTH_ASYNC_OK) {
        return async_fail(async, path_error, CP_OEM_AUTH_OK);
    }
    fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) {
        return async_fail(async, CP_OEM_AUTH_ASYNC_ERR_CONNECT,
                          CP_OEM_AUTH_OK);
    }
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        (void)close(fd);
        return async_fail(async, CP_OEM_AUTH_ASYNC_ERR_CONNECT,
                          CP_OEM_AUTH_OK);
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, async->socket_path,
           strlen(async->socket_path) + 1u);
    async->fd = fd;
    rc = connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (rc == 0) {
        async->state = CP_OEM_AUTH_ASYNC_STATE_SENDING;
        async->poll_events = POLLOUT;
        return CP_OEM_AUTH_ASYNC_OK; /* connected */
    }
    if (errno == EINPROGRESS || errno == EINTR || errno == EAGAIN ||
        errno == EWOULDBLOCK) {
        async->state = CP_OEM_AUTH_ASYNC_STATE_CONNECTING;
        async->poll_events = POLLOUT;
        return CP_OEM_AUTH_ASYNC_OK;
    }
    return async_fail(async, CP_OEM_AUTH_ASYNC_ERR_CONNECT, CP_OEM_AUTH_OK);
}

static cp_oem_auth_async_error async_try_send(cp_oem_auth_async *async)
{
    ssize_t sent;

    async->state = CP_OEM_AUTH_ASYNC_STATE_SENDING;
    async->poll_events = POLLOUT;
    sent = send(async->fd, async->tx, async->tx_len,
                MSG_NOSIGNAL | MSG_DONTWAIT);
    if (sent == (ssize_t)async->tx_len) {
        async->state = CP_OEM_AUTH_ASYNC_STATE_RECEIVING;
        async->poll_events = POLLIN;
        return CP_OEM_AUTH_ASYNC_OK;
    }
    if (sent < 0 &&
        (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return CP_OEM_AUTH_ASYNC_OK;
    }
    if (sent >= 0 && sent != (ssize_t)async->tx_len) {
        /* SOCK_SEQPACKET records are atomic; a short send is not a stream
         * partial write and must never be retried as one. */
        return async_fail(async, CP_OEM_AUTH_ASYNC_ERR_PROTOCOL,
                          CP_OEM_AUTH_OK);
    }
    return async_fail(async, CP_OEM_AUTH_ASYNC_ERR_SEND, CP_OEM_AUTH_OK);
}

static cp_oem_auth_async_error async_peer_then_send(
    cp_oem_auth_async *async)
{
    cp_oem_auth_async_error peer_error = async_peer_ok(async);

    if (peer_error != CP_OEM_AUTH_ASYNC_OK) {
        return async_fail(async, peer_error, CP_OEM_AUTH_OK);
    }
    return async_try_send(async);
}

static cp_oem_auth_async_error async_try_connect(cp_oem_auth_async *async)
{
    cp_oem_auth_async_error error;
    int so_error = 0;
    socklen_t so_len = (socklen_t)sizeof(so_error);
    int rc;

    if (async->fd < 0) {
        error = async_connect_start(async);
        if (error != CP_OEM_AUTH_ASYNC_OK) {
            return error;
        }
        if (async->state == CP_OEM_AUTH_ASYNC_STATE_CONNECTING) {
            return CP_OEM_AUTH_ASYNC_OK;
        }
        return async_peer_then_send(async);
    }

    if (getsockopt(async->fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) != 0 ||
        so_len != (socklen_t)sizeof(so_error) || so_error != 0) {
        return async_fail(async, CP_OEM_AUTH_ASYNC_ERR_CONNECT,
                          CP_OEM_AUTH_OK);
    }
    /* Retry connect once.  This also handles an earlier EAGAIN/EINTR without
     * treating an unconnected socket as connected. */
    {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        memcpy(addr.sun_path, async->socket_path,
               strlen(async->socket_path) + 1u);
        rc = connect(async->fd, (const struct sockaddr *)&addr, sizeof(addr));
    }
    if (rc == 0 || (rc != 0 && errno == EISCONN)) {
        return async_peer_then_send(async);
    }
    if (errno == EINPROGRESS || errno == EALREADY || errno == EINTR ||
        errno == EAGAIN || errno == EWOULDBLOCK) {
        async->state = CP_OEM_AUTH_ASYNC_STATE_CONNECTING;
        async->poll_events = POLLOUT;
        return CP_OEM_AUTH_ASYNC_OK;
    }
    return async_fail(async, CP_OEM_AUTH_ASYNC_ERR_CONNECT, CP_OEM_AUTH_OK);
}

static cp_oem_auth_async_error async_try_receive(cp_oem_auth_async *async)
{
    struct msghdr msg;
    struct iovec iov;
    unsigned char control[CMSG_SPACE(sizeof(int) * 4u)];
    struct cmsghdr *cmsg;
    ssize_t received;
    size_t record_len;
    cp_oem_auth_rpc_response response;
    bool rights = false;
    int flags = MSG_DONTWAIT;

    memset(&msg, 0, sizeof(msg));
    memset(control, 0, sizeof(control));
    iov.iov_base = async->rx;
    iov.iov_len = sizeof(async->rx);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
#ifdef MSG_CMSG_CLOEXEC
    flags |= MSG_CMSG_CLOEXEC;
#endif
    received = recvmsg(async->fd, &msg, flags);
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            async->poll_events = POLLIN;
            return CP_OEM_AUTH_ASYNC_OK;
        }
        return async_fail(async, CP_OEM_AUTH_ASYNC_ERR_RECV, CP_OEM_AUTH_OK);
    }
    if (received == 0) {
        return async_fail(async, CP_OEM_AUTH_ASYNC_ERR_RECV, CP_OEM_AUTH_OK);
    }

    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET &&
            cmsg->cmsg_type == SCM_RIGHTS) {
            size_t data_len;
            size_t count;
            size_t i;
            int *fds;

            if (cmsg->cmsg_len < CMSG_LEN(0u)) {
                continue;
            }
            data_len = (size_t)cmsg->cmsg_len - CMSG_LEN(0u);
            fds = (int *)CMSG_DATA(cmsg);
            count = data_len / sizeof(int);
            for (i = 0; i < count; i++) {
                if (fds[i] >= 0) {
                    (void)close(fds[i]);
                    async->rights_fds_closed++;
                }
            }
            async->rights_fds_received += (uint32_t)count;
            rights = true;
        }
    }
    if (rights || (msg.msg_flags & MSG_CTRUNC) != 0) {
        return async_fail(async, CP_OEM_AUTH_ASYNC_ERR_RIGHTS,
                          CP_OEM_AUTH_OK);
    }
    if ((msg.msg_flags & MSG_TRUNC) != 0) {
        return async_fail(async, CP_OEM_AUTH_ASYNC_ERR_TRUNCATED,
                          CP_OEM_AUTH_OK);
    }
    record_len = (size_t)received;
    if (record_len < CP_OEM_AUTH_RPC_RESPONSE_LEN ||
        record_len > sizeof(async->rx)) {
        return async_fail(async, CP_OEM_AUTH_ASYNC_ERR_TRUNCATED,
                          CP_OEM_AUTH_OK);
    }
    if (!cp_oem_auth_rpc_response_decode(async->rx, record_len, &response)) {
        return async_fail(async, CP_OEM_AUTH_ASYNC_ERR_PROTOCOL,
                          CP_OEM_AUTH_OK);
    }
    if (response.request_id != async->request_id ||
        response.op != async_wire_op(async->op)) {
        return async_fail(async, CP_OEM_AUTH_ASYNC_ERR_PROTOCOL,
                          CP_OEM_AUTH_OK);
    }
    if (response.status != 0) {
        if (response.status < 1 ||
            response.status > (int32_t)CP_OEM_AUTH_ERR_INTERNAL ||
            response.payload_len != 0u ||
            record_len != CP_OEM_AUTH_RPC_RESPONSE_LEN) {
            return async_fail(async, CP_OEM_AUTH_ASYNC_ERR_PROTOCOL,
                              CP_OEM_AUTH_OK);
        }
        return async_fail(async, CP_OEM_AUTH_ASYNC_ERR_HELPER,
                          (cp_oem_auth_error)response.status);
    }
    if (response.payload_len == 0u ||
        record_len != CP_OEM_AUTH_RPC_RESPONSE_LEN + response.payload_len) {
        return async_fail(async, CP_OEM_AUTH_ASYNC_ERR_PROTOCOL,
                          CP_OEM_AUTH_OK);
    }
    if (async->op == CP_OEM_AUTH_ASYNC_OP_CERTIFICATE) {
        if (response.payload_len > CP_OEM_AUTH_ASYNC_MAX_CERTIFICATE) {
            return async_fail(async, CP_OEM_AUTH_ASYNC_ERR_TOO_LARGE,
                              CP_OEM_AUTH_OK);
        }
    } else if (response.payload_len > CP_OEM_AUTH_ASYNC_MAX_SIGNATURE) {
        return async_fail(async, CP_OEM_AUTH_ASYNC_ERR_TOO_LARGE,
                          CP_OEM_AUTH_OK);
    }
    return async_succeed(async,
                         async->rx + CP_OEM_AUTH_RPC_RESPONSE_LEN,
                         response.payload_len);
}

cp_oem_auth_async_error cp_oem_auth_async_init(
    cp_oem_auth_async *async, const cp_oem_auth_async_config *config)
{
    const char *path;
    size_t path_len;

    if (async == NULL || config == NULL) {
        return CP_OEM_AUTH_ASYNC_ERR_ARGUMENT;
    }
    if (async->initialized) {
        return CP_OEM_AUTH_ASYNC_ERR_STATE;
    }
    memset(async, 0, sizeof(*async));
    async->fd = -1;
    async->next_request_id = 1u;
    async->state = CP_OEM_AUTH_ASYNC_STATE_IDLE;
    async->op = CP_OEM_AUTH_ASYNC_OP_NONE;
    async->last_now_ms = 0u;

    if (!config->experimental) {
        return CP_OEM_AUTH_ASYNC_ERR_DISABLED;
    }
#ifdef CP_OEM_AUTH_ASYNC_TEST
    path = config->socket_path != NULL ? config->socket_path
                                       : CP_OEM_AUTH_RPC_DEFAULT_SOCKET;
#else
    path = CP_OEM_AUTH_RPC_DEFAULT_SOCKET;
#endif
    path_len = async_strlen(path, CP_OEM_AUTH_SOCKET_PATH_MAX);
    if (path_len == 0u || path_len >= CP_OEM_AUTH_SOCKET_PATH_MAX ||
        path[0] != '/' || path[path_len - 1u] == '/') {
        return CP_OEM_AUTH_ASYNC_ERR_ARGUMENT;
    }
    memcpy(async->socket_path, path, path_len + 1u);
#ifdef CP_OEM_AUTH_ASYNC_TEST
    async->expected_peer_uid = config->expected_peer_uid;
#endif
    async->experimental = true;
    async->initialized = true;
    return CP_OEM_AUTH_ASYNC_OK;
}

static cp_oem_auth_async_error async_begin(
    cp_oem_auth_async *async, cp_oem_auth_async_op op,
    uint64_t generation, const uint8_t *digest, size_t digest_len,
    uint64_t now_ms, uint64_t deadline_ms)
{
    cp_oem_auth_rpc_request request;
    uint32_t request_id;

    if (async == NULL || !async->initialized) {
        return CP_OEM_AUTH_ASYNC_ERR_ARGUMENT;
    }
    if (!async->experimental) {
        return CP_OEM_AUTH_ASYNC_ERR_DISABLED;
    }
    if (async->completion_ready ||
        async->state != CP_OEM_AUTH_ASYNC_STATE_IDLE) {
        return CP_OEM_AUTH_ASYNC_ERR_BUSY;
    }
    if (now_ms < async->last_now_ms) {
        return CP_OEM_AUTH_ASYNC_ERR_CLOCK;
    }
    if (deadline_ms <= now_ms) {
        return CP_OEM_AUTH_ASYNC_ERR_DEADLINE;
    }
    async_close_fd(async);
    request_id = async->next_request_id++;
    if (request_id == 0u) {
        request_id = async->next_request_id++;
    }
    memset(&request, 0, sizeof(request));
    request.magic = CP_OEM_AUTH_RPC_MAGIC;
    request.version = CP_OEM_AUTH_RPC_VERSION;
    request.op = async_wire_op(op);
    request.request_id = request_id;
    request.payload_len = (uint32_t)digest_len;
    request.reserved = 0u;
    memset(async->tx, 0, sizeof(async->tx));
    cp_oem_auth_rpc_request_encode(async->tx, &request);
    if (digest_len != 0u) {
        memcpy(async->tx + CP_OEM_AUTH_RPC_REQUEST_LEN, digest, digest_len);
    }
    async->tx_len = CP_OEM_AUTH_RPC_REQUEST_LEN + (uint32_t)digest_len;
    memset(&async->completion, 0, sizeof(async->completion));
    async->completion_ready = false;
    async->op = op;
    async->generation = generation;
    async->request_id = request_id;
    async->deadline_ms = deadline_ms;
    async->last_now_ms = now_ms;
    async->state = CP_OEM_AUTH_ASYNC_STATE_CONNECTING;
    async->poll_events = 0;
    async->fd = -1;
    return CP_OEM_AUTH_ASYNC_OK;
}

cp_oem_auth_async_error cp_oem_auth_async_begin_certificate(
    cp_oem_auth_async *async, uint64_t generation,
    uint64_t now_ms, uint64_t deadline_ms)
{
    return async_begin(async, CP_OEM_AUTH_ASYNC_OP_CERTIFICATE, generation,
                       NULL, 0u, now_ms, deadline_ms);
}

cp_oem_auth_async_error cp_oem_auth_async_begin_signature(
    cp_oem_auth_async *async, uint64_t generation,
    const uint8_t *digest, size_t digest_len,
    uint64_t now_ms, uint64_t deadline_ms)
{
    if (digest == NULL) {
        return CP_OEM_AUTH_ASYNC_ERR_ARGUMENT;
    }
    if (digest_len != CP_OEM_AUTH_CHALLENGE_SHA1 &&
        digest_len != CP_OEM_AUTH_CHALLENGE_SHA256) {
        return CP_OEM_AUTH_ASYNC_ERR_CHALLENGE_LENGTH;
    }
    return async_begin(async, CP_OEM_AUTH_ASYNC_OP_SIGNATURE, generation,
                       digest, digest_len, now_ms, deadline_ms);
}

cp_oem_auth_async_error cp_oem_auth_async_service(
    cp_oem_auth_async *async, short revents, uint64_t now_ms)
{
    if (async == NULL || !async->initialized) {
        return CP_OEM_AUTH_ASYNC_ERR_ARGUMENT;
    }
    if (async->state == CP_OEM_AUTH_ASYNC_STATE_IDLE ||
        async->state == CP_OEM_AUTH_ASYNC_STATE_DONE) {
        return CP_OEM_AUTH_ASYNC_ERR_STATE;
    }
    if (now_ms < async->last_now_ms) {
        return async_fail(async, CP_OEM_AUTH_ASYNC_ERR_CLOCK,
                          CP_OEM_AUTH_OK);
    }
    async->last_now_ms = now_ms;
    if (now_ms >= async->deadline_ms) {
        return async_fail(async, CP_OEM_AUTH_ASYNC_ERR_TIMEOUT,
                          CP_OEM_AUTH_OK);
    }
    if ((revents & POLLNVAL) != 0) {
        return async_fail(async, CP_OEM_AUTH_ASYNC_ERR_FD, CP_OEM_AUTH_OK);
    }
    switch (async->state) {
    case CP_OEM_AUTH_ASYNC_STATE_CONNECTING:
        return async_try_connect(async);
    case CP_OEM_AUTH_ASYNC_STATE_SENDING:
        return async_try_send(async);
    case CP_OEM_AUTH_ASYNC_STATE_RECEIVING:
        return async_try_receive(async);
    default:
        return CP_OEM_AUTH_ASYNC_ERR_STATE;
    }
}

bool cp_oem_auth_async_take_completion(
    cp_oem_auth_async *async, cp_oem_auth_async_completion *completion)
{
    if (async == NULL || completion == NULL || !async->completion_ready) {
        return false;
    }
    memset(completion, 0, sizeof(*completion));
    completion->op = async->completion.op;
    completion->generation = async->completion.generation;
    completion->request_id = async->completion.request_id;
    completion->error = async->completion.error;
    completion->helper_status = async->completion.helper_status;
    completion->length = async->completion.length;
    if (completion->length != 0u) {
        memcpy(completion->bytes, async->completion.bytes,
               completion->length);
    }
    cp_oem_auth_secure_zero(&async->completion, sizeof(async->completion));
    async->completion_ready = false;
    async->state = CP_OEM_AUTH_ASYNC_STATE_IDLE;
    async->op = CP_OEM_AUTH_ASYNC_OP_NONE;
    async->generation = 0u;
    async->request_id = 0u;
    async->poll_events = 0;
    async_wipe_transport(async);
    return true;
}

cp_oem_auth_async_error cp_oem_auth_async_cancel(
    cp_oem_auth_async *async, uint64_t generation)
{
    if (async == NULL || !async->initialized) {
        return CP_OEM_AUTH_ASYNC_ERR_ARGUMENT;
    }
    if (async->completion_ready) {
        return CP_OEM_AUTH_ASYNC_ERR_STATE;
    }
    if (async->state == CP_OEM_AUTH_ASYNC_STATE_IDLE) {
        async_close_fd(async);
        async_wipe_transport(async);
        return CP_OEM_AUTH_ASYNC_OK;
    }
    if (async->state == CP_OEM_AUTH_ASYNC_STATE_DONE) {
        return CP_OEM_AUTH_ASYNC_ERR_STATE;
    }
    if (generation != async->generation) {
        return CP_OEM_AUTH_ASYNC_ERR_GENERATION;
    }
    (void)async_fail(async, CP_OEM_AUTH_ASYNC_ERR_CANCELLED,
                     CP_OEM_AUTH_OK);
    return CP_OEM_AUTH_ASYNC_OK;
}

void cp_oem_auth_async_destroy(cp_oem_auth_async *async)
{
    if (async == NULL) {
        return;
    }
    if (async->initialized) {
        async_close_fd(async);
    }
    cp_oem_auth_secure_zero(async, sizeof(*async));
}

cp_oem_auth_async_state cp_oem_auth_async_get_state(
    const cp_oem_auth_async *async)
{
    return async != NULL ? async->state : CP_OEM_AUTH_ASYNC_STATE_IDLE;
}

int cp_oem_auth_async_get_fd(const cp_oem_auth_async *async)
{
    if (async == NULL || !async->initialized || async->fd < 0) {
        return -1;
    }
    return async->fd;
}

short cp_oem_auth_async_get_poll_events(const cp_oem_auth_async *async)
{
    if (async == NULL || !async->initialized) {
        return 0;
    }
    return async->poll_events;
}

bool cp_oem_auth_async_completion_ready(const cp_oem_auth_async *async)
{
    return async != NULL && async->completion_ready;
}

const char *cp_oem_auth_async_error_name(cp_oem_auth_async_error error)
{
    switch (error) {
    case CP_OEM_AUTH_ASYNC_OK:
        return "ok";
    case CP_OEM_AUTH_ASYNC_ERR_ARGUMENT:
        return "argument";
    case CP_OEM_AUTH_ASYNC_ERR_DISABLED:
        return "disabled";
    case CP_OEM_AUTH_ASYNC_ERR_BUSY:
        return "busy";
    case CP_OEM_AUTH_ASYNC_ERR_STATE:
        return "state";
    case CP_OEM_AUTH_ASYNC_ERR_GENERATION:
        return "generation";
    case CP_OEM_AUTH_ASYNC_ERR_CLOCK:
        return "clock";
    case CP_OEM_AUTH_ASYNC_ERR_DEADLINE:
        return "deadline";
    case CP_OEM_AUTH_ASYNC_ERR_PATH:
        return "path";
    case CP_OEM_AUTH_ASYNC_ERR_CONNECT:
        return "connect";
    case CP_OEM_AUTH_ASYNC_ERR_PEER:
        return "peer";
    case CP_OEM_AUTH_ASYNC_ERR_SEND:
        return "send";
    case CP_OEM_AUTH_ASYNC_ERR_RECV:
        return "recv";
    case CP_OEM_AUTH_ASYNC_ERR_PROTOCOL:
        return "protocol";
    case CP_OEM_AUTH_ASYNC_ERR_TRUNCATED:
        return "truncated";
    case CP_OEM_AUTH_ASYNC_ERR_RIGHTS:
        return "rights";
    case CP_OEM_AUTH_ASYNC_ERR_TOO_LARGE:
        return "too-large";
    case CP_OEM_AUTH_ASYNC_ERR_CHALLENGE_LENGTH:
        return "challenge-length";
    case CP_OEM_AUTH_ASYNC_ERR_HELPER:
        return "helper";
    case CP_OEM_AUTH_ASYNC_ERR_CANCELLED:
        return "cancelled";
    case CP_OEM_AUTH_ASYNC_ERR_TIMEOUT:
        return "timeout";
    case CP_OEM_AUTH_ASYNC_ERR_FD:
        return "fd";
    case CP_OEM_AUTH_ASYNC_ERR_INTERNAL:
        return "internal";
    default:
        return "unknown";
    }
}

const char *cp_oem_auth_async_state_name(cp_oem_auth_async_state state)
{
    switch (state) {
    case CP_OEM_AUTH_ASYNC_STATE_IDLE:
        return "idle";
    case CP_OEM_AUTH_ASYNC_STATE_CONNECTING:
        return "connecting";
    case CP_OEM_AUTH_ASYNC_STATE_SENDING:
        return "sending";
    case CP_OEM_AUTH_ASYNC_STATE_RECEIVING:
        return "receiving";
    case CP_OEM_AUTH_ASYNC_STATE_DONE:
        return "done";
    default:
        return "unknown";
    }
}
