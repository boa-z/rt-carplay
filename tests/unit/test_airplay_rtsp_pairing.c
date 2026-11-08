/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_rtsp_pairing.h"
#include "airplay_tlv8.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t random_counter;
} provider_fixture_t;

static const uint8_t ACCESSORY_ID[] = "rtsp-accessory";
static const uint8_t ACCESSORY_PUBLIC_KEY[32] = {
    0x41,0x43,0x43,0x45,0x53,0x53,0x4f,0x52,
    0x59,0x2d,0x54,0x45,0x53,0x54,0x2d,0x50,
    0x55,0x42,0x4c,0x49,0x43,0x2d,0x4b,0x45,
    0x59,0x2d,0x30,0x30,0x30,0x30,0x30,0x31,
};

static bool random_bytes(uint8_t *output, size_t output_len, void *context)
{
    provider_fixture_t *fixture = context;
    for (size_t i = 0; i < output_len; ++i) {
        output[i] = fixture->random_counter++;
    }
    return true;
}

static bool sign_accessory(const uint8_t *message,
                           size_t message_len,
                           uint8_t signature[64],
                           void *context)
{
    (void)context;
    uint8_t folded = 0U;
    for (size_t i = 0; i < message_len; ++i) folded ^= message[i];
    memset(signature, folded, 64U);
    return true;
}

static bool verify_controller(const uint8_t public_key[32],
                              const uint8_t *message,
                              size_t message_len,
                              const uint8_t signature[64],
                              void *context)
{
    (void)public_key;
    (void)message;
    (void)message_len;
    (void)signature;
    (void)context;
    return true;
}

static bool save_pairing(const uint8_t *identifier,
                         size_t identifier_len,
                         const uint8_t public_key[32],
                         void *context)
{
    (void)identifier;
    (void)identifier_len;
    (void)public_key;
    (void)context;
    return true;
}

static bool lookup_controller(const uint8_t *identifier,
                              size_t identifier_len,
                              uint8_t public_key[32],
                              void *context)
{
    (void)identifier;
    (void)identifier_len;
    (void)context;
    memset(public_key, 0x55, 32U);
    return true;
}

static size_t encode_tlv(const airplay_tlv8_item_t *items,
                         size_t count,
                         uint8_t *output,
                         size_t capacity)
{
    size_t written = 0U;
    assert(airplay_tlv8_encode(items, count, output, capacity, &written) ==
           AIRPLAY_TLV8_OK);
    return written;
}

static void parse_response_body(const uint8_t *response,
                                size_t response_len,
                                const uint8_t **body,
                                size_t *body_len)
{
    static const uint8_t separator[] = "\r\n\r\n";
    bool found = false;
    for (size_t i = 0; i + sizeof(separator) - 1U <= response_len; ++i) {
        if (memcmp(response + i, separator, sizeof(separator) - 1U) == 0) {
            *body = response + i + sizeof(separator) - 1U;
            *body_len = response_len - i - (sizeof(separator) - 1U);
            found = true;
            break;
        }
    }
    assert(found);
}

static bool contains_bytes(const uint8_t *haystack,
                           size_t haystack_len,
                           const char *needle)
{
    size_t needle_len = strlen(needle);
    if (needle_len > haystack_len) return false;
    for (size_t i = 0; i <= haystack_len - needle_len; ++i) {
        if (memcmp(haystack + i, needle, needle_len) == 0) return true;
    }
    return false;
}

static uint8_t get_state(const uint8_t *body, size_t body_len)
{
    uint8_t state = 0U;
    size_t state_len = 0U;
    assert(airplay_tlv8_get(body, body_len, 0x06U, &state, sizeof(state),
                            &state_len) == AIRPLAY_TLV8_OK);
    assert(state_len == 1U);
    return state;
}

