/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * cp_spp_rx_hook — C3 experimental, build-only SPP receive hook.
 *
 * This is the only source file that knows the private OEM event-object ABI.
 * It is linked freestanding and placed at the pinned vbtd-63 ELF address by
 * packages/wifi1b/scripts/build_carplay_spp_patch.py.  It is not part of the
 * Wifi1b daemon, not activated by the installer, and not car-validated.
 *
 * OEM EVENT ABI (private; do not publish through cp_spp_rx.h)
 * ----------------------------------------------------------
 * vbtd-63 calls the replacement synchronously as:
 *
 *   void cp_spp_rx_hook(T_VBT_EVENT_BASE *event);   // event in RDI
 *
 * Recovered layout for T_VBT_SPP_RECEIVED_DATA_CFM:
 *
 *   +0x08  std::string peer address  (data pointer at +0x08,
 *                                      size at +0x10)
 *   +0x30  std::string UUID          (data pointer at +0x30,
 *                                      size at +0x38)
 *   +0x50  const uint8_t *payload
 *   +0x58  uint32_t payload length
 *
 * This assumes the OEM libstdc++ std::string layout: 32-byte object, data
 * pointer at offset 0, size at offset 8, SSO buffer at offset 16.  The
 * offline tests model that layout and include a host C++ ABI probe.  The
 * live target ABI remains an activation-time verification item.
 *
 * The payload is valid only for the duration of the synchronous callback.
 * sendmsg copies the header and payload into the socket queue before it
 * returns; the hook retains no pointer.
 *
 * SAFETY
 * ------
 * The hook uses only raw x86-64 syscalls.  It has no libc dependency, no
 * allocation, no logging, no threads, no locks, no blocking wait, no global
 * writable state and no signal generation.  It creates one ephemeral
 * AF_UNIX SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC socket per accepted event,
 * sends with MSG_DONTWAIT|MSG_NOSIGNAL, and closes it unconditionally.  A
 * missing receiver or full queue is a silent drop.  A FIFO is deliberately
 * not used because a disappearing reader can raise SIGPIPE and kill vbtd.
 *
 * The hook validates the event pointer, both std::string pointers/lengths,
 * the exact known UUIDs, and payload pointer/length before doing any I/O.
 * Unrelated UUID events remain ignored.
 */

#include "cp_spp_rx.h"

#include <stddef.h>
#include <stdint.h>

/* Private OEM event-object offsets.  Keep these out of cp_spp_rx.h. */
#define CP_SPP_RX_EVENT_OFF_PEER_PTR 0x08u
#define CP_SPP_RX_EVENT_OFF_PEER_LEN 0x10u
#define CP_SPP_RX_EVENT_OFF_UUID_PTR 0x30u
#define CP_SPP_RX_EVENT_OFF_UUID_LEN 0x38u
#define CP_SPP_RX_EVENT_OFF_PAYLOAD 0x50u
#define CP_SPP_RX_EVENT_OFF_PAYLOAD_LEN 0x58u

/* Linux x86-64 syscall and ABI constants.  No libc headers are used. */
#define CP_SPP_RX_AF_UNIX 1
#define CP_SPP_RX_SOCK_DGRAM 2
#define CP_SPP_RX_SOCK_NONBLOCK 00004000
#define CP_SPP_RX_SOCK_CLOEXEC 02000000
#define CP_SPP_RX_MSG_DONTWAIT 0x40
#define CP_SPP_RX_MSG_NOSIGNAL 0x4000
#define CP_SPP_RX_SYS_CLOSE 3
#define CP_SPP_RX_SYS_SOCKET 41
#define CP_SPP_RX_SYS_SENDMSG 46

/* Kernel sockaddr_un starts with the 2-byte sa_family_t.  The path is
 * copied into a stack array using the exact bounded size, so no uninitialised
 * bytes are read by the kernel. */
struct cp_spp_rx_iovec {
    void *base;
    size_t len;
};

struct cp_spp_rx_msghdr {
    void *name;
    uint32_t namelen;
    uint32_t pad0;
    struct cp_spp_rx_iovec *iov;
    uint64_t iovlen;
    void *control;
    uint64_t controllen;
    int32_t flags;
    uint32_t pad1;
};

_Static_assert(sizeof(struct cp_spp_rx_iovec) == 16u,
               "iovec ABI mismatch");
_Static_assert(offsetof(struct cp_spp_rx_iovec, base) == 0u,
               "iovec base ABI mismatch");
