/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 */

/*
 * mock_dm_server - loopback-only lab DM endpoint for dmengine tests.
 *
 * Listens on 127.0.0.1, accepts one request at a time, and logs a structured
 * summary of the request (method, path, content type, length and a short body
 * prefix). For WBXML DM requests it extracts the SessionID, MsgID, target URL
 * and IMEI, then returns a minimal WBXML SyncML DM response with Status entries
 * for the captured Alert/Replace commands and a Final element.
 *
 * The response is a lab fixture: it acknowledges the client's initial commands
 * but performs no management operations and makes no update-success claim.
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
#define MAX_STRINGS 32
#define MAX_STRING 256
#define MAX_RESPONSE (16 * 1024)

#define TOKEN_SYNCML 0x6d
#define TOKEN_SYNCHDR 0x6c
#define TOKEN_VERDTD 0x71
#define TOKEN_VERPROTO 0x72
#define TOKEN_SESSIONID 0x65
#define TOKEN_MSGID 0x5b
#define TOKEN_TARGET 0x6e
#define TOKEN_LOCURI 0x57
#define TOKEN_SOURCE 0x67
#define TOKEN_SYNCBODY 0x6b
#define TOKEN_STATUS 0x69
#define TOKEN_CMDID 0x4b
#define TOKEN_MSGREF 0x5c
#define TOKEN_CMDREF 0x4c
#define TOKEN_CMD 0x4a
#define TOKEN_DATA 0x4f
#define TOKEN_FINAL 0x12
#define TOKEN_REPLACE 0x60
#define TOKEN_EXEC 0x51
#define WBXML_END 0x01
#define WBXML_STRING 0x03
#define WBXML_OPAQUE 0xc3

struct wbxml_string {
    size_t offset;
    size_t length;
    char value[MAX_STRING];
};

struct buffer {
    unsigned char data[MAX_RESPONSE];
    size_t length;
};

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

static int collect_wbxml_strings(const unsigned char *body, size_t length,
                                 struct wbxml_string *strings, int max)
{
    int count = 0;
    size_t index = 0;
    while (index + 2 < length && count < max) {
        if (body[index] == 0xC3) {
            size_t string_length = body[index + 1];
            if (string_length > 0 && string_length < MAX_STRING
                && index + 2 + string_length <= length) {
                strings[count].offset = index;
                strings[count].length = string_length;
                memcpy(strings[count].value, body + index + 2, string_length);
                strings[count].value[string_length] = '\0';
                count++;
                index += 2 + string_length;
                continue;
            }
        }
        index++;
    }
    return count;
}

static const char *find_string(const struct wbxml_string *strings, int count,
                               const char *prefix)
{
    for (int index = 0; index < count; index++) {
        if (strncmp(strings[index].value, prefix, strlen(prefix)) == 0)
            return strings[index].value;
    }
    return NULL;
}

static int find_msgid(const unsigned char *body, size_t length, size_t after,
                      char *out, size_t out_size)
{
    for (size_t index = after; index + 2 < length; index++) {
        if (body[index] != 0x5B)
            continue;
        size_t cursor = index + 1;
        if (cursor < length && body[cursor] == 0x01)
            cursor++;
        if (cursor + 1 < length && body[cursor] == 0x03) {
            size_t string_length = body[cursor + 1];
            if (string_length > 0 && string_length + 1 < out_size
                && cursor + 2 + string_length <= length) {
                memcpy(out, body + cursor + 2, string_length);
                out[string_length] = '\0';
                return 1;
            }
        }
    }
    return 0;
}

static void put_byte(struct buffer *buffer, unsigned char value)
{
    if (buffer->length < sizeof(buffer->data))
        buffer->data[buffer->length++] = value;
}

static void put_string(struct buffer *buffer, const char *value)
{
    put_byte(buffer, WBXML_STRING);
    for (const char *cursor = value; *cursor; cursor++)
        put_byte(buffer, (unsigned char)*cursor);
    put_byte(buffer, 0x00);
}

static void put_opaque(struct buffer *buffer, const char *value)
{
    size_t length = strlen(value);
    if (length > 0x7f)
        return;
    put_byte(buffer, WBXML_OPAQUE);
    put_byte(buffer, (unsigned char)length);
    for (size_t index = 0; index < length; index++)
        put_byte(buffer, (unsigned char)value[index]);
}

static void put_status(struct buffer *buffer, const char *cmdid,
                       const char *cmdref, const char *command)
{
    put_byte(buffer, TOKEN_STATUS);
    put_byte(buffer, TOKEN_CMDID);
    put_string(buffer, cmdid);
    put_byte(buffer, WBXML_END);
    put_byte(buffer, TOKEN_MSGREF);
    put_string(buffer, "1");
    put_byte(buffer, WBXML_END);
    put_byte(buffer, TOKEN_CMDREF);
    put_string(buffer, cmdref);
    put_byte(buffer, WBXML_END);
    put_byte(buffer, TOKEN_CMD);
    put_string(buffer, command);
    put_byte(buffer, WBXML_END);
    put_byte(buffer, TOKEN_DATA);
    put_string(buffer, "200");
    put_byte(buffer, WBXML_END);
    put_byte(buffer, WBXML_END);
}

static void put_replace(struct buffer *buffer, const char *locuri,
                        const char *pkgurl)
{
    (void)pkgurl;
    put_byte(buffer, TOKEN_EXEC);
    put_byte(buffer, TOKEN_CMDID);
    put_string(buffer, "4");
    put_byte(buffer, WBXML_END);
    put_byte(buffer, 0x54);             /* Item */
    put_byte(buffer, TOKEN_TARGET);
    put_byte(buffer, TOKEN_LOCURI);
    put_opaque(buffer, locuri);
    put_byte(buffer, WBXML_END);
    put_byte(buffer, WBXML_END);
    put_byte(buffer, WBXML_END);        /* Item */
    put_byte(buffer, WBXML_END);        /* Exec */
}

