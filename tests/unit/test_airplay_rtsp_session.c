/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_rtsp_session.h"
#include "airplay_bplist.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../fixtures/audio/session-fixtures.h"

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

/* {"streams":[{"type":100,"streamConnectionID":777,"audioFormat":2048}]} */
static const uint8_t k_audio_setup_plist[] = {
    0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30, 0xd1, 0x01, 0x02, 0x57,
    0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x73, 0xa1, 0x03, 0xd3, 0x04, 0x05,
    0x06, 0x07, 0x08, 0x09, 0x54, 0x74, 0x79, 0x70, 0x65, 0x5f, 0x10, 0x12,
    0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x43, 0x6f, 0x6e, 0x6e, 0x65, 0x63,
    0x74, 0x69, 0x6f, 0x6e, 0x49, 0x44, 0x5b, 0x61, 0x75, 0x64, 0x69, 0x6f,
    0x46, 0x6f, 0x72, 0x6d, 0x61, 0x74, 0x10, 0x64, 0x11, 0x03, 0x09, 0x11,
    0x08, 0x00, 0x08, 0x0b, 0x13, 0x15, 0x1c, 0x21, 0x36, 0x42, 0x44, 0x47,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4a,
};

/* {"streams":[{"type":101,"streamConnectionID":778,"audioFormat":1024}]} */
static const uint8_t k_audio_alt_setup_plist[] = {
    0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30, 0xd1, 0x01, 0x02, 0x57,
    0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x73, 0xa1, 0x03, 0xd3, 0x04, 0x05,
    0x06, 0x07, 0x08, 0x09, 0x54, 0x74, 0x79, 0x70, 0x65, 0x5f, 0x10, 0x12,
    0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x43, 0x6f, 0x6e, 0x6e, 0x65, 0x63,
    0x74, 0x69, 0x6f, 0x6e, 0x49, 0x44, 0x5b, 0x61, 0x75, 0x64, 0x69, 0x6f,
    0x46, 0x6f, 0x72, 0x6d, 0x61, 0x74, 0x10, 0x65, 0x11, 0x03, 0x0a, 0x11,
    0x04, 0x00, 0x08, 0x0b, 0x13, 0x15, 0x1c, 0x21, 0x36, 0x42, 0x44, 0x47,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4a,
};

/* Screen (12345) plus main audio (779, 44.1 kHz stereo) in one SETUP. */
static const uint8_t k_screen_and_audio_setup_plist[] = {
    0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30, 0xd1, 0x01, 0x02, 0x57,
    0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x73, 0xa2, 0x03, 0x08, 0xd2, 0x04,
    0x05, 0x06, 0x07, 0x54, 0x74, 0x79, 0x70, 0x65, 0x5f, 0x10, 0x12, 0x73,
    0x74, 0x72, 0x65, 0x61, 0x6d, 0x43, 0x6f, 0x6e, 0x6e, 0x65, 0x63, 0x74,
    0x69, 0x6f, 0x6e, 0x49, 0x44, 0x10, 0x6e, 0x11, 0x30, 0x39, 0xd3, 0x04,
    0x05, 0x09, 0x0a, 0x0b, 0x0c, 0x5b, 0x61, 0x75, 0x64, 0x69, 0x6f, 0x46,
    0x6f, 0x72, 0x6d, 0x61, 0x74, 0x10, 0x64, 0x11, 0x03, 0x0b, 0x11, 0x08,
    0x00, 0x08, 0x0b, 0x13, 0x16, 0x1b, 0x20, 0x35, 0x37, 0x3a, 0x41, 0x4d,
    0x4f, 0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x55,
};

/* {"streams":[{"type":100,"streamConnectionID":777}]}: audioFormat is absent. */
static const uint8_t k_audio_missing_format_plist[] = {
    0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30, 0xd1, 0x01, 0x02, 0x57,
    0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x73, 0xa1, 0x03, 0xd2, 0x04, 0x05,
    0x06, 0x07, 0x54, 0x74, 0x79, 0x70, 0x65, 0x5f, 0x10, 0x12, 0x73, 0x74,
    0x72, 0x65, 0x61, 0x6d, 0x43, 0x6f, 0x6e, 0x6e, 0x65, 0x63, 0x74, 0x69,
    0x6f, 0x6e, 0x49, 0x44, 0x10, 0x64, 0x11, 0x03, 0x09, 0x08, 0x0b, 0x13,
    0x15, 0x1a, 0x1f, 0x34, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x39,
};

