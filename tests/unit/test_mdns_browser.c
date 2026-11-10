/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mdns_browser.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rtcp/rtcp.h"

#define SERVICE "_carplay-ctrl._tcp.local"

typedef struct {
    uint8_t data[768];
    size_t len;
    bool ok;
} packet_t;

static void w8(packet_t *p, uint8_t value)
{
    if (!p->ok || p->len >= sizeof(p->data)) {
        p->ok = false;
        return;
    }
    p->data[p->len++] = value;
}

static void w16(packet_t *p, uint16_t value)
{
    w8(p, (uint8_t)(value >> 8U));
    w8(p, (uint8_t)value);
}

static void w32(packet_t *p, uint32_t value)
{
    w16(p, (uint16_t)(value >> 16U));
    w16(p, (uint16_t)value);
}

static void wbytes(packet_t *p, const void *bytes, size_t length)
{
    if (!p->ok || length > sizeof(p->data) - p->len) {
        p->ok = false;
        return;
    }
    memcpy(p->data + p->len, bytes, length);
    p->len += length;
}

static size_t wname(packet_t *p, const char *name)
{
    size_t start = p->len;
    const char *label = name;
    while (label != NULL && label[0] != '\0') {
        const char *dot = strchr(label, '.');
        size_t length = dot != NULL ? (size_t)(dot - label) : strlen(label);
        assert(length > 0U && length <= 63U);
        w8(p, (uint8_t)length);
        wbytes(p, label, length);
        label = dot != NULL ? dot + 1 : NULL;
    }
    w8(p, 0U);
    return start;
}

static void begin_header(packet_t *p)
{
    memset(p, 0, sizeof(*p));
    p->ok = true;
    for (size_t i = 0U; i < 12U; ++i) w8(p, 0U);
}

static void put_counts(packet_t *p, unsigned questions, unsigned answers,
                       unsigned authority, unsigned additional)
{
    p->data[2] = 0x84U; /* QR + authoritative */
    p->data[3] = 0x00U;
    p->data[4] = (uint8_t)(questions >> 8U);
    p->data[5] = (uint8_t)questions;
    p->data[6] = (uint8_t)(answers >> 8U);
    p->data[7] = (uint8_t)answers;
    p->data[8] = (uint8_t)(authority >> 8U);
    p->data[9] = (uint8_t)authority;
    p->data[10] = (uint8_t)(additional >> 8U);
    p->data[11] = (uint8_t)additional;
}

static void w_rr_header(packet_t *p, uint16_t type, uint16_t rclass,
                        uint32_t ttl, uint16_t rdlen)
{
    w16(p, type);
    w16(p, rclass);
    w32(p, ttl);
    w16(p, rdlen);
}

static void test_query_builder(void)
{
    mdns_browser_t *browser = mdns_browser_create(SERVICE);
    assert(browser != NULL);
    uint8_t query[128];
    size_t length = mdns_browser_build_query(browser, query, sizeof(query));
    assert(length > 12U);

    assert(query[0] == 0U && query[1] == 0U);
    assert(query[4] == 0U && query[5] == 1U); /* QDCOUNT = 1 */
    assert(query[6] == 0U && query[7] == 0U);
    /* Question name decodes to the service. */
    size_t offset = 12U;
    char name[MDNS_MAX_NAME];
    size_t used = 0U;
    while (query[offset] != 0U) {
        uint8_t label = query[offset++];
        if (used != 0U) name[used++] = '.';
        memcpy(name + used, query + offset, label);
        used += label;
        offset += label;
    }
    name[used] = '\0';
    assert(strcmp(name, SERVICE) == 0);
    assert(offset + 1U + 4U == length);
    offset += 1U;
    assert(query[offset] == 0U && query[offset + 1U] == 12U); /* PTR */
    assert(query[offset + 2U] == 0x80U && query[offset + 3U] == 0x01U);
    mdns_browser_destroy(browser);

    /* A trailing dot is accepted and produces the same query. */
    mdns_browser_t *dotted = mdns_browser_create(SERVICE ".");
    assert(dotted != NULL);
    uint8_t query2[128];
    size_t length2 = mdns_browser_build_query(dotted, query2, sizeof(query2));
    assert(length2 == length && memcmp(query, query2, length) == 0);
    mdns_browser_destroy(dotted);
}

/* PTR answer with a compressed instance name, SRV and A additionals. */
static packet_t build_response(bool include_a, bool address_first)
{
    packet_t packet;
    begin_header(&packet);
    size_t service_offset = wname(&packet, SERVICE);
    assert(service_offset == 12U);
    w16(&packet, 12U); /* PTR */
    w16(&packet, 1U);  /* IN */

    /* PTR -> "AbCd" + compression pointer to the service name. */
    wname(&packet, SERVICE);
    w16(&packet, 12U);
    w16(&packet, 1U);
    w32(&packet, 4500U);
    size_t rdlen_at = packet.len;
    w16(&packet, 0U);
    size_t rdata_start = packet.len;
    w8(&packet, 4U);
    wbytes(&packet, "AbCd", 4U);
    w8(&packet, (uint8_t)(0xC0U | (service_offset >> 8U)));
    w8(&packet, (uint8_t)service_offset);
    packet.data[rdlen_at] = (uint8_t)((packet.len - rdata_start) >> 8U);
    packet.data[rdlen_at + 1U] = (uint8_t)(packet.len - rdata_start);

    const char *instance = "abcd._carplay-ctrl._tcp.local";
    const char *target = "iPhone-12.local";
    const uint8_t address[4] = {192U, 168U, 43U, 2U};

    unsigned additionals = 1U; /* SRV */
    if (include_a || address_first) ++additionals;

    if (address_first) {
        wname(&packet, target);
        w_rr_header(&packet, 1U, 0x8001U, 120U, 4U);
        wbytes(&packet, address, 4U);
    }

    wname(&packet, instance);
    w16(&packet, 33U);    /* SRV */
    w16(&packet, 0x8001U);
    w32(&packet, 4500U);
    size_t srv_rdlen_at = packet.len;
    w16(&packet, 0U);
    size_t srv_start = packet.len;
    w16(&packet, 0U);
    w16(&packet, 0U);
    w16(&packet, 51234U);
    wname(&packet, target);
    packet.data[srv_rdlen_at] = (uint8_t)((packet.len - srv_start) >> 8U);
    packet.data[srv_rdlen_at + 1U] = (uint8_t)(packet.len - srv_start);

    if (include_a && !address_first) {
        wname(&packet, target);
        w_rr_header(&packet, 1U, 0x8001U, 120U, 4U);
        wbytes(&packet, address, 4U);
    }

    put_counts(&packet, 1U, 1U, 0U, additionals);
    assert(packet.ok);
    return packet;
}

