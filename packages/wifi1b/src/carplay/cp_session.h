/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * cp_session — serialized wireless CarPlay bring-up wrapper.
 *
 * This module joins the bounded iAP2 link engine (cp_link), the bootstrap
 * state machine (cp_bootstrap) and the CSM codec (cp_wire). It is deliberately
 * hardware-, socket- and clock-agnostic: the caller feeds arbitrary transport
 * bytes, supplies monotonic milliseconds, drains partial output writes and
 * owns the hardware/provider/policy callbacks.
 *
 * Layering:
 *
 *   transport bytes
 *        -> cp_link (marker, 0xff5a framing, SYN/ACK/EAK/RST, retransmit)
 *        -> control-session byte stream (deferred reassembly)
 *        -> cp_bootstrap / cp_wire (identification, MFi, WiFi, start)
 *        -> bounded CSM reply queue (segmented to the peer's negotiated size)
 *
 * The link callback NEVER calls cp_link or cp_bootstrap recursively. It only
 * copies control payload bytes and records flags. CSM work happens after
 * cp_link_feed()/cp_link_tick() returns. This keeps callback-time state stable.
 *
 * Security and lifetime invariants:
 *
 *   - Every cp_link generation advance discards identification, authentication,
 *     pending CSM replies, partial RX/TX state, receiver observation and any
 *     pending start/policy action. `generation` is the caller's auth epoch.
 *   - Caller monotonic time must not go backwards. A rollback is rejected
 *     explicitly; no link/bootstrap state is mutated.
 *   - Cancel/reset clears queued replies, partial segments and RX bytes.
 *   - Only control-session link payloads are considered for CSM processing.
 *     Other/unnegotiated sessions cannot inject CSM.
 *   - ACK/EAK/control replies are never silently dropped. A full reply queue
 *     aborts the session with an explicit error and RST.
 *   - Receiver-active is a separate local observation, gated by the current
 *     generation, a successfully started bootstrap offer, an identified
 *     Bluetooth peer id and a ready AP. It is never inferred from a network
 *     request, a bare boolean or a CSM.
 *   - Events and stats contain no CSM payloads, SSIDs, passphrases or keys.
 *     Events report only states, enum reasons, flags and generations.
 *
 * Single-threaded use only. The event callback is invoked after link work
 * returns, from the public API call that caused it. Do not re-enter cp_session
 * from the callback. All storage is fixed; no allocation or logging.
 */
#ifndef CP_SESSION_H
#define CP_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cp_bootstrap.h"
#include "cp_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded reply queue and CSM reassembly buffer. One wire CSM is at most
 * CP_WIRE_MAX_FRAME; a link payload is at most CP_LINK_MAX_PAYLOAD, so the RX
 * buffer needs one maximum partial CSM plus one maximum incoming payload. */
#ifndef CP_SESSION_TX_QUEUE
#define CP_SESSION_TX_QUEUE 8u
#endif
#define CP_SESSION_RX_BUFFER (CP_WIRE_MAX_FRAME + CP_LINK_MAX_PAYLOAD)

typedef enum cp_session_state {
    CP_SESSION_STATE_IDLE = 0,
    CP_SESSION_STATE_STARTING,
    CP_SESSION_STATE_IDENTIFYING,
    CP_SESSION_STATE_IDENTIFIED,
    CP_SESSION_STATE_AUTHENTICATING,
    CP_SESSION_STATE_AUTHENTICATED,
    /* Start-session has been produced by bootstrap. This is NOT proof that a
     * receiver is active; see cp_session_receiver_active(). */
    CP_SESSION_STATE_AWAITING_RECEIVER,
    CP_SESSION_STATE_DISCONNECTED,
    CP_SESSION_STATE_FAILED,
    CP_SESSION_STATE_CANCELLED
} cp_session_state_t;

typedef enum cp_session_error {
    CP_SESSION_OK = 0,
    CP_SESSION_ERR_ARGUMENT,
    CP_SESSION_ERR_CONFIG,
    CP_SESSION_ERR_STATE,
    CP_SESSION_ERR_CLOCK_ROLLBACK,
    CP_SESSION_ERR_LINK,
    CP_SESSION_ERR_BOOTSTRAP,
    CP_SESSION_ERR_CSM_MALFORMED,
    CP_SESSION_ERR_RX_OVERFLOW,
    CP_SESSION_ERR_TX_QUEUE_OVERFLOW,
    CP_SESSION_ERR_TX_STALE,
    CP_SESSION_ERR_LINK_LIMIT,
    CP_SESSION_ERR_AUTH_FAILED,
    CP_SESSION_ERR_IDENTIFICATION_REJECTED,
    CP_SESSION_ERR_POLICY,
    CP_SESSION_ERR_TIMEOUT,
    CP_SESSION_ERR_CANCELLED,
    CP_SESSION_ERR_GENERATION_MISMATCH,
    CP_SESSION_ERR_PEER_MISMATCH,
    CP_SESSION_ERR_RECEIVER_STATE,
    CP_SESSION_ERR_PROVIDER
} cp_session_error_t;

typedef enum cp_session_event_type {
    CP_SESSION_EVENT_STATE = 1,
    CP_SESSION_EVENT_LINK_UP,
    CP_SESSION_EVENT_LINK_RESET,
    CP_SESSION_EVENT_BOOT,
    CP_SESSION_EVENT_RECEIVER,
    CP_SESSION_EVENT_ERROR
} cp_session_event_type_t;

/* Sanitised event. No payload, SSID, passphrase, key or transport-id bytes. */
typedef struct cp_session_event {
    cp_session_event_type_t type;
    cp_session_state_t state;
    cp_link_state_t link_state;
    cp_boot_state boot_state;
    cp_boot_auth_state auth_state;
    cp_boot_ap_state ap_state;
    cp_boot_event boot_event;
    cp_boot_error boot_error;
    cp_session_error_t error;
    cp_link_reset_reason_t link_reset_reason;
    uint32_t generation; /* authoritative link/auth generation */
    bool receiver_active;
    bool start_sent;
} cp_session_event_t;

typedef void (*cp_session_event_fn)(void *user, const cp_session_event_t *event);

typedef struct cp_session_stats {
    cp_link_stats_t link;

    uint32_t link_up_events;
    uint32_t link_reset_events;
    uint32_t generation_resets;
    uint32_t clock_rollbacks;

    uint32_t csm_link_payloads_rx;
    uint32_t csm_frames_rx;
    uint32_t csm_bytes_rx;
    uint32_t csm_fragmented_frames;
    uint32_t csm_coalesced_frames;

    uint32_t csm_replies_queued;
    uint32_t csm_replies_sent;
    uint32_t csm_segments_sent;
    uint32_t csm_bytes_queued;
    uint32_t csm_bytes_sent;
    uint32_t csm_stale_replies_dropped;
    uint32_t csm_tx_queue_overflows;
    uint32_t csm_rx_overflows;
    uint32_t csm_malformed;

    uint32_t unsupported_session_payloads;
    uint32_t boot_events;
    uint32_t boot_errors;
    uint32_t auth_failures;
    uint32_t identification_rejections;
    uint32_t policy_refusals;
    uint32_t receiver_observations;
    uint32_t receiver_rejections;
    uint32_t link_backpressure_retries;
} cp_session_stats_t;

typedef struct cp_session_config {
    cp_link_config_t link;
    cp_boot_config boot;
} cp_session_config_t;

/* Internal fixed records; treat as private. At default limits cp_session is
 * dominated by cp_link_t (roughly 300 KiB). Allocate once in static or
 * caller-owned storage, not on a small task stack. */
typedef struct cp_session_tx_slot {
    uint16_t len;
    uint16_t sent;
    uint32_t boot_generation;
    uint8_t bytes[CP_WIRE_MAX_FRAME];
} cp_session_tx_slot_t;

typedef struct cp_session {
    cp_link_t link;
    cp_bootstrap boot;
    cp_session_event_fn on_event;
    void *user;

    cp_session_state_t state;
    cp_session_error_t last_error;
    uint32_t bound_generation;
    bool initialized;
    bool started;
    uint64_t now_ms;
    uint64_t last_now_ms;

    cp_session_tx_slot_t tx_slots[CP_SESSION_TX_QUEUE];
    uint8_t tx_head;
    uint8_t tx_count;

    uint8_t rx_buf[CP_SESSION_RX_BUFFER];
    size_t rx_len;
    bool rx_partial_seen;
    bool rx_current_fragmented;

    bool cb_rx_overflow;
    bool cb_link_reset;
    cp_link_reset_reason_t cb_reset_reason;
    bool cb_link_up;
    cp_link_state_t cb_link_state;

    bool receiver_active;
    uint32_t receiver_generation;

    cp_session_stats_t stats;
} cp_session_t;

void cp_session_config_default(cp_session_config_t *config);

/* `now_ms` is the caller's current monotonic clock at construction. It is the
 * baseline for callback-time bootstrap work and rollback rejection. */
cp_session_error_t cp_session_init(cp_session_t *session,
                                   const cp_session_config_t *config,
                                   cp_session_event_fn on_event, void *user,
                                   uint64_t now_ms);

cp_session_error_t cp_session_start(cp_session_t *session,
                                    cp_link_role_t role, uint64_t now_ms);

/* Arbitrary transport bytes. Link framing, CSM reassembly, bootstrap and
 * queued reply flushing all complete before this returns. */
cp_session_error_t cp_session_feed(cp_session_t *session, const void *data,
                                   size_t len, uint64_t now_ms);

/* Monotonic service: link deadlines, bootstrap pending-start deadline and
 * queued reply flush. */
cp_session_error_t cp_session_tick(cp_session_t *session, uint64_t now_ms);

uint64_t cp_session_next_deadline(const cp_session_t *session);

cp_session_error_t cp_session_set_auth_provider(
    cp_session_t *session, const cp_boot_auth_provider *provider);
cp_session_error_t cp_session_set_policy(cp_session_t *session,
                                         cp_boot_consent_fn consent,
                                         cp_boot_usb_active_fn usb_active,
                                         void *context);
cp_session_error_t cp_session_set_ap_readback(cp_session_t *session,
                                              const cp_boot_ap_readback *readback,
                                              uint64_t now_ms);

/* Trusted local receiver observation. Requires the caller's generation to
 * equal the live link generation, a normal link, a started offer, successful
 * authentication, a ready AP and a Bluetooth peer id recorded from the phone.
 * `peer_id` must match that identified id (ASCII case-insensitive). A network
 * request, CSM or bare boolean can never set this state. */
cp_session_error_t cp_session_set_receiver_active(cp_session_t *session,
                                                  uint64_t now_ms,
                                                  uint32_t generation,
                                                  const char *peer_id,
                                                  bool active);

/* Local cancel (no wire RST): clears link TX/RX, partial CSM segments,
 * bootstrap identification/authentication/pending start and advances the link
 * generation. */
cp_session_error_t cp_session_cancel(cp_session_t *session, uint64_t now_ms);

/* Transport loss reset: same clearing as cancel, reported as disconnected. */
cp_session_error_t cp_session_transport_reset(cp_session_t *session,
                                              uint64_t now_ms);

/* Output is the cp_link wire queue. The caller may write any prefix and then
 * consume exactly those bytes. Consuming output resumes deferred CSM reply
 * segments without re-sending already accepted bytes. */
size_t cp_session_output_peek(const cp_session_t *session,
                              const uint8_t **data);
size_t cp_session_output_pending(const cp_session_t *session);
cp_session_error_t cp_session_output_consume(cp_session_t *session,
                                             size_t len);

size_t cp_session_pending_replies(const cp_session_t *session);
size_t cp_session_pending_rx(const cp_session_t *session);

cp_session_state_t cp_session_state(const cp_session_t *session);
cp_link_state_t cp_session_link_state(const cp_session_t *session);
cp_boot_state cp_session_boot_state(const cp_session_t *session);
cp_boot_auth_state cp_session_auth_state(const cp_session_t *session);
cp_boot_ap_state cp_session_ap_state(const cp_session_t *session);
bool cp_session_start_sent(const cp_session_t *session);
bool cp_session_receiver_active(const cp_session_t *session);
uint32_t cp_session_receiver_generation(const cp_session_t *session);
/* This wrapper never claims projection is active. */
bool cp_session_projection_active(const cp_session_t *session);

uint32_t cp_session_generation(const cp_session_t *session);
cp_session_error_t cp_session_last_error(const cp_session_t *session);
const cp_session_stats_t *cp_session_stats(const cp_session_t *session);

const char *cp_session_state_name(cp_session_state_t state);
const char *cp_session_error_name(cp_session_error_t error);

#ifdef __cplusplus
}
#endif

#endif /* CP_SESSION_H */
