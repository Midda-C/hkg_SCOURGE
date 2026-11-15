/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 * Linux-only bounded receive endpoint for the experimental SPP hook.
 * Not opened by the existing daemon or installer. Never log frame contents.
 */
#ifndef CP_SPP_SOCKET_H
#define CP_SPP_SOCKET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "cp_spp_rx.h"

#define CP_SPP_SOCKET_PATH CP_SPP_RX_SOCKET_PATH
#define CP_SPP_SOCKET_HEADER CP_SPP_RX_HEADER_SIZE
#define CP_SPP_SOCKET_MAX_PAYLOAD CP_SPP_RX_MAX_PAYLOAD
#define CP_SPP_SOCKET_MAX_FRAME (CP_SPP_SOCKET_HEADER + CP_SPP_SOCKET_MAX_PAYLOAD)

typedef struct {
    int fd;
    int directory_fd;
    dev_t device;
    ino_t inode;
} cp_spp_socket;

typedef struct {
    char peer[18];
    uint8_t uuid_kind;
    const uint8_t *payload; /* Borrowed from caller buffer, never retained. */
    size_t payload_length;
    pid_t sender_pid;
} cp_spp_packet;

/* Initialize before open; a failed open leaves the object safe to close. */
void cp_spp_socket_init(cp_spp_socket *receiver);
/* Requires root and an existing root-owned 0700 final directory. Refuses an
 * existing endpoint (including stale sockets); never removes unknown paths.
 * Returns 0 or -1 with errno. This does not create directories. */
int cp_spp_socket_open(cp_spp_socket *receiver);
/* Return 1 for a validated root-origin packet, 0 for no data (EAGAIN), -1 for
 * a rejected datagram or I/O error (errno). A bad datagram is consumed. */
int cp_spp_socket_receive(cp_spp_socket *receiver, uint8_t *buffer,
                          size_t capacity, cp_spp_packet *packet);
/* Remove only the same socket inode this object created. */
void cp_spp_socket_close(cp_spp_socket *receiver);
/* Pure wire decoder; no credential trust established by this function. */
int cp_spp_packet_decode(const uint8_t *buffer, size_t length,
                          cp_spp_packet *packet);

#ifdef CP_SPP_SOCKET_TESTING
/* Fixture builds only: same checks with current uid and a private test path. */
int cp_spp_socket_open_fixture(cp_spp_socket *receiver, const char *path);
#endif
#endif