int main(void)
{
    provider_fixture_t fixture = {0};
    airplay_rtsp_pairing_config_t config = {0};
    config.pair_setup.pairing_code = "rtsp-test-code";
    config.pair_setup.accessory_identifier = ACCESSORY_ID;
    config.pair_setup.accessory_identifier_len = sizeof(ACCESSORY_ID) - 1U;
    config.pair_setup.accessory_public_key = ACCESSORY_PUBLIC_KEY;
    config.pair_setup.random = random_bytes;
    config.pair_setup.sign_accessory = sign_accessory;
    config.pair_setup.verify_controller = verify_controller;
    config.pair_setup.save_pairing = save_pairing;
    config.pair_setup.provider_context = &fixture;
    config.pair_verify.accessory_identifier = ACCESSORY_ID;
    config.pair_verify.accessory_identifier_len = sizeof(ACCESSORY_ID) - 1U;
    config.pair_verify.random = random_bytes;
    config.pair_verify.sign_accessory = sign_accessory;
    config.pair_verify.verify_controller = verify_controller;
    config.pair_verify.lookup_controller = lookup_controller;
    config.pair_verify.provider_context = &fixture;

    airplay_rtsp_pairing_t *session = NULL;
    assert(airplay_rtsp_pairing_create(&config, &session) ==
           AIRPLAY_RTSP_PAIRING_HANDLED);
    uint8_t request_body[64];
    uint8_t response[1024];
    size_t response_len = 0U;
    const uint8_t state1 = 1U;
    const airplay_tlv8_item_t state_item = {
        .type = 0x06U, .value = &state1, .value_len = 1U,
    };
    size_t request_body_len = encode_tlv(&state_item, 1U, request_body,
                                         sizeof(request_body));
    airplay_rtsp_request_t request = {0};
    memcpy(request.method, "POST", sizeof("POST"));
    memcpy(request.path, "/pair-setup", sizeof("/pair-setup"));
    memcpy(request.protocol, "RTSP/1.0", sizeof("RTSP/1.0"));
    memcpy(request.cseq, "1", sizeof("1"));
    request.body = request_body;
    request.body_len = request_body_len;
    airplay_pair_setup_result_t setup_result;
    airplay_pair_verify_result_t verify_result;
    assert(airplay_rtsp_pairing_route(
               session, &request, response, sizeof(response), &response_len,
               &setup_result, &verify_result) == AIRPLAY_RTSP_PAIRING_HANDLED);
    assert(setup_result == AIRPLAY_PAIR_SETUP_OK);
    assert(verify_result == AIRPLAY_PAIR_VERIFY_OK);
    assert(contains_bytes(response, response_len, "200 OK"));
    assert(contains_bytes(response, response_len, "application/pairing+tlv8"));
    const uint8_t *response_body = NULL;
    size_t response_body_len = 0U;
    parse_response_body(response, response_len, &response_body,
                        &response_body_len);
    assert(get_state(response_body, response_body_len) == 2U);

    request.method[0] = 'G';
    request.method[1] = 'E';
    request.method[2] = 'T';
    request.method[3] = '\0';
    response_len = 0U;
    assert(airplay_rtsp_pairing_route(
               session, &request, response, sizeof(response), &response_len,
               &setup_result, &verify_result) == AIRPLAY_RTSP_PAIRING_HANDLED);
    assert(contains_bytes(response, response_len, "405 Method Not Allowed"));

    memcpy(request.method, "POST", sizeof("POST"));
    memcpy(request.path, "/pair-verify", sizeof("/pair-verify"));
    uint8_t controller_public[32] = {9U};
    const airplay_tlv8_item_t verify_items[] = {
        {.type = 0x06U, .value = &state1, .value_len = 1U},
        {.type = 0x03U, .value = controller_public,
         .value_len = sizeof(controller_public)},
    };
    request_body_len = encode_tlv(verify_items, 2U, request_body,
                                  sizeof(request_body));
    request.body_len = request_body_len;
    response_len = 0U;
    assert(airplay_rtsp_pairing_route(
               session, &request, response, sizeof(response), &response_len,
               &setup_result, &verify_result) == AIRPLAY_RTSP_PAIRING_HANDLED);
    assert(verify_result == AIRPLAY_PAIR_VERIFY_OK);
    parse_response_body(response, response_len, &response_body,
                        &response_body_len);
    assert(get_state(response_body, response_body_len) == 2U);
    airplay_pair_verify_control_keys_t keys;
    assert(airplay_rtsp_pairing_get_control_keys(session, &keys) ==
           AIRPLAY_PAIR_VERIFY_STATE_ERROR);
    airplay_pair_verify_event_keys_t event_keys;
    assert(airplay_rtsp_pairing_get_event_keys(session, &event_keys) ==
           AIRPLAY_PAIR_VERIFY_STATE_ERROR);

    memcpy(request.path, "/info", sizeof("/info"));
    assert(airplay_rtsp_pairing_route(
               session, &request, response, sizeof(response), &response_len,
               &setup_result, &verify_result) ==
           AIRPLAY_RTSP_PAIRING_NOT_HANDLED);
    airplay_rtsp_pairing_destroy(session);
    puts("airplay RTSP pairing route host test passed");
    return 0;
}
