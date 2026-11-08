/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_rtsp_session.h"
#include "airplay_bplist.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t k_setup_plist[] = {
    0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30,
    0xd1, 0x01, 0x02,
    0x57, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x73,
    0xa1, 0x03,
    0xd2, 0x04, 0x05, 0x06, 0x07,
    0x5f, 0x10, 0x12, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d,
    0x43, 0x6f, 0x6e, 0x6e, 0x65, 0x63, 0x74, 0x69, 0x6f, 0x6e, 0x49, 0x44,
    0x54, 0x74, 0x79, 0x70, 0x65,
    0x11, 0x30, 0x39,
    0x10, 0x6e,
    0x08, 0x0b, 0x13, 0x15, 0x1a, 0x2f, 0x34, 0x37,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x39,
};

static const uint8_t k_initial_setup_plist[] = {
    0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30,
    0xd1, 0x01, 0x02, 0x5a, 0x74, 0x69, 0x6d, 0x69,
    0x6e, 0x67, 0x50, 0x6f, 0x72, 0x74, 0x11, 0x04,
    0xd2, 0x08, 0x0b, 0x16, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x19,
};

static const uint8_t k_mixed_stream_setup_plist[] = {
    0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30, 0xd1, 0x01, 0x02,
    0x57, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x73, 0xa2, 0x03, 0x08,
    0xd2, 0x04, 0x05, 0x06, 0x07, 0x5f, 0x10, 0x12, 0x73, 0x74, 0x72,
    0x65, 0x61, 0x6d, 0x43, 0x6f, 0x6e, 0x6e, 0x65, 0x63, 0x74, 0x69,
    0x6f, 0x6e, 0x49, 0x44, 0x54, 0x74, 0x79, 0x70, 0x65, 0x11, 0x30,
    0x39, 0x10, 0x67, 0xd2, 0x04, 0x05, 0x06, 0x09, 0x10, 0x6e, 0x08,
    0x0b, 0x13, 0x16, 0x1b, 0x30, 0x35, 0x38, 0x3a, 0x3f, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41,
};

static const uint8_t k_unsupported_stream_setup_plist[] = {
    0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30, 0xd1, 0x01, 0x02,
    0x57, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x73, 0xa1, 0x03, 0xd2,
    0x04, 0x05, 0x06, 0x07, 0x5f, 0x10, 0x12, 0x73, 0x74, 0x72, 0x65,
    0x61, 0x6d, 0x43, 0x6f, 0x6e, 0x6e, 0x65, 0x63, 0x74, 0x69, 0x6f,
    0x6e, 0x49, 0x44, 0x54, 0x74, 0x79, 0x70, 0x65, 0x11, 0x30, 0x39,
    0x10, 0x67, 0x08, 0x0b, 0x13, 0x15, 0x1a, 0x2f, 0x34, 0x37, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x39,
};

typedef struct {
    unsigned event_starts;
    unsigned timing_starts;
    unsigned event_stops;
    unsigned timing_stops;
    unsigned screen_starts;
    unsigned screen_stops;
    uint64_t last_screen_connection_id;
    uint16_t requested_timing_port;
} test_ports_t;

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

static bool test_start_event(const airplay_rtsp_pairing_t *pairing,
                             uint16_t *bound_port, void *context)
{
    (void)pairing;
    test_ports_t *ports = context;
    ports->event_starts++;
    *bound_port = 49153U;
    return true;
}

static bool test_start_timing(uint16_t peer_port, uint16_t *bound_port,
                              void *context)
{
    test_ports_t *ports = context;
    ports->timing_starts++;
    ports->requested_timing_port = peer_port;
    *bound_port = 49152U;
    return true;
}

static bool test_start_screen(const airplay_rtsp_pairing_t *pairing,
                              uint64_t stream_connection_id,
                              uint16_t requested_port,
                              uint16_t *bound_port,
                              void *context)
{
    (void)pairing;
    (void)requested_port;
    test_ports_t *ports = context;
    ports->screen_starts++;
    ports->last_screen_connection_id = stream_connection_id;
    *bound_port = 49154U;
    return true;
}

static void test_stop_event(void *context)
{
    test_ports_t *ports = context;
    ports->event_stops++;
}

static void test_stop_timing(void *context)
{
    test_ports_t *ports = context;
    ports->timing_stops++;
}

static void test_stop_screen(void *context)
{
    test_ports_t *ports = context;
    ports->screen_stops++;
}

