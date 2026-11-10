/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mdns_browser.h"

#include <stdlib.h>
#include <string.h>

#include "rtcp/rtcp.h"

#define DNS_TYPE_A 1U
#define DNS_TYPE_PTR 12U
#define DNS_TYPE_SRV 33U

#define DNS_CLASS_IN 1U
#define DNS_CLASS_UNICAST 0x8000U
#define DNS_FLAG_RESPONSE 0x8000U

#define DNS_HEADER_BYTES 12U
/* A defensive cap: one packet with more records than this is not something
 * any real mDNS responder emits. */
#define DNS_MAX_RECORDS 64U

typedef struct {
    char name[MDNS_MAX_NAME];
    uint8_t ipv4[4];
    bool valid;
    uint32_t last_seen_ms;
} mdns_browser_host_t;

struct mdns_browser {
    char service[MDNS_MAX_NAME];
    mdns_browser_endpoint_t endpoints[MDNS_BROWSER_MAX_ENDPOINTS];
    mdns_browser_host_t hosts[MDNS_BROWSER_MAX_HOSTS];
    bool queried;
    uint32_t last_query_ms;
};

/* ------------------------------------------------------------------ */
/* DNS wire helpers                                                    */
/* ------------------------------------------------------------------ */

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8U) | p[1]);
}

static char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Same decoding as mdns_responder.c: length-prefixed labels, compression
 * pointers, output lowercased dotted text without a trailing dot.  DNS names
 * are case-insensitive, and the phone may mix case in the instance label. */
static bool dns_read_name(const uint8_t *packet, size_t packet_len,
                          size_t *offset, char *output, size_t output_capacity)
{
    if (packet == NULL || offset == NULL || output == NULL ||
        output_capacity == 0U) {
        return false;
    }
    size_t pos = *offset;
    size_t out_used = 0U;
    bool jumped = false;
    unsigned jumps = 0U;

    for (;;) {
        if (pos >= packet_len) return false;
        uint8_t length = packet[pos++];
        if (length == 0U) {
            if (!jumped) *offset = pos;
            output[out_used] = '\0';
            return true;
        }
        if ((length & 0xC0U) == 0xC0U) {
            if (pos >= packet_len || jumps++ > 16U) return false;
            size_t target = (size_t)(((length & 0x3FU) << 8U) | packet[pos++]);
            if (target >= packet_len) return false;
            if (!jumped) *offset = pos;
            jumped = true;
            pos = target;
            continue;
        }
        if ((length & 0xC0U) != 0U || length > 63U) return false;
        if (pos + length > packet_len) return false;
        if (out_used != 0U) {
            if (out_used + 1U >= output_capacity) return false;
            output[out_used++] = '.';
        }
        if (out_used + length >= output_capacity) return false;
        for (size_t i = 0U; i < length; ++i) {
            output[out_used++] = ascii_lower((char)packet[pos + i]);
        }
        pos += length;
    }
}

static bool name_ends_with(const char *name, const char *suffix)
{
    size_t name_len = strlen(name);
    size_t suffix_len = strlen(suffix);
    if (name_len <= suffix_len) return false;
    if (name[name_len - suffix_len - 1U] != '.') return false;
    return strcmp(name + name_len - suffix_len, suffix) == 0;
}

static bool time_reached(uint32_t now_ms, uint32_t then_ms, uint32_t interval)
{
    return (uint32_t)(now_ms - then_ms) >= interval;
}

/* ------------------------------------------------------------------ */
/* Browse state                                                        */
/* ------------------------------------------------------------------ */

static void prune_state(mdns_browser_t *browser, uint32_t now_ms)
{
    for (size_t i = 0U; i < MDNS_BROWSER_MAX_ENDPOINTS; ++i) {
        mdns_browser_endpoint_t *endpoint = &browser->endpoints[i];
        if (endpoint->instance[0] != '\0' &&
            time_reached(now_ms, endpoint->last_seen_ms,
                         MDNS_BROWSER_ENDPOINT_MAX_AGE_MS)) {
            memset(endpoint, 0, sizeof(*endpoint));
        }
    }
    for (size_t i = 0U; i < MDNS_BROWSER_MAX_HOSTS; ++i) {
        mdns_browser_host_t *host = &browser->hosts[i];
        if (host->valid &&
            time_reached(now_ms, host->last_seen_ms,
                         MDNS_BROWSER_ENDPOINT_MAX_AGE_MS)) {
            memset(host, 0, sizeof(*host));
        }
    }
}

