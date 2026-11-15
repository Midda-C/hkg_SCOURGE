/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Optional experimental CLI for the passive _airplay._tcp observer.
 *
 * Default operation is inert.  Network and libdns_sd are touched only with
 * --probe.  The CLI never loads OEM libraries, never starts/stops the OEM
 * receiver and never claims trusted readiness: no local process/listener/
 * identity attestation is available in this build.
 *
 * This source is target-compatible C11, but a host-built binary is not
 * deployable: host glibc exposes dlopen/dlsym at GLIBC_2.34.  A target build
 * must link against the rootfs libdl/libc 2.25 (reuse the native auth tool's
 * startup once that tool is stable).
 */

#define _POSIX_C_SOURCE 200809L

#include "cp_oem_receiver.h"

#include <dlfcn.h>
#include <errno.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CP_PROBE_DEFAULT_TIMEOUT_MS 15000u
#define CP_PROBE_POLL_MS 200
#define CP_PROBE_MAX_RESOLVES CP_OEM_RECEIVER_MAX_SERVICES

typedef void *cp_dnssd_ref;
typedef void (*cp_dnssd_browse_reply)(cp_dnssd_ref sd_ref, uint32_t flags,
                                      uint32_t interface_index, int32_t error_code,
                                      const char *service_name, const char *regtype,
                                      const char *reply_domain, void *context);
typedef void (*cp_dnssd_resolve_reply)(cp_dnssd_ref sd_ref, uint32_t flags,
                                       uint32_t interface_index, int32_t error_code,
                                       const char *fullname, const char *hosttarget,
                                       uint16_t port, uint16_t txt_len,
                                       const unsigned char *txt_record, void *context);
typedef int32_t (*cp_dnssd_browse_fn)(cp_dnssd_ref *sd_ref, uint32_t flags,
                                       uint32_t interface_index, const char *regtype,
                                       const char *domain, cp_dnssd_browse_reply reply,
                                       void *context);
typedef int32_t (*cp_dnssd_resolve_fn)(cp_dnssd_ref *sd_ref, uint32_t flags,
                                        uint32_t interface_index, const char *name,
                                        const char *regtype, const char *domain,
                                        cp_dnssd_resolve_reply reply, void *context);
typedef int (*cp_dnssd_sockfd_fn)(cp_dnssd_ref sd_ref);
typedef int32_t (*cp_dnssd_process_fn)(cp_dnssd_ref sd_ref);
typedef void (*cp_dnssd_dealloc_fn)(cp_dnssd_ref sd_ref);

typedef union {
    void *object;
    cp_dnssd_browse_fn browse;
    cp_dnssd_resolve_fn resolve;
    cp_dnssd_sockfd_fn sockfd;
    cp_dnssd_process_fn process;
    cp_dnssd_dealloc_fn dealloc;
} dns_symbol;

typedef struct {
    cp_dnssd_ref ref;
    char service_name[CP_OEM_RECEIVER_MAX_SERVICE_NAME];
    uint32_t interface_index;
} resolve_slot;

typedef struct {
    void *dl;
    cp_dnssd_browse_fn browse;
    cp_dnssd_resolve_fn resolve;
    cp_dnssd_sockfd_fn sockfd;
    cp_dnssd_process_fn process;
    cp_dnssd_dealloc_fn dealloc;
    cp_dnssd_ref browse_ref;
    resolve_slot resolves[CP_PROBE_MAX_RESOLVES];
    cp_oem_receiver_dns_event_fn event_fn;
    void *event_context;
    uint32_t interface_index;
} dns_sd_backend;

static volatile sig_atomic_t stop_requested;

static void on_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0u;
    }
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static uint16_t host_byte_order_port(uint16_t port)
{
    return (uint16_t)((uint16_t)(port >> 8) | (uint16_t)(port << 8));
}

static void emit_event(dns_sd_backend *backend, const cp_oem_receiver_dns_event *event)
{
    if (backend->event_fn != NULL) {
        backend->event_fn(backend->event_context, event);
    }
}

static void emit_error(dns_sd_backend *backend, uint32_t interface_index, int32_t error)
{
    cp_oem_receiver_dns_event event;

    memset(&event, 0, sizeof(event));
    event.type = CP_OEM_RECEIVER_DNS_ERROR;
    event.observed_ms = monotonic_ms();
    event.interface_index = interface_index;
    event.error = error;
    emit_event(backend, &event);
}