_Static_assert(offsetof(struct cp_spp_rx_iovec, len) == 8u,
               "iovec length ABI mismatch");
_Static_assert(sizeof(struct cp_spp_rx_msghdr) == 56u,
               "msghdr ABI mismatch");
_Static_assert(offsetof(struct cp_spp_rx_msghdr, name) == 0u,
               "msghdr name ABI mismatch");
_Static_assert(offsetof(struct cp_spp_rx_msghdr, namelen) == 8u,
               "msghdr namelen ABI mismatch");
_Static_assert(offsetof(struct cp_spp_rx_msghdr, iov) == 16u,
               "msghdr iov ABI mismatch");
_Static_assert(offsetof(struct cp_spp_rx_msghdr, iovlen) == 24u,
               "msghdr iovlen ABI mismatch");
_Static_assert(offsetof(struct cp_spp_rx_msghdr, control) == 32u,
               "msghdr control ABI mismatch");
_Static_assert(offsetof(struct cp_spp_rx_msghdr, controllen) == 40u,
               "msghdr controllen ABI mismatch");
_Static_assert(offsetof(struct cp_spp_rx_msghdr, flags) == 48u,
               "msghdr flags ABI mismatch");

static const char cp_spp_rx_path[] = CP_SPP_RX_SOCKET_PATH;
static const char cp_spp_rx_uuid_caff[] = CP_SPP_RX_UUID_IAP2_ACCESSORY_CAFF;
static const char cp_spp_rx_uuid_cafe[] = CP_SPP_RX_UUID_IAP2_PHONE_CAFE;
static const char cp_spp_rx_uuid_carplay[] = CP_SPP_RX_UUID_CARPLAY;

static inline long cp_spp_rx_syscall3(long nr, long a, long b, long c)
{
    long ret;

    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(nr), "D"(a), "S"(b), "d"(c)
                         : "rcx", "r11", "memory");
    return ret;
}

static inline char cp_spp_rx_ascii_upper(char c)
{
    if (c >= 'a' && c <= 'z') {
        return (char)(c - ('a' - 'A'));
    }
    return c;
}

static inline int cp_spp_rx_uuid_eq(const char *uuid, const char *known)
{
    size_t i;

    for (i = 0u; i < (size_t)CP_SPP_RX_UUID_LEN; i++) {
        if (cp_spp_rx_ascii_upper(uuid[i]) !=
            cp_spp_rx_ascii_upper(known[i])) {
            return 0;
        }
    }
    return 1;
}

static inline unsigned int cp_spp_rx_uuid_kind(const char *uuid)
{
    if (cp_spp_rx_uuid_eq(uuid, cp_spp_rx_uuid_carplay)) {
        return CP_SPP_RX_UUID_KIND_CARPLAY;
    }
    if (cp_spp_rx_uuid_eq(uuid, cp_spp_rx_uuid_caff)) {
        return CP_SPP_RX_UUID_KIND_IAP2_ACCESSORY_CAFF;
    }
    if (cp_spp_rx_uuid_eq(uuid, cp_spp_rx_uuid_cafe)) {
        return CP_SPP_RX_UUID_KIND_IAP2_PHONE_CAFE;
    }
    return CP_SPP_RX_UUID_KIND_NONE;
}

static inline int cp_spp_rx_peer_ok(const char *peer)
{
    size_t i;

    for (i = 0u; i < (size_t)CP_SPP_RX_PEER_LEN; i++) {
        unsigned char c = (unsigned char)peer[i];

        if (c < 0x20u || c > 0x7eu) {
            return 0;
        }
    }
    return 1;
}

