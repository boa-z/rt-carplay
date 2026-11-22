/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_event_port.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "airplay_bplist.h"
#include "airplay_control_cipher.h"
#include "rtcp/rtcp.h"
#include "rtcp_linux.h"

#define WAIT_MS     500
#define FRAME_BYTES (AIRPLAY_CONTROL_CIPHER_MAX_FRAME_BYTES + \
                     AIRPLAY_CONTROL_CIPHER_OVERHEAD_BYTES)

typedef struct
{
    bool connected;
    unsigned transitions;
} state_capture_t;

static void on_state(bool connected, void *context)
{
    state_capture_t *capture = context;
    capture->connected = connected;
    ++capture->transitions;
}

static rtcp_addr_t loopback(uint16_t port)
{
    rtcp_addr_t address = { 0 };
    address.family = RTCP_AF_INET;
    address.addr[0] = 127U;
    address.addr[3] = 1U;
    address.port = port;
    return address;
}

static bool wait_event(int fd)
{
    struct rtcp_pollfd pfd = { .fd = fd, .events = RTCP_POLLIN };
    int ready = rtcp_net_linux.poll(&pfd, 1, WAIT_MS);
    return ready > 0 && pfd.revents != 0;
}

static bool send_all(int fd, const uint8_t *bytes, size_t len)
{
    size_t offset = 0U;
    while (offset < len)
    {
        int sent = rtcp_net_linux.send(fd, bytes + offset, len - offset);
        if (sent <= 0)
            return false;
        offset += (size_t)sent;
    }
    return true;
}

static void make_keys(uint8_t read_key[32], uint8_t write_key[32])
{
    for (size_t i = 0U; i < 32U; ++i)
    {
        read_key[i] = (uint8_t)(0x30U + i);
        write_key[i] = (uint8_t)(0xa0U + i);
    }
}

static void expect_projection_command(airplay_event_port_t *port,
                                      airplay_control_cipher_t *peer, int client, bool enabled)
{
    assert(airplay_event_port_set_projection_enabled(port, enabled));
    uint8_t frame[1024], plaintext[1024];
    size_t received = 0, consumed, length;
    do
    {
        assert(wait_event(client) && received < sizeof(frame));
        int count = rtcp_net_linux.recv(client, frame + received, sizeof(frame) - received);
        assert(count > 0);
        received += (size_t)count;
    } while (received < 2U ||
             received < (size_t)(frame[0] | ((unsigned)frame[1] << 8)) + AIRPLAY_CONTROL_CIPHER_OVERHEAD_BYTES);
    assert(airplay_control_cipher_decrypt(peer, frame, received, &consumed,
                                          plaintext, sizeof(plaintext) - 1U, &length) == AIRPLAY_CONTROL_CIPHER_OK);
    plaintext[length] = '\0';
    assert(strncmp((const char *)plaintext, "POST /command RTSP/1.0\r\n", 24U) == 0);
    const char *body = strstr((const char *)plaintext, "\r\n\r\n");
    assert(body);
    body += 4;
    airplay_bplist_doc_t doc;
    airplay_bplist_object_t root, type, params, resources, resource;
    char name[16];
    size_t size;
    assert(airplay_bplist_open((const uint8_t *)body, length - (size_t)(body - (const char *)plaintext), &doc) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc, root, "type", &type) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_string(&doc, type, name, sizeof(name), &size) == AIRPLAY_BPLIST_OK);
    assert(strcmp(name, enabled ? "requestUI" : "changeModes") == 0);
    assert(airplay_bplist_dict_get(&doc, root, "params", &params) == AIRPLAY_BPLIST_OK);
    if (enabled)
    {
        assert(airplay_bplist_dict_get(&doc, params, "url", &type) == AIRPLAY_BPLIST_NOT_FOUND);
        assert(airplay_bplist_dict_get(&doc, params, "resources", &type) == AIRPLAY_BPLIST_NOT_FOUND);
        return;
    }
    assert(airplay_bplist_dict_get(&doc, params, "resources", &resources) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_array_get(&doc, resources, 0, &resource) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_array_get(&doc, resources, 1, &type) == AIRPLAY_BPLIST_NOT_FOUND);
    static const char * const keys[] = { "resourceID", "transferType", "transferPriority", "takeConstraint", "borrowConstraint" };
    static const uint64_t values[] = { 1, 1, 500, 100, 100 };
    for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); ++index)
    {
        uint64_t value;
        assert(airplay_bplist_dict_get(&doc, resource, keys[index], &type) == AIRPLAY_BPLIST_OK);
        assert(airplay_bplist_get_uint(&doc, type, &value) == AIRPLAY_BPLIST_OK && value == values[index]);
    }
}

