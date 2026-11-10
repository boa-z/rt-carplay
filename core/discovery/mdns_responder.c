/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mdns_responder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtcp/log.h"
#include "rtcp/rtcp.h"

#define TAG "mdns"

#define DNS_TYPE_A 1U
#define DNS_TYPE_PTR 12U
#define DNS_TYPE_TXT 16U
#define DNS_TYPE_SRV 33U
#define DNS_TYPE_ANY 255U

#define DNS_CLASS_IN 1U
#define DNS_CLASS_ANY 255U
#define DNS_CLASS_UNICAST 0x8000U
#define DNS_CLASS_CACHE_FLUSH 0x8000U

#define DNS_FLAG_RESPONSE 0x8000U
#define DNS_FLAG_AUTHORITATIVE 0x0400U

#define MDNS_TTL_SERVICE 4500U
#define MDNS_TTL_HOST 120U

#define MDNS_SERVICE "_airplay._tcp.local"

struct mdns_responder {
    char host[64];
    char instance[64];
    char service[MDNS_MAX_NAME];
    char instance_service[MDNS_MAX_NAME];
    char host_local[MDNS_MAX_NAME];
    uint8_t ipv4[4];
    uint16_t port;
    uint8_t txt[1024];
    size_t txt_len;
    int fd;

    uint8_t rx[MDNS_MAX_PACKET];
    uint8_t tx[MDNS_MAX_PACKET];
};

typedef struct {
    uint8_t *out;
    size_t capacity;
    size_t used;
    bool ok;
} dns_writer_t;

static void w_u8(dns_writer_t *w, uint8_t value)
{
    if (!w->ok || w->used >= w->capacity) {
        w->ok = false;
        return;
    }
    w->out[w->used++] = value;
}

static void w_be16(dns_writer_t *w, uint16_t value)
{
    w_u8(w, (uint8_t)(value >> 8U));
    w_u8(w, (uint8_t)value);
}

static void w_be32(dns_writer_t *w, uint32_t value)
{
    w_u8(w, (uint8_t)(value >> 24U));
    w_u8(w, (uint8_t)(value >> 16U));
    w_u8(w, (uint8_t)(value >> 8U));
    w_u8(w, (uint8_t)value);
}

static void w_bytes(dns_writer_t *w, const uint8_t *data, size_t len)
{
    if (!w->ok || len > w->capacity - w->used) {
        w->ok = false;
        return;
    }
    memcpy(w->out + w->used, data, len);
    w->used += len;
}

/* Encodes a dotted name as length-prefixed labels.  No compression. */
static void w_name(dns_writer_t *w, const char *name)
{
    size_t total = 0U;
    const char *label = name;
    while (label != NULL && label[0] != '\0') {
        const char *dot = strchr(label, '.');
        size_t len = dot != NULL ? (size_t)(dot - label) : strlen(label);
        total += 1U + len;
        if (len == 0U || len > 63U || total > 254U) {
            w->ok = false;
            return;
        }
        w_u8(w, (uint8_t)len);
        w_bytes(w, (const uint8_t *)label, len);
        label = dot != NULL ? dot + 1 : NULL;
    }
    w_u8(w, 0U);
}

static void w_rr(dns_writer_t *w, const char *name, uint16_t type,
                 uint16_t class_flags, uint32_t ttl,
                 const uint8_t *rdata, size_t rdlen)
{
    w_name(w, name);
    w_be16(w, type);
    w_be16(w, class_flags);
    w_be32(w, ttl);
    if (rdlen > 0xFFFFU) {
        w->ok = false;
        return;
    }
    w_be16(w, (uint16_t)rdlen);
    w_bytes(w, rdata, rdlen);
}

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8U) | p[1]);
}

static char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/*
 * Decodes a possibly compressed name starting at *offset.  Output is
 * lowercased dotted text without a trailing dot.
 */
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

static bool dns_label_valid(const char *name, size_t max_length)
{
    size_t total = 0U;
    const char *label = name;
    while (label != NULL && label[0] != '\0') {
        const char *dot = strchr(label, '.');
        size_t len = dot != NULL ? (size_t)(dot - label) : strlen(label);
        if (len == 0U || len > 63U) return false;
        total += 1U + len;
        label = dot != NULL ? dot + 1 : NULL;
    }
    return total != 0U && total <= max_length;
}

static void copy_text_lower(char *destination, size_t capacity, const char *source)
{
    size_t i = 0U;
    for (; source != NULL && source[i] != '\0' && i + 1U < capacity; ++i) {
        destination[i] = ascii_lower(source[i]);
    }
    destination[i] = '\0';
}