static resolve_slot *find_resolve_by_ref(dns_sd_backend *backend, cp_dnssd_ref ref)
{
    size_t i;

    for (i = 0u; i < CP_PROBE_MAX_RESOLVES; i++) {
        if (backend->resolves[i].ref == ref) {
            return &backend->resolves[i];
        }
    }
    return NULL;
}

static resolve_slot *find_resolve_by_name(dns_sd_backend *backend, const char *name,
                                          uint32_t interface_index)
{
    size_t i;

    for (i = 0u; i < CP_PROBE_MAX_RESOLVES; i++) {
        if (backend->resolves[i].ref != NULL &&
            backend->resolves[i].interface_index == interface_index &&
            strcmp(backend->resolves[i].service_name, name) == 0) {
            return &backend->resolves[i];
        }
    }
    return NULL;
}

static resolve_slot *alloc_resolve(dns_sd_backend *backend)
{
    size_t i;

    for (i = 0u; i < CP_PROBE_MAX_RESOLVES; i++) {
        if (backend->resolves[i].ref == NULL) {
            return &backend->resolves[i];
        }
    }
    return NULL;
}

static void resolve_reply(cp_dnssd_ref sd_ref, uint32_t flags, uint32_t interface_index,
                          int32_t error_code, const char *fullname,
                          const char *hosttarget, uint16_t port, uint16_t txt_len,
                          const unsigned char *txt_record, void *context)
{
    dns_sd_backend *backend = (dns_sd_backend *)context;
    resolve_slot *slot;
    cp_oem_receiver_dns_event event;

    (void)flags;
    (void)fullname;
    if (backend == NULL) {
        return;
    }
    slot = find_resolve_by_ref(backend, sd_ref);
    if (error_code != 0) {
        emit_error(backend, interface_index, error_code);
        if (slot != NULL) {
            backend->dealloc(sd_ref);
            memset(slot, 0, sizeof(*slot));
        }
        return;
    }
    if (slot == NULL) {
        backend->dealloc(sd_ref);
        return;
    }
    memset(&event, 0, sizeof(event));
    event.type = CP_OEM_RECEIVER_DNS_RESOLVE;
    event.observed_ms = monotonic_ms();
    event.interface_index = interface_index;
    event.service_name = slot->service_name;
    event.service_name_len = strlen(slot->service_name);
    event.regtype = CP_OEM_RECEIVER_SERVICE_TYPE;
    event.regtype_len = strlen(CP_OEM_RECEIVER_SERVICE_TYPE);
    event.domain = CP_OEM_RECEIVER_SERVICE_DOMAIN;
    event.domain_len = strlen(CP_OEM_RECEIVER_SERVICE_DOMAIN);
    event.hosttarget = hosttarget;
    event.hosttarget_len = hosttarget != NULL ? strlen(hosttarget) : 0u;
    event.port = host_byte_order_port(port);
    event.txt = txt_record;
    event.txt_len = txt_len;
    emit_event(backend, &event);
    backend->dealloc(sd_ref);
    memset(slot, 0, sizeof(*slot));
}