static int build_response(const unsigned char *body, size_t length,
                          unsigned char *output, size_t capacity,
                          char *session, size_t session_size,
                          int send_exec)
{
    static const char string_table[] = "-//SYNCML//DTD SyncML 1.2//EN";
    struct wbxml_string strings[MAX_STRINGS];
    struct buffer buffer;
    int count = collect_wbxml_strings(body, length, strings, MAX_STRINGS);
    const char *url = find_string(strings, count, "http");
    const char *imei = find_string(strings, count, "IMEI:");
    char msgid[32] = "1";
    size_t session_offset;
    if (count == 0)
        return 0;
    const char *sid = strings[0].value;
    session_offset = strings[0].offset;
    if (sid[0] == '\0')
        return 0;
    if (url == NULL)
        url = "http://127.0.0.1:8002/dm/message";
    if (imei == NULL)
        imei = "IMEI:LAB000000000000";
    find_msgid(body, length, session_offset, msgid, sizeof(msgid));
    if (strlen(sid) >= session_size)
        return 0;
    memcpy(session, sid, strlen(sid) + 1);
    if (capacity < 5 + sizeof(string_table) + 512)
        return 0;
    buffer.length = 0;
    put_byte(&buffer, 0x02);            /* WBXML 1.2 */
    put_byte(&buffer, 0xa4);            /* SyncML 1.2 public ID */
    put_byte(&buffer, 0x01);
    put_byte(&buffer, 0x6a);            /* UTF-8 */
    put_byte(&buffer, (unsigned char)(sizeof(string_table) - 1));
    for (size_t index = 0; index + 1 < sizeof(string_table); index++)
        put_byte(&buffer, (unsigned char)string_table[index]);

    put_byte(&buffer, TOKEN_SYNCML);
    put_byte(&buffer, TOKEN_SYNCHDR);
    put_byte(&buffer, TOKEN_VERDTD);
    put_string(&buffer, "1.2");
    put_byte(&buffer, WBXML_END);
    put_byte(&buffer, TOKEN_VERPROTO);
    put_string(&buffer, "DM/1.2");
    put_byte(&buffer, WBXML_END);
    put_byte(&buffer, TOKEN_SESSIONID);
    put_opaque(&buffer, sid);
    put_byte(&buffer, WBXML_END);
    put_byte(&buffer, TOKEN_MSGID);
    put_string(&buffer, "2");
    put_byte(&buffer, WBXML_END);
    put_byte(&buffer, TOKEN_TARGET);
    put_byte(&buffer, TOKEN_LOCURI);
    put_opaque(&buffer, imei);
    put_byte(&buffer, WBXML_END);
    put_byte(&buffer, WBXML_END);
    put_byte(&buffer, TOKEN_SOURCE);
    put_byte(&buffer, TOKEN_LOCURI);
    put_opaque(&buffer, url);
    put_byte(&buffer, WBXML_END);
    put_byte(&buffer, WBXML_END);
    put_byte(&buffer, WBXML_END);       /* SyncHdr */
    put_byte(&buffer, TOKEN_SYNCBODY);
    put_status(&buffer, "1", "1", "Alert");
    put_status(&buffer, "2", "2", "Replace");
    put_status(&buffer, "3", "3", "Alert");
    if (send_exec)
        put_replace(&buffer, "./ManagedObjects/FUMO/Download",
                    "http://127.0.0.1:8002/package.bin");
    put_byte(&buffer, TOKEN_FINAL);
    put_byte(&buffer, WBXML_END);       /* SyncBody */
    put_byte(&buffer, WBXML_END);       /* SyncML */
    if (buffer.length > capacity)
        return 0;
    memcpy(output, buffer.data, buffer.length);
    return (int)buffer.length;
}