mdns_responder_t *mdns_responder_create(const mdns_responder_config_t *config)
{
    if (config == NULL || config->host_name == NULL ||
        config->host_name[0] == '\0' || config->port == 0U) {
        return NULL;
    }
    const char *instance = config->instance_name != NULL &&
                                   config->instance_name[0] != '\0'
                               ? config->instance_name
                               : config->host_name;
    if (strchr(config->host_name, '.') != NULL ||
        strchr(instance, '.') != NULL) {
        /* A dot would split the name into extra labels; DNS-SD escaping of
         * literal dots is not implemented.  Spaces are fine on the wire. */
        return NULL;
    }
    if (!dns_label_valid(config->host_name, sizeof(((mdns_responder_t *)0)->host) - 1U) ||
        !dns_label_valid(instance, sizeof(((mdns_responder_t *)0)->instance) - 1U) ||
        strlen(instance) + strlen(MDNS_SERVICE) + 2U >
            sizeof(((mdns_responder_t *)0)->instance_service)) {
        return NULL;
    }

    mdns_responder_t *responder = calloc(1U, sizeof(*responder));
    if (responder == NULL) return NULL;

    responder->fd = -1;
    responder->port = config->port;
    memcpy(responder->ipv4, config->ipv4, 4U);
    copy_text_lower(responder->host, sizeof(responder->host), config->host_name);
    copy_text_lower(responder->instance, sizeof(responder->instance), instance);

    snprintf(responder->service, sizeof(responder->service), "%s",
             MDNS_SERVICE);
    size_t instance_len = strlen(responder->instance);
    size_t service_len = strlen(responder->service);
    memcpy(responder->instance_service, responder->instance, instance_len);
    responder->instance_service[instance_len] = '.';
    memcpy(responder->instance_service + instance_len + 1U, responder->service,
           service_len + 1U);
    snprintf(responder->host_local, sizeof(responder->host_local), "%s.local",
             responder->host);

    if (config->txt != NULL && config->txt_len != 0U) {
        if (config->txt_len > sizeof(responder->txt)) {
            free(responder);
            return NULL;
        }
        memcpy(responder->txt, config->txt, config->txt_len);
        responder->txt_len = config->txt_len;
    }
    return responder;
}

/* Call mdns_responder_close() first: the socket belongs to the caller's seam. */
void mdns_responder_destroy(mdns_responder_t *responder)
{
    free(responder);
}

typedef struct {
    dns_writer_t writer;
    unsigned answers;
    unsigned additionals;
    bool ptr_done;
    bool srv_done;
    bool txt_done;
    bool a_done;
} mdns_builder_t;

static bool emit_ptr(mdns_builder_t *builder, const mdns_responder_t *responder,
                     bool answer)
{
    if (builder->ptr_done) return true;
    uint8_t rdata[MDNS_MAX_NAME];
    dns_writer_t scratch = {rdata, sizeof(rdata), 0U, true};
    w_name(&scratch, responder->instance_service);
    if (!scratch.ok) return false;
    w_rr(&builder->writer, responder->service, DNS_TYPE_PTR, DNS_CLASS_IN,
         MDNS_TTL_SERVICE, rdata, scratch.used);
    builder->ptr_done = true;
    if (answer) builder->answers++;
    else builder->additionals++;
    return builder->writer.ok;
}

static bool emit_srv(mdns_builder_t *builder, const mdns_responder_t *responder,
                     bool answer)
{
    if (builder->srv_done) return true;
    uint8_t rdata[MDNS_MAX_NAME];
    dns_writer_t scratch = {rdata, sizeof(rdata), 0U, true};
    w_be16(&scratch, 0U);
    w_be16(&scratch, 0U);
    w_be16(&scratch, responder->port);
    w_name(&scratch, responder->host_local);
    if (!scratch.ok) return false;
    w_rr(&builder->writer, responder->instance_service, DNS_TYPE_SRV,
         (uint16_t)(DNS_CLASS_IN | DNS_CLASS_CACHE_FLUSH), MDNS_TTL_SERVICE,
         rdata, scratch.used);
    builder->srv_done = true;
    if (answer) builder->answers++;
    else builder->additionals++;
    return builder->writer.ok;
}

static bool emit_txt(mdns_builder_t *builder, const mdns_responder_t *responder,
                     bool answer)
{
    if (builder->txt_done) return true;
    w_rr(&builder->writer, responder->instance_service, DNS_TYPE_TXT,
         (uint16_t)(DNS_CLASS_IN | DNS_CLASS_CACHE_FLUSH), MDNS_TTL_SERVICE,
         responder->txt, responder->txt_len);
    builder->txt_done = true;
    if (answer) builder->answers++;
    else builder->additionals++;
    return builder->writer.ok;
}

