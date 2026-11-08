#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "iap2_carplay.h"

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static bool find_parameter(const uint8_t *body, size_t body_len, uint16_t id,
                           const uint8_t **payload, size_t *payload_len)
{
    size_t offset = 0;
    while (offset < body_len) {
        if (body_len - offset < 4U) return false;
        uint16_t length = read_be16(body + offset);
        if (length < 4U || length > body_len - offset) return false;
        if (read_be16(body + offset + 2U) == id) {
            *payload = body + offset + 4U;
            *payload_len = length - 4U;
            return true;
        }
        offset += length;
    }
    return false;
}

int main(void)
{
    const char *ips[] = {"192.168.4.1"};
    const uint8_t bssid[] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    iap2_carplay_start_session_config_t config = {
        .ssid = "relay-test",
        .passphrase = "test-passphrase",
        .channel = 6,
        .security_type = 2,
        .ip_addresses = ips,
        .ip_address_count = 1,
        .airplay_port = 7000,
        .device_identifier = "authorized-device-id",
        .public_key = "provider-public-key",
        .source_version = "1.0",
    };
    uint8_t body[1024];
    size_t body_len = 0;
    iap2_carplay_wifi_config_t wifi = {
        .ssid = "relay-test",
        .passphrase = "test-passphrase",
        .channel = 6,
        .security_type = 2,
        .bssid = bssid,
    };
    assert(iap2_carplay_build_wifi_configuration(&wifi, body, sizeof(body), &body_len));
    const uint8_t *field = NULL;
    size_t field_len = 0;
    assert(find_parameter(body, body_len, 0, &field, &field_len));
    assert(field_len == sizeof(bssid) && memcmp(field, bssid, sizeof(bssid)) == 0);
    assert(find_parameter(body, body_len, 1, &field, &field_len));
    assert(field_len == sizeof("relay-test") && memcmp(field, "relay-test", field_len) == 0);
    assert(find_parameter(body, body_len, 3, &field, &field_len));
    assert(field_len == 1 && field[0] == 2);
    assert(find_parameter(body, body_len, 4, &field, &field_len));
    assert(field_len == 1 && field[0] == 6);
    wifi.passphrase = "short";
    assert(!iap2_carplay_build_wifi_configuration(&wifi, body, sizeof(body), &body_len));
    wifi.passphrase = "test-passphrase";
    assert(!iap2_carplay_build_wifi_configuration(&wifi, body, 8, &body_len));

    assert(iap2_carplay_build_start_session(&config, body, sizeof(body), &body_len));

    const uint8_t *wireless = NULL;
    size_t wireless_len = 0;
    assert(find_parameter(body, body_len, 1, &wireless, &wireless_len));
    assert(find_parameter(wireless, wireless_len, 0, &field, &field_len));
    assert(field_len == sizeof("relay-test") && memcmp(field, "relay-test", field_len) == 0);
    assert(find_parameter(wireless, wireless_len, 2, &field, &field_len));
    assert(field_len == 1 && field[0] == 6);
    assert(find_parameter(wireless, wireless_len, 3, &field, &field_len));
    assert(field_len == sizeof("192.168.4.1") && memcmp(field, "192.168.4.1", field_len) == 0);
    assert(find_parameter(wireless, wireless_len, 4, &field, &field_len));
    assert(field_len == 1 && field[0] == 2);

    assert(find_parameter(body, body_len, 2, &field, &field_len));
    assert(field_len == 4 && read_be32(field) == 7000);
    assert(find_parameter(body, body_len, 3, &field, &field_len));
    assert(field_len == sizeof("authorized-device-id"));
    assert(find_parameter(body, body_len, 4, &field, &field_len));
    assert(field_len == sizeof("provider-public-key"));
    assert(find_parameter(body, body_len, 5, &field, &field_len));
    assert(field_len == sizeof("1.0"));

    assert(!iap2_carplay_build_start_session(&config, body, 8, &body_len));
    assert(body_len == 0);
    config.security_type = 2;
    config.passphrase = "short";
    assert(!iap2_carplay_build_start_session(&config, body, sizeof(body), &body_len));
    config.passphrase = "test-passphrase";
    config.airplay_port = 0;
    assert(!iap2_carplay_build_start_session(&config, body, sizeof(body), &body_len));
    config.airplay_port = 7000;
    config.channel = 15;
    assert(!iap2_carplay_build_start_session(&config, body, sizeof(body), &body_len));
    config.channel = 6;
    config.ip_address_count = 0;
    assert(!iap2_carplay_build_start_session(&config, body, sizeof(body), &body_len));

    puts("iap2_carplay host test passed");
    return 0;
}