__attribute__((section(".text.cp_spp_rx_hook"), used, visibility("default")))
void cp_spp_rx_hook(void *event)
{
    unsigned char *ev = (unsigned char *)event;
    const char *peer;
    const char *uuid;
    const unsigned char *payload;
    uint64_t peer_len;
    uint64_t uuid_len;
    uint32_t payload_len;
    unsigned int uuid_kind;
    unsigned char header[CP_SPP_RX_HEADER_SIZE];
    unsigned char address[2u + sizeof(cp_spp_rx_path)];
    struct cp_spp_rx_iovec iov[2];
    struct cp_spp_rx_msghdr msg;
    size_t i;
    long fd;

    if (ev == NULL) {
        return;
    }

    peer_len = *(const uint64_t *)(const void *)(ev +
                                                 CP_SPP_RX_EVENT_OFF_PEER_LEN);
    peer = *(const char *const *)(const void *)(ev +
                                                CP_SPP_RX_EVENT_OFF_PEER_PTR);
    if (peer == NULL || peer_len != (uint64_t)CP_SPP_RX_PEER_LEN ||
        !cp_spp_rx_peer_ok(peer)) {
        return;
    }

    uuid_len = *(const uint64_t *)(const void *)(ev +
                                                 CP_SPP_RX_EVENT_OFF_UUID_LEN);
    uuid = *(const char *const *)(const void *)(ev +
                                                CP_SPP_RX_EVENT_OFF_UUID_PTR);
    if (uuid == NULL || uuid_len != (uint64_t)CP_SPP_RX_UUID_LEN) {
        return;
    }
    uuid_kind = cp_spp_rx_uuid_kind(uuid);
    if (uuid_kind == CP_SPP_RX_UUID_KIND_NONE) {
        return;
    }

    payload_len = *(const uint32_t *)(const void *)(ev +
                                                    CP_SPP_RX_EVENT_OFF_PAYLOAD_LEN);
    payload = *(const unsigned char *const *)(const void *)(ev +
                                                            CP_SPP_RX_EVENT_OFF_PAYLOAD);
    if (payload == NULL || payload_len == 0u ||
        payload_len > (uint32_t)CP_SPP_RX_MAX_PAYLOAD) {
        return;
    }

    header[0] = (unsigned char)'W';
    header[1] = (unsigned char)'1';
    header[2] = (unsigned char)'B';
    header[3] = (unsigned char)'S';
    header[4] = (unsigned char)'P';
    header[5] = (unsigned char)'P';
    header[6] = (unsigned char)'0';
    header[7] = (unsigned char)'1';
    header[CP_SPP_RX_OFF_DATALEN + 0u] =
        (unsigned char)(payload_len & 0xffu);
    header[CP_SPP_RX_OFF_DATALEN + 1u] =
        (unsigned char)((payload_len >> 8) & 0xffu);
    header[CP_SPP_RX_OFF_DATALEN + 2u] =
        (unsigned char)((payload_len >> 16) & 0xffu);
    header[CP_SPP_RX_OFF_DATALEN + 3u] =
        (unsigned char)((payload_len >> 24) & 0xffu);
    for (i = 0u; i < (size_t)CP_SPP_RX_PEER_LEN; i++) {
        header[CP_SPP_RX_OFF_PEER + i] = (unsigned char)peer[i];
    }
    header[CP_SPP_RX_OFF_UUID_KIND] = (unsigned char)uuid_kind;
    header[CP_SPP_RX_OFF_RESERVED + 0u] = 0u;
    header[CP_SPP_RX_OFF_RESERVED + 1u] = 0u;

    address[0] = (unsigned char)CP_SPP_RX_AF_UNIX;
    address[1] = 0u;
    for (i = 0u; i < sizeof(cp_spp_rx_path); i++) {
        address[2u + i] = (unsigned char)cp_spp_rx_path[i];
    }

    iov[0].base = header;
    iov[0].len = CP_SPP_RX_HEADER_SIZE;
    iov[1].base = (void *)payload;
    iov[1].len = (size_t)payload_len;

    msg.name = address;
    msg.namelen = (uint32_t)(2u + sizeof(cp_spp_rx_path));
    msg.pad0 = 0u;
    msg.iov = iov;
    msg.iovlen = 2u;
    msg.control = NULL;
    msg.controllen = 0u;
    msg.flags = 0;
    msg.pad1 = 0u;

    fd = cp_spp_rx_syscall3(CP_SPP_RX_SYS_SOCKET, CP_SPP_RX_AF_UNIX,
                            CP_SPP_RX_SOCK_DGRAM | CP_SPP_RX_SOCK_NONBLOCK |
                                CP_SPP_RX_SOCK_CLOEXEC,
                            0);
    if (fd < 0) {
        return;
    }
    (void)cp_spp_rx_syscall3(CP_SPP_RX_SYS_SENDMSG, fd,
                             (long)(void *)&msg,
                             CP_SPP_RX_MSG_DONTWAIT |
                                 CP_SPP_RX_MSG_NOSIGNAL);
    (void)cp_spp_rx_syscall3(CP_SPP_RX_SYS_CLOSE, fd, 0, 0);
}
