/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mdns_responder.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "airplay_txt.h"

static const uint8_t k_ipv4[4] = {192U, 168U, 1U, 10U};

typedef struct {
    const uint8_t *buffer;
    size_t length;
    size_t offset;
    unsigned questions;
    unsigned answers;
    unsigned authority;
    unsigned additional;
} dns_view_t;

static uint16_t read16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8U) | p[1]);
}

static bool read_name(const uint8_t *packet, size_t length, size_t *offset,
                      char *out, size_t capacity)
{
    size_t pos = *offset;
    size_t used = 0U;
    bool jumped = false;
    unsigned jumps = 0U;
    for (;;) {
        assert(pos < length);
        uint8_t label = packet[pos++];
        if (label == 0U) {
            if (!jumped) *offset = pos;
            out[used] = '\0';
            return true;
        }
        if ((label & 0xC0U) == 0xC0U) {
            assert(pos < length);
            size_t target = (size_t)(((label & 0x3FU) << 8U) | packet[pos++]);
            if (!jumped) *offset = pos;
            jumped = true;
            pos = target;
            assert(jumps++ < 16U);
            continue;
        }
        if (used != 0U) out[used++] = '.';
        assert(used + label < capacity);
        memcpy(out + used, packet + pos, label);
        used += label;
        pos += label;
    }
}

static void open_packet(dns_view_t *view, const uint8_t *buffer, size_t length)
{
    memset(view, 0, sizeof(*view));
    assert(length >= 12U);
    view->buffer = buffer;
    view->length = length;
    view->offset = 12U;
    view->questions = read16(buffer + 4U);
    view->answers = read16(buffer + 6U);
    view->authority = read16(buffer + 8U);
    view->additional = read16(buffer + 10U);
}

typedef struct {
    char name[128];
    uint16_t type;
    uint16_t class_flags;
    uint32_t ttl;
    const uint8_t *rdata;
    size_t rdlength;
} rr_view_t;

static void next_record(dns_view_t *view, rr_view_t *record)
{
    assert(read_name(view->buffer, view->length, &view->offset, record->name,
                     sizeof(record->name)));
    assert(view->offset + 10U <= view->length);
    record->type = read16(view->buffer + view->offset);
    record->class_flags = read16(view->buffer + view->offset + 2U);
    record->ttl = ((uint32_t)view->buffer[view->offset + 4U] << 24U) |
                  ((uint32_t)view->buffer[view->offset + 5U] << 16U) |
                  ((uint32_t)view->buffer[view->offset + 6U] << 8U) |
                  (uint32_t)view->buffer[view->offset + 7U];
    record->rdlength = read16(view->buffer + view->offset + 8U);
    record->rdata = view->buffer + view->offset + 10U;
    view->offset += 10U + record->rdlength;
    assert(view->offset <= view->length);
}

static const rr_view_t *find_record(dns_view_t *view, unsigned count,
                                    uint16_t type, rr_view_t *storage)
{
    for (unsigned i = 0U; i < count; ++i) {
        next_record(view, storage);
        if (storage->type == type) return storage;
    }
    return NULL;
}

static const rr_view_t *find_in_section(const uint8_t *buffer, size_t length,
                                        unsigned skip, unsigned count,
                                        uint16_t type, rr_view_t *storage)
{
    dns_view_t view;
    open_packet(&view, buffer, length);
    rr_view_t scratch;
    for (unsigned i = 0U; i < skip; ++i) next_record(&view, &scratch);
    return find_record(&view, count, type, storage);
}

static size_t build_query(uint8_t *packet, size_t capacity, uint16_t id,
                          const char *name, uint16_t type, uint16_t qclass)
{
    size_t used = 0U;
    memset(packet, 0, 12U);
    packet[0] = (uint8_t)(id >> 8U);
    packet[1] = (uint8_t)id;
    packet[5] = 1U; /* qdcount */
    used = 12U;

    const char *label = name;
    while (label != NULL && *label != '\0') {
        const char *dot = strchr(label, '.');
        size_t len = dot != NULL ? (size_t)(dot - label) : strlen(label);
        assert(used + 1U + len < capacity);
        packet[used++] = (uint8_t)len;
        memcpy(packet + used, label, len);
        used += len;
        label = dot != NULL ? dot + 1 : NULL;
    }
    assert(used + 1U + 4U < capacity);
    packet[used++] = 0U;
    packet[used++] = (uint8_t)(type >> 8U);
    packet[used++] = (uint8_t)type;
    packet[used++] = (uint8_t)(qclass >> 8U);
    packet[used++] = (uint8_t)qclass;
    return used;
}

