/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * cp_spp_rx — published wire contract for the C3 experimental SPP RX channel.
 *
 * This header is intentionally only the datagram/wire contract.  The OEM
 * event-object ABI and the fixed-address hook entry point are private to
 * cp_spp_rx_hook.c and must not be used by the future root-only socket
 * decoder.  A separate decoder can include this header and consume
 * /run/wifi1b/carplay/rx.sock without linking or knowing the OEM hook ABI.
 *
 * STATUS: experimental build-only artifact only.  Not car-validated.  The
 * Wifi1b installer does not activate it.  Do not present it as working
 * wireless CarPlay.
 *
 * DATAGRAM FORMAT (little-endian, exactly 32-byte header)
 * ------------------------------------------------------
 *   offset  size  field
 *   0       8     magic ASCII "W1BSPP01"
 *   8       4     payload length, uint32 little-endian
 *   12      17    peer Bluetooth address, ASCII "AA:BB:CC:DD:EE:FF"
 *   29      1     UUID kind: 1=CAFF accessory/listener, 2=CAFE phone client,
 *                 3=CarPlay service EC884348-CD41-40A2-9727-575D50BF1FD3
 *   30      2     reserved, zero
 *   32      N     payload bytes, no trailing terminator
 *
 * The header carries no keys, identities, or log text.  A decoder must treat
 * the payload as opaque iAP2 bytes and must not log it.  UUID kind is
 * evidence only: a server/listener UUID ends CAFF; an outgoing phone client
 * UUID ends CAFE.  Do not treat CAFE as an inbound listener.
 *
 * Receiver contract for the future broker (not implemented by this header):
 *   path        /run/wifi1b/carplay/rx.sock
 *   parent      /run/wifi1b/carplay  mode 0700, owned by root
 *   socket      AF_UNIX SOCK_DGRAM   mode 0600, owned by root
 *   sender      stateless unconnected datagram, MSG_DONTWAIT|MSG_NOSIGNAL
 *
 * A missing receiver or a full receive queue is a drop, never a block and
 * never SIGPIPE.  Do not replace this channel with a FIFO: a disappearing
 * FIFO reader can signal SIGPIPE and kill vbtd.
 */

#ifndef CP_SPP_RX_H
#define CP_SPP_RX_H

#ifdef __cplusplus
extern "C" {
#endif

#define CP_SPP_RX_MAGIC "W1BSPP01"
#define CP_SPP_RX_MAGIC_LEN 8u
#define CP_SPP_RX_HEADER_SIZE 32u

#define CP_SPP_RX_OFF_MAGIC 0u
#define CP_SPP_RX_OFF_DATALEN 8u
#define CP_SPP_RX_OFF_PEER 12u
#define CP_SPP_RX_OFF_UUID_KIND 29u
#define CP_SPP_RX_OFF_RESERVED 30u
#define CP_SPP_RX_RESERVED_LEN 2u

#define CP_SPP_RX_PEER_LEN 17u
#define CP_SPP_RX_UUID_LEN 36u
#define CP_SPP_RX_MAX_PAYLOAD 16384u

#define CP_SPP_RX_UUID_IAP2_ACCESSORY_CAFF \
    "00000000-DECA-FADE-DECA-DEAFDECACAFF"
#define CP_SPP_RX_UUID_IAP2_PHONE_CAFE \
    "00000000-DECA-FADE-DECA-DEAFDECACAFE"
#define CP_SPP_RX_UUID_CARPLAY "EC884348-CD41-40A2-9727-575D50BF1FD3"

#define CP_SPP_RX_UUID_KIND_NONE 0u
#define CP_SPP_RX_UUID_KIND_IAP2_ACCESSORY_CAFF 1u
#define CP_SPP_RX_UUID_KIND_IAP2_PHONE_CAFE 2u
#define CP_SPP_RX_UUID_KIND_CARPLAY 3u

#ifndef CP_SPP_RX_SOCKET_PATH
#define CP_SPP_RX_SOCKET_PATH "/run/wifi1b/carplay/rx.sock"
#endif

#if defined(__cplusplus)
#define CP_SPP_RX_STATIC_ASSERT(cond, msg) static_assert((cond), msg)
#else
#define CP_SPP_RX_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)
#endif

CP_SPP_RX_STATIC_ASSERT(CP_SPP_RX_MAGIC_LEN == 8u,
                        "magic must be 8 bytes");
CP_SPP_RX_STATIC_ASSERT(CP_SPP_RX_HEADER_SIZE == 32u,
                        "SPP RX header must be exactly 32 bytes");
CP_SPP_RX_STATIC_ASSERT(CP_SPP_RX_OFF_MAGIC == 0u,
                        "magic offset");
CP_SPP_RX_STATIC_ASSERT(CP_SPP_RX_OFF_DATALEN == 8u,
                        "length offset");
CP_SPP_RX_STATIC_ASSERT(CP_SPP_RX_OFF_PEER == 12u,
                        "peer offset");
CP_SPP_RX_STATIC_ASSERT(CP_SPP_RX_OFF_PEER + CP_SPP_RX_PEER_LEN ==
                            CP_SPP_RX_OFF_UUID_KIND,
                        "peer must end at UUID kind");
CP_SPP_RX_STATIC_ASSERT(CP_SPP_RX_OFF_UUID_KIND == 29u,
                        "UUID kind offset");
CP_SPP_RX_STATIC_ASSERT(CP_SPP_RX_OFF_RESERVED == 30u,
                        "reserved offset");
CP_SPP_RX_STATIC_ASSERT(CP_SPP_RX_OFF_RESERVED + CP_SPP_RX_RESERVED_LEN ==
                            CP_SPP_RX_HEADER_SIZE,
                        "reserved must end at header end");
CP_SPP_RX_STATIC_ASSERT(CP_SPP_RX_MAX_PAYLOAD == 16384u,
                        "payload bound");

#ifdef __cplusplus
}
#endif

#endif /* CP_SPP_RX_H */
