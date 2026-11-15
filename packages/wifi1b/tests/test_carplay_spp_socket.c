/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 */
#define _GNU_SOURCE
#include "cp_spp_socket.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

void cp_spp_rx_hook(void *event);

static size_t fixture(uint8_t *data, size_t payload)
{
    memset(data, 0, CP_SPP_SOCKET_MAX_FRAME + 1);
    memcpy(data, "W1BSPP01", 8);
    data[8] = (uint8_t)payload;
    data[9] = (uint8_t)(payload >> 8);
    data[10] = (uint8_t)(payload >> 16);
    data[11] = (uint8_t)(payload >> 24);
    memcpy(data + 12, "02:00:00:00:00:01", 17);
    data[29] = 1;
    memset(data + 32, 0x5a, payload);
    return 32 + payload;
}

static unsigned descriptor_count(void)
{
    DIR *dir = opendir("/proc/self/fd");
    struct dirent *entry;
    unsigned count = 0;
    assert(dir != NULL);
    while ((entry = readdir(dir)) != NULL)
        if (entry->d_name[0] != '.') count++;
    closedir(dir);
    return count;
}

int main(int argc, char **argv)
{
    uint8_t wire[CP_SPP_SOCKET_MAX_FRAME + 1], received[CP_SPP_SOCKET_MAX_FRAME];
    char path[108], moved[108], link_path[108], sub_path[108];
    cp_spp_packet packet;
    cp_spp_socket receiver, other;
    struct sockaddr_un address;
    struct stat st;
    size_t length;
    int sender, file;
    unsigned before, i;
    int pair[2];
    assert(argc == 2);
    assert(snprintf(path, sizeof(path), "%s/rx.sock", argv[1]) < (int)sizeof(path));
    assert(snprintf(moved, sizeof(moved), "%s/saved.sock", argv[1]) < (int)sizeof(moved));
    assert(snprintf(link_path, sizeof(link_path), "%s/alias", argv[1]) < (int)sizeof(link_path));
    assert(snprintf(sub_path, sizeof(sub_path), "%s/alias/rx.sock", argv[1]) < (int)sizeof(sub_path));
    length = fixture(wire, 1);
    assert(cp_spp_packet_decode(wire, length, &packet) == 0);
    assert(packet.payload_length == 1 && packet.payload[0] == 0x5a);
    assert(strcmp(packet.peer, "02:00:00:00:00:01") == 0);
    for (i = 0; i < 32; i++) {
        uint8_t saved = wire[i];
        wire[i] = 0xff;
        assert(cp_spp_packet_decode(wire, length, &packet) == -1);
        assert(packet.payload == NULL && packet.payload_length == 0);
        wire[i] = saved;
    }
    for (i = 0; i < 33; i++)
        assert(cp_spp_packet_decode(wire, i, &packet) == -1);
    assert(cp_spp_packet_decode(wire, length + 1, &packet) == -1);
    length = fixture(wire, CP_SPP_SOCKET_MAX_PAYLOAD);
    assert(cp_spp_packet_decode(wire, length, &packet) == 0);
    cp_spp_socket_init(&receiver);
    cp_spp_socket_init(&other);
    before = descriptor_count();
    /* A descriptor without SO_PASSCRED must never establish sender trust. */
    assert(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
    other.fd = pair[0];
    assert(send(pair[1], wire, length, MSG_NOSIGNAL) == (ssize_t)length);
    assert(cp_spp_socket_receive(&other, received, sizeof(received), &packet) == -1);
    assert(errno == EPERM && packet.payload == NULL);
    cp_spp_socket_close(&other);
    close(pair[1]);
    assert(chmod(argv[1], 0755) == 0);
    assert(cp_spp_socket_open_fixture(&receiver, path) == -1 && errno == EPERM);
    assert(chmod(argv[1], 0700) == 0);
    assert(symlink(argv[1], link_path) == 0);
    assert(cp_spp_socket_open_fixture(&receiver, sub_path) == -1);
    assert(unlink(link_path) == 0);
    file = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(file >= 0); close(file);
    assert(cp_spp_socket_open_fixture(&receiver, path) == -1 && errno == EEXIST);
    assert(unlink(path) == 0);
    assert(symlink("missing", path) == 0);
    assert(cp_spp_socket_open_fixture(&receiver, path) == -1 && errno == EEXIST);
    assert(unlink(path) == 0);
    assert(cp_spp_socket_open_fixture(&receiver, path) == 0);
    assert(lstat(path, &st) == 0 && S_ISSOCK(st.st_mode) && (st.st_mode & 0777) == 0600);
    assert(cp_spp_socket_open_fixture(&other, path) == -1 && errno == EEXIST);
    assert(cp_spp_socket_receive(&receiver, received, sizeof(received), &packet) == 0);
    /* End-to-end first-party hook -> kernel credentials -> socket decoder.
     * This is a synthetic event, never an OEM process or live Bluetooth peer. */
    {
        union { uint64_t align; uint8_t bytes[0x60]; } event;
        const char *peer = "02:00:00:00:00:01";
        const char *uuid = CP_SPP_RX_UUID_IAP2_ACCESSORY_CAFF;
        const uint8_t payload[] = {0xff, 0x55, 0x02, 0x00, 0xee, 0x10};
        const uint8_t *payload_pointer = payload;
        uint64_t peer_length = 17, uuid_length = 36;
        uint32_t payload_length = sizeof(payload);
        memset(&event, 0, sizeof(event));
        memcpy(event.bytes + 0x08, &peer, sizeof(peer));
        memcpy(event.bytes + 0x10, &peer_length, sizeof(peer_length));
        memcpy(event.bytes + 0x30, &uuid, sizeof(uuid));
        memcpy(event.bytes + 0x38, &uuid_length, sizeof(uuid_length));
        memcpy(event.bytes + 0x50, &payload_pointer, sizeof(payload_pointer));
        memcpy(event.bytes + 0x58, &payload_length, sizeof(payload_length));
        cp_spp_rx_hook(event.bytes);
        assert(cp_spp_socket_receive(&receiver, received, sizeof(received), &packet) == 1);
        assert(packet.sender_pid == getpid() && packet.uuid_kind == 1);
        assert(packet.payload_length == sizeof(payload));
        assert(memcmp(packet.payload, payload, sizeof(payload)) == 0);
    }
    sender = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    assert(sender >= 0);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    assert(sendto(sender, wire, length, MSG_NOSIGNAL, (struct sockaddr *)&address,
                  sizeof(address)) == (ssize_t)length);
    assert(cp_spp_socket_receive(&receiver, received, sizeof(received), &packet) == 1);
    assert(packet.sender_pid == getpid() && packet.payload_length == CP_SPP_SOCKET_MAX_PAYLOAD);
    assert(sendto(sender, wire, length + 1, MSG_NOSIGNAL, (struct sockaddr *)&address,
                  sizeof(address)) == (ssize_t)length + 1);
    assert(cp_spp_socket_receive(&receiver, received, sizeof(received), &packet) == -1);
    assert(errno == EMSGSIZE && packet.payload == NULL);
    length = fixture(wire, 1);
    wire[30] = 1;
    assert(sendto(sender, wire, length, MSG_NOSIGNAL, (struct sockaddr *)&address,
                  sizeof(address)) == (ssize_t)length);
    assert(cp_spp_socket_receive(&receiver, received, sizeof(received), &packet) == -1);
    assert(errno == EBADMSG);
    wire[30] = 0;
    /* Reject ancillary rights without leaking the descriptors recvmsg installs. */
    for (i = 0; i < 100; i++) {
        union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control;
        struct msghdr message;
        struct iovec vector = {wire, length};
        struct cmsghdr *header;
        memset(&message, 0, sizeof(message));
        memset(&control, 0, sizeof(control));
        message.msg_name = &address;
        message.msg_namelen = sizeof(address);
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        message.msg_control = control.bytes;
        message.msg_controllen = sizeof(control.bytes);
        header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(header), &sender, sizeof(sender));
        assert(sendmsg(sender, &message, MSG_NOSIGNAL) == (ssize_t)length);
        assert(cp_spp_socket_receive(&receiver, received, sizeof(received), &packet) == -1);
    }
    close(sender);
    assert(rename(path, moved) == 0);
    file = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(file >= 0); close(file);
    cp_spp_socket_close(&receiver);
    cp_spp_socket_close(&receiver);
    assert(lstat(path, &st) == 0 && S_ISREG(st.st_mode));
    assert(unlink(path) == 0 && unlink(moved) == 0);
    for (i = 0; i < 100; i++) {
        assert(cp_spp_socket_open_fixture(&receiver, path) == 0);
        cp_spp_socket_close(&receiver);
        assert(lstat(path, &st) == -1 && errno == ENOENT);
    }
    assert(descriptor_count() == before);
    puts("SPP receive socket: PASS (fixture uid, no OEM or radio execution)");
    return 0;
}
