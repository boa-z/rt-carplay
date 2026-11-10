/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_rtsp_server.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "airplay_info_plist.h"
#include "rtcp/rtcp.h"
#include "rtcp_linux.h"

#define INFO_PLIST_BYTES AIRPLAY_RTSP_SESSION_INFO_MAX_BYTES
#define WAIT_LIMIT_MS 3000U

static unsigned screen_messages;

static int capture_screen(airplay_screen_message_type_t type, uint8_t *annex_b,
                          size_t annex_b_len, size_t prefix_capacity,
                          uint64_t sender_time_ns, uint64_t arrival_time_us,
                          void *context)
{
    (void)annex_b;
    (void)annex_b_len;
    (void)prefix_capacity;
    (void)sender_time_ns;
    (void)arrival_time_us;
    (void)context;
    (void)type;
    ++screen_messages;
    return RTCP_OK;
}

static bool test_random(uint8_t *output, size_t output_len, void *context)
{
    (void)context;
    memset(output, 0x5a, output_len);
    return true;
}

static bool test_sign(const uint8_t *message, size_t message_len,
                      uint8_t signature[64], void *context)
{
    (void)message;
    (void)message_len;
    (void)context;
    memset(signature, 0x3c, 64U);
    return true;
}

static bool test_verify(const uint8_t public_key[32], const uint8_t *message,
                        size_t message_len, const uint8_t signature[64],
                        void *context)
{
    (void)public_key;
    (void)message;
    (void)message_len;
    (void)signature;
    (void)context;
    return true;
}

static bool test_save(const uint8_t *identifier, size_t identifier_len,
                      const uint8_t public_key[32], void *context)
{
    (void)identifier;
    (void)identifier_len;
    (void)public_key;
    (void)context;
    return true;
}

static bool test_lookup(const uint8_t *identifier, size_t identifier_len,
                        uint8_t public_key[32], void *context)
{
    (void)identifier;
    (void)identifier_len;
    (void)public_key;
    (void)context;
    return false;
}

static void fill_pairing(airplay_rtsp_pairing_config_t *pairing)
{
    static const uint8_t identifier[] = "rtsp-server-test";
    static const uint8_t public_key[32] = {1U};
    memset(pairing, 0, sizeof(*pairing));
    pairing->pair_setup.pairing_code = "server-test-code";
    pairing->pair_setup.accessory_identifier = identifier;
    pairing->pair_setup.accessory_identifier_len = sizeof(identifier) - 1U;
    pairing->pair_setup.accessory_public_key = public_key;
    pairing->pair_setup.random = test_random;
    pairing->pair_setup.sign_accessory = test_sign;
    pairing->pair_setup.verify_controller = test_verify;
    pairing->pair_setup.save_pairing = test_save;
    pairing->pair_verify.accessory_identifier = identifier;
    pairing->pair_verify.accessory_identifier_len = sizeof(identifier) - 1U;
    pairing->pair_verify.random = test_random;
    pairing->pair_verify.sign_accessory = test_sign;
    pairing->pair_verify.verify_controller = test_verify;
    pairing->pair_verify.lookup_controller = test_lookup;
}

static size_t build_info_plist(uint8_t *buffer, size_t capacity)
{
    airplay_info_plist_config_t info = {0};
    info.device_id = "AA:BB:CC:DD:EE:FF";
    info.bluetooth_id = "11:22:33:44:55:66";
    info.name = "rt-carplay test";
    info.width_pixels = 800U;
    info.height_pixels = 480U;
    size_t info_len = 0U;
    assert(airplay_info_plist_build(&info, buffer, capacity, &info_len));
    return info_len;
}

typedef struct {
    int client_fd;
    uint8_t bytes[8192];
    size_t len;
} client_t;

static bool client_connect(client_t *client, uint16_t port)
{
    rtcp_addr_t address = {0};
    address.family = RTCP_AF_INET;
    address.addr[0] = 127U;
    address.addr[3] = 1U;
    address.port = port;
    client->client_fd = rtcp_net_linux.connect(&address);
    client->len = 0U;
    return client->client_fd >= 0;
}

static bool client_send(client_t *client, const void *bytes, size_t len)
{
    size_t offset = 0U;
    while (offset < len) {
        int sent = rtcp_net_linux.send(client->client_fd,
                                       (const uint8_t *)bytes + offset,
                                       len - offset);
        if (sent <= 0) return false;
        offset += (size_t)sent;
    }
    return true;
}

static bool client_has_data(int fd, int timeout_ms)
{
    struct rtcp_pollfd pfd = {.fd = fd, .events = RTCP_POLLIN};
    int ready = rtcp_net_linux.poll(&pfd, 1, timeout_ms);
    return ready > 0 && (pfd.revents & RTCP_POLLIN) != 0;
}