typedef struct {
    unsigned event_starts;
    unsigned timing_starts;
    unsigned event_stops;
    unsigned timing_stops;
    unsigned screen_starts;
    unsigned screen_stops;
    unsigned audio_starts;
    unsigned audio_stops;
    uint64_t last_screen_connection_id;
    uint16_t last_audio_type;
    uint64_t last_audio_connection_id;
    uint64_t last_audio_format_bits;
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

static bool test_start_audio(const airplay_rtsp_pairing_t *pairing,
                             uint16_t stream_type,
                             uint64_t stream_connection_id,
                             uint64_t audio_format_bits,
                             uint16_t *data_port, uint16_t *control_port,
                             void *context)
{
    (void)pairing;
    test_ports_t *ports = context;
    ports->audio_starts++;
    ports->last_audio_type = stream_type;
    ports->last_audio_connection_id = stream_connection_id;
    ports->last_audio_format_bits = audio_format_bits;
    *data_port = (uint16_t)(41000U + stream_type * 2U);
    *control_port = (uint16_t)(41001U + stream_type * 2U);
    return true;
}

static void test_stop_audio(uint16_t stream_type, void *context)
{
    test_ports_t *ports = context;
    ports->audio_stops++;
    ports->last_audio_type = stream_type;
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

static void expect_stream_entry(const airplay_bplist_doc_t *doc,
                                airplay_bplist_object_t streams, uint64_t index,
                                uint64_t type, uint64_t data_port,
                                uint64_t control_port, uint64_t connection_id)
{
    airplay_bplist_object_t entry;
    airplay_bplist_object_t value;
    uint64_t number = 0U;
    assert(airplay_bplist_array_get(doc, streams, index, &entry) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(doc, entry, "type", &value) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_uint(doc, value, &number) == AIRPLAY_BPLIST_OK);
    assert(number == type);
    assert(airplay_bplist_dict_get(doc, entry, "dataPort", &value) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_uint(doc, value, &number) == AIRPLAY_BPLIST_OK);
    assert(number == data_port);
    if (control_port == 0U) {
        assert(airplay_bplist_dict_get(doc, entry, "controlPort", &value) ==
               AIRPLAY_BPLIST_NOT_FOUND);
        return;
    }
    assert(airplay_bplist_dict_get(doc, entry, "controlPort", &value) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_uint(doc, value, &number) == AIRPLAY_BPLIST_OK);
    assert(number == control_port);
    assert(airplay_bplist_dict_get(doc, entry, "streamConnectionID", &value) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_uint(doc, value, &number) == AIRPLAY_BPLIST_OK);
    assert(number == connection_id);
}

static void test_audio_stream_lifecycle(void)
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
    config.start_audio = test_start_audio;
    config.stop_event = test_stop_event;
    config.stop_timing = test_stop_timing;
    config.stop_screen = test_stop_screen;
    config.stop_audio = test_stop_audio;
    config.media_context = &ports;

    airplay_rtsp_session_t *session = NULL;
    assert(airplay_rtsp_session_create(&config, &session) == AIRPLAY_RTSP_SESSION_OK);
    uint8_t response[AIRPLAY_RTSP_SESSION_RESPONSE_MAX_BYTES];
    size_t response_len = 0U;
    bool enable_cipher = false;

    airplay_rtsp_request_t request = parse_request(
        "SETUP /stream RTSP/1.0\r\nCSeq: 1\r\n\r\n");
    request.body = k_initial_setup_plist;
    request.body_len = sizeof(k_initial_setup_plist);
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(contains_bytes(response, response_len, "200 OK"));

    /* A single audio stream SETUP answers with both ports and the ID. */
    request = parse_request("SETUP /stream RTSP/1.0\r\nCSeq: 2\r\n\r\n");
    request.body = k_audio_setup_plist;
    request.body_len = sizeof(k_audio_setup_plist);
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(contains_bytes(response, response_len, "200 OK"));
    assert(ports.audio_starts == 1U);
    assert(ports.last_audio_type == 100U);
    assert(ports.last_audio_connection_id == 777U);
    assert(ports.last_audio_format_bits == 2048U);
    assert(ports.screen_starts == 0U);
    const uint8_t *plist = find_bytes(response, response_len, "bplist00");
    assert(plist != NULL);
    airplay_bplist_doc_t doc;
    airplay_bplist_object_t root;
    airplay_bplist_object_t streams;
    airplay_bplist_type_t type;
    uint64_t count = 0U;
    assert(airplay_bplist_open(plist, response_len - (size_t)(plist - response),
                               &doc) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc, root, "streams", &streams) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_type(&doc, streams, &type, &count) ==
           AIRPLAY_BPLIST_OK);
    assert(type == AIRPLAY_BPLIST_ARRAY && count == 1U);
    expect_stream_entry(&doc, streams, 0U, 100U, 41200U, 41201U, 777U);

    /* Repeating the same SETUP reuses the running stream. */
    request = parse_request("SETUP /stream RTSP/1.0\r\nCSeq: 3\r\n\r\n");
    request.body = k_audio_setup_plist;
    request.body_len = sizeof(k_audio_setup_plist);
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(contains_bytes(response, response_len, "200 OK"));
    assert(ports.audio_starts == 1U && ports.audio_stops == 0U);

    /* audioFormat is mandatory for an audio stream. */
    request = parse_request("SETUP /stream RTSP/1.0\r\nCSeq: 4\r\n\r\n");
    request.body = k_audio_missing_format_plist;
    request.body_len = sizeof(k_audio_missing_format_plist);
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(contains_bytes(response, response_len, "400 Bad Request"));
    assert(ports.audio_starts == 1U);

    /* Audio-only TEARDOWN leaves event and timing running. */
    request = parse_request("TEARDOWN /stream RTSP/1.0\r\nCSeq: 5\r\n\r\n");
    request.body = k_audio_setup_plist;
    request.body_len = sizeof(k_audio_setup_plist);
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(contains_bytes(response, response_len, "200 OK"));
    assert(ports.audio_stops == 1U);
    assert(ports.event_stops == 0U && ports.timing_stops == 0U);
    assert(ports.screen_stops == 0U);

    /* The torn-down connection ID may not be reused for a new key. */
    request = parse_request("SETUP /stream RTSP/1.0\r\nCSeq: 6\r\n\r\n");
    request.body = k_audio_setup_plist;
    request.body_len = sizeof(k_audio_setup_plist);
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(contains_bytes(response, response_len, "503 Service Unavailable"));
    assert(ports.audio_starts == 1U);

    /* The alternate stream uses its own slot. */
    request = parse_request("SETUP /stream RTSP/1.0\r\nCSeq: 7\r\n\r\n");
    request.body = k_audio_alt_setup_plist;
    request.body_len = sizeof(k_audio_alt_setup_plist);
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(contains_bytes(response, response_len, "200 OK"));
    assert(ports.audio_starts == 2U);
    assert(ports.last_audio_type == 101U);
    assert(ports.last_audio_format_bits == 1024U);
    plist = find_bytes(response, response_len, "bplist00");
    assert(plist != NULL);
    assert(airplay_bplist_open(plist, response_len - (size_t)(plist - response),
                               &doc) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc, root, "streams", &streams) ==
           AIRPLAY_BPLIST_OK);
    expect_stream_entry(&doc, streams, 0U, 101U, 41202U, 41203U, 778U);

    /* Screen and audio can be accepted in one SETUP. */
    request = parse_request("SETUP /stream RTSP/1.0\r\nCSeq: 8\r\n\r\n");
    request.body = k_screen_and_audio_setup_plist;
    request.body_len = sizeof(k_screen_and_audio_setup_plist);
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(contains_bytes(response, response_len, "200 OK"));
    assert(ports.screen_starts == 1U && ports.last_screen_connection_id == 12345U);
    assert(ports.audio_starts == 3U && ports.last_audio_connection_id == 779U);
    plist = find_bytes(response, response_len, "bplist00");
    assert(plist != NULL);
    assert(airplay_bplist_open(plist, response_len - (size_t)(plist - response),
                               &doc) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc, root, "streams", &streams) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_type(&doc, streams, &type, &count) ==
           AIRPLAY_BPLIST_OK);
    assert(type == AIRPLAY_BPLIST_ARRAY && count == 2U);
    expect_stream_entry(&doc, streams, 0U, 110U, 49154U, 0U, 0U);
    expect_stream_entry(&doc, streams, 1U, 100U, 41200U, 41201U, 779U);

    /* Selecting audio by type stops the main stream but not the alt stream,
     * the screen, or the transport endpoints. */
    request = parse_request("TEARDOWN /stream RTSP/1.0\r\nCSeq: 9\r\n\r\n");
    request.body = k_audio_setup_plist;
    request.body_len = sizeof(k_audio_setup_plist);
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(ports.audio_stops == 2U);
    assert(ports.screen_stops == 0U);
    assert(ports.event_stops == 0U && ports.timing_stops == 0U);

    request = parse_request("TEARDOWN /stream RTSP/1.0\r\nCSeq: 10\r\n\r\n");
    request.body = k_setup_plist;
    request.body_len = sizeof(k_setup_plist);
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(ports.screen_stops == 1U);
    assert(ports.audio_stops == 2U);

    airplay_rtsp_session_destroy(session);
    assert(ports.audio_stops == 3U); /* the alt stream */
}