static void test_response_parse(void)
{
    mdns_browser_t *browser = mdns_browser_create(SERVICE);
    assert(browser != NULL);
    packet_t packet = build_response(true, false);
    int updates = mdns_browser_handle_packet(browser, packet.data,
                                             packet.len, NULL, 1000U);
    assert(updates > 0);
    assert(mdns_browser_endpoint_count(browser) == 1U);

    const mdns_browser_endpoint_t *endpoint =
        mdns_browser_best(browser, 1000U);
    assert(endpoint != NULL);
    assert(strcmp(endpoint->instance,
                  "abcd._carplay-ctrl._tcp.local") == 0);
    assert(strcmp(endpoint->target, "iphone-12.local") == 0);
    assert(endpoint->port == 51234U);
    assert(endpoint->has_port && endpoint->has_ipv4);
    assert(endpoint->ipv4[0] == 192U && endpoint->ipv4[3] == 2U);

    /* After the record lifetime the endpoint is no longer offered. */
    assert(mdns_browser_best(browser, 1000U + 60001U) == NULL);
    mdns_browser_destroy(browser);
}

static void test_address_before_srv(void)
{
    mdns_browser_t *browser = mdns_browser_create(SERVICE);
    assert(browser != NULL);
    packet_t packet = build_response(false, true);
    /* The A record arrives before the SRV that names its host. */
    assert(mdns_browser_handle_packet(browser, packet.data, packet.len, NULL,
                                      2000U) > 0);
    const mdns_browser_endpoint_t *endpoint =
        mdns_browser_best(browser, 2000U);
    assert(endpoint != NULL);
    assert(endpoint->has_ipv4);
    assert(endpoint->ipv4[0] == 192U && endpoint->ipv4[1] == 168U &&
           endpoint->ipv4[2] == 43U && endpoint->ipv4[3] == 2U);
    mdns_browser_destroy(browser);
}

static void test_source_address_fallback(void)
{
    const uint8_t from[4] = {192U, 168U, 43U, 9U};
    mdns_browser_t *browser = mdns_browser_create(SERVICE);
    assert(browser != NULL);
    packet_t packet = build_response(false, false);
    assert(mdns_browser_handle_packet(browser, packet.data, packet.len, from,
                                      3000U) > 0);
    const mdns_browser_endpoint_t *endpoint =
        mdns_browser_best(browser, 3000U);
    assert(endpoint != NULL);
    assert(endpoint->has_ipv4);
    assert(endpoint->ipv4[0] == 192U && endpoint->ipv4[1] == 168U &&
           endpoint->ipv4[2] == 43U && endpoint->ipv4[3] == 9U);
    mdns_browser_destroy(browser);
}

static void test_queries_ignored(void)
{
    mdns_browser_t *browser = mdns_browser_create(SERVICE);
    assert(browser != NULL);
    packet_t packet;
    begin_header(&packet);
    wname(&packet, SERVICE);
    w16(&packet, 12U);
    w16(&packet, 1U);
    put_counts(&packet, 1U, 0U, 0U, 0U);
    packet.data[2] = 0x00U; /* QR = 0: a query, not a response */
    assert(mdns_browser_handle_packet(browser, packet.data, packet.len, NULL,
                                      4000U) == 0);
    assert(mdns_browser_endpoint_count(browser) == 0U);

    /* Malformed input is rejected, not crashed on. */
    assert(mdns_browser_handle_packet(browser, packet.data, 4U, NULL, 4000U) ==
           RTCP_EINVAL);
    packet.data[2] = 0x84U; /* turn it into a (truncated) response */
    assert(mdns_browser_handle_packet(browser, packet.data,
                                      packet.len - 3U, NULL, 4000U) ==
           RTCP_EIO);
    mdns_browser_destroy(browser);
}

static void test_query_schedule(void)
{
    mdns_browser_t *browser = mdns_browser_create(SERVICE);
    assert(browser != NULL);
    assert(mdns_browser_query_due(browser, 100U));
    mdns_browser_mark_queried(browser, 100U);
    assert(!mdns_browser_query_due(browser, 100U + 1000U));
    assert(mdns_browser_query_due(browser,
                                  100U + MDNS_BROWSER_QUERY_INTERVAL_MS));
    mdns_browser_destroy(browser);
}

int main(void)
{
    test_query_builder();
    test_response_parse();
    test_address_before_srv();
    test_source_address_fallback();
    test_queries_ignored();
    test_query_schedule();
    puts("mdns browser host test passed");
    return 0;
}
