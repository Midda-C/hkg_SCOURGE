/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 */
#define _GNU_SOURCE
#include "cp_spp_socket.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static uid_t endpoint_uid(void)
{
#ifdef CP_SPP_SOCKET_TESTING
    return geteuid();
#else
    return 0;
#endif
}

void cp_spp_socket_init(cp_spp_socket *receiver)
{
    if (receiver != NULL) {
        memset(receiver, 0, sizeof(*receiver));
        receiver->fd = -1;
        receiver->directory_fd = -1;
    }
}

static bool hex_digit(uint8_t c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

int cp_spp_packet_decode(const uint8_t *buffer, size_t length,
                          cp_spp_packet *packet)
{
    uint32_t payload_length;
    size_t i;
    if (packet != NULL) memset(packet, 0, sizeof(*packet));
    if (buffer == NULL || packet == NULL || length < CP_SPP_SOCKET_HEADER ||
        length > CP_SPP_SOCKET_MAX_FRAME ||
        memcmp(buffer, CP_SPP_RX_MAGIC, CP_SPP_RX_MAGIC_LEN) != 0) goto invalid;
    payload_length = (uint32_t)buffer[CP_SPP_RX_OFF_DATALEN] |
                     ((uint32_t)buffer[CP_SPP_RX_OFF_DATALEN + 1] << 8) |
                     ((uint32_t)buffer[CP_SPP_RX_OFF_DATALEN + 2] << 16) |
                     ((uint32_t)buffer[CP_SPP_RX_OFF_DATALEN + 3] << 24);
    if (payload_length == 0 || payload_length > CP_SPP_SOCKET_MAX_PAYLOAD ||
        length != CP_SPP_SOCKET_HEADER + (size_t)payload_length ||
        buffer[CP_SPP_RX_OFF_UUID_KIND] < CP_SPP_RX_UUID_KIND_IAP2_ACCESSORY_CAFF ||
        buffer[CP_SPP_RX_OFF_UUID_KIND] > CP_SPP_RX_UUID_KIND_CARPLAY ||
        buffer[CP_SPP_RX_OFF_RESERVED] != 0 || buffer[CP_SPP_RX_OFF_RESERVED + 1] != 0)
        goto invalid;
    for (i = 0; i < CP_SPP_RX_PEER_LEN; i++) {
        if ((i % 3 == 2) ? buffer[CP_SPP_RX_OFF_PEER + i] != ':' :
                          !hex_digit(buffer[CP_SPP_RX_OFF_PEER + i]))
            goto invalid;
    }
    memcpy(packet->peer, buffer + CP_SPP_RX_OFF_PEER, CP_SPP_RX_PEER_LEN);
    packet->uuid_kind = buffer[CP_SPP_RX_OFF_UUID_KIND];
    packet->payload = buffer + CP_SPP_SOCKET_HEADER;
    packet->payload_length = payload_length;
    return 0;
invalid:
    errno = EBADMSG;
    return -1;
}

/* Open each directory component without following symlinks. */
static int directory_open(const char *path)
{
    char copy[sizeof(((struct sockaddr_un *)0)->sun_path)];
    char *part, *save = NULL;
    int fd, next;
    if (strlen(path) >= sizeof(copy)) { errno = ENAMETOOLONG; return -1; }
    strcpy(copy, path);
    fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    for (part = strtok_r(copy, "/", &save); part != NULL;
         part = strtok_r(NULL, "/", &save)) {
        if (strcmp(part, ".") == 0 || strcmp(part, "..") == 0) {
            close(fd); errno = EINVAL; return -1;
        }
        next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) { int error = errno; close(fd); errno = error; return -1; }
        close(fd);
        fd = next;
    }
    return fd;
}