typedef struct {
    unsigned calls;
    float last_db;
} volume_capture_t;

static void test_volume(float volume_db, void *context)
{
    volume_capture_t *capture = context;
    ++capture->calls;
    capture->last_db = volume_db;
}

static void test_set_parameter_volume(void)
{
    static const uint8_t identifier[] = "test-accessory";
    static const uint8_t public_key[32] = {0};
    volume_capture_t capture = {0};
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
    config.on_volume = test_volume;
    config.volume_context = &capture;

    airplay_rtsp_session_t *session = NULL;
    assert(airplay_rtsp_session_create(&config, &session) == AIRPLAY_RTSP_SESSION_OK);
    uint8_t response[AIRPLAY_RTSP_SESSION_RESPONSE_MAX_BYTES];
    size_t response_len = 0U;
    bool enable_cipher = false;

    /* The sender's text/parameters volume line reaches the callback. */
    airplay_rtsp_request_t request = parse_request(
        "SET_PARAMETER /stream RTSP/1.0\r\n"
        "CSeq: 20\r\n"
        "Content-Type: text/parameters\r\n"
        "Content-Length: 20\r\n"
        "\r\n"
        "volume: -20.500000\r\n");
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(contains_bytes(response, response_len, "200 OK"));
    assert(capture.calls == 1U);
    assert(capture.last_db == -20.5f);

    /* Full scale is 0 dB and volumes outside [-144, 0] are clamped. */
    request = parse_request(
        "SET_PARAMETER /stream RTSP/1.0\r\n"
        "CSeq: 21\r\n"
        "Content-Type: text/parameters\r\n"
        "Content-Length: 18\r\n"
        "\r\n"
        "volume: 0.000000\r\n");
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(capture.calls == 2U && capture.last_db == 0.0f);

    request = parse_request(
        "SET_PARAMETER /stream RTSP/1.0\r\n"
        "CSeq: 22\r\n"
        "Content-Type: text/parameters\r\n"
        "Content-Length: 18\r\n"
        "\r\n"
        "volume: 5.000000\r\n");
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(capture.calls == 3U && capture.last_db == 0.0f);

    request = parse_request(
        "SET_PARAMETER /stream RTSP/1.0\r\n"
        "CSeq: 23\r\n"
        "Content-Type: text/parameters\r\n"
        "Content-Length: 21\r\n"
        "\r\n"
        "volume: -200.000000\r\n");
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(capture.calls == 4U && capture.last_db == -144.0f);

    /* Other parameters and malformed values are acknowledged but ignored. */
    request = parse_request(
        "SET_PARAMETER /stream RTSP/1.0\r\n"
        "CSeq: 24\r\n"
        "Content-Type: text/parameters\r\n"
        "Content-Length: 17\r\n"
        "\r\n"
        "progress: 1/2/3\r\n");
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(contains_bytes(response, response_len, "200 OK"));
    assert(capture.calls == 4U);

    request = parse_request(
        "SET_PARAMETER /stream RTSP/1.0\r\n"
        "CSeq: 25\r\n"
        "Content-Type: text/parameters\r\n"
        "Content-Length: 14\r\n"
        "\r\n"
        "volume: loud\r\n");
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(contains_bytes(response, response_len, "200 OK"));
    assert(capture.calls == 4U);

    request = parse_request("GET_PARAMETER /stream RTSP/1.0\r\nCSeq: 26\r\n\r\n");
    assert(airplay_rtsp_session_handle(session, &request, response, sizeof(response),
                                       &response_len, &enable_cipher) == AIRPLAY_RTSP_SESSION_OK);
    assert(contains_bytes(response, response_len, "200 OK"));
    assert(capture.calls == 4U);

    airplay_rtsp_session_destroy(session);
}