static void browse_reply(cp_dnssd_ref sd_ref, uint32_t flags, uint32_t interface_index,
                         int32_t error_code, const char *service_name,
                         const char *regtype, const char *reply_domain, void *context)
{
    dns_sd_backend *backend = (dns_sd_backend *)context;
    cp_oem_receiver_dns_event event;

    (void)sd_ref;
    if (backend == NULL) {
        return;
    }
    if (error_code != 0) {
        emit_error(backend, interface_index, error_code);
        return;
    }
    if (service_name == NULL || strlen(service_name) >= CP_OEM_RECEIVER_MAX_SERVICE_NAME) {
        emit_error(backend, interface_index, -1);
        return;
    }
    memset(&event, 0, sizeof(event));
    event.type = (flags & 0x2u) != 0u ? CP_OEM_RECEIVER_DNS_ADD
                                     : CP_OEM_RECEIVER_DNS_REMOVE;
    event.observed_ms = monotonic_ms();
    event.interface_index = interface_index;
    event.service_name = service_name;
    event.service_name_len = strlen(service_name);
    event.regtype = regtype;
    event.regtype_len = regtype != NULL ? strlen(regtype) : 0u;
    event.domain = reply_domain;
    event.domain_len = reply_domain != NULL ? strlen(reply_domain) : 0u;
    emit_event(backend, &event);

    if (event.type == CP_OEM_RECEIVER_DNS_ADD) {
        resolve_slot *slot = alloc_resolve(backend);
        const char *resolve_regtype = regtype != NULL ? regtype : CP_OEM_RECEIVER_SERVICE_TYPE;
        const char *resolve_domain =
            reply_domain != NULL ? reply_domain : CP_OEM_RECEIVER_SERVICE_DOMAIN;
        int32_t result;

        if (find_resolve_by_name(backend, service_name, interface_index) != NULL) {
            return;
        }
        if (slot == NULL) {
            emit_error(backend, interface_index, -2);
            return;
        }
        memset(slot, 0, sizeof(*slot));
        memcpy(slot->service_name, service_name, strlen(service_name) + 1u);
        slot->interface_index = interface_index;
        result = backend->resolve(&slot->ref, 0u, interface_index, service_name,
                                  resolve_regtype, resolve_domain, resolve_reply, backend);
        if (result != 0) {
            emit_error(backend, interface_index, result);
            memset(slot, 0, sizeof(*slot));
        }
    } else {
        resolve_slot *slot = find_resolve_by_name(backend, service_name, interface_index);

        if (slot != NULL) {
            backend->dealloc(slot->ref);
            memset(slot, 0, sizeof(*slot));
        }
    }
}