static airplay_rtsp_request_t parse_request(const char *wire)
{
    airplay_rtsp_request_t request;
    size_t wire_len = strlen(wire);
    assert(airplay_rtsp_parse_request((const uint8_t *)wire, wire_len, &request) ==
           AIRPLAY_RTSP_PARSE_COMPLETE);
    return request;
}

static bool contains_bytes(const uint8_t *bytes, size_t length,
                           const char *needle)
{
    size_t needle_len = strlen(needle);
    if (needle_len > length) return false;
    for (size_t i = 0U; i <= length - needle_len; ++i) {
        if (memcmp(bytes + i, needle, needle_len) == 0) return true;
    }
    return false;
}

static const uint8_t *find_bytes(const uint8_t *bytes, size_t length,
                                 const char *needle)
{
    size_t needle_len = strlen(needle);
    if (needle_len > length) return NULL;
    for (size_t i = 0U; i <= length - needle_len; ++i) {
        if (memcmp(bytes + i, needle, needle_len) == 0) return bytes + i;
    }
    return NULL;
}

static bool replace_setup_connection_id(uint8_t *plist, size_t plist_len,
                                        uint16_t connection_id)
{
    for (size_t index = 0U; index + 4U < plist_len; ++index) {
        if (plist[index] == 0x11U && plist[index + 3U] == 0x10U &&
            plist[index + 4U] == 0x6eU) {
            plist[index + 1U] = (uint8_t)(connection_id >> 8U);
            plist[index + 2U] = (uint8_t)connection_id;
            return true;
        }
    }
    return false;
}

static void test_info_and_setup_gates(void)
{
    static const uint8_t identifier[] = "test-accessory";
    static const uint8_t public_key[32] = {0};
    airplay_rtsp_session_config_t config;
    memset(&config, 0, sizeof(config));
    config.pairing.pair_setup.pairing_code = "external-test-code";
    config.pairing.pair_setup.accessory_identifier = identifier;
    config.pairing.pair_setup.accessory_identifier_len = sizeof(identifier) - 1U;
    config.pairing.pair_setup.accessory_public_key = public_key;
    config.pairing.pair_setup.random = test_random;
    config.pairing.pair_setup.sign_accessory = test_sign;
    config.pairing.pair_setup.verify_controller = test_verify;
    config.pairing.pair_setup.save_pairing = test_save;
    config.pairing.pair_verify.accessory_identifier = identifier;
    config.pairing.pair_verify.accessory_identifier_len = sizeof(identifier) - 1U;
    config.pairing.pair_verify.random = test_random;
    config.pairing.pair_verify.sign_accessory = test_sign;
    config.pairing.pair_verify.verify_controller = test_verify;
    config.pairing.pair_verify.lookup_controller = test_lookup;
    uint8_t mutable_info_plist[sizeof(k_setup_plist)];
    memcpy(mutable_info_plist, k_setup_plist, sizeof(mutable_info_plist));
    config.info_plist = mutable_info_plist;
    config.info_plist_len = sizeof(k_setup_plist);

    airplay_rtsp_session_t *session = NULL;
    assert(airplay_rtsp_session_create(&config, &session) == AIRPLAY_RTSP_SESSION_OK);
    memset(mutable_info_plist, 0, sizeof(mutable_info_plist));
    uint8_t response[AIRPLAY_RTSP_SESSION_RESPONSE_MAX_BYTES];
    size_t response_len = 0U;
    bool enable_cipher = false;

    airplay_rtsp_request_t request = parse_request(
        "GET /info RTSP/1.0\r\nCSeq: 1\r\n\r\n");
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(!enable_cipher && response_len > sizeof(k_setup_plist));
    assert(contains_bytes(response, response_len, "200 OK"));
    assert(memcmp(response + response_len - sizeof(k_setup_plist),
                  k_setup_plist, sizeof(k_setup_plist)) == 0);

    memset(&request, 0, sizeof(request));
    memcpy(request.method, "SETUP", sizeof("SETUP"));
    memcpy(request.path, "/stream", sizeof("/stream"));
    memcpy(request.protocol, "RTSP/1.0", sizeof("RTSP/1.0"));
    memcpy(request.cseq, "2", sizeof("2"));
    request.body = k_setup_plist;
    request.body_len = sizeof(k_setup_plist);
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(contains_bytes(response, response_len, "503 Service Unavailable"));

    request = parse_request(
        "RECORD /stream RTSP/1.0\r\nCSeq: 3\r\n\r\n");
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(contains_bytes(response, response_len, "503 Service Unavailable"));

    airplay_rtsp_session_destroy(session);

    memcpy(mutable_info_plist, k_setup_plist, sizeof(mutable_info_plist));
    config.info_plist = mutable_info_plist;
    assert(airplay_rtsp_session_create_borrowing_info_plist(
               &config, &session) == AIRPLAY_RTSP_SESSION_OK);
    mutable_info_plist[sizeof(mutable_info_plist) - 1U] ^= 1U;
    request = parse_request("GET /info RTSP/1.0\r\nCSeq: 4\r\n\r\n");
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(memcmp(response + response_len - sizeof(mutable_info_plist),
                  mutable_info_plist, sizeof(mutable_info_plist)) == 0);
    airplay_rtsp_session_destroy(session);
}