static const uint8_t k_iap_setup[] = {
    0x62,0x70,0x6c,0x69,0x73,0x74,0x30,0x30,0xd1,0x01,0x02,0x57,0x73,0x74,0x72,0x65,
    0x61,0x6d,0x73,0xa1,0x03,0xd4,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x54,0x74,
    0x79,0x70,0x65,0x5e,0x63,0x6c,0x69,0x65,0x6e,0x74,0x54,0x79,0x70,0x65,0x55,0x55,
    0x49,0x44,0x54,0x73,0x65,0x65,0x64,0x5f,0x10,0x12,0x73,0x74,0x72,0x65,0x61,0x6d,
    0x43,0x6f,0x6e,0x6e,0x65,0x63,0x74,0x69,0x6f,0x6e,0x49,0x44,0x10,0x82,0x5f,0x10,
    0x24,0x45,0x39,0x34,0x35,0x39,0x46,0x44,0x30,0x2d,0x42,0x43,0x41,0x44,0x2d,0x34,
    0x43,0x34,0x35,0x2d,0x38,0x32,0x30,0x46,0x2d,0x31,0x45,0x37,0x32,0x34,0x34,0x37,
    0x45,0x46,0x32,0x46,0x32,0x13,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x10,0x7b,
    0x08,0x0b,0x13,0x15,0x1e,0x23,0x32,0x37,0x4c,0x4e,0x75,0x7e,0x00,0x00,0x00,0x00,
    0x00,0x00,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0c,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x80,
};
static const uint8_t k_iap_new_seed[] = {
    0x62,0x70,0x6c,0x69,0x73,0x74,0x30,0x30,0xd1,0x01,0x02,0x57,0x73,0x74,0x72,0x65,
    0x61,0x6d,0x73,0xa1,0x03,0xd3,0x04,0x05,0x06,0x07,0x08,0x09,0x54,0x74,0x79,0x70,
    0x65,0x5e,0x63,0x6c,0x69,0x65,0x6e,0x74,0x54,0x79,0x70,0x65,0x55,0x55,0x49,0x44,
    0x54,0x73,0x65,0x65,0x64,0x10,0x82,0x5f,0x10,0x24,0x45,0x39,0x34,0x35,0x39,0x46,
    0x44,0x30,0x2d,0x42,0x43,0x41,0x44,0x2d,0x34,0x43,0x34,0x35,0x2d,0x38,0x32,0x30,
    0x46,0x2d,0x31,0x45,0x37,0x32,0x34,0x34,0x37,0x45,0x46,0x32,0x46,0x32,0x10,0x02,
    0x08,0x0b,0x13,0x15,0x1c,0x21,0x30,0x35,0x37,0x5e,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0a,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x60,
};
static const uint8_t k_iap_bad_uuid[] = {
    0x62,0x70,0x6c,0x69,0x73,0x74,0x30,0x30,0xd1,0x01,0x02,0x57,0x73,0x74,0x72,0x65,
    0x61,0x6d,0x73,0xa1,0x03,0xd3,0x04,0x05,0x06,0x07,0x08,0x09,0x54,0x74,0x79,0x70,
    0x65,0x5e,0x63,0x6c,0x69,0x65,0x6e,0x74,0x54,0x79,0x70,0x65,0x55,0x55,0x49,0x44,
    0x54,0x73,0x65,0x65,0x64,0x10,0x82,0x5b,0x75,0x6e,0x73,0x75,0x70,0x70,0x6f,0x72,
    0x74,0x65,0x64,0x10,0x01,0x08,0x0b,0x13,0x15,0x1c,0x21,0x30,0x35,0x37,0x43,0x00,
    0x00,0x00,0x00,0x00,0x00,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0a,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x45,
};
static const uint8_t k_iap_stop[] = {
    0x62,0x70,0x6c,0x69,0x73,0x74,0x30,0x30,0xd1,0x01,0x02,0x57,0x73,0x74,0x72,0x65,
    0x61,0x6d,0x73,0xa1,0x03,0xd2,0x04,0x05,0x06,0x07,0x54,0x74,0x79,0x70,0x65,0x58,
    0x73,0x74,0x72,0x65,0x61,0x6d,0x49,0x44,0x10,0x82,0x10,0x01,0x08,0x0b,0x13,0x15,
    0x1a,0x1f,0x28,0x2a,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x01,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x2c,
};
static const uint8_t k_iap_other_stop[] = {
    0x62,0x70,0x6c,0x69,0x73,0x74,0x30,0x30,0xd1,0x01,0x02,0x57,0x73,0x74,0x72,0x65,
    0x61,0x6d,0x73,0xa1,0x03,0xd2,0x04,0x05,0x06,0x07,0x54,0x74,0x79,0x70,0x65,0x58,
    0x73,0x74,0x72,0x65,0x61,0x6d,0x49,0x44,0x10,0x82,0x10,0x02,0x08,0x0b,0x13,0x15,
    0x1a,0x1f,0x28,0x2a,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x01,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x2c,
};
static const uint8_t k_modes[] = {
    0x62,0x70,0x6c,0x69,0x73,0x74,0x30,0x30,0xd2,0x01,0x02,0x03,0x04,0x54,0x74,0x79,
    0x70,0x65,0x56,0x70,0x61,0x72,0x61,0x6d,0x73,0x5c,0x6d,0x6f,0x64,0x65,0x73,0x43,
    0x68,0x61,0x6e,0x67,0x65,0x64,0xd2,0x05,0x06,0x07,0x08,0x59,0x61,0x70,0x70,0x53,
    0x74,0x61,0x74,0x65,0x73,0x59,0x72,0x65,0x73,0x6f,0x75,0x72,0x63,0x65,0x73,0xa0,
    0xa0,0x08,0x0d,0x12,0x19,0x26,0x2b,0x35,0x3f,0x40,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x09,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x41,
};
static const uint8_t k_disable[] = {
    0x62,0x70,0x6c,0x69,0x73,0x74,0x30,0x30,0xd2,0x01,0x02,0x03,0x04,0x54,0x74,0x79,
    0x70,0x65,0x56,0x70,0x61,0x72,0x61,0x6d,0x73,0x5f,0x10,0x10,0x64,0x69,0x73,0x61,
    0x62,0x6c,0x65,0x42,0x6c,0x75,0x65,0x74,0x6f,0x6f,0x74,0x68,0xd1,0x05,0x06,0x58,
    0x64,0x65,0x76,0x69,0x63,0x65,0x49,0x44,0x5f,0x10,0x11,0x41,0x41,0x3a,0x42,0x42,
    0x3a,0x43,0x43,0x3a,0x44,0x44,0x3a,0x45,0x45,0x3a,0x46,0x46,0x08,0x0d,0x12,0x19,
    0x2c,0x2f,0x38,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x01,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x07,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x4c,
};
static const uint8_t k_bad_disable[] = {
    0x62,0x70,0x6c,0x69,0x73,0x74,0x30,0x30,0xd2,0x01,0x02,0x03,0x04,0x54,0x74,0x79,
    0x70,0x65,0x56,0x70,0x61,0x72,0x61,0x6d,0x73,0x5f,0x10,0x10,0x64,0x69,0x73,0x61,
    0x62,0x6c,0x65,0x42,0x6c,0x75,0x65,0x74,0x6f,0x6f,0x74,0x68,0xd1,0x05,0x06,0x58,
    0x64,0x65,0x76,0x69,0x63,0x65,0x49,0x44,0x10,0x7b,0x08,0x0d,0x12,0x19,0x2c,0x2f,
    0x38,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x07,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x3a,
};
static const uint8_t k_unknown[] = {
    0x62,0x70,0x6c,0x69,0x73,0x74,0x30,0x30,0xd2,0x01,0x02,0x03,0x04,0x54,0x74,0x79,
    0x70,0x65,0x56,0x70,0x61,0x72,0x61,0x6d,0x73,0x5d,0x75,0x6e,0x69,0x6d,0x70,0x6c,
    0x65,0x6d,0x65,0x6e,0x74,0x65,0x64,0xd0,0x08,0x0d,0x12,0x19,0x27,0x00,0x00,0x00,
    0x00,0x00,0x00,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x05,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x28,
};
static const uint8_t k_bad_command[] = {
    0x62,0x70,0x6c,0x69,0x73,0x74,0x30,0x30,0xd2,0x01,0x02,0x03,0x04,0x54,0x74,0x79,
    0x70,0x65,0x56,0x70,0x61,0x72,0x61,0x6d,0x73,0x5c,0x6d,0x6f,0x64,0x65,0x73,0x43,
    0x68,0x61,0x6e,0x67,0x65,0x64,0xa0,0x08,0x0d,0x12,0x19,0x26,0x00,0x00,0x00,0x00,
    0x00,0x00,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x05,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x27,
};