static cp_oem_receiver_status dns_sd_start(void *context, const char *service_type,
                                           const char *domain, uint32_t interface_index,
                                           cp_oem_receiver_dns_event_fn event_fn,
                                           void *event_context)
{
    dns_sd_backend *backend = (dns_sd_backend *)context;
    dns_symbol symbol;
    int32_t result;

    if (backend == NULL || event_fn == NULL) {
        return CP_OEM_RECEIVER_ERR_ARGUMENT;
    }
    memset(backend->resolves, 0, sizeof(backend->resolves));
    backend->browse_ref = NULL;
    backend->event_fn = event_fn;
    backend->event_context = event_context;
    backend->interface_index = interface_index;
    backend->dl = dlopen("/app/lib/libdns_sd.so", RTLD_NOW | RTLD_LOCAL);
    if (backend->dl == NULL) {
        backend->dl = dlopen("libdns_sd.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (backend->dl == NULL) {
        return CP_OEM_RECEIVER_ERR_NO_BACKEND;
    }
    symbol.object = dlsym(backend->dl, "DNSServiceBrowse");
    backend->browse = symbol.browse;
    symbol.object = dlsym(backend->dl, "DNSServiceResolve");
    backend->resolve = symbol.resolve;
    symbol.object = dlsym(backend->dl, "DNSServiceRefSockFD");
    backend->sockfd = symbol.sockfd;
    symbol.object = dlsym(backend->dl, "DNSServiceProcessResult");
    backend->process = symbol.process;
    symbol.object = dlsym(backend->dl, "DNSServiceRefDeallocate");
    backend->dealloc = symbol.dealloc;
    if (backend->browse == NULL || backend->resolve == NULL ||
        backend->sockfd == NULL || backend->process == NULL ||
        backend->dealloc == NULL) {
        (void)dlclose(backend->dl);
        memset(backend, 0, sizeof(*backend));
        return CP_OEM_RECEIVER_ERR_NO_BACKEND;
    }
    result = backend->browse(&backend->browse_ref, 0u, interface_index, service_type,
                             domain, browse_reply, backend);
    if (result != 0) {
        if (backend->browse_ref != NULL) {
            backend->dealloc(backend->browse_ref);
            backend->browse_ref = NULL;
        }
        (void)dlclose(backend->dl);
        memset(backend, 0, sizeof(*backend));
        return CP_OEM_RECEIVER_ERR_DNS;
    }
    return CP_OEM_RECEIVER_OK;
}

static void dns_sd_stop(void *context)
{
    dns_sd_backend *backend = (dns_sd_backend *)context;
    size_t i;

    if (backend == NULL) {
        return;
    }
    if (backend->dealloc != NULL) {
        for (i = 0u; i < CP_PROBE_MAX_RESOLVES; i++) {
            if (backend->resolves[i].ref != NULL) {
                backend->dealloc(backend->resolves[i].ref);
                memset(&backend->resolves[i], 0, sizeof(backend->resolves[i]));
            }
        }
        if (backend->browse_ref != NULL) {
            backend->dealloc(backend->browse_ref);
            backend->browse_ref = NULL;
        }
    }
    if (backend->dl != NULL) {
        (void)dlclose(backend->dl);
    }
    memset(backend, 0, sizeof(*backend));
}

static cp_oem_receiver_dns_ops dns_sd_ops(dns_sd_backend *backend)
{
    cp_oem_receiver_dns_ops ops;

    memset(&ops, 0, sizeof(ops));
    ops.context = backend;
    ops.start = dns_sd_start;
    ops.stop = dns_sd_stop;
    return ops;
}

static int dns_sd_poll(dns_sd_backend *backend)
{
    struct pollfd fds[CP_PROBE_MAX_RESOLVES + 1u];
    cp_dnssd_ref refs[CP_PROBE_MAX_RESOLVES + 1u];
    size_t count = 0u;
    size_t i;
    int ready;

    if (backend == NULL || backend->sockfd == NULL || backend->process == NULL) {
        return -1;
    }
    if (backend->browse_ref != NULL) {
        int fd = backend->sockfd(backend->browse_ref);

        if (fd >= 0) {
            fds[count].fd = fd;
            fds[count].events = POLLIN;
            fds[count].revents = 0;
            refs[count] = backend->browse_ref;
            count++;
        }
    }
    for (i = 0u; i < CP_PROBE_MAX_RESOLVES; i++) {
        if (backend->resolves[i].ref != NULL) {
            int fd = backend->sockfd(backend->resolves[i].ref);

            if (fd >= 0) {
                fds[count].fd = fd;
                fds[count].events = POLLIN;
                fds[count].revents = 0;
                refs[count] = backend->resolves[i].ref;
                count++;
            }
        }
    }
    if (count == 0u) {
        return 0;
    }
    ready = poll(fds, (nfds_t)count, CP_PROBE_POLL_MS);
    if (ready < 0) {
        return errno == EINTR ? 0 : -1;
    }
    if (ready == 0) {
        return 0;
    }
    for (i = 0u; i < count; i++) {
        if ((fds[i].revents & POLLIN) != 0) {
            int32_t result = backend->process(refs[i]);

            if (result != 0) {
                emit_error(backend, backend->interface_index, result);
            }
            /* Process one reference per call: a resolve callback can deallocate
             * its own reference, invalidating the rest of this snapshot. */
            return 1;
        }
    }
    return 0;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [--probe] [--dry-run] [--identity] [--root PATH] "
            "[--interface NAME] [--timeout-ms N]\n"
            "  default: inert; --probe is required before any network access\n"
            "  --dry-run: verify target pins and print the plan without browsing\n"
            "  --identity: report the unimplemented native identity adapter blocker\n",
            program);
}