static void test_initial_setup_timing_and_event_ports(void)
{
    static const uint8_t identifier[] = "test-accessory";
    static const uint8_t public_key[32] = {0};
    test_ports_t ports = {0};
    airplay_rtsp_session_config_t config;
    memset(&config, 0, sizeof(config));
    config.pairing.pair_setup.pairing_code = "external-test-code";
    config.pairing.pair_setup.accessory_identifier = identifier;
    config.pairing.pair_setup.accessory_identifier_len = sizeof(identifier) - 1U;
    config.pairing.pair_setup.accessory_public_key = public_key;
    config.pairing.pair_setup.random = test_random;
    config.pairing.pair_setup.sign_accessory = test_sign;
    config.pairing.pair_setup.verify_controller = test_verify;
    config.pairing.pair_setup.save_pairing = test_save;
    config.pairing.pair_verify.accessory_identifier = identifier;
    config.pairing.pair_verify.accessory_identifier_len = sizeof(identifier) - 1U;
    config.pairing.pair_verify.random = test_random;
    config.pairing.pair_verify.sign_accessory = test_sign;
    config.pairing.pair_verify.verify_controller = test_verify;
    config.pairing.pair_verify.lookup_controller = test_lookup;
    config.info_plist = k_setup_plist;
    config.info_plist_len = sizeof(k_setup_plist);
    config.start_event = test_start_event;
    config.start_timing = test_start_timing;
    config.stop_event = test_stop_event;
    config.stop_timing = test_stop_timing;
    config.media_context = &ports;

    airplay_rtsp_session_t *session = NULL;
    assert(airplay_rtsp_session_create(&config, &session) == AIRPLAY_RTSP_SESSION_OK);
    airplay_rtsp_request_t request = parse_request(
        "SETUP /stream RTSP/1.0\r\nCSeq: 4\r\n\r\n");
    uint8_t response[AIRPLAY_RTSP_SESSION_RESPONSE_MAX_BYTES];
    size_t response_len = 0U;
    bool enable_cipher = false;

    uint8_t malformed_plist[sizeof(k_initial_setup_plist)];
    memcpy(malformed_plist, k_initial_setup_plist, sizeof(malformed_plist));
    malformed_plist[22] = 0x51U; /* timingPort value is a string, not an integer. */
    request.body = malformed_plist;
    request.body_len = sizeof(malformed_plist);
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(contains_bytes(response, response_len, "400 Bad Request"));
    assert(ports.event_starts == 0U && ports.timing_starts == 0U);

    request.body = k_initial_setup_plist;
    request.body_len = sizeof(k_initial_setup_plist);
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(!enable_cipher);
    assert(ports.event_starts == 1U && ports.timing_starts == 1U);
    assert(ports.requested_timing_port == 1234U);

    const uint8_t *plist = find_bytes(response, response_len, "bplist00");
    assert(plist != NULL);
    airplay_bplist_doc_t doc;
    airplay_bplist_object_t root;
    airplay_bplist_object_t value;
    uint64_t port = 0U;
    assert(airplay_bplist_open(plist, response_len - (size_t)(plist - response), &doc) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc, root, "timingPort", &value) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_uint(&doc, value, &port) == AIRPLAY_BPLIST_OK);
    assert(port == 49152U);
    assert(airplay_bplist_dict_get(&doc, root, "eventPort", &value) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_uint(&doc, value, &port) == AIRPLAY_BPLIST_OK);
    assert(port == 49153U);

    /* iPhone activates the transport before selecting the Screen stream:
     * initial SETUP -> /info -> RECORD -> Screen SETUP. */
    request = parse_request("GET /info RTSP/1.0\r\nCSeq: 5\r\n\r\n");
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(contains_bytes(response, response_len, "200 OK"));
    request = parse_request("RECORD /stream RTSP/1.0\r\nCSeq: 6\r\n\r\n");
    for (unsigned attempt = 0U; attempt < 2U; ++attempt) {
        assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                           &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
        assert(contains_bytes(response, response_len, "200 OK"));
        assert(ports.screen_starts == 0U);
        assert(ports.event_starts == 1U && ports.timing_starts == 1U);
    }

    request = parse_request("TEARDOWN /stream RTSP/1.0\r\nCSeq: 5\r\n\r\n");
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(ports.event_stops == 1U && ports.timing_stops == 1U);
    request = parse_request("RECORD /stream RTSP/1.0\r\nCSeq: 7\r\n\r\n");
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(contains_bytes(response, response_len, "503 Service Unavailable"));
    airplay_rtsp_session_destroy(session);
    assert(ports.event_stops == 1U && ports.timing_stops == 1U);
}