static mdns_browser_endpoint_t *endpoint_get(mdns_browser_t *browser,
                                             const char *instance,
                                             uint32_t now_ms)
{
    mdns_browser_endpoint_t *free_slot = NULL;
    mdns_browser_endpoint_t *oldest = NULL;

    for (size_t i = 0U; i < MDNS_BROWSER_MAX_ENDPOINTS; ++i) {
        mdns_browser_endpoint_t *endpoint = &browser->endpoints[i];
        if (strcmp(endpoint->instance, instance) == 0) return endpoint;
        if (endpoint->instance[0] == '\0') {
            if (free_slot == NULL) free_slot = endpoint;
        } else if (oldest == NULL ||
                   time_reached(oldest->last_seen_ms, endpoint->last_seen_ms,
                                1U)) {
            /* oldest is newer than this candidate -> the candidate is older */
            oldest = endpoint;
        }
    }

    mdns_browser_endpoint_t *endpoint = free_slot != NULL ? free_slot : oldest;
    memset(endpoint, 0, sizeof(*endpoint));
    strncpy(endpoint->instance, instance, sizeof(endpoint->instance) - 1U);
    endpoint->last_seen_ms = now_ms;
    return endpoint;
}

static void host_put(mdns_browser_t *browser, const char *name,
                     const uint8_t ipv4[4], uint32_t now_ms)
{
    mdns_browser_host_t *free_slot = NULL;
    mdns_browser_host_t *oldest = NULL;

    for (size_t i = 0U; i < MDNS_BROWSER_MAX_HOSTS; ++i) {
        mdns_browser_host_t *host = &browser->hosts[i];
        if (host->valid && strcmp(host->name, name) == 0) {
            memcpy(host->ipv4, ipv4, 4U);
            host->last_seen_ms = now_ms;
            return;
        }
        if (!host->valid) {
            if (free_slot == NULL) free_slot = host;
        } else if (oldest == NULL ||
                   time_reached(oldest->last_seen_ms, host->last_seen_ms,
                                1U)) {
            oldest = host;
        }
    }
    mdns_browser_host_t *host = free_slot != NULL ? free_slot : oldest;
    memset(host, 0, sizeof(*host));
    strncpy(host->name, name, sizeof(host->name) - 1U);
    memcpy(host->ipv4, ipv4, 4U);
    host->valid = true;
    host->last_seen_ms = now_ms;
}

static const uint8_t *host_get(const mdns_browser_t *browser, const char *name)
{
    for (size_t i = 0U; i < MDNS_BROWSER_MAX_HOSTS; ++i) {
        const mdns_browser_host_t *host = &browser->hosts[i];
        if (host->valid && strcmp(host->name, name) == 0) return host->ipv4;
    }
    return NULL;
}