static void send_all(int fd, const unsigned char *data, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        ssize_t written = write(fd, data + sent, length - sent);
        if (written <= 0)
            break;
        sent += (size_t)written;
    }
}

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : 8002;
    const char *log_path = argc > 2 ? argv[2] : "/tmp/mock-dm.log";
    int download_mode = argc > 3 && strcmp(argv[3], "download") == 0;
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
        char session[64] = "";
        unsigned char response[MAX_RESPONSE];
        int response_body = 0;
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
        if (strcmp(method, "GET") == 0 && strcmp(path, "/package.bin") == 0) {
            unsigned char package[4096];
            char header[256];
            int header_length;
            for (size_t index = 0; index < sizeof(package); index++)
                package[index] = (unsigned char)(index & 0xff);
            header_length = snprintf(
                header, sizeof(header),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/octet-stream\r\n"
                "Content-Length: %zu\r\n"
                "Connection: close\r\n\r\n", sizeof(package));
            send_all(client, (const unsigned char *)header,
                     (size_t)header_length);
            send_all(client, package, sizeof(package));
            fprintf(log, "MOCK_DM_DOWNLOAD method=GET path=/package.bin "
                         "bytes=%zu sha256_prefix=%02x%02x%02x%02x\n",
                    sizeof(package), package[0], package[1], package[2],
                    package[3]);
            fflush(log);
            close(client);
            free(request);
            requests++;
            fprintf(log, "MOCK_DM_DOWNLOAD_DONE requests=%d\n", requests);
            fflush(log);
            continue;
        }
        if (requests == 1 && (size_t)used > body_offset) {
            FILE *second = fopen("/tmp/dm-request2.bin", "wb");
            if (second != NULL) {
                fwrite(request + body_offset, 1,
                       (size_t)used - body_offset, second);
                fclose(second);
            }
            for (size_t index = 0; index + 3 <= (size_t)used - body_offset;
                 index++) {
                if (request[body_offset + index] == '4'
                    && request[body_offset + index + 1] == '0'
                    && request[body_offset + index + 2] == '7') {
                    fprintf(log, "MOCK_DM_CLIENT_STATUS status=407\n");
                    fflush(log);
                    break;
                }
            }
        }
        if ((size_t)used > body_offset) {
            response_body = build_response(
                request + body_offset, (size_t)used - body_offset,
                response, sizeof(response), session, sizeof(session),
                download_mode && requests == 0);
        }
        fprintf(log,
                "MOCK_DM_REQUEST method=%s path=%s content_type=%s "
                "content_length=%zu body_prefix=%s\n",
                method, path, content_type, content_length, prefix);
        if (response_body > 0) {
            char header[256];
            int header_length = snprintf(
                header, sizeof(header),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/vnd.syncml.dm+wbxml\r\n"
                "Content-Length: %d\r\n"
                "Connection: close\r\n\r\n", response_body);
            send_all(client, (const unsigned char *)header,
                     (size_t)header_length);
            send_all(client, response, (size_t)response_body);
            fprintf(log,
                    "MOCK_DM_RESPONSE status=200 syncml=1 session=%s "
                    "bytes=%d exec=%d\n", session, response_body,
                    download_mode && requests == 0);
        } else {
            static const char empty_response[] =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/vnd.syncml.dm+wbxml\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n";
            send_all(client, (const unsigned char *)empty_response,
                     sizeof(empty_response) - 1);
            fprintf(log, "MOCK_DM_RESPONSE status=200 syncml=0\n");
        }
        fflush(log);
        close(client);
        free(request);
        requests++;
        fprintf(log, "MOCK_DM_RESPONSE_DONE requests=%d\n", requests);
        fflush(log);
    }
    fclose(log);
    close(server);
    return 0;
}