static unsigned iap_starts, iap_stops, handoff_requests;
static bool start_iap_test(const airplay_rtsp_pairing_t *pairing, uint64_t seed, uint16_t *port, void *context)
{
    (void)pairing; (void)context;
    assert(seed == UINT64_MAX);
    ++iap_starts; *port = 52000; return true;
}
static void stop_iap_test(void *context) { (void)context; ++iap_stops; }
static bool disable_bt_test(const char *device, void *context)
{
    (void)context;
    assert(!strcmp(device,"AA:BB:CC:DD:EE:FF")); ++handoff_requests; return true;
}
static void expect_body_status(airplay_rtsp_session_t *session, const char *method,
    const char *path, const uint8_t *body, size_t len, const char *status)
{
    airplay_rtsp_request_t r = parse_request("POST /stream RTSP/1.0\r\nCSeq: 77\r\n\r\n");
    strcpy(r.method,method); strcpy(r.path,path); r.body=body; r.body_len=len;
    uint8_t out[8192]; size_t written=0; bool cipher=false;
    assert(airplay_rtsp_session_handle(session,&r,out,sizeof(out),&written,&cipher)==AIRPLAY_RTSP_SESSION_OK);
    assert(contains_bytes(out,written,status));
    if (!strcmp(method,"SETUP") && body==k_iap_setup && !strcmp(status,"200 OK")) {
        /* Parse the actual response: this stream requires streamID and echoing
         * streamConnectionID, not the screen-only SETUP shape. */
        size_t offset=0;
        for (;offset+4<written;++offset) if (!memcmp(out+offset,"\r\n\r\n",4)) break;
        airplay_bplist_doc_t doc; airplay_bplist_object_t root,o; uint64_t value=0;
        assert(airplay_bplist_open(out+offset+4,written-offset-4,&doc)==AIRPLAY_BPLIST_OK);
        assert(airplay_bplist_root(&doc,&root)==AIRPLAY_BPLIST_OK);
        assert(airplay_bplist_dict_get(&doc,root,"streams",&o)==AIRPLAY_BPLIST_OK);
        assert(airplay_bplist_array_get(&doc,o,0,&o)==AIRPLAY_BPLIST_OK);
        root=o;
        assert(airplay_bplist_dict_get(&doc,root,"streamID",&o)==AIRPLAY_BPLIST_OK);
        assert(airplay_bplist_get_uint(&doc,o,&value)==AIRPLAY_BPLIST_OK && value==1);
        assert(airplay_bplist_dict_get(&doc,root,"streamConnectionID",&o)==AIRPLAY_BPLIST_OK);
        assert(airplay_bplist_get_uint(&doc,o,&value)==AIRPLAY_BPLIST_OK && value==123);
    }
}
#define EXPECT_IAP(method,path,name,status) expect_body_status(session,method,path,k_##name,sizeof(k_##name),status)
static unsigned modes_seen,ducks_seen,configured_seen;
static void audio_modes_test(const airplay_audio_modes_t *m,void *context)
{
    (void)context;if(!m->has_call && !m->has_speech && !m->has_main_audio)return;
    ++modes_seen;
    assert(m->has_call && m->call_active && m->has_speech && m->speech_mode==2);
    assert(m->has_main_audio && m->main_audio_entity==1);
}
static bool audio_duck_test(bool ducked,float db,uint32_t duration,void *context)
{
    (void)context;++ducks_seen;
    assert(ducked ? db== -18 && duration==250 : db==0 && duration==100);return true;
}
static bool configure_audio_test(const airplay_rtsp_pairing_t *p,uint16_t type,const airplay_audio_options_t *o,void *context)
{
    (void)p;(void)context;++configured_seen;
    assert(type==100 && o->input && o->input_port==50000 && o->frames_per_packet==960);
    assert(o->connection_id==9000 && !strcmp(o->audio_type,"speechRecognition"));return true;
}
static void test_iap_runtime_and_commands(void)
{
    static const uint8_t identifier[]="accessory", public_key[32]={0};
    test_ports_t ports={0};
    airplay_rtsp_session_config_t config={0};
    config.pairing.pair_setup.pairing_code="3939";
    config.pairing.pair_setup.accessory_identifier=identifier;
    config.pairing.pair_setup.accessory_identifier_len=sizeof(identifier)-1;
    config.pairing.pair_setup.accessory_public_key=public_key;
    config.pairing.pair_setup.random=test_random;
    config.pairing.pair_setup.sign_accessory=test_sign;
    config.pairing.pair_setup.verify_controller=test_verify;
    config.pairing.pair_setup.save_pairing=test_save;
    config.pairing.pair_verify.accessory_identifier=identifier;
    config.pairing.pair_verify.accessory_identifier_len=sizeof(identifier)-1;
    config.pairing.pair_verify.random=test_random;
    config.pairing.pair_verify.sign_accessory=test_sign;
    config.pairing.pair_verify.verify_controller=test_verify;
    config.pairing.pair_verify.lookup_controller=test_lookup;
    config.info_plist=k_setup_plist; config.info_plist_len=sizeof(k_setup_plist);
    config.start_event=test_start_event; config.start_timing=test_start_timing;
    config.stop_event=test_stop_event; config.stop_timing=test_stop_timing;
    config.media_context=&ports;
    config.start_iap=start_iap_test; config.stop_iap=stop_iap_test;
    config.disable_bluetooth=disable_bt_test;
    config.on_audio_modes=audio_modes_test;config.on_audio_duck=audio_duck_test;
    config.start_audio=test_start_audio;config.stop_audio=test_stop_audio;
    config.configure_audio=configure_audio_test;
    airplay_rtsp_session_t *session=NULL;
    assert(airplay_rtsp_session_create(&config,&session)==AIRPLAY_RTSP_SESSION_OK);
    EXPECT_IAP("POST","/command",disable,"503 Service Unavailable");
    EXPECT_IAP("SETUP","/stream",iap_setup,"503 Service Unavailable");
    expect_body_status(session,"SETUP","/stream",k_initial_setup_plist,sizeof(k_initial_setup_plist),"200 OK");
    EXPECT_IAP("POST","/command",modes,"200 OK");
    EXPECT_IAP("POST","/command",voice_modes,"200 OK");assert(modes_seen==1);
    EXPECT_IAP("POST","/command",voice_bad_modes,"400 Bad Request");assert(modes_seen==1);
    EXPECT_IAP("POST","/command",voice_duck,"200 OK");
    EXPECT_IAP("POST","/command",voice_unduck,"200 OK");assert(ducks_seen==2);
    EXPECT_IAP("POST","/command",voice_bad_duck,"400 Bad Request");
    EXPECT_IAP("POST","/command",voice_bad_duration,"400 Bad Request");assert(ducks_seen==2);
    EXPECT_IAP("SETUP","/stream",voice_bad_port,"400 Bad Request");
    EXPECT_IAP("SETUP","/stream",duplicate_audio_keys,"400 Bad Request");assert(configured_seen==0);
    EXPECT_IAP("SETUP","/stream",voice_setup,"200 OK");assert(configured_seen==1);
    EXPECT_IAP("SETUP","/stream",voice_setup,"200 OK");assert(configured_seen==1);
    EXPECT_IAP("SETUP","/stream",voice_changed_format,"400 Bad Request");
    expect_body_status(session,"POST","/feedback",NULL,0,"200 OK");

    EXPECT_IAP("POST","/command",disable,"200 OK");
    assert(handoff_requests==1);
    EXPECT_IAP("POST","/command",bad_disable,"400 Bad Request");
    EXPECT_IAP("POST","/command",bad_command,"400 Bad Request");
    EXPECT_IAP("POST","/command",unknown,"405 Method Not Allowed");
    assert(handoff_requests==1);
    EXPECT_IAP("SETUP","/stream",iap_bad_uuid,"400 Bad Request");
    EXPECT_IAP("SETUP","/stream",iap_setup,"200 OK");
    EXPECT_IAP("SETUP","/stream",iap_setup,"200 OK");
    assert(iap_starts==1);
    EXPECT_IAP("SETUP","/stream",iap_new_seed,"503 Service Unavailable");
    EXPECT_IAP("TEARDOWN","/stream",iap_other_stop,"200 OK");
    assert(iap_stops==0);
    EXPECT_IAP("TEARDOWN","/stream",iap_stop,"200 OK");
    assert(iap_stops==1);
    EXPECT_IAP("SETUP","/stream",iap_setup,"503 Service Unavailable");
    airplay_rtsp_session_destroy(session);
    assert(iap_stops==1);
}
int main(void)
{
    test_iap_runtime_and_commands();
    test_info_and_setup_gates();
    test_initial_setup_timing_and_event_ports();
    test_screen_key_lifecycle();
    test_audio_stream_lifecycle();
    test_set_parameter_volume();
    puts("airplay RTSP session host test passed (copy/borrow, RECORD before Screen SETUP, teardown gate, test-only signer)");
    return 0;
}