int main(void)
{
    airplay_txt_record_t records[AIRPLAY_TXT_MAX_RECORDS];
    size_t record_count = airplay_txt_build(
        &(airplay_txt_config_t){
            .device_id = "00:11:22:33:44:55",
            .model = "D213",
            .source_version = "950.7.1",
            .protocol_version = "1.1",
            .pairing_id = "00000000-1111-2222-3333-444444444444",
            .public_key = "00112233445566778899aabbccddeeff",
        },
        records, AIRPLAY_TXT_MAX_RECORDS);
    assert(record_count == 8U);

    uint8_t txt[512];
    size_t txt_len = airplay_txt_encode(records, record_count, txt, sizeof(txt));
    assert(txt_len > 0U);

    mdns_responder_config_t config = {
        .host_name = "rt-carplay",
        .instance_name = "Demo Car",
        .port = 7000U,
        .txt = txt,
        .txt_len = txt_len,
    };
    memcpy(config.ipv4, k_ipv4, 4U);

    mdns_responder_t *responder = mdns_responder_create(&config);
    assert(responder != NULL);

    uint8_t query[512];
    uint8_t response[MDNS_MAX_PACKET];
    bool unicast = true;

    /* PTR query: PTR is the answer, SRV/TXT/A are additionals. */
    size_t query_len = build_query(query, sizeof(query), 0x1234U,
                                   "_airplay._tcp.local", 12U, 1U);
    size_t response_len = mdns_responder_build_response(
        responder, query, query_len, response, sizeof(response), &unicast);
    assert(response_len > 12U);
    assert(!unicast);
    assert(read16(response) == 0x1234U);
    assert(read16(response + 2U) == 0x8400U);

    dns_view_t view;
    open_packet(&view, response, response_len);
    assert(view.questions == 0U);
    assert(view.answers == 1U);
    assert(view.authority == 0U);
    assert(view.additional == 3U);

    rr_view_t record;
    const rr_view_t *ptr = find_record(&view, view.answers, 12U, &record);
    assert(ptr != NULL);
    assert(strcmp(ptr->name, "_airplay._tcp.local") == 0);
    assert(ptr->class_flags == 1U);
    char target[128];
    size_t target_offset = 0U;
    size_t rdata_offset = (size_t)(ptr->rdata - response);
    assert(read_name(response, response_len, &rdata_offset, target,
                     sizeof(target)));
    assert(target_offset == 0U);
    assert(strcmp(target, "demo car._airplay._tcp.local") == 0);

    const rr_view_t *srv = find_in_section(response, response_len, 1U, 3U, 33U,
                                           &record);
    assert(srv != NULL);
    assert(strcmp(srv->name, "demo car._airplay._tcp.local") == 0);
    assert((srv->class_flags & 0x8001U) == 0x8001U);
    assert(read16(srv->rdata + 4U) == 7000U);
    /* SRV target follows priority/weight/port. */
    size_t srv_target = (size_t)(srv->rdata + 6U - response);
    assert(read_name(response, response_len, &srv_target, target,
                     sizeof(target)));
    assert(strcmp(target, "rt-carplay.local") == 0);

    const rr_view_t *txt_rec = find_in_section(response, response_len, 1U, 3U,
                                               16U, &record);
    assert(txt_rec != NULL);
    assert(txt_rec->rdlength == txt_len);
    assert(memcmp(txt_rec->rdata, txt, txt_len) == 0);

    const rr_view_t *a_record = find_in_section(response, response_len, 1U, 3U,
                                                1U, &record);
    assert(a_record != NULL);
    assert(strcmp(a_record->name, "rt-carplay.local") == 0);
    assert(a_record->rdlength == 4U);
    assert(memcmp(a_record->rdata, k_ipv4, 4U) == 0);

    /* Direct SRV query returns SRV as the answer and A as additional. */
    query_len = build_query(query, sizeof(query), 0U,
                            "Demo Car._airplay._tcp.local", 33U, 1U);
    response_len = mdns_responder_build_response(
        responder, query, query_len, response, sizeof(response), &unicast);
    assert(response_len > 0U);
    open_packet(&view, response, response_len);
    assert(view.answers == 1U && view.additional == 1U);

    /* Direct A query, case-insensitive, with the unicast-response bit. */
    query_len = build_query(query, sizeof(query), 0U, "RT-CarPlay.LOCAL", 1U,
                            0x8001U);
    response_len = mdns_responder_build_response(
        responder, query, query_len, response, sizeof(response), &unicast);
    assert(response_len > 0U);
    assert(unicast);
    open_packet(&view, response, response_len);
    assert(view.answers == 1U && view.additional == 0U);

    /* Unrelated names and malformed packets are ignored. */
    query_len = build_query(query, sizeof(query), 0U, "_googlecast._tcp.local",
                            12U, 1U);
    assert(mdns_responder_build_response(responder, query, query_len, response,
                                         sizeof(response), &unicast) == 0U);
    assert(mdns_responder_build_response(responder, query, 8U, response,
                                         sizeof(response), &unicast) == 0U);
    assert(mdns_responder_build_response(NULL, query, query_len, response,
                                         sizeof(response), &unicast) == 0U);

    /* Announcement carries every record. */
    response_len = mdns_responder_build_announcement(responder, response,
                                                     sizeof(response));
    assert(response_len > 0U);
    open_packet(&view, response, response_len);
    assert(view.answers == 4U);

    /* Invalid configuration is rejected. */
    assert(mdns_responder_create(NULL) == NULL);
    mdns_responder_config_t bad = config;
    bad.host_name = "";
    assert(mdns_responder_create(&bad) == NULL);
    bad = config;
    bad.host_name = "with.dot";
    assert(mdns_responder_create(&bad) == NULL);

    mdns_responder_destroy(responder);
    puts("mDNS responder host test passed");
    return 0;
}