static bool client_read(client_t *client)
{
    int received = rtcp_net_linux.recv(client->client_fd,
                                       client->bytes + client->len,
                                       sizeof(client->bytes) - client->len);
    if (received <= 0) return false;
    client->len += (size_t)received;
    return true;
}

static bool contains(const uint8_t *bytes, size_t length, const char *needle)
{
    size_t needle_len = strlen(needle);
    if (needle_len > length) return false;
    for (size_t i = 0U; i <= length - needle_len; ++i) {
        if (memcmp(bytes + i, needle, needle_len) == 0) return true;
    }
    return false;
}

/* Drives the server until the client has received `needle`. */
static void expect_response(airplay_rtsp_server_t *server, client_t *client,
                            const char *needle)
{
    uint64_t start = rtcp_os_linux.now_ms();
    while (rtcp_os_linux.now_ms() - start < WAIT_LIMIT_MS) {
        airplay_rtsp_server_poll(server, 10);
        if (contains(client->bytes, client->len, needle)) return;
        if (client_has_data(client->client_fd, 0) && !client_read(client)) break;
    }
    assert(contains(client->bytes, client->len, needle));
}

static void connect_client(airplay_rtsp_server_t *server, client_t *client)
{
    uint16_t port = airplay_rtsp_server_get_port(server);
    assert(port != 0U);
    assert(client_connect(client, port));

    uint64_t start = rtcp_os_linux.now_ms();
    while (rtcp_os_linux.now_ms() - start < WAIT_LIMIT_MS &&
           !airplay_rtsp_server_is_connected(server)) {
        airplay_rtsp_server_poll(server, 10);
    }
    assert(airplay_rtsp_server_is_connected(server));
    assert(!airplay_rtsp_server_is_encrypted(server));
}

static void test_plaintext_routing(void)
{
    uint8_t info_plist[INFO_PLIST_BYTES];
    size_t info_len = build_info_plist(info_plist, sizeof(info_plist));

    airplay_rtsp_server_config_t config = {0};
    config.os = &rtcp_os_linux;
    config.net = &rtcp_net_linux;
    fill_pairing(&config.pairing);
    config.info_plist = info_plist;
    config.info_plist_len = info_len;
    config.on_screen_message = capture_screen;

    airplay_rtsp_server_t *server = NULL;
    assert(airplay_rtsp_server_start(&config, &server) == RTCP_OK);
    assert(server != NULL);

    client_t client;
    connect_client(server, &client);

    /* A split request must not be answered early. */
    static const char info_request[] =
        "GET /info RTSP/1.0\r\nCSeq: 1\r\n\r\n";
    assert(client_send(&client, info_request, 12U));
    airplay_rtsp_server_poll(server, 20);
    airplay_rtsp_server_poll(server, 20);
    assert(!client_has_data(client.client_fd, 30));
    assert(client_send(&client, info_request + 12U,
                       sizeof(info_request) - 1U - 12U));

    expect_response(server, &client, "200 OK");
    assert(contains(client.bytes, client.len, "Content-Type: application/x-apple-binary-plist"));
    /* The capability document is served byte for byte. */
    assert(client.len > info_len);
    assert(memcmp(client.bytes + client.len - info_len, info_plist, info_len) == 0);

    /* Pipelined requests are answered in order. */
    client.len = 0U;
    static const char pipelined[] =
        "OPTIONS * RTSP/1.0\r\nCSeq: 2\r\n\r\n"
        "PUT /stream RTSP/1.0\r\nCSeq: 3\r\n\r\n";
    assert(client_send(&client, pipelined, sizeof(pipelined) - 1U));
    expect_response(server, &client, "CSeq: 2");
    expect_response(server, &client, "405 Method Not Allowed");
    assert(contains(client.bytes, client.len, "CSeq: 3"));

    /* RECORD before any SETUP is gated. */
    client.len = 0U;
    static const char record[] = "RECORD /stream RTSP/1.0\r\nCSeq: 4\r\n\r\n";
    assert(client_send(&client, record, sizeof(record) - 1U));
    expect_response(server, &client, "503 Service Unavailable");

    /* Losing the controller tears the session down. */
    rtcp_net_linux.close(client.client_fd);
    uint64_t start = rtcp_os_linux.now_ms();
    while (rtcp_os_linux.now_ms() - start < WAIT_LIMIT_MS &&
           airplay_rtsp_server_is_connected(server)) {
        airplay_rtsp_server_poll(server, 10);
    }
    assert(!airplay_rtsp_server_is_connected(server));

    /* The listener is still there: a second controller can attach. */
    client_t second;
    connect_client(server, &second);
    static const char options[] = "OPTIONS * RTSP/1.0\r\nCSeq: 9\r\n\r\n";
    assert(client_send(&second, options, sizeof(options) - 1U));
    expect_response(server, &second, "CSeq: 9");
    rtcp_net_linux.close(second.client_fd);

    airplay_rtsp_server_stop(server);
}