static bool emit_a(mdns_builder_t *builder, const mdns_responder_t *responder,
                   bool answer)
{
    if (builder->a_done) return true;
    w_rr(&builder->writer, responder->host_local, DNS_TYPE_A,
         (uint16_t)(DNS_CLASS_IN | DNS_CLASS_CACHE_FLUSH), MDNS_TTL_HOST,
         responder->ipv4, 4U);
    builder->a_done = true;
    if (answer) builder->answers++;
    else builder->additionals++;
    return builder->writer.ok;
}

static void begin_builder(mdns_builder_t *builder, uint8_t *output,
                          size_t capacity, uint16_t id)
{
    memset(builder, 0, sizeof(*builder));
    builder->writer.out = output;
    builder->writer.capacity = capacity;
    builder->writer.used = 0U;
    builder->writer.ok = true;

    w_be16(&builder->writer, id);
    w_be16(&builder->writer, (uint16_t)(DNS_FLAG_RESPONSE | DNS_FLAG_AUTHORITATIVE));
    w_be16(&builder->writer, 0U); /* questions */
    w_be16(&builder->writer, 0U); /* answers, patched below */
    w_be16(&builder->writer, 0U); /* authority */
    w_be16(&builder->writer, 0U); /* additional, patched below */
}

static size_t finish_builder(mdns_builder_t *builder)
{
    if (!builder->writer.ok || builder->writer.used < 12U) return 0U;
    uint8_t *out = builder->writer.out;
    out[6] = (uint8_t)(builder->answers >> 8U);
    out[7] = (uint8_t)builder->answers;
    out[10] = (uint8_t)(builder->additionals >> 8U);
    out[11] = (uint8_t)builder->additionals;
    if (builder->answers == 0U) return 0U;
    return builder->writer.used;
}

size_t mdns_responder_build_announcement(const mdns_responder_t *responder,
                                         uint8_t *output,
                                         size_t output_capacity)
{
    if (responder == NULL || output == NULL) return 0U;
    mdns_builder_t builder;
    begin_builder(&builder, output, output_capacity, 0U);
    if (!emit_ptr(&builder, responder, true) ||
        !emit_srv(&builder, responder, true) ||
        !emit_txt(&builder, responder, true) ||
        !emit_a(&builder, responder, true)) {
        return 0U;
    }
    return finish_builder(&builder);
}

size_t mdns_responder_build_response(const mdns_responder_t *responder,
                                     const uint8_t *query,
                                     size_t query_len,
                                     uint8_t *output,
                                     size_t output_capacity,
                                     bool *unicast)
{
    if (unicast != NULL) *unicast = false;
    if (responder == NULL || query == NULL || output == NULL ||
        query_len < 12U) {
        return 0U;
    }
    if ((read_be16(query + 2U) & DNS_FLAG_RESPONSE) != 0U) return 0U;

    uint16_t question_count = read_be16(query + 4U);
    if (question_count == 0U || question_count > 32U) return 0U;

    mdns_builder_t builder;
    begin_builder(&builder, output, output_capacity, read_be16(query));
    size_t offset = 12U;
    unsigned matches = 0U;
    unsigned unicast_matches = 0U;

    for (uint16_t q = 0U; q < question_count; ++q) {
        char name[MDNS_MAX_NAME];
        if (!dns_read_name(query, query_len, &offset, name, sizeof(name))) {
            return 0U;
        }
        if (offset + 4U > query_len) return 0U;
        uint16_t type = read_be16(query + offset);
        uint16_t qclass = read_be16(query + offset + 2U);
        offset += 4U;
        if ((qclass & (uint16_t)~DNS_CLASS_UNICAST) != DNS_CLASS_IN &&
            (qclass & (uint16_t)~DNS_CLASS_UNICAST) != DNS_CLASS_ANY) {
            continue;
        }
        bool wants_unicast = (qclass & DNS_CLASS_UNICAST) != 0U;
        bool matched = false;

        if (strcmp(name, responder->service) == 0 &&
            (type == DNS_TYPE_PTR || type == DNS_TYPE_ANY)) {
            if (!emit_ptr(&builder, responder, true) ||
                !emit_srv(&builder, responder, false) ||
                !emit_txt(&builder, responder, false) ||
                !emit_a(&builder, responder, false)) {
                return 0U;
            }
            matched = true;
        } else if (strcmp(name, responder->instance_service) == 0) {
            if (type == DNS_TYPE_SRV || type == DNS_TYPE_ANY) {
                if (!emit_srv(&builder, responder, true)) return 0U;
                if (!emit_a(&builder, responder, false)) return 0U;
                matched = true;
            }
            if (type == DNS_TYPE_TXT || type == DNS_TYPE_ANY) {
                if (!emit_txt(&builder, responder, true)) return 0U;
                matched = true;
            }
        } else if (strcmp(name, responder->host_local) == 0 &&
                   (type == DNS_TYPE_A || type == DNS_TYPE_ANY)) {
            if (!emit_a(&builder, responder, true)) return 0U;
            matched = true;
        }

        if (matched) {
            matches++;
            if (wants_unicast) unicast_matches++;
        }
    }

    if (matches == 0U) return 0U;
    if (unicast != NULL) *unicast = unicast_matches == matches;
    return finish_builder(&builder);
}