static void apply_address(mdns_browser_t *browser, const char *target,
                          const uint8_t ipv4[4], uint32_t now_ms)
{
    host_put(browser, target, ipv4, now_ms);
    for (size_t i = 0U; i < MDNS_BROWSER_MAX_ENDPOINTS; ++i) {
        mdns_browser_endpoint_t *endpoint = &browser->endpoints[i];
        if (endpoint->instance[0] == '\0') continue;
        if (strcmp(endpoint->target, target) != 0) continue;
        memcpy(endpoint->ipv4, ipv4, 4U);
        endpoint->has_ipv4 = true;
        endpoint->last_seen_ms = now_ms;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

static bool copy_service(char *destination, size_t capacity,
                         const char *service)
{
    size_t length = service != NULL ? strlen(service) : 0U;
    if (length == 0U || length >= capacity) return false;
    if (service[length - 1U] == '.') --length;
    if (length == 0U || length >= capacity) return false;
    for (size_t i = 0U; i < length; ++i) {
        destination[i] = ascii_lower(service[i]);
    }
    destination[length] = '\0';
    return true;
}

mdns_browser_t *mdns_browser_create(const char *service)
{
    if (service == NULL) return NULL;
    mdns_browser_t *browser = calloc(1U, sizeof(*browser));
    if (browser == NULL) return NULL;
    if (!copy_service(browser->service, sizeof(browser->service), service)) {
        free(browser);
        return NULL;
    }
    return browser;
}

void mdns_browser_destroy(mdns_browser_t *browser)
{
    free(browser);
}

static bool write_name(uint8_t *output, size_t capacity, size_t *used,
                       const char *name)
{
    size_t pos = *used;
    const char *label = name;
    while (label != NULL && label[0] != '\0') {
        const char *dot = strchr(label, '.');
        size_t length = dot != NULL ? (size_t)(dot - label) : strlen(label);
        if (length == 0U || length > 63U) return false;
        if (pos + 1U + length + 1U > capacity) return false;
        output[pos++] = (uint8_t)length;
        memcpy(output + pos, label, length);
        pos += length;
        label = dot != NULL ? dot + 1 : NULL;
    }
    if (pos + 1U > capacity) return false;
    output[pos++] = 0U;
    *used = pos;
    return true;
}

size_t mdns_browser_build_query(const mdns_browser_t *browser,
                                uint8_t *output, size_t output_capacity)
{
    if (browser == NULL || output == NULL) return 0U;
    if (output_capacity < DNS_HEADER_BYTES) return 0U;

    memset(output, 0, DNS_HEADER_BYTES);
    output[5] = 1U;                 /* QDCOUNT = 1 */
    size_t used = DNS_HEADER_BYTES;
    if (!write_name(output, output_capacity, &used, browser->service)) {
        return 0U;
    }
    if (used + 4U > output_capacity) return 0U;
    output[used++] = 0U;            /* QTYPE = PTR */
    output[used++] = DNS_TYPE_PTR;
    /* Class IN with the mDNS unicast-response bit: Apple's responder sends
     * the PTR answer straight back to the querier's port. */
    output[used++] = (uint8_t)((DNS_CLASS_IN | DNS_CLASS_UNICAST) >> 8U);
    output[used++] = (uint8_t)((DNS_CLASS_IN | DNS_CLASS_UNICAST) & 0xFFU);
    return used;
}

int mdns_browser_handle_packet(mdns_browser_t *browser, const uint8_t *packet,
                               size_t packet_len, const uint8_t *from_ipv4,
                               uint32_t now_ms)
{
    if (browser == NULL || packet == NULL || packet_len < DNS_HEADER_BYTES) {
        return RTCP_EINVAL;
    }
    if ((read_be16(packet + 2U) & DNS_FLAG_RESPONSE) == 0U) return 0;

    unsigned questions = read_be16(packet + 4U);
    unsigned answers = read_be16(packet + 6U);
    unsigned authority = read_be16(packet + 8U);
    unsigned additional = read_be16(packet + 10U);
    if (answers + authority + additional > DNS_MAX_RECORDS) return 0;

    prune_state(browser, now_ms);

    size_t offset = DNS_HEADER_BYTES;
    char name[MDNS_MAX_NAME];
    for (unsigned q = 0U; q < questions; ++q) {
        if (!dns_read_name(packet, packet_len, &offset, name, sizeof(name))) {
            return RTCP_EIO;
        }
        if (offset + 4U > packet_len) return RTCP_EIO;
        offset += 4U;
    }

    int updates = 0;
    unsigned records = answers + authority + additional;
    for (unsigned r = 0U; r < records; ++r) {
        char owner[MDNS_MAX_NAME];
        if (!dns_read_name(packet, packet_len, &offset, owner,
                           sizeof(owner))) {
            return RTCP_EIO;
        }
        if (offset + 10U > packet_len) return RTCP_EIO;
        uint16_t type = read_be16(packet + offset);
        uint16_t rclass = read_be16(packet + offset + 2U);
        /* uint32_t ttl = read_be32(packet + offset + 4U); TTL 0 (goodbye)
         * is deliberately ignored: the endpoint age check retires entries. */
        uint16_t rdlen = read_be16(packet + offset + 8U);
        offset += 10U;
        if (offset + rdlen > packet_len) return RTCP_EIO;
        size_t rdata = offset;
        offset += rdlen;

        if ((rclass & (uint16_t)~DNS_CLASS_UNICAST) != DNS_CLASS_IN) continue;

        if (type == DNS_TYPE_PTR) {
            if (strcmp(owner, browser->service) != 0) continue;
            char instance[MDNS_MAX_NAME];
            size_t name_offset = rdata;
            if (!dns_read_name(packet, packet_len, &name_offset, instance,
                               sizeof(instance))) {
                continue;
            }
            if (!name_ends_with(instance, browser->service)) continue;
            mdns_browser_endpoint_t *endpoint =
                endpoint_get(browser, instance, now_ms);
            endpoint->last_seen_ms = now_ms;
            ++updates;
        } else if (type == DNS_TYPE_SRV) {
            if (!name_ends_with(owner, browser->service) ||
                rdlen < 6U) {
                continue;
            }
            uint16_t port = read_be16(packet + rdata + 4U);
            char target[MDNS_MAX_NAME];
            size_t name_offset = rdata + 6U;
            if (!dns_read_name(packet, packet_len, &name_offset, target,
                               sizeof(target))) {
                continue;
            }
            mdns_browser_endpoint_t *endpoint =
                endpoint_get(browser, owner, now_ms);
            strncpy(endpoint->target, target, sizeof(endpoint->target) - 1U);
            endpoint->port = port;
            endpoint->has_port = port != 0U;
            endpoint->last_seen_ms = now_ms;
            const uint8_t *address = host_get(browser, target);
            if (address != NULL) {
                memcpy(endpoint->ipv4, address, 4U);
                endpoint->has_ipv4 = true;
            } else if (from_ipv4 != NULL) {
                memcpy(endpoint->ipv4, from_ipv4, 4U);
                endpoint->has_ipv4 = true;
            }
            ++updates;
        } else if (type == DNS_TYPE_A) {
            if (rdlen != 4U) continue;
            apply_address(browser, owner, packet + rdata, now_ms);
            ++updates;
        }
    }
    return updates;
}

bool mdns_browser_query_due(const mdns_browser_t *browser, uint32_t now_ms)
{
    if (browser == NULL) return false;
    if (!browser->queried) return true;
    return time_reached(now_ms, browser->last_query_ms,
                        MDNS_BROWSER_QUERY_INTERVAL_MS);
}

void mdns_browser_mark_queried(mdns_browser_t *browser, uint32_t now_ms)
{
    if (browser == NULL) return;
    browser->queried = true;
    browser->last_query_ms = now_ms;
}

const mdns_browser_endpoint_t *mdns_browser_best(const mdns_browser_t *browser,
                                                 uint32_t now_ms)
{
    if (browser == NULL) return NULL;
    const mdns_browser_endpoint_t *best = NULL;
    for (size_t i = 0U; i < MDNS_BROWSER_MAX_ENDPOINTS; ++i) {
        const mdns_browser_endpoint_t *endpoint = &browser->endpoints[i];
        if (endpoint->instance[0] == '\0' || !endpoint->has_port ||
            !endpoint->has_ipv4) {
            continue;
        }
        if (time_reached(now_ms, endpoint->last_seen_ms,
                         MDNS_BROWSER_ENDPOINT_MAX_AGE_MS)) {
            continue;
        }
        if (best == NULL ||
            time_reached(endpoint->last_seen_ms, best->last_seen_ms, 1U)) {
            best = endpoint;
        }
    }
    return best;
}

size_t mdns_browser_endpoint_count(const mdns_browser_t *browser)
{
    if (browser == NULL) return 0U;
    size_t count = 0U;
    for (size_t i = 0U; i < MDNS_BROWSER_MAX_ENDPOINTS; ++i) {
        if (browser->endpoints[i].instance[0] != '\0') ++count;
    }
    return count;
}