int main(void)
{
    uint8_t read_key[32];
    uint8_t write_key[32];
    make_keys(read_key, write_key);

    state_capture_t states = { 0 };
    airplay_event_port_config_t config = { 0 };
    memcpy(config.keys.read_key, read_key, sizeof(read_key));
    memcpy(config.keys.write_key, write_key, sizeof(write_key));
    config.stream_id = 42U;
    config.initial_cseq = 200U;
    config.on_state = on_state;
    config.context = &states;

    airplay_event_port_t *port = NULL;
    assert(airplay_event_port_start(&rtcp_os_linux, &rtcp_net_linux, &config,
                                    &port) == RTCP_OK);
    assert(port != NULL);
    uint16_t port_number = airplay_event_port_get_port(port);
    assert(port_number != 0U);
    assert(states.transitions == 0U);
    assert(!airplay_event_port_set_projection_enabled(port, false));

    rtcp_addr_t address = loopback(port_number);
    int client = rtcp_net_linux.connect(&address);
    assert(client >= 0);

    assert(wait_event(airplay_event_port_get_fd(port)));
    airplay_event_port_poll(port);
    assert(states.connected);
    assert(states.transitions == 1U);

    airplay_control_cipher_t *peer = NULL;
    assert(airplay_control_cipher_create(write_key, read_key, &peer) ==
           AIRPLAY_CONTROL_CIPHER_OK);

    static const char request[] =
        "POST /command RTSP/1.0\r\n"
        "CSeq: 9\r\n"
        "Content-Type: application/x-apple-binary-plist\r\n"
        "Content-Length: 0\r\n\r\n";
    uint8_t frame[FRAME_BYTES];
    size_t frame_len = 0U;
    assert(airplay_control_cipher_encrypt(peer, (const uint8_t *)request,
                                          sizeof(request) - 1U, frame,
                                          sizeof(frame), &frame_len) ==
           AIRPLAY_CONTROL_CIPHER_OK);
    assert(send_all(client, frame, frame_len));

    /* The event port drains with one recv per readable event. */
    bool acknowledged = false;
    for (unsigned i = 0U; i < 16U && !acknowledged; ++i)
    {
        struct rtcp_pollfd pfds[2] = {
            { .fd = client, .events = RTCP_POLLIN },
            { .fd = airplay_event_port_get_fd(port), .events = RTCP_POLLIN },
        };
        int ready = rtcp_net_linux.poll(pfds, 2, WAIT_MS);
        assert(ready > 0);
        if (pfds[0].revents & RTCP_POLLIN)
        {
            acknowledged = true;
        }
        else if (pfds[1].revents != 0)
        {
            airplay_event_port_poll(port);
        }
    }
    assert(acknowledged);

    assert(wait_event(client));
    uint8_t encrypted[1024];
    int received = rtcp_net_linux.recv(client, encrypted, sizeof(encrypted));
    assert(received > 0);

    char plaintext[1024];
    size_t consumed = 0U;
    size_t plaintext_len = 0U;
    assert(airplay_control_cipher_decrypt(peer, encrypted, (size_t)received,
                                          &consumed, (uint8_t *)plaintext,
                                          sizeof(plaintext) - 1U,
                                          &plaintext_len) ==
           AIRPLAY_CONTROL_CIPHER_OK);
    plaintext[plaintext_len] = '\0';
    assert(strncmp(plaintext, "RTSP/1.0 200 OK\r\n", 17U) == 0);
    assert(strstr(plaintext, "CSeq: 9\r\n") != NULL);

    expect_projection_command(port, peer, client, false);
    expect_projection_command(port, peer, client, true);
    assert(states.connected && states.transitions == 1U);

    /* Losing the controller tears the channel down and reports it. */
    rtcp_net_linux.close(client);
    assert(wait_event(airplay_event_port_get_fd(port)));
    airplay_event_port_poll(port);
    assert(!states.connected);
    assert(states.transitions == 2U);
    assert(airplay_event_port_get_fd(port) == -1);

    airplay_control_cipher_destroy(peer);
    airplay_event_port_stop(port, &rtcp_net_linux);
    assert(states.transitions == 2U);

    /* Argument checks. */
    airplay_event_port_t *rejected = NULL;
    assert(airplay_event_port_start(NULL, &rtcp_net_linux, &config, &rejected) ==
           RTCP_EINVAL);
    assert(airplay_event_port_start(&rtcp_os_linux, &rtcp_net_linux, NULL,
                                    &rejected) == RTCP_EINVAL);
    assert(airplay_event_port_start(&rtcp_os_linux, &rtcp_net_linux, &config,
                                    NULL) == RTCP_EINVAL);
    assert(airplay_event_port_get_port(NULL) == 0U);
    assert(airplay_event_port_get_fd(NULL) == -1);
    airplay_event_port_poll(NULL);
    airplay_event_port_stop(NULL, &rtcp_net_linux);

    puts("airplay event port host test passed");
    return 0;
}