int mdns_responder_open(mdns_responder_t *responder, const struct rtcp_net *net)
{
    if (responder == NULL || net == NULL || net->udp_open == NULL ||
        net->udp_join == NULL) {
        return RTCP_EINVAL;
    }
    if (responder->fd >= 0) return RTCP_OK;

    int fd = net->udp_open(MDNS_PORT, true);
    if (fd < 0) {
        RTCP_LOGE(TAG, "udp_open failed rc=%d", fd);
        return fd;
    }
    static const uint8_t group[4] = {224U, 0U, 0U, 251U};
    int rc = net->udp_join(fd, group);
    if (rc != RTCP_OK) {
        RTCP_LOGE(TAG, "udp_join failed rc=%d", rc);
        net->close(fd);
        return rc;
    }
    responder->fd = fd;
    RTCP_LOGI(TAG, "listening on :%u host=%s.local", (unsigned)MDNS_PORT,
              responder->host);
    return RTCP_OK;
}

void mdns_responder_close(mdns_responder_t *responder, const struct rtcp_net *net)
{
    if (responder == NULL || responder->fd < 0) return;
    if (net != NULL && net->close != NULL) net->close(responder->fd);
    responder->fd = -1;
}

int mdns_responder_send_to(mdns_responder_t *responder,
                           const struct rtcp_net *net,
                           const rtcp_addr_t *destination, const void *packet,
                           size_t packet_len)
{
    if (responder == NULL || net == NULL || net->udp_sendto == NULL ||
        destination == NULL || packet == NULL || packet_len == 0U ||
        responder->fd < 0) {
        return RTCP_EINVAL;
    }
    return net->udp_sendto(responder->fd, destination, packet, packet_len);
}

int mdns_responder_send(mdns_responder_t *responder, const struct rtcp_net *net,
                        const void *packet, size_t packet_len)
{
    if (responder == NULL || net == NULL || net->udp_sendto == NULL ||
        packet == NULL || packet_len == 0U || responder->fd < 0) {
        return RTCP_EINVAL;
    }
    rtcp_addr_t destination;
    memset(&destination, 0, sizeof(destination));
    destination.family = RTCP_AF_INET;
    static const uint8_t group[4] = {224U, 0U, 0U, 251U};
    memcpy(destination.addr, group, 4U);
    destination.port = MDNS_PORT;
    return net->udp_sendto(responder->fd, &destination, packet, packet_len);
}

int mdns_responder_poll_ex(mdns_responder_t *responder, const struct rtcp_net *net,
                           int timeout_ms, mdns_responder_packet_sink_t sink,
                           void *sink_context)
{
    if (responder == NULL || net == NULL || net->poll == NULL ||
        net->udp_recvfrom == NULL || net->udp_sendto == NULL ||
        responder->fd < 0) {
        return RTCP_EINVAL;
    }

    struct rtcp_pollfd descriptor = {
        .fd = responder->fd,
        .events = RTCP_POLLIN,
        .revents = 0,
    };
    int ready = net->poll(&descriptor, 1, timeout_ms);
    if (ready < 0) return ready;
    if (ready == 0) return 0;

    rtcp_addr_t from;
    memset(&from, 0, sizeof(from));
    int received = net->udp_recvfrom(responder->fd, responder->rx,
                                     sizeof(responder->rx), &from);
    if (received < 0) return received;
    if (received == 0) return 0;

    if (sink != NULL) {
        sink(responder->rx, (size_t)received, &from, sink_context);
    }

    bool unicast = false;
    size_t response_len = mdns_responder_build_response(
        responder, responder->rx, (size_t)received, responder->tx,
        sizeof(responder->tx), &unicast);
    if (response_len == 0U) return 0;

    rtcp_addr_t destination;
    memset(&destination, 0, sizeof(destination));
    destination.family = RTCP_AF_INET;
    destination.port = MDNS_PORT;
    if (unicast) {
        destination = from;
    } else {
        static const uint8_t group[4] = {224U, 0U, 0U, 251U};
        memcpy(destination.addr, group, 4U);
    }

    int sent = net->udp_sendto(responder->fd, &destination, responder->tx,
                               response_len);
    if (sent < 0) return sent;
    RTCP_LOGD(TAG, "answered %u bytes (%s)", (unsigned)response_len,
              unicast ? "unicast" : "multicast");
    return 1;
}

int mdns_responder_poll(mdns_responder_t *responder, const struct rtcp_net *net,
                        int timeout_ms)
{
    return mdns_responder_poll_ex(responder, net, timeout_ms, NULL, NULL);
}