static bool client_connect6(client_t *client, uint16_t port)
{
    rtcp_addr_t address = {0};
    address.family = RTCP_AF_INET6;
    address.addr[15] = 1U; /* ::1 */
    address.port = port;
    client->client_fd = rtcp_net_linux.connect(&address);
    client->len = 0U;
    return client->client_fd >= 0;
}

/* Wired CarPlay attaches over IPv6 link-local, so the same RTSP server must
 * serve an IPv6 loopback controller and report the IPv6 peer address. */
static void test_ipv6_listener(void)
{
    uint8_t info_plist[INFO_PLIST_BYTES];
    size_t info_len = build_info_plist(info_plist, sizeof(info_plist));

    airplay_rtsp_server_config_t config = {0};
    config.os = &rtcp_os_linux;
    config.net = &rtcp_net_linux;
    fill_pairing(&config.pairing);
    config.info_plist = info_plist;
    config.info_plist_len = info_len;
    config.on_screen_message = capture_screen;
    config.family = RTCP_AF_INET6;

    airplay_rtsp_server_t *server = NULL;
    int result = airplay_rtsp_server_start(&config, &server);
    if (result != RTCP_OK) {
        puts("IPv6 RTSP listener unavailable; skipped");
        return;
    }

    client_t client;
    uint16_t port = airplay_rtsp_server_get_port(server);
    assert(port != 0U);
    assert(client_connect6(&client, port));
    uint64_t start = rtcp_os_linux.now_ms();
    while (rtcp_os_linux.now_ms() - start < WAIT_LIMIT_MS &&
           !airplay_rtsp_server_is_connected(server)) {
        airplay_rtsp_server_poll(server, 10);
    }
    assert(airplay_rtsp_server_is_connected(server));

    rtcp_addr_t peer = {0};
    assert(airplay_rtsp_server_get_peer(server, &peer));
    assert(peer.family == RTCP_AF_INET6);
    assert(peer.addr[15] == 1U);

    static const char info_request[] =
        "GET /info RTSP/1.0\r\nCSeq: 1\r\n\r\n";
    assert(client_send(&client, info_request, sizeof(info_request) - 1U));
    expect_response(server, &client, "200 OK");

    rtcp_net_linux.close(client.client_fd);
    airplay_rtsp_server_stop(server);
}

static void test_argument_checks(void)
{
    uint8_t info_plist[INFO_PLIST_BYTES];
    size_t info_len = build_info_plist(info_plist, sizeof(info_plist));

    airplay_rtsp_server_config_t config = {0};
    config.os = &rtcp_os_linux;
    config.net = &rtcp_net_linux;
    fill_pairing(&config.pairing);
    config.info_plist = info_plist;
    config.info_plist_len = info_len;
    config.on_screen_message = capture_screen;

    airplay_rtsp_server_t *server = NULL;
    assert(airplay_rtsp_server_start(NULL, &server) == RTCP_EINVAL);
    assert(airplay_rtsp_server_start(&config, NULL) == RTCP_EINVAL);

    config.on_screen_message = NULL;
    assert(airplay_rtsp_server_start(&config, &server) == RTCP_EINVAL);
    config.on_screen_message = capture_screen;

    config.info_plist = NULL;
    assert(airplay_rtsp_server_start(&config, &server) == RTCP_EINVAL);
    config.info_plist = info_plist;

    config.info_plist_len = info_len;
    info_plist[0] ^= 0xffU; /* not a binary plist anymore */
    assert(airplay_rtsp_server_start(&config, &server) == RTCP_EINVAL);
    info_plist[0] ^= 0xffU;

    config.net = NULL;
    assert(airplay_rtsp_server_start(&config, &server) == RTCP_EINVAL);
    config.net = &rtcp_net_linux;

    assert(airplay_rtsp_server_get_port(NULL) == 0U);
    assert(!airplay_rtsp_server_is_connected(NULL));
    assert(!airplay_rtsp_server_is_encrypted(NULL));
    assert(airplay_rtsp_server_poll(NULL, 0) == RTCP_EINVAL);
    airplay_rtsp_server_stop(NULL);
}

int main(void)
{
    test_plaintext_routing();
    test_ipv6_listener();
    test_argument_checks();
    assert(screen_messages == 0U);
    puts("airplay RTSP server host test passed");
    return 0;
}
