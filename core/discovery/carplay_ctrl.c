/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "carplay_ctrl.h"

#include <stdio.h>
#include <string.h>

#include "rtcp/rtcp.h"

#define CARPLAY_CTRL_REQUEST_MAX 512U
#define CARPLAY_CTRL_STATUS_MAX 128U
#define CARPLAY_CTRL_POLL_SLICE_MS 100

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool carplay_ctrl_parse_device_id(const char *text, uint64_t *value)
{
    if (text == NULL || value == NULL) return false;
    uint64_t result = 0U;
    unsigned digits = 0U;
    for (const char *p = text; *p != '\0'; ++p) {
        if (*p == ':') continue;
        int digit = hex_value(*p);
        if (digit < 0 || digits >= 12U) return false;
        result = (result << 4U) | (uint64_t)digit;
        ++digits;
    }
    if (digits != 12U) return false;
    *value = result;
    return true;
}

size_t carplay_ctrl_build_connect_request(uint64_t receiver_device_id,
                                          const char *user_agent,
                                          uint8_t *output,
                                          size_t output_capacity)
{
    if (user_agent == NULL || user_agent[0] == '\0' || output == NULL) {
        return 0U;
    }
    int length = snprintf(
        (char *)output, output_capacity,
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: %s\r\n"
        "AirPlay-Receiver-Device-ID: %llu\r\n"
        "Connection: close\r\n"
        "\r\n",
        CARPLAY_CTRL_CONNECT_PATH, CARPLAY_CTRL_HOST, user_agent,
        (unsigned long long)receiver_device_id);
    if (length <= 0 || (size_t)length >= output_capacity) return 0U;
    return (size_t)length;
}

static bool send_all(const struct rtcp_net *net, int fd, const uint8_t *bytes,
                     size_t length)
{
    size_t offset = 0U;
    while (offset < length) {
        int sent = net->send(fd, bytes + offset, length - offset);
        if (sent <= 0) return false;
        offset += (size_t)sent;
    }
    return true;
}

/* "HTTP/1.1 200 OK" -> 200.  Anything else -> 0. */
static int parse_status_line(const char *line)
{
    if (strncmp(line, "HTTP/1.", 7) != 0) return 0;
    const char *space = strchr(line, ' ');
    if (space == NULL) return 0;
    ++space;
    if (space[0] < '0' || space[0] > '9' || space[1] < '0' || space[1] > '9' ||
        space[2] < '0' || space[2] > '9') {
        return 0;
    }
    return (space[0] - '0') * 100 + (space[1] - '0') * 10 + (space[2] - '0');
}

int carplay_ctrl_connect(const struct rtcp_net *net, const uint8_t ipv4[4],
                         uint16_t port, uint64_t receiver_device_id,
                         const char *user_agent, int timeout_ms,
                         int *http_status)
{
    if (http_status != NULL) *http_status = 0;
    if (net == NULL || ipv4 == NULL || port == 0U || net->send == NULL ||
        net->recv == NULL || net->poll == NULL || net->close == NULL) {
        return RTCP_EINVAL;
    }
    if (timeout_ms <= 0) timeout_ms = CARPLAY_CTRL_TIMEOUT_MS;

    uint8_t request[CARPLAY_CTRL_REQUEST_MAX];
    size_t request_len = carplay_ctrl_build_connect_request(
        receiver_device_id, user_agent, request, sizeof(request));
    if (request_len == 0U) return RTCP_EINVAL;

    rtcp_addr_t address;
    memset(&address, 0, sizeof(address));
    address.family = RTCP_AF_INET;
    memcpy(address.addr, ipv4, 4U);
    address.port = port;

    int fd = net->connect_timeout != NULL
                 ? net->connect_timeout(&address, timeout_ms)
                 : net->connect(&address);
    if (fd < 0) return fd;

    int result = RTCP_ETIMEDOUT;
    int status = 0;
    if (send_all(net, fd, request, request_len)) {
        char line[CARPLAY_CTRL_STATUS_MAX];
        size_t used = 0U;
        int elapsed = 0;
        while (elapsed < timeout_ms) {
            struct rtcp_pollfd descriptor = {
                .fd = fd,
                .events = RTCP_POLLIN,
                .revents = 0,
            };
            int ready = net->poll(&descriptor, 1, CARPLAY_CTRL_POLL_SLICE_MS);
            elapsed += CARPLAY_CTRL_POLL_SLICE_MS;
            if (ready < 0) break;
            if (ready == 0) continue;
            if (used + 1U >= sizeof(line)) break;
            int received = net->recv(fd, line + used, sizeof(line) - 1U - used);
            if (received <= 0) break;
            used += (size_t)received;
            line[used] = '\0';
            char *newline = memchr(line, '\n', used);
            if (newline != NULL) {
                *newline = '\0';
                status = parse_status_line(line);
                result = status != 0 ? RTCP_OK : RTCP_EIO;
                break;
            }
        }
    }

    net->close(fd);
    memset(request, 0, sizeof(request));
    if (result == RTCP_OK && http_status != NULL) *http_status = status;
    return result;
}
