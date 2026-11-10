/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "carplay_ctrl.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rtcp/rtcp.h"

typedef struct {
    uint8_t request[512];
    size_t request_len;
    rtcp_addr_t peer;
    const char *response;
    size_t response_len;
    size_t response_used;
    bool closed;
    bool connected;
} mock_net_t;

static mock_net_t s_mock;

static void mock_reset(const char *response)
{
    memset(&s_mock, 0, sizeof(s_mock));
    s_mock.response = response;
    s_mock.response_len = response != NULL ? strlen(response) : 0U;
    s_mock.connected = true;
}

static int mock_connect(const rtcp_addr_t *addr)
{
    (void)addr;
    assert(false && "connect_timeout should be preferred");
    return RTCP_EIO;
}

static int mock_connect_timeout(const rtcp_addr_t *addr, int timeout_ms)
{
    assert(addr != NULL && timeout_ms > 0);
    s_mock.peer = *addr;
    return 42;
}

static int mock_send(int fd, const void *buf, size_t len)
{
    assert(fd == 42);
    assert(s_mock.request_len + len <= sizeof(s_mock.request));
    memcpy(s_mock.request + s_mock.request_len, buf, len);
    s_mock.request_len += len;
    return (int)len;
}

static int mock_recv(int fd, void *buf, size_t len)
{
    assert(fd == 42);
    size_t remaining = s_mock.response_len - s_mock.response_used;
    if (remaining == 0U) return 0;
    if (len > remaining) len = remaining;
    memcpy(buf, s_mock.response + s_mock.response_used, len);
    s_mock.response_used += len;
    return (int)len;
}

static int mock_poll(struct rtcp_pollfd *fds, int n, int timeout_ms)
{
    (void)timeout_ms;
    assert(n == 1 && fds != NULL);
    fds[0].revents = RTCP_POLLIN;
    return 1;
}

static void mock_close(int fd)
{
    assert(fd == 42);
    s_mock.closed = true;
}

static const struct rtcp_net s_net = {
    .connect = mock_connect,
    .connect_timeout = mock_connect_timeout,
    .send = mock_send,
    .recv = mock_recv,
    .poll = mock_poll,
    .close = mock_close,
};

static void test_device_id(void)
{
    uint64_t value = 0U;
    assert(carplay_ctrl_parse_device_id("AA:BB:CC:DD:EE:FF", &value));
    assert(value == 0xAABBCCDDEEFFULL);
    assert(carplay_ctrl_parse_device_id("aabbccddeeff", &value));
    assert(value == 0xAABBCCDDEEFFULL);
    assert(carplay_ctrl_parse_device_id("00:11:22:33:44:55", &value));
    assert(value == 0x001122334455ULL);
    assert(!carplay_ctrl_parse_device_id("AA:BB:CC:DD:EE", &value));
    assert(!carplay_ctrl_parse_device_id("AA:BB:CC:DD:EE:FF:00", &value));
    assert(!carplay_ctrl_parse_device_id("GG:BB:CC:DD:EE:FF", &value));
    assert(!carplay_ctrl_parse_device_id("", &value));
    assert(!carplay_ctrl_parse_device_id(NULL, &value));
}

static void test_request_builder(void)
{
    static const char expected[] =
        "GET /ctrl-int/1/connect HTTP/1.1\r\n"
        "Host: carplay\r\n"
        "User-Agent: AirPlay/950.7.1\r\n"
        "AirPlay-Receiver-Device-ID: 187723572702975\r\n"
        "Connection: close\r\n"
        "\r\n";
    uint8_t request[512];
    size_t length = carplay_ctrl_build_connect_request(
        0xAABBCCDDEEFFULL, "AirPlay/950.7.1", request, sizeof(request));
    assert(length == sizeof(expected) - 1U);
    assert(memcmp(request, expected, length) == 0);

    /* A short buffer never produces a partial request. */
    assert(carplay_ctrl_build_connect_request(1U, "AirPlay/1.0", request, 32U) ==
           0U);
    assert(carplay_ctrl_build_connect_request(1U, NULL, request,
                                              sizeof(request)) == 0U);
}

static void test_connect(void)
{
    const uint8_t ipv4[4] = {192U, 168U, 43U, 2U};
    int status = -1;

    mock_reset("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    assert(carplay_ctrl_connect(&s_net, ipv4, 52000, 0xAABBCCDDEEFFULL,
                                "AirPlay/950.7.1", 1000, &status) == RTCP_OK);
    assert(status == 200);
    assert(s_mock.closed);
    assert(s_mock.peer.family == RTCP_AF_INET);
    assert(s_mock.peer.port == 52000U);
    assert(memcmp(s_mock.peer.addr, ipv4, 4U) == 0);
    assert(s_mock.request_len > 0U);
    assert(strstr((const char *)s_mock.request, "GET /ctrl-int/1/connect") !=
           NULL);
    assert(strstr((const char *)s_mock.request, "AirPlay-Receiver-Device-ID: "
                                                "187723572702975") != NULL);

    mock_reset("HTTP/1.1 403 Forbidden\r\n\r\n");
    status = -1;
    assert(carplay_ctrl_connect(&s_net, ipv4, 52000, 1U, "AirPlay/950.7.1",
                                1000, &status) == RTCP_OK);
    assert(status == 403);
    assert(s_mock.closed);

    /* The peer accepts but never answers: bounded, reported as a timeout. */
    mock_reset(NULL);
    assert(carplay_ctrl_connect(&s_net, ipv4, 52000, 1U, "AirPlay/950.7.1",
                                300, &status) == RTCP_ETIMEDOUT);
    assert(s_mock.closed);

    /* A garbage status line is not a success. */
    mock_reset("NOT-HTTP\r\n");
    assert(carplay_ctrl_connect(&s_net, ipv4, 52000, 1U, "AirPlay/950.7.1",
                                300, &status) == RTCP_EIO);
}

int main(void)
{
    test_device_id();
    test_request_builder();
    test_connect();
    puts("carplay ctrl host test passed");
    return 0;
}
