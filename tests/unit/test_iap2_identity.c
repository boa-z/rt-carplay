#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "iap2_identity.h"

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static bool list_contains(const uint8_t *list, size_t list_len, uint16_t message_id)
{
    assert((list_len & 1U) == 0U);
    for (size_t i = 0U; i < list_len; i += 2U) {
        if (read_be16(list + i) == message_id) return true;
    }
    return false;
}

static bool find_parameter(const uint8_t *body, size_t body_len, uint16_t id,
                           const uint8_t **payload, size_t *payload_len)
{
    size_t offset = 0;
    while (offset + 4 <= body_len) {
        uint16_t len = read_be16(body + offset);
        if (len < 4 || offset + len > body_len) return false;
        if (read_be16(body + offset + 2) == id) {
            *payload = body + offset + 4;
            *payload_len = len - 4;
            return true;
        }
        offset += len;
    }
    return false;
}

int main(void)
{
    iap2_identity_config_t config = {
        .name = "ESP32 CarPlay",
        .model_identifier = "ESP32-CP-WEB",
        .manufacturer = "Open Relay",
        .serial_number = "test-serial",
        .firmware_version = "0.1",
        .hardware_version = "ESP32",
        .language = "en",
        .wifi_ssid = "relay-test",
        .bluetooth_address = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55},
    };
    uint8_t body[1024];
    size_t body_len = 0;
    assert(iap2_identity_build_wireless(&config, body, sizeof(body), &body_len));
    assert(body_len > 0 && body_len < sizeof(body));

    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    assert(find_parameter(body, body_len, 0, &payload, &payload_len));
    assert(payload_len == sizeof("ESP32 CarPlay"));
    assert(payload[payload_len - 1] == 0);
    assert(find_parameter(body, body_len, 6, &payload, &payload_len));
    assert(payload_len == 28 && read_be16(payload) == 0xaa01 &&
           read_be16(payload + 24) == 0x4301 && read_be16(payload + 26) == 0x5703);
    assert(find_parameter(body, body_len, 7, &payload, &payload_len));
    assert(payload_len == 28 && read_be16(payload) == 0xaa00 &&
           read_be16(payload + 20) == 0x4300 && read_be16(payload + 22) == 0x4e0d &&
           read_be16(payload + 24) == 0x4e0e && read_be16(payload + 26) == 0x5702);
    assert(find_parameter(body, body_len, 17, &payload, &payload_len));
    const uint8_t *mac = NULL;
    size_t mac_len = 0;
    assert(find_parameter(payload, payload_len, 3, &mac, &mac_len));
    assert(mac_len == 6 && mac[0] == 0x02 && mac[5] == 0x55);
    assert(find_parameter(body, body_len, 24, &payload, &payload_len));
    const uint8_t *ssid = NULL;
    size_t ssid_len = 0;
    assert(find_parameter(payload, payload_len, 1, &ssid, &ssid_len));
    assert(ssid_len == sizeof("relay-test") && ssid[ssid_len - 1] == 0);

    uint8_t rejected[] = {
        0, 8, 0, 6, 0x43, 0x01, 0x43, 0x00,
        0, 6, 0, 7, 0x43, 0x01,
    };
    uint16_t omitted[IAP2_IDENTITY_MAX_OMITTED_MESSAGES];
    size_t omitted_count = 0U;
    assert(iap2_identity_parse_unsupported_messages(rejected, sizeof(rejected), omitted,
                                                    IAP2_IDENTITY_MAX_OMITTED_MESSAGES,
                                                    &omitted_count));
    assert(omitted_count == 2 && omitted[0] == 0x4301 && omitted[1] == 0x4300);
    assert(iap2_identity_build_wireless_omitting(&config, omitted, omitted_count,
                                                 body, sizeof(body), &body_len));
    assert(find_parameter(body, body_len, 6, &payload, &payload_len));
    assert(!list_contains(payload, payload_len, 0x4301));
    assert(list_contains(payload, payload_len, 0x5703));
    assert(find_parameter(body, body_len, 7, &payload, &payload_len));
    assert(!list_contains(payload, payload_len, 0x4300));
    assert(list_contains(payload, payload_len, 0x5702));

    const uint8_t auth_rejection[] = {0, 6, 0, 6, 0xaa, 0x02};
    const uint8_t unrelated_rejection[] = {0, 4, 0, 3};
    const uint8_t malformed_rejection[] = {0, 5, 0, 6, 0x43};
    assert(!iap2_identity_parse_unsupported_messages(auth_rejection, sizeof(auth_rejection),
                                                     omitted, IAP2_IDENTITY_MAX_OMITTED_MESSAGES,
                                                     &omitted_count));
    assert(!iap2_identity_parse_unsupported_messages(unrelated_rejection, sizeof(unrelated_rejection),
                                                     omitted, IAP2_IDENTITY_MAX_OMITTED_MESSAGES,
                                                     &omitted_count));
    assert(!iap2_identity_parse_unsupported_messages(malformed_rejection, sizeof(malformed_rejection),
                                                     omitted, IAP2_IDENTITY_MAX_OMITTED_MESSAGES,
                                                     &omitted_count));

    assert(!iap2_identity_build_wireless(&config, body, 8, &body_len));

    /* Wired USB-host profile: no SSID requirement, USBHostTransportComponent
     * (group 16), power capability 2 and the wired message lists. */
    iap2_identity_config_t wired_config = config;
    wired_config.wifi_ssid = NULL;
    assert(!iap2_identity_build_wireless(&wired_config, body, sizeof(body),
                                         &body_len));
    assert(iap2_identity_build_wired(&wired_config, 3U, body, sizeof(body),
                                     &body_len));
    assert(find_parameter(body, body_len, 8, &payload, &payload_len));
    assert(payload_len == 1U && payload[0] == 2U);
    assert(find_parameter(body, body_len, 6, &payload, &payload_len));
    assert(payload_len == 28 && read_be16(payload) == 0xaa01 &&
           list_contains(payload, payload_len, 0xae03) &&
           !list_contains(payload, payload_len, 0x5703) &&
           read_be16(payload + 26) == 0x4301);
    assert(find_parameter(body, body_len, 7, &payload, &payload_len));
    assert(payload_len == 22 && read_be16(payload) == 0xaa00 &&
           list_contains(payload, payload_len, 0x4300) &&
           !list_contains(payload, payload_len, 0x4e0d));

    const uint8_t *group = NULL;
    size_t group_len = 0U;
    assert(find_parameter(body, body_len, 16, &group, &group_len));
    assert(find_parameter(group, group_len, 0, &payload, &payload_len));
    assert(payload_len == 2U && read_be16(payload) == 0U);
    assert(find_parameter(group, group_len, 1, &payload, &payload_len));
    assert(payload_len == sizeof("USBHostTransport"));
    assert(memcmp(payload, "USBHostTransport", payload_len) == 0);
    assert(find_parameter(group, group_len, 2, &payload, &payload_len));
    assert(payload_len == 0U);
    assert(find_parameter(group, group_len, 3, &payload, &payload_len));
    assert(payload_len == 1U && payload[0] == 3U);
    assert(find_parameter(group, group_len, 4, &payload, &payload_len));
    assert(payload_len == 0U);
    assert(!find_parameter(body, body_len, 17, &payload, &payload_len));
    assert(!find_parameter(body, body_len, 24, &payload, &payload_len));
    assert(find_parameter(body, body_len, 30, &payload, &payload_len));

    /* The negative parse checks above cleared the count; re-parse. */
    assert(iap2_identity_parse_unsupported_messages(rejected, sizeof(rejected),
                                                    omitted,
                                                    IAP2_IDENTITY_MAX_OMITTED_MESSAGES,
                                                    &omitted_count));
    assert(omitted_count == 2U);
    assert(iap2_identity_build_wired_omitting(&wired_config, 3U, omitted,
                                              omitted_count, body, sizeof(body),
                                              &body_len));
    assert(find_parameter(body, body_len, 6, &payload, &payload_len));
    assert(!list_contains(payload, payload_len, 0x4301));
    assert(list_contains(payload, payload_len, 0xae03));
    assert(find_parameter(body, body_len, 7, &payload, &payload_len));
    assert(!list_contains(payload, payload_len, 0x4300));
    assert(find_parameter(body, body_len, 16, &group, &group_len));

    puts("iap2_identity host test passed");
    return 0;
}
