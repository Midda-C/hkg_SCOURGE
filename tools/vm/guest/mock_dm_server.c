/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 */

/*
 * mock_dm_server - loopback-only lab DM endpoint for dmengine tests.
 *
 * Listens on 127.0.0.1, accepts one request at a time, and logs a structured
 * summary of the request (method, path, content type, length and a short body
 * prefix) without printing the full WBXML body. The response is an empty 200,
 * which is enough to prove the client attempted a real DM exchange; it is not
 * a valid SyncML response and no update-success claim is made.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_REQUEST (1024 * 1024)
#define BODY_PREFIX 24

static volatile sig_atomic_t running = 1;

static void stop(int sig)
{
    (void)sig;
    running = 0;
}

static int read_request(int fd, unsigned char *buffer, size_t capacity,
                        size_t *body_offset, size_t *content_length)
{
    size_t used = 0;
    *body_offset = 0;
    *content_length = 0;
    while (used + 1 < capacity) {
        ssize_t got = read(fd, buffer + used, capacity - used - 1);
        if (got <= 0)
            break;
        used += (size_t)got;
        buffer[used] = '\0';
        unsigned char *header_end = (unsigned char *)strstr((char *)buffer,
                                                            "\r\n\r\n");
        if (header_end != NULL) {
            *body_offset = (size_t)(header_end - buffer) + 4;
            char *line = strcasestr((char *)buffer, "Content-Length:");
            if (line != NULL && (unsigned char *)line < header_end)
                *content_length = (size_t)strtoul(line + 15, NULL, 10);
            if (used >= *body_offset + *content_length)
                break;
        }
    }
    return (int)used;
}

static void header_value(const char *buffer, const char *name,
                         char *out, size_t out_size)
{
    const char *line = strcasestr(buffer, name);
    size_t used = 0;
    out[0] = '\0';
    if (line == NULL)
        return;
    line += strlen(name);
    while (*line == ' ' || *line == '\t')
        line++;
    while (*line && *line != '\r' && *line != '\n' && used + 1 < out_size)
        out[used++] = *line++;
    out[used] = '\0';
}

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : 8002;
    const char *log_path = argc > 2 ? argv[2] : "/tmp/mock-dm.log";
    int server = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    struct sockaddr_in addr;
    FILE *log;
    int requests = 0;
    if (server < 0) {
        perror("socket");
        return 1;
    }
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((unsigned short)port);
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(server, 8) < 0) {
        perror("listen");
        return 1;
    }
    log = fopen(log_path, "a");
    if (log == NULL) {
        perror("log");
        return 1;
    }
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    printf("MOCK_DM_READY port=%d\n", port);
    fflush(stdout);
    while (running) {
        int client = accept(server, NULL, NULL);
        unsigned char *request = malloc(MAX_REQUEST);
        size_t body_offset = 0;
        size_t content_length = 0;
        static const char response[] =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/vnd.syncml.dm+xml\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        if (client < 0)
            continue;
        if (request == NULL) {
            close(client);
            continue;
        }
        int used = read_request(client, request, MAX_REQUEST,
                                &body_offset, &content_length);
        char method[16];
        char path[256];
        char content_type[128];
        char prefix[BODY_PREFIX * 3 + 1];
        size_t prefix_len = 0;
        header_value((char *)request, "Content-Type:", content_type,
                     sizeof(content_type));
        method[0] = '\0';
        path[0] = '\0';
        if (used > 0) {
            char *space = strchr((char *)request, ' ');
            if (space != NULL) {
                size_t method_len = (size_t)(space - (char *)request);
                if (method_len < sizeof(method)) {
                    memcpy(method, request, method_len);
                    method[method_len] = '\0';
                }
                char *path_start = space + 1;
                char *path_end = strchr(path_start, ' ');
                if (path_end != NULL) {
                    size_t path_len = (size_t)(path_end - path_start);
                    if (path_len < sizeof(path)) {
                        memcpy(path, path_start, path_len);
                        path[path_len] = '\0';
                    }
                }
            }
        }
        for (size_t index = 0; index < BODY_PREFIX
             && body_offset + index < (size_t)used; index++) {
            prefix_len += (size_t)snprintf(prefix + prefix_len,
                                           sizeof(prefix) - prefix_len,
                                           "%02x", request[body_offset + index]);
        }
        prefix[prefix_len] = '\0';
        fprintf(log,
                "MOCK_DM_REQUEST method=%s path=%s content_type=%s "
                "content_length=%zu body_prefix=%s\n",
                method, path, content_type, content_length, prefix);
        fflush(log);
        write(client, response, sizeof(response) - 1);
        close(client);
        free(request);
        requests++;
        fprintf(log, "MOCK_DM_RESPONSE status=200 requests=%d\n", requests);
        fflush(log);
    }
    fclose(log);
    close(server);
    return 0;
}