static void test_screen_key_lifecycle(void)
{
    static const uint8_t identifier[] = "test-accessory";
    static const uint8_t public_key[32] = {0};
    test_ports_t ports = {0};
    airplay_rtsp_session_config_t config;
    memset(&config, 0, sizeof(config));
    config.pairing.pair_setup.pairing_code = "external-test-code";
    config.pairing.pair_setup.accessory_identifier = identifier;
    config.pairing.pair_setup.accessory_identifier_len = sizeof(identifier) - 1U;
    config.pairing.pair_setup.accessory_public_key = public_key;
    config.pairing.pair_setup.random = test_random;
    config.pairing.pair_setup.sign_accessory = test_sign;
    config.pairing.pair_setup.verify_controller = test_verify;
    config.pairing.pair_setup.save_pairing = test_save;
    config.pairing.pair_verify.accessory_identifier = identifier;
    config.pairing.pair_verify.accessory_identifier_len = sizeof(identifier) - 1U;
    config.pairing.pair_verify.random = test_random;
    config.pairing.pair_verify.sign_accessory = test_sign;
    config.pairing.pair_verify.verify_controller = test_verify;
    config.pairing.pair_verify.lookup_controller = test_lookup;
    config.info_plist = k_setup_plist;
    config.info_plist_len = sizeof(k_setup_plist);
    config.start_event = test_start_event;
    config.start_timing = test_start_timing;
    config.start_screen = test_start_screen;
    config.stop_event = test_stop_event;
    config.stop_timing = test_stop_timing;
    config.stop_screen = test_stop_screen;
    config.media_context = &ports;

    airplay_rtsp_session_t *session = NULL;
    assert(airplay_rtsp_session_create(&config, &session) == AIRPLAY_RTSP_SESSION_OK);
    uint8_t response[AIRPLAY_RTSP_SESSION_RESPONSE_MAX_BYTES];
    size_t response_len = 0U;
    bool enable_cipher = false;
    airplay_rtsp_request_t request = parse_request(
        "SETUP /stream RTSP/1.0\r\nCSeq: 9\r\n\r\n");
    request.body = k_initial_setup_plist;
    request.body_len = sizeof(k_initial_setup_plist);
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(contains_bytes(response, response_len, "200 OK"));
    assert(ports.event_starts == 1U && ports.timing_starts == 1U);

    request = parse_request("RECORD /stream RTSP/1.0\r\nCSeq: 10\r\n\r\n");
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(contains_bytes(response, response_len, "200 OK"));
    assert(ports.screen_starts == 0U);

    request = parse_request("SETUP /stream RTSP/1.0\r\nCSeq: 10\r\n\r\n");

    request.body = k_unsupported_stream_setup_plist;
    request.body_len = sizeof(k_unsupported_stream_setup_plist);
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(contains_bytes(response, response_len, "200 OK"));
    const uint8_t *plist = find_bytes(response, response_len, "bplist00");
    assert(plist != NULL);
    airplay_bplist_doc_t response_doc;
    airplay_bplist_object_t root;
    airplay_bplist_object_t response_streams;
    airplay_bplist_type_t response_type;
    uint64_t response_count = 1U;
    assert(airplay_bplist_open(plist,
                               response_len - (size_t)(plist - response),
                               &response_doc) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&response_doc, &root) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&response_doc, root, "streams",
                                   &response_streams) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_type(&response_doc, response_streams,
                                   &response_type, &response_count) == AIRPLAY_BPLIST_OK);
    assert(response_type == AIRPLAY_BPLIST_ARRAY && response_count == 0U);
    assert(ports.screen_starts == 0U);

    request.body = k_mixed_stream_setup_plist;
    request.body_len = sizeof(k_mixed_stream_setup_plist);
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(contains_bytes(response, response_len, "200 OK"));
    plist = find_bytes(response, response_len, "bplist00");
    assert(plist != NULL);
    assert(airplay_bplist_open(plist,
                               response_len - (size_t)(plist - response),
                               &response_doc) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&response_doc, &root) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&response_doc, root, "streams",
                                   &response_streams) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_type(&response_doc, response_streams,
                                   &response_type, &response_count) == AIRPLAY_BPLIST_OK);
    assert(response_type == AIRPLAY_BPLIST_ARRAY && response_count == 1U);
    assert(ports.screen_starts == 1U && ports.last_screen_connection_id == 12345U);

    request = parse_request("RECORD /stream RTSP/1.0\r\nCSeq: 11\r\n\r\n");
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(contains_bytes(response, response_len, "200 OK"));

    request = parse_request("SETUP /stream RTSP/1.0\r\nCSeq: 12\r\n\r\n");
    request.body = k_setup_plist;
    request.body_len = sizeof(k_setup_plist);
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(contains_bytes(response, response_len, "200 OK"));
    assert(ports.screen_starts == 1U && ports.last_screen_connection_id == 12345U);

    request = parse_request("SETUP /stream RTSP/1.0\r\nCSeq: 13\r\n\r\n");
    request.body = k_setup_plist;
    request.body_len = sizeof(k_setup_plist);
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(contains_bytes(response, response_len, "200 OK"));
    assert(ports.screen_starts == 1U && ports.screen_stops == 0U);

    request = parse_request("TEARDOWN /stream RTSP/1.0\r\nCSeq: 14\r\n\r\n");
    request.body = k_unsupported_stream_setup_plist;
    request.body_len = sizeof(k_unsupported_stream_setup_plist);
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(contains_bytes(response, response_len, "200 OK"));
    assert(ports.screen_stops == 0U);
    assert(ports.event_stops == 0U && ports.timing_stops == 0U);

    request = parse_request("TEARDOWN /stream RTSP/1.0\r\nCSeq: 15\r\n\r\n");
    request.body = k_setup_plist;
    request.body_len = sizeof(k_setup_plist);
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(ports.screen_stops == 1U);
    assert(ports.event_stops == 0U && ports.timing_stops == 0U);

    request = parse_request("SETUP /stream RTSP/1.0\r\nCSeq: 16\r\n\r\n");
    request.body = k_setup_plist;
    request.body_len = sizeof(k_setup_plist);
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(contains_bytes(response, response_len, "503 Service Unavailable"));
    assert(ports.screen_starts == 1U && ports.screen_stops == 1U);

    uint8_t different_key_id_plist[sizeof(k_setup_plist)];
    memcpy(different_key_id_plist, k_setup_plist, sizeof(different_key_id_plist));
    assert(replace_setup_connection_id(different_key_id_plist,
                                      sizeof(different_key_id_plist), 12346U));
    request = parse_request("SETUP /stream RTSP/1.0\r\nCSeq: 17\r\n\r\n");
    request.body = different_key_id_plist;
    request.body_len = sizeof(different_key_id_plist);
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(contains_bytes(response, response_len, "200 OK"));
    assert(ports.screen_starts == 2U && ports.last_screen_connection_id == 12346U);

    request = parse_request("TEARDOWN /stream RTSP/1.0\r\nCSeq: 18\r\n\r\n");
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(ports.screen_stops == 2U);
    assert(ports.event_stops == 1U && ports.timing_stops == 1U);

    airplay_rtsp_session_destroy(session);
    assert(ports.screen_stops == 2U);
    assert(ports.event_stops == 1U && ports.timing_stops == 1U);
}

int main(void)
{
    test_info_and_setup_gates();
    test_initial_setup_timing_and_event_ports();
    test_screen_key_lifecycle();
    puts("airplay RTSP session host test passed (copy/borrow, RECORD before Screen SETUP, teardown gate, test-only signer)");
    return 0;
}