static int verify_target(const char *root)
{
    size_t failed = 0u;
    cp_oem_receiver_status status;

    status = cp_oem_receiver_verify_files(root, cp_oem_receiver_target_pins,
                                          cp_oem_receiver_target_pin_count, &failed);
    if (status != CP_OEM_RECEIVER_OK) {
        fprintf(stderr, "target pin check failed: %s (%s)\n",
                cp_oem_receiver_status_string(status),
                cp_oem_receiver_target_pins[failed].relative_path);
        return 4;
    }
    status = cp_oem_receiver_verify_files(root, &cp_oem_receiver_dns_sd_pin, 1u, &failed);
    if (status != CP_OEM_RECEIVER_OK) {
        fprintf(stderr, "dns_sd pin check failed: %s (%s)\n",
                cp_oem_receiver_status_string(status),
                cp_oem_receiver_dns_sd_pin.relative_path);
        return 4;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *root = "/";
    const char *interface_name = CP_OEM_RECEIVER_DEFAULT_INTERFACE;
    uint64_t timeout_ms = CP_PROBE_DEFAULT_TIMEOUT_MS;
    bool probe = false;
    bool dry_run = false;
    bool identity = false;
    unsigned ifindex;
    cp_oem_receiver_config config;
    cp_oem_receiver_dns_ops ops;
    cp_oem_receiver probe_state;
    dns_sd_backend backend;
    cp_oem_receiver_snapshot snapshot;
    uint64_t deadline;
    int result;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--probe") == 0) {
            probe = true;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = true;
        } else if (strcmp(argv[i], "--identity") == 0) {
            identity = true;
        } else if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
            root = argv[++i];
        } else if (strcmp(argv[i], "--interface") == 0 && i + 1 < argc) {
            interface_name = argv[++i];
        } else if (strcmp(argv[i], "--timeout-ms") == 0 && i + 1 < argc) {
            char *end = NULL;
            unsigned long value;

            errno = 0;
            value = strtoul(argv[++i], &end, 10);
            if (errno != 0 || end == argv[i] || *end != '\0' || value == 0u) {
                usage(argv[0]);
                return 2;
            }
            timeout_ms = (uint64_t)value;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (identity) {
        fprintf(stderr,
                "identity adapter unavailable: native read-only identity semantics "
                "remain unverified; no identity create/copy call is made\n");
        return 3;
    }
    if (!probe) {
        fprintf(stderr,
                "passive probe disabled; pass --probe to browse _airplay._tcp on %s "
                "(no receiver activation)\n",
                interface_name);
        return 2;
    }
    result = verify_target(root);
    if (result != 0) {
        return result;
    }
    ifindex = if_nametoindex(interface_name);
    if (ifindex == 0u) {
        fprintf(stderr, "interface %s has no index (is it up?)\n", interface_name);
        return 5;
    }
    if (dry_run) {
        printf("probe plan: service=%s interface=%s ifindex=%u backend=libdns_sd "
               "activation=none trusted_readiness=unavailable\n",
               CP_OEM_RECEIVER_SERVICE_TYPE, interface_name, ifindex);
        return 0;
    }

    cp_oem_receiver_config_default(&config);
    if (strlen(interface_name) >= sizeof(config.interface_name)) {
        fprintf(stderr, "interface name too long\n");
        return 2;
    }
    memcpy(config.interface_name, interface_name, strlen(interface_name) + 1u);
    config.interface_index = ifindex;
    config.browse_timeout_ms = timeout_ms < 5000u ? timeout_ms : 5000u;
    config.resolve_timeout_ms = 2000u;
    config.settle_ms = 500u;
    config.stale_after_ms = 30000u;
    config.max_services = 2u;
    memset(&backend, 0, sizeof(backend));
    ops = dns_sd_ops(&backend);
    if (cp_oem_receiver_init(&probe_state, &config, &ops) != CP_OEM_RECEIVER_OK) {
        fprintf(stderr, "probe init failed\n");
        return 2;
    }
    if (cp_oem_receiver_start(&probe_state, monotonic_ms()) != CP_OEM_RECEIVER_OK) {
        fprintf(stderr, "probe backend start failed\n");
        return 5;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    deadline = monotonic_ms() + timeout_ms;
    while (!stop_requested) {
        uint32_t changed = 0u;
        uint64_t now = monotonic_ms();

        (void)dns_sd_poll(&backend);
        (void)cp_oem_receiver_poll(&probe_state, now, &changed);
        if (changed != 0u) {
            char summary[1024];

            if (cp_oem_receiver_get_snapshot(&probe_state, &snapshot) ==
                    CP_OEM_RECEIVER_OK &&
                cp_oem_receiver_safe_summary(&snapshot, summary, sizeof(summary)) ==
                    CP_OEM_RECEIVER_OK) {
                puts(summary);
                fflush(stdout);
            }
        }
        if (now >= deadline) {
            break;
        }
    }
    {
        char summary[1024];

        if (cp_oem_receiver_get_snapshot(&probe_state, &snapshot) ==
                CP_OEM_RECEIVER_OK &&
            cp_oem_receiver_safe_summary(&snapshot, summary, sizeof(summary)) ==
                CP_OEM_RECEIVER_OK) {
            puts(summary);
        }
        cp_oem_receiver_stop(&probe_state);
        return snapshot.port != 0u ? 0 : 1;
    }
}