static int open_path(cp_spp_socket *receiver, const char *path)
{
    struct sockaddr_un address;
    struct stat st;
    char parent[sizeof(address.sun_path)];
    char *slash;
    int enabled = 1, error;
    if (receiver == NULL || receiver->fd != -1 || receiver->directory_fd != -1 ||
        path == NULL || path[0] != '/' || strlen(path) >= sizeof(parent)) {
        errno = EINVAL; return -1;
    }
    if (geteuid() != endpoint_uid()) { errno = EPERM; return -1; }
    strcpy(parent, path);
    slash = strrchr(parent, '/');
    /* Only the fixed basename is supported, keeping cleanup narrowly scoped. */
    if (slash == parent || strcmp(slash + 1, "rx.sock") != 0) {
        errno = EINVAL; return -1;
    }
    *slash = '\0';
    receiver->directory_fd = directory_open(parent);
    if (receiver->directory_fd < 0) return -1;
    if (fstat(receiver->directory_fd, &st) < 0) goto failure;
    if (st.st_uid != endpoint_uid() || (st.st_mode & 07777) != 0700) {
        errno = EPERM; goto failure;
    }
    if (fstatat(receiver->directory_fd, "rx.sock", &st, AT_SYMLINK_NOFOLLOW) == 0) {
        errno = EEXIST; goto failure;
    }
    if (errno != ENOENT) goto failure;
    receiver->fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (receiver->fd < 0) goto failure;
    if (setsockopt(receiver->fd, SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) < 0)
        goto failure;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    if (bind(receiver->fd, (struct sockaddr *)&address, sizeof(address)) < 0)
        goto failure;
    if (fstatat(receiver->directory_fd, "rx.sock", &st, AT_SYMLINK_NOFOLLOW) < 0)
        goto failure;
    if (!S_ISSOCK(st.st_mode) || st.st_uid != endpoint_uid()) {
        errno = EPERM; goto failure;
    }
    receiver->device = st.st_dev;
    receiver->inode = st.st_ino;
    if (fchmodat(receiver->directory_fd, "rx.sock", 0600, 0) < 0) goto failure;
    return 0;
failure:
    error = errno;
    cp_spp_socket_close(receiver);
    errno = error;
    return -1;
}

int cp_spp_socket_open(cp_spp_socket *receiver)
{
    return open_path(receiver, CP_SPP_SOCKET_PATH);
}

#ifdef CP_SPP_SOCKET_TESTING
int cp_spp_socket_open_fixture(cp_spp_socket *receiver, const char *path)
{
    return open_path(receiver, path);
}
#endif

int cp_spp_socket_receive(cp_spp_socket *receiver, uint8_t *buffer,
                          size_t capacity, cp_spp_packet *packet)
{
    union { struct cmsghdr alignment; char bytes[CMSG_SPACE(sizeof(struct ucred))]; } control;
    struct iovec vector;
    struct msghdr message;
    struct cmsghdr *header;
    struct ucred credentials;
    ssize_t count;
    bool found = false, unexpected = false;
    if (packet != NULL) memset(packet, 0, sizeof(*packet));
    if (receiver == NULL || receiver->fd < 0 || buffer == NULL ||
        packet == NULL || capacity < CP_SPP_SOCKET_MAX_FRAME) {
        errno = EINVAL; return -1;
    }
    memset(&message, 0, sizeof(message));
    memset(&control, 0, sizeof(control));
    vector.iov_base = buffer;
    vector.iov_len = CP_SPP_SOCKET_MAX_FRAME;
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    count = recvmsg(receiver->fd, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (count < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    for (header = CMSG_FIRSTHDR(&message); header != NULL;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_CREDENTIALS &&
            header->cmsg_len == CMSG_LEN(sizeof(credentials)) && !found) {
            memcpy(&credentials, CMSG_DATA(header), sizeof(credentials));
            found = true;
        } else {
            /* Unexpected rights are still installed by recvmsg. Close any
             * delivered descriptors even when ancillary data was truncated. */
            if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS &&
                header->cmsg_len >= CMSG_LEN(0)) {
                size_t i, bytes = header->cmsg_len - CMSG_LEN(0);
                for (i = 0; i + sizeof(int) <= bytes; i += sizeof(int)) {
                    int descriptor;
                    memcpy(&descriptor, (char *)CMSG_DATA(header) + i, sizeof(descriptor));
                    close(descriptor);
                }
            }
            unexpected = true;
        }
    }
    if (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) { errno = EMSGSIZE; return -1; }
    if (unexpected) { errno = EPERM; return -1; }
    if (!found || credentials.uid != endpoint_uid() || credentials.pid <= 0) {
        errno = EPERM; return -1;
    }
    if (cp_spp_packet_decode(buffer, (size_t)count, packet) < 0) return -1;
    packet->sender_pid = credentials.pid;
    return 1;
}

void cp_spp_socket_close(cp_spp_socket *receiver)
{
    struct stat st;
    if (receiver == NULL) return;
    if (receiver->fd >= 0) close(receiver->fd);
    if (receiver->directory_fd >= 0) {
        if (receiver->inode != 0 &&
            fstatat(receiver->directory_fd, "rx.sock", &st, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISSOCK(st.st_mode) && st.st_dev == receiver->device &&
            st.st_ino == receiver->inode && st.st_uid == endpoint_uid())
            (void)unlinkat(receiver->directory_fd, "rx.sock", 0);
        close(receiver->directory_fd);
    }
    cp_spp_socket_init(receiver);
}
