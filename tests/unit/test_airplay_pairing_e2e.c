/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * End-to-end pairing test.
 *
 * A real controller implementation drives airplay_rtsp_server over its TCP
 * socket: Pair-Setup (SRP-6a, Ed25519), Pair-Verify (X25519, Ed25519) and
 * then an RTSP request encrypted with the ControlCipher keys.  The module
 * tests cover each state machine with fake signatures; this one proves the
 * server's socket framing, cipher transition and key derivation agree with an
 * independent implementation of the controller side.
 */
#include "airplay_rtsp_server.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airplay_bplist.h"
#include "airplay_control_cipher.h"
#include "airplay_ed25519.h"
#include "airplay_info_plist.h"
#include "airplay_screen.h"
#include "airplay_srp6a.h"
#include "airplay_srp6a_test_client.h"
#include "airplay_tlv8.h"
#include "mbedtls/chachapoly.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/ecp.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"
#include "rtcp/rtcp.h"
#include "rtcp_linux.h"

#define WAIT_LIMIT_MS 5000U
#define PAIRING_CODE "3939"
#define CONTROLLER_ID "rt-carplay-e2e-controller"
#define ACCESSORY_ID "02:00:00:00:00:01"
#define KEY_BYTES 32U
#define PAIRING_TYPE "application/pairing+tlv8"
#define PLIST_TYPE "application/x-apple-binary-plist"
#define INFO_PLIST_BYTES AIRPLAY_RTSP_SESSION_INFO_MAX_BYTES

/* Ed25519 seeds are test fixtures, not identities. */
static const uint8_t ACCESSORY_SEED[KEY_BYTES] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
};
static const uint8_t CONTROLLER_SEED[KEY_BYTES] = {
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
};

typedef struct {
    uint8_t counter;
} rng_t;

typedef struct {
    airplay_ed25519_keypair_t accessory;
    airplay_ed25519_keypair_t controller;
    uint8_t paired_identifier[AIRPLAY_PAIR_SETUP_MAX_IDENTIFIER_BYTES];
    size_t paired_identifier_len;
    uint8_t paired_public_key[KEY_BYTES];
    bool has_pairing;
    rng_t rng;
} fixture_t;

static unsigned screen_messages;
static unsigned screen_frames;
static uint8_t screen_frame[256];
static size_t screen_frame_len;
static uint64_t screen_sender_time_ns;

static int capture_screen(airplay_screen_message_type_t type, uint8_t *annex_b,
                          size_t annex_b_len, size_t prefix_capacity,
                          uint64_t sender_time_ns, uint64_t arrival_time_us,
                          void *context)
{
    (void)prefix_capacity;
    (void)arrival_time_us;
    (void)context;
    ++screen_messages;
    if (type == AIRPLAY_SCREEN_MESSAGE_FRAME_H264) {
        assert(annex_b_len <= sizeof(screen_frame));
        memcpy(screen_frame, annex_b, annex_b_len);
        screen_frame_len = annex_b_len;
        screen_sender_time_ns = sender_time_ns;
        ++screen_frames;
    }
    return RTCP_OK;
}

/* ------------------------------------------------------------------ */
/* Providers                                                          */
/* ------------------------------------------------------------------ */

static int mbed_rng(void *context, unsigned char *output, size_t len)
{
    rng_t *rng = context;
    for (size_t i = 0U; i < len; ++i) output[i] = rng->counter++;
    return 0;
}

static bool provider_random(uint8_t *output, size_t len, void *context)
{
    fixture_t *fixture = context;
    return mbed_rng(&fixture->rng, output, len) == 0;
}

static bool provider_sign(const uint8_t *message, size_t message_len,
                          uint8_t signature[64], void *context)
{
    fixture_t *fixture = context;
    return airplay_ed25519_sign(&fixture->accessory, message, message_len,
                                signature);
}

static bool provider_verify(const uint8_t public_key[32],
                            const uint8_t *message, size_t message_len,
                            const uint8_t signature[64], void *context)
{
    (void)context;
    return airplay_ed25519_verify(public_key, message, message_len, signature);
}

static bool provider_save(const uint8_t *identifier, size_t identifier_len,
                          const uint8_t public_key[32], void *context)
{
    fixture_t *fixture = context;
    if (identifier_len == 0U ||
        identifier_len > sizeof(fixture->paired_identifier)) {
        return false;
    }
    memcpy(fixture->paired_identifier, identifier, identifier_len);
    fixture->paired_identifier_len = identifier_len;
    memcpy(fixture->paired_public_key, public_key, KEY_BYTES);
    fixture->has_pairing = true;
    return true;
}

static bool provider_lookup(const uint8_t *identifier, size_t identifier_len,
                            uint8_t public_key[32], void *context)
{
    fixture_t *fixture = context;
    if (!fixture->has_pairing ||
        identifier_len != fixture->paired_identifier_len ||
        memcmp(identifier, fixture->paired_identifier, identifier_len) != 0) {
        return false;
    }
    memcpy(public_key, fixture->paired_public_key, KEY_BYTES);
    return true;
}

static void fill_pairing(fixture_t *fixture,
                         airplay_rtsp_pairing_config_t *pairing)
{
    memset(pairing, 0, sizeof(*pairing));
    pairing->pair_setup.pairing_code = PAIRING_CODE;
    pairing->pair_setup.accessory_identifier = (const uint8_t *)ACCESSORY_ID;
    pairing->pair_setup.accessory_identifier_len = sizeof(ACCESSORY_ID) - 1U;
    pairing->pair_setup.accessory_public_key = fixture->accessory.public_key;
    pairing->pair_setup.random = provider_random;
    pairing->pair_setup.sign_accessory = provider_sign;
    pairing->pair_setup.verify_controller = provider_verify;
    pairing->pair_setup.save_pairing = provider_save;
    pairing->pair_setup.provider_context = fixture;

    pairing->pair_verify.accessory_identifier = (const uint8_t *)ACCESSORY_ID;
    pairing->pair_verify.accessory_identifier_len = sizeof(ACCESSORY_ID) - 1U;
    pairing->pair_verify.random = provider_random;
    pairing->pair_verify.sign_accessory = provider_sign;
    pairing->pair_verify.verify_controller = provider_verify;
    pairing->pair_verify.lookup_controller = provider_lookup;
    pairing->pair_verify.provider_context = fixture;
}

/* ------------------------------------------------------------------ */
/* Crypto helpers (controller side)                                   */
/* ------------------------------------------------------------------ */

static void nonce_for(const char label[8], uint8_t nonce[12])
{
    memset(nonce, 0, 12U);
    memcpy(nonce + 4U, label, 8U);
}

static void hkdf_key(const uint8_t *input, size_t input_len, const char *salt,
                     const char *info, uint8_t output[KEY_BYTES])
{
    const mbedtls_md_info_t *sha512 =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA512);
    assert(sha512 != NULL);
    assert(mbedtls_hkdf(sha512, (const uint8_t *)salt, strlen(salt), input,
                        input_len, (const uint8_t *)info, strlen(info), output,
                        KEY_BYTES) == 0);
}

static size_t chacha_encrypt(const uint8_t key[KEY_BYTES], const char label[8],
                             const uint8_t *plaintext, size_t plaintext_len,
                             uint8_t *output, size_t capacity)
{
    uint8_t nonce[12];
    nonce_for(label, nonce);
    assert(plaintext_len + 16U <= capacity);
    mbedtls_chachapoly_context context;
    mbedtls_chachapoly_init(&context);
    assert(mbedtls_chachapoly_setkey(&context, key) == 0);
    assert(mbedtls_chachapoly_encrypt_and_tag(
               &context, plaintext_len, nonce, NULL, 0U, plaintext, output,
               output + plaintext_len) == 0);
    mbedtls_chachapoly_free(&context);
    mbedtls_platform_zeroize(nonce, sizeof(nonce));
    return plaintext_len + 16U;
}

static size_t chacha_decrypt(const uint8_t key[KEY_BYTES], const char label[8],
                             const uint8_t *input, size_t input_len,
                             uint8_t *plaintext, size_t capacity)
{
    uint8_t nonce[12];
    nonce_for(label, nonce);
    assert(input_len >= 16U && input_len - 16U <= capacity);
    size_t ciphertext_len = input_len - 16U;
    mbedtls_chachapoly_context context;
    mbedtls_chachapoly_init(&context);
    assert(mbedtls_chachapoly_setkey(&context, key) == 0);
    assert(mbedtls_chachapoly_auth_decrypt(
               &context, ciphertext_len, nonce, NULL, 0U,
               input + ciphertext_len, input, plaintext) == 0);
    mbedtls_chachapoly_free(&context);
    mbedtls_platform_zeroize(nonce, sizeof(nonce));
    return ciphertext_len;
}

/* ------------------------------------------------------------------ */
/* TLV helpers                                                        */
/* ------------------------------------------------------------------ */

static size_t tlv_encode(const airplay_tlv8_item_t *items, size_t count,
                         uint8_t *output, size_t capacity)
{
    size_t written = 0U;
    assert(airplay_tlv8_encode(items, count, output, capacity, &written) ==
           AIRPLAY_TLV8_OK);
    return written;
}

static size_t tlv_get(const uint8_t *input, size_t input_len, uint8_t type,
                      uint8_t *output, size_t capacity)
{
    size_t written = 0U;
    assert(airplay_tlv8_get(input, input_len, type, output, capacity,
                            &written) == AIRPLAY_TLV8_OK);
    return written;
}

static uint8_t tlv_state(const uint8_t *body, size_t body_len)
{
    uint8_t state = 0U;
    assert(tlv_get(body, body_len, 0x06U, &state, sizeof(state)) == 1U);
    return state;
}

/* ------------------------------------------------------------------ */
/* RTSP client                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    int fd;
    uint8_t raw[4096];
    uint8_t plain[32768];
    size_t plain_len;
    airplay_control_cipher_t *cipher;
} client_t;

typedef struct {
    const uint8_t *headers;
    size_t headers_len;
    const uint8_t *body;
    size_t body_len;
} response_t;

static void client_send(client_t *client, const uint8_t *bytes, size_t len)
{
    size_t offset = 0U;
    while (offset < len) {
        int sent = rtcp_net_linux.send(client->fd, bytes + offset,
                                       len - offset);
        assert(sent > 0);
        offset += (size_t)sent;
    }
}

static bool client_has_data(int fd)
{
    struct rtcp_pollfd pfd = {.fd = fd, .events = RTCP_POLLIN};
    int ready = rtcp_net_linux.poll(&pfd, 1, 0);
    return ready > 0 && (pfd.revents & RTCP_POLLIN) != 0;
}

static bool contains(const uint8_t *bytes, size_t len, const char *needle)
{
    size_t needle_len = strlen(needle);
    if (needle_len > len) return false;
    for (size_t i = 0U; i <= len - needle_len; ++i) {
        if (memcmp(bytes + i, needle, needle_len) == 0) return true;
    }
    return false;
}

static void client_pump(airplay_rtsp_server_t *server, client_t *client)
{
    (void)airplay_rtsp_server_poll(server, 5);
    while (client_has_data(client->fd)) {
        int received = rtcp_net_linux.recv(client->fd, client->raw,
                                           sizeof(client->raw));
        assert(received > 0);
        if (client->cipher == NULL) {
            assert(client->plain_len + (size_t)received <=
                   sizeof(client->plain));
            memcpy(client->plain + client->plain_len, client->raw,
                   (size_t)received);
            client->plain_len += (size_t)received;
            continue;
        }
        size_t offset = 0U;
        while (offset < (size_t)received) {
            size_t consumed = 0U;
            size_t plaintext_len = 0U;
            airplay_control_cipher_result_t result =
                airplay_control_cipher_decrypt(
                    client->cipher, client->raw + offset,
                    (size_t)received - offset, &consumed,
                    client->plain + client->plain_len,
                    sizeof(client->plain) - client->plain_len,
                    &plaintext_len);
            offset += consumed;
            client->plain_len += plaintext_len;
            if (result == AIRPLAY_CONTROL_CIPHER_INCOMPLETE) break;
            if (result != AIRPLAY_CONTROL_CIPHER_OK) {
                assert(0 && "server response failed to decrypt");
            }
            if (consumed == 0U && plaintext_len == 0U) break;
        }
    }
}

static bool parse_response(const client_t *client, response_t *response)
{
    static const char separator[] = "\r\n\r\n";
    size_t separator_at = 0U;
    bool found = false;
    for (size_t i = 0U; i + sizeof(separator) - 1U <= client->plain_len; ++i) {
        if (memcmp(client->plain + i, separator, sizeof(separator) - 1U) == 0) {
            separator_at = i;
            found = true;
            break;
        }
    }
    if (!found) return false;
    size_t headers_len = separator_at + sizeof(separator) - 1U;
    size_t content_length = 0U;
    size_t cursor = 0U;
    while (cursor < separator_at) {
        size_t line_end = cursor;
        while (line_end < separator_at &&
               !(client->plain[line_end] == '\r' &&
                 client->plain[line_end + 1U] == '\n')) {
            ++line_end;
        }
        static const char length_header[] = "Content-Length:";
        size_t length_header_len = sizeof(length_header) - 1U;
        if (line_end - cursor >= length_header_len &&
            memcmp(client->plain + cursor, length_header,
                   length_header_len) == 0) {
            size_t value_at = cursor + length_header_len;
            while (value_at < line_end && client->plain[value_at] == ' ') {
                ++value_at;
            }
            while (value_at < line_end &&
                   client->plain[value_at] >= '0' &&
                   client->plain[value_at] <= '9') {
                content_length =
                    content_length * 10U + (size_t)(client->plain[value_at] - '0');
                ++value_at;
            }
        }
        cursor = line_end + 2U;
    }
    if (client->plain_len < headers_len + content_length) return false;
    response->headers = client->plain;
    response->headers_len = headers_len;
    response->body = client->plain + headers_len;
    response->body_len = content_length;
    return true;
}

static size_t build_request(uint8_t *output, size_t capacity,
                            const char *method, const char *path,
                            const char *content_type, uint32_t cseq,
                            const uint8_t *body, size_t body_len)
{
    int written = snprintf(
        (char *)output, capacity,
        "%s %s RTSP/1.0\r\n"
        "CSeq: %u\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %u\r\n\r\n",
        method, path, (unsigned)cseq, content_type, (unsigned)body_len);
    assert(written > 0 && (size_t)written + body_len <= capacity);
    if (body_len != 0U) memcpy(output + written, body, body_len);
    return (size_t)written + body_len;
}

static void client_exchange(airplay_rtsp_server_t *server, client_t *client,
                            const uint8_t *request, size_t request_len,
                            response_t *response)
{
    client->plain_len = 0U;
    if (client->cipher == NULL) {
        client_send(client, request, request_len);
    } else {
        uint8_t encrypted[8192];
        size_t encrypted_len = 0U;
        assert(airplay_control_cipher_encrypt(client->cipher, request,
                                              request_len, encrypted,
                                              sizeof(encrypted),
                                              &encrypted_len) ==
               AIRPLAY_CONTROL_CIPHER_OK);
        client_send(client, encrypted, encrypted_len);
        mbedtls_platform_zeroize(encrypted, sizeof(encrypted));
    }
    uint64_t start = rtcp_os_linux.now_ms();
    while (!parse_response(client, response)) {
        assert(rtcp_os_linux.now_ms() - start < WAIT_LIMIT_MS);
        client_pump(server, client);
    }
    assert(contains(response->headers, response->headers_len, "200 OK"));
}

static void client_connect(airplay_rtsp_server_t *server, client_t *client)
{
    uint16_t port = airplay_rtsp_server_get_port(server);
    assert(port != 0U);
    rtcp_addr_t address = {0};
    address.family = RTCP_AF_INET;
    address.addr[0] = 127U;
    address.addr[3] = 1U;
    address.port = port;
    memset(client, 0, sizeof(*client));
    client->fd = rtcp_net_linux.connect(&address);
    assert(client->fd >= 0);

    uint64_t start = rtcp_os_linux.now_ms();
    while (rtcp_os_linux.now_ms() - start < WAIT_LIMIT_MS &&
           !airplay_rtsp_server_is_connected(server)) {
        airplay_rtsp_server_poll(server, 10);
    }
    assert(airplay_rtsp_server_is_connected(server));
    assert(!airplay_rtsp_server_is_encrypted(server));
}

/* ------------------------------------------------------------------ */
/* Pair-Setup                                                         */
/* ------------------------------------------------------------------ */

static void run_pair_setup(airplay_rtsp_server_t *server, client_t *client,
                           fixture_t *fixture)
{
    uint8_t request[4096];
    uint8_t body[1024];
    response_t response;
    uint32_t cseq = 1U;

    /* M1: start SRP. */
    uint8_t state1 = 1U;
    const airplay_tlv8_item_t m1_items[] = {
        {.type = 0x06U, .value = &state1, .value_len = 1U},
    };
    size_t body_len = tlv_encode(m1_items, 1U, body, sizeof(body));
    size_t request_len = build_request(request, sizeof(request), "POST",
                                       "/pair-setup", PAIRING_TYPE, cseq++, body,
                                       body_len);
    client_exchange(server, client, request, request_len, &response);
    assert(tlv_state(response.body, response.body_len) == 2U);

    uint8_t salt[AIRPLAY_SRP6A_SALT_BYTES];
    uint8_t public_b[AIRPLAY_SRP6A_PUBLIC_BYTES];
    assert(tlv_get(response.body, response.body_len, 0x02U, salt,
                   sizeof(salt)) == sizeof(salt));
    assert(tlv_get(response.body, response.body_len, 0x03U, public_b,
                   sizeof(public_b)) == sizeof(public_b));

    /* M3: SRP client proof. */
    uint8_t public_a[AIRPLAY_SRP6A_PUBLIC_BYTES];
    uint8_t proof_m1[AIRPLAY_SRP6A_PROOF_BYTES];
    uint8_t expected_m2[AIRPLAY_SRP6A_PROOF_BYTES];
    uint8_t session_key[AIRPLAY_SRP6A_SESSION_KEY_BYTES];
    airplay_srp6a_test_compute_client_proofs(
        salt, public_b, "Pair-Setup", PAIRING_CODE, public_a, proof_m1,
        expected_m2, session_key);

    uint8_t state3 = 3U;
    const airplay_tlv8_item_t m3_items[] = {
        {.type = 0x06U, .value = &state3, .value_len = 1U},
        {.type = 0x03U, .value = public_a, .value_len = sizeof(public_a)},
        {.type = 0x04U, .value = proof_m1, .value_len = sizeof(proof_m1)},
    };
    body_len = tlv_encode(m3_items, 3U, body, sizeof(body));
    request_len = build_request(request, sizeof(request), "POST",
                                "/pair-setup", PAIRING_TYPE, cseq++, body,
                                body_len);
    client_exchange(server, client, request, request_len, &response);
    assert(tlv_state(response.body, response.body_len) == 4U);
    uint8_t proof_m2[AIRPLAY_SRP6A_PROOF_BYTES];
    assert(tlv_get(response.body, response.body_len, 0x04U, proof_m2,
                   sizeof(proof_m2)) == sizeof(proof_m2));
    assert(memcmp(proof_m2, expected_m2, sizeof(proof_m2)) == 0);
    assert(!airplay_rtsp_server_is_encrypted(server));

    /* M5: encrypted controller identity. */
    uint8_t encryption_key[KEY_BYTES];
    uint8_t signing_key[KEY_BYTES];
    hkdf_key(session_key, sizeof(session_key), "Pair-Setup-Encrypt-Salt",
             "Pair-Setup-Encrypt-Info", encryption_key);
    hkdf_key(session_key, sizeof(session_key),
             "Pair-Setup-Controller-Sign-Salt",
             "Pair-Setup-Controller-Sign-Info", signing_key);

    uint8_t signature_input[KEY_BYTES * 3U];
    memcpy(signature_input, signing_key, KEY_BYTES);
    memcpy(signature_input + KEY_BYTES, CONTROLLER_ID,
           sizeof(CONTROLLER_ID) - 1U);
    memcpy(signature_input + KEY_BYTES + sizeof(CONTROLLER_ID) - 1U,
           fixture->controller.public_key, KEY_BYTES);
    size_t signature_input_len = KEY_BYTES + sizeof(CONTROLLER_ID) - 1U +
                                 KEY_BYTES;
    uint8_t controller_signature[64];
    assert(airplay_ed25519_sign(&fixture->controller, signature_input,
                                signature_input_len, controller_signature));

    const airplay_tlv8_item_t controller_items[] = {
        {.type = 0x01U, .value = (const uint8_t *)CONTROLLER_ID,
         .value_len = sizeof(CONTROLLER_ID) - 1U},
        {.type = 0x03U, .value = fixture->controller.public_key,
         .value_len = KEY_BYTES},
        {.type = 0x0aU, .value = controller_signature,
         .value_len = sizeof(controller_signature)},
    };
    uint8_t submessage[512];
    size_t submessage_len = tlv_encode(controller_items, 3U, submessage,
                                       sizeof(submessage));
    uint8_t encrypted[sizeof(submessage) + 16U];
    size_t encrypted_len = chacha_encrypt(encryption_key, "PS-Msg05",
                                          submessage, submessage_len,
                                          encrypted, sizeof(encrypted));

    uint8_t state5 = 5U;
    const airplay_tlv8_item_t m5_items[] = {
        {.type = 0x06U, .value = &state5, .value_len = 1U},
        {.type = 0x05U, .value = encrypted, .value_len = encrypted_len},
    };
    body_len = tlv_encode(m5_items, 2U, body, sizeof(body));
    request_len = build_request(request, sizeof(request), "POST",
                                "/pair-setup", PAIRING_TYPE, cseq++, body,
                                body_len);
    client_exchange(server, client, request, request_len, &response);
    assert(tlv_state(response.body, response.body_len) == 6U);

    uint8_t response_encrypted[sizeof(submessage) + 16U];
    size_t response_encrypted_len =
        tlv_get(response.body, response.body_len, 0x05U, response_encrypted,
                sizeof(response_encrypted));
    assert(response_encrypted_len > 16U);
    uint8_t response_plaintext[512];
    size_t response_plaintext_len = chacha_decrypt(
        encryption_key, "PS-Msg06", response_encrypted,
        response_encrypted_len, response_plaintext,
        sizeof(response_plaintext));

    uint8_t accessory_id[AIRPLAY_PAIR_SETUP_MAX_IDENTIFIER_BYTES];
    uint8_t accessory_key[KEY_BYTES];
    uint8_t accessory_signature[64];
    size_t accessory_id_len = tlv_get(response_plaintext,
                                      response_plaintext_len, 0x01U,
                                      accessory_id, sizeof(accessory_id));
    assert(accessory_id_len == sizeof(ACCESSORY_ID) - 1U);
    assert(memcmp(accessory_id, ACCESSORY_ID, accessory_id_len) == 0);
    assert(tlv_get(response_plaintext, response_plaintext_len, 0x03U,
                   accessory_key, sizeof(accessory_key)) == KEY_BYTES);
    assert(memcmp(accessory_key, fixture->accessory.public_key, KEY_BYTES) ==
           0);
    assert(tlv_get(response_plaintext, response_plaintext_len, 0x0aU,
                   accessory_signature, sizeof(accessory_signature)) ==
           sizeof(accessory_signature));

    uint8_t accessory_signing_key[KEY_BYTES];
    hkdf_key(session_key, sizeof(session_key),
             "Pair-Setup-Accessory-Sign-Salt",
             "Pair-Setup-Accessory-Sign-Info", accessory_signing_key);
    uint8_t accessory_signature_input[KEY_BYTES * 3U];
    memcpy(accessory_signature_input, accessory_signing_key, KEY_BYTES);
    memcpy(accessory_signature_input + KEY_BYTES, accessory_id,
           accessory_id_len);
    memcpy(accessory_signature_input + KEY_BYTES + accessory_id_len,
           accessory_key, KEY_BYTES);
    assert(airplay_ed25519_verify(fixture->accessory.public_key,
                                  accessory_signature_input,
                                  KEY_BYTES + accessory_id_len + KEY_BYTES,
                                  accessory_signature));

    assert(fixture->has_pairing);
    assert(fixture->paired_identifier_len == sizeof(CONTROLLER_ID) - 1U);
    assert(memcmp(fixture->paired_identifier, CONTROLLER_ID,
                  fixture->paired_identifier_len) == 0);
    assert(memcmp(fixture->paired_public_key, fixture->controller.public_key,
                  KEY_BYTES) == 0);

    mbedtls_platform_zeroize(session_key, sizeof(session_key));
    mbedtls_platform_zeroize(encryption_key, sizeof(encryption_key));
    mbedtls_platform_zeroize(signing_key, sizeof(signing_key));
    mbedtls_platform_zeroize(signature_input, sizeof(signature_input));
    mbedtls_platform_zeroize(controller_signature,
                             sizeof(controller_signature));
    mbedtls_platform_zeroize(submessage, sizeof(submessage));
    mbedtls_platform_zeroize(encrypted, sizeof(encrypted));
    mbedtls_platform_zeroize(response_encrypted,
                             sizeof(response_encrypted));
    mbedtls_platform_zeroize(response_plaintext,
                             sizeof(response_plaintext));
    mbedtls_platform_zeroize(accessory_signing_key,
                             sizeof(accessory_signing_key));
    mbedtls_platform_zeroize(accessory_signature_input,
                             sizeof(accessory_signature_input));
}

/* ------------------------------------------------------------------ */
/* Pair-Verify                                                        */
/* ------------------------------------------------------------------ */

static void run_pair_verify(airplay_rtsp_server_t *server, client_t *client,
                            fixture_t *fixture, uint8_t shared_out[KEY_BYTES])
{
    uint8_t request[1024];
    uint8_t body[256];
    response_t response;
    uint32_t cseq = 10U;

    mbedtls_ecp_group group;
    mbedtls_mpi private_key;
    mbedtls_mpi shared_secret;
    mbedtls_ecp_point public_key;
    mbedtls_ecp_group_init(&group);
    mbedtls_mpi_init(&private_key);
    mbedtls_mpi_init(&shared_secret);
    mbedtls_ecp_point_init(&public_key);
    assert(mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_CURVE25519) == 0);

    uint8_t controller_public[KEY_BYTES];
    size_t public_len = 0U;
    rng_t client_rng = {.counter = 0x40};
    assert(mbedtls_ecp_gen_keypair(&group, &private_key, &public_key,
                                   mbed_rng, &client_rng) == 0);
    assert(mbedtls_ecp_point_write_binary(
               &group, &public_key, MBEDTLS_ECP_PF_UNCOMPRESSED, &public_len,
               controller_public, sizeof(controller_public)) == 0);
    assert(public_len == KEY_BYTES);

    /* M1: controller ephemeral key. */
    uint8_t state1 = 1U;
    const airplay_tlv8_item_t m1_items[] = {
        {.type = 0x06U, .value = &state1, .value_len = 1U},
        {.type = 0x03U, .value = controller_public,
         .value_len = sizeof(controller_public)},
    };
    size_t body_len = tlv_encode(m1_items, 2U, body, sizeof(body));
    size_t request_len = build_request(request, sizeof(request), "POST",
                                       "/pair-verify", PAIRING_TYPE, cseq++, body,
                                       body_len);
    client_exchange(server, client, request, request_len, &response);
    assert(tlv_state(response.body, response.body_len) == 2U);
    uint8_t accessory_public[KEY_BYTES];
    assert(tlv_get(response.body, response.body_len, 0x03U, accessory_public,
                   sizeof(accessory_public)) == KEY_BYTES);
    uint8_t encrypted[512];
    size_t encrypted_len = tlv_get(response.body, response.body_len, 0x05U,
                                   encrypted, sizeof(encrypted));

    /* Shared secret + Pair-Verify encryption key. */
    mbedtls_ecp_point accessory_point;
    mbedtls_ecp_point_init(&accessory_point);
    assert(mbedtls_ecp_point_read_binary(&group, &accessory_point,
                                         accessory_public,
                                         sizeof(accessory_public)) == 0);
    assert(mbedtls_ecdh_compute_shared(&group, &shared_secret,
                                       &accessory_point, &private_key,
                                       mbed_rng, &client_rng) == 0);
    uint8_t shared[KEY_BYTES];
    assert(mbedtls_mpi_write_binary_le(&shared_secret, shared,
                                       sizeof(shared)) == 0);
    memcpy(shared_out, shared, KEY_BYTES);
    uint8_t verify_key[KEY_BYTES];
    hkdf_key(shared, sizeof(shared), "Pair-Verify-Encrypt-Salt",
             "Pair-Verify-Encrypt-Info", verify_key);

    uint8_t submessage[256];
    size_t submessage_len = chacha_decrypt(verify_key, "PV-Msg02", encrypted,
                                           encrypted_len, submessage,
                                           sizeof(submessage));
    uint8_t accessory_id[AIRPLAY_PAIR_SETUP_MAX_IDENTIFIER_BYTES];
    uint8_t accessory_signature[64];
    size_t accessory_id_len = tlv_get(submessage, submessage_len, 0x01U,
                                      accessory_id, sizeof(accessory_id));
    assert(accessory_id_len == sizeof(ACCESSORY_ID) - 1U);
    assert(memcmp(accessory_id, ACCESSORY_ID, accessory_id_len) == 0);
    assert(tlv_get(submessage, submessage_len, 0x0aU, accessory_signature,
                   sizeof(accessory_signature)) ==
           sizeof(accessory_signature));

    uint8_t accessory_signature_input[KEY_BYTES * 3U];
    memcpy(accessory_signature_input, accessory_public, KEY_BYTES);
    memcpy(accessory_signature_input + KEY_BYTES, accessory_id,
           accessory_id_len);
    memcpy(accessory_signature_input + KEY_BYTES + accessory_id_len,
           controller_public, KEY_BYTES);
    assert(airplay_ed25519_verify(
        fixture->accessory.public_key, accessory_signature_input,
        KEY_BYTES + accessory_id_len + KEY_BYTES, accessory_signature));

    /* M3: controller signature over both ephemeral keys. */
    uint8_t controller_signature_input[KEY_BYTES * 3U];
    memcpy(controller_signature_input, controller_public, KEY_BYTES);
    memcpy(controller_signature_input + KEY_BYTES, CONTROLLER_ID,
           sizeof(CONTROLLER_ID) - 1U);
    memcpy(controller_signature_input + KEY_BYTES +
               sizeof(CONTROLLER_ID) - 1U,
           accessory_public, KEY_BYTES);
    uint8_t controller_signature[64];
    assert(airplay_ed25519_sign(
        &fixture->controller, controller_signature_input,
        KEY_BYTES + sizeof(CONTROLLER_ID) - 1U + KEY_BYTES,
        controller_signature));

    const airplay_tlv8_item_t controller_items[] = {
        {.type = 0x01U, .value = (const uint8_t *)CONTROLLER_ID,
         .value_len = sizeof(CONTROLLER_ID) - 1U},
        {.type = 0x0aU, .value = controller_signature,
         .value_len = sizeof(controller_signature)},
    };
    submessage_len = tlv_encode(controller_items, 2U, submessage,
                                sizeof(submessage));
    encrypted_len = chacha_encrypt(verify_key, "PV-Msg03", submessage,
                                   submessage_len, encrypted,
                                   sizeof(encrypted));

    uint8_t state3 = 3U;
    const airplay_tlv8_item_t m3_items[] = {
        {.type = 0x06U, .value = &state3, .value_len = 1U},
        {.type = 0x05U, .value = encrypted, .value_len = encrypted_len},
    };
    body_len = tlv_encode(m3_items, 2U, body, sizeof(body));
    request_len = build_request(request, sizeof(request), "POST",
                                "/pair-verify", PAIRING_TYPE, cseq++, body,
                                body_len);
    client_exchange(server, client, request, request_len, &response);
    assert(tlv_state(response.body, response.body_len) == 4U);
    assert(airplay_rtsp_server_is_encrypted(server));

    /* The client encrypts with the controller write key and decrypts with
     * the controller read key; the server's read/write naming is the
     * mirror image. */
    uint8_t controller_write_key[KEY_BYTES];
    uint8_t controller_read_key[KEY_BYTES];
    hkdf_key(shared, sizeof(shared), "Control-Salt",
             "Control-Write-Encryption-Key", controller_write_key);
    hkdf_key(shared, sizeof(shared), "Control-Salt",
             "Control-Read-Encryption-Key", controller_read_key);
    assert(airplay_control_cipher_create(controller_read_key,
                                         controller_write_key,
                                         &client->cipher) ==
           AIRPLAY_CONTROL_CIPHER_OK);

    mbedtls_ecp_group_free(&group);
    mbedtls_mpi_free(&private_key);
    mbedtls_mpi_free(&shared_secret);
    mbedtls_ecp_point_free(&public_key);
    mbedtls_ecp_point_free(&accessory_point);
    mbedtls_platform_zeroize(shared, sizeof(shared));
    mbedtls_platform_zeroize(verify_key, sizeof(verify_key));
    mbedtls_platform_zeroize(controller_write_key,
                             sizeof(controller_write_key));
    mbedtls_platform_zeroize(controller_read_key,
                             sizeof(controller_read_key));
    mbedtls_platform_zeroize(submessage, sizeof(submessage));
    mbedtls_platform_zeroize(encrypted, sizeof(encrypted));
    mbedtls_platform_zeroize(accessory_signature_input,
                             sizeof(accessory_signature_input));
    mbedtls_platform_zeroize(controller_signature_input,
                             sizeof(controller_signature_input));
    mbedtls_platform_zeroize(controller_signature,
                             sizeof(controller_signature));
}

/* ------------------------------------------------------------------ */
/* Encrypted traffic                                                  */
/* ------------------------------------------------------------------ */

static void run_encrypted_info(airplay_rtsp_server_t *server, client_t *client,
                               const uint8_t *info_plist, size_t info_len)
{
    uint8_t request[192];
    response_t response;
    size_t request_len = build_request(request, sizeof(request), "GET", "/info",
                                       PAIRING_TYPE, 20U, NULL, 0U);
    client_exchange(server, client, request, request_len, &response);
    assert(response.body_len == info_len);
    assert(memcmp(response.body, info_plist, info_len) == 0);
}

static void run_tampered_frame(airplay_rtsp_server_t *server,
                               client_t *client)
{
    uint8_t request[192];
    size_t request_len = build_request(request, sizeof(request), "GET", "/info",
                                       PAIRING_TYPE, 21U, NULL, 0U);
    uint8_t encrypted[256];
    size_t encrypted_len = 0U;
    assert(airplay_control_cipher_encrypt(client->cipher, request, request_len,
                                          encrypted, sizeof(encrypted),
                                          &encrypted_len) ==
           AIRPLAY_CONTROL_CIPHER_OK);
    encrypted[encrypted_len - 1U] ^= 0x01U;
    client_send(client, encrypted, encrypted_len);

    uint64_t start = rtcp_os_linux.now_ms();
    while (airplay_rtsp_server_is_connected(server) &&
           rtcp_os_linux.now_ms() - start < WAIT_LIMIT_MS) {
        airplay_rtsp_server_poll(server, 10);
    }
    assert(!airplay_rtsp_server_is_connected(server));
}

/* ------------------------------------------------------------------ */
/* Screen DataStream                                                  */
/* ------------------------------------------------------------------ */

static void write_le32(uint8_t *output, uint32_t value)
{
    for (unsigned i = 0U; i < 4U; ++i) {
        output[i] = (uint8_t)(value >> (8U * i));
    }
}

static void write_le64(uint8_t *output, uint64_t value)
{
    for (unsigned i = 0U; i < 8U; ++i) {
        output[i] = (uint8_t)(value >> (8U * i));
    }
}

static void write_be64(uint8_t *output, uint64_t value)
{
    for (unsigned i = 0U; i < 8U; ++i) {
        output[7U - i] = (uint8_t)(value >> (8U * i));
    }
}

/* Closes a binary plist: offset table followed by the 32-byte trailer. */
static size_t bplist_finish(uint8_t *output, size_t objects_len,
                            const size_t *offsets, size_t object_count,
                            size_t offset_int_size)
{
    size_t table_at = objects_len;
    for (size_t i = 0U; i < object_count; ++i) {
        for (size_t byte = 0U; byte < offset_int_size; ++byte) {
            output[table_at + i * offset_int_size + byte] =
                (uint8_t)(offsets[i] >>
                          (8U * (offset_int_size - byte - 1U)));
        }
    }
    size_t trailer_at = table_at + object_count * offset_int_size;
    memset(output + trailer_at, 0, 32U);
    output[trailer_at + 6U] = (uint8_t)offset_int_size;
    output[trailer_at + 7U] = 1U; /* one-byte object references */
    write_be64(output + trailer_at + 8U, object_count);
    write_be64(output + trailer_at + 16U, 0U); /* top object */
    write_be64(output + trailer_at + 24U, table_at);
    return trailer_at + 32U;
}

static size_t build_bplist_empty_dict(uint8_t *output, size_t capacity)
{
    assert(capacity >= 64U);
    memcpy(output, "bplist00", 8U);
    size_t offsets[1] = {8U};
    output[8] = 0xd0U; /* empty dictionary */
    return bplist_finish(output, 9U, offsets, 1U, 1U);
}

/* {"streams": [{"type": 110, "streamConnectionID": id}]} */
static size_t build_bplist_screen_setup(uint8_t *output, size_t capacity,
                                        uint64_t connection_id)
{
    assert(capacity >= 128U);
    memcpy(output, "bplist00", 8U);
    size_t offsets[8];
    size_t at = 8U;

    offsets[0] = at;
    output[at++] = 0xd1U; /* dictionary, one entry */
    output[at++] = 0x01U;
    output[at++] = 0x02U;

    offsets[1] = at;
    output[at++] = 0x57U; /* 7-character string */
    memcpy(output + at, "streams", 7U);
    at += 7U;

    offsets[2] = at;
    output[at++] = 0xa1U; /* array, one element */
    output[at++] = 0x03U;

    offsets[3] = at;
    output[at++] = 0xd2U; /* dictionary, two entries */
    output[at++] = 0x04U;
    output[at++] = 0x05U;
    output[at++] = 0x06U;
    output[at++] = 0x07U;

    offsets[4] = at;
    output[at++] = 0x54U;
    memcpy(output + at, "type", 4U);
    at += 4U;

    offsets[5] = at;
    output[at++] = 0x5fU; /* 18-character string */
    output[at++] = 0x10U;
    output[at++] = 18U;
    memcpy(output + at, "streamConnectionID", 18U);
    at += 18U;

    offsets[6] = at;
    output[at++] = 0x10U; /* one-byte integer */
    output[at++] = 110U;

    offsets[7] = at;
    output[at++] = 0x13U; /* eight-byte integer */
    write_be64(output + at, connection_id);
    at += 8U;

    return bplist_finish(output, at, offsets, 8U, 2U);
}

static uint16_t response_data_port(const response_t *response)
{
    airplay_bplist_doc_t doc;
    airplay_bplist_object_t root;
    airplay_bplist_object_t streams;
    airplay_bplist_object_t entry;
    airplay_bplist_object_t object;
    uint64_t port = 0U;
    assert(airplay_bplist_open(response->body, response->body_len, &doc) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc, root, "streams", &streams) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_array_get(&doc, streams, 0U, &entry) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc, entry, "dataPort", &object) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_uint(&doc, object, &port) == AIRPLAY_BPLIST_OK);
    assert(port != 0U && port <= UINT16_MAX);
    return (uint16_t)port;
}

static void run_screen_stream(airplay_rtsp_server_t *server, client_t *client,
                              const uint8_t shared_secret[KEY_BYTES],
                              uint32_t *cseq)
{
    uint8_t request[512];
    uint8_t body[256];
    response_t response;

    /* Initial SETUP: starts the timing and event endpoints. */
    size_t body_len = build_bplist_empty_dict(body, sizeof(body));
    size_t request_len = build_request(request, sizeof(request), "SETUP",
                                       "/stream", PLIST_TYPE, (*cseq)++, body,
                                       body_len);
    client_exchange(server, client, request, request_len, &response);

    static const uint64_t connection_id = UINT64_C(0x1122334455667788);
    body_len = build_bplist_screen_setup(body, sizeof(body), connection_id);
    request_len = build_request(request, sizeof(request), "SETUP", "/stream",
                                PLIST_TYPE, (*cseq)++, body, body_len);
    client_exchange(server, client, request, request_len, &response);
    uint16_t data_port = response_data_port(&response);

    request_len = build_request(request, sizeof(request), "RECORD", "/stream",
                                PLIST_TYPE, (*cseq)++, NULL, 0U);
    client_exchange(server, client, request, request_len, &response);

    uint8_t output_key[KEY_BYTES];
    assert(airplay_screen_derive_output_key(shared_secret, KEY_BYTES,
                                            connection_id, output_key) ==
           RTCP_OK);

    rtcp_addr_t address = {0};
    address.family = RTCP_AF_INET;
    address.addr[0] = 127U;
    address.addr[3] = 1U;
    address.port = data_port;
    int screen_fd = rtcp_net_linux.connect(&address);
    assert(screen_fd >= 0);

    static const uint8_t nal[] = {0x65U, 0x11U, 0x22U, 0x33U, 0x44U};
    uint8_t plaintext[4U + sizeof(nal)];
    plaintext[0] = 0U;
    plaintext[1] = 0U;
    plaintext[2] = 0U;
    plaintext[3] = (uint8_t)sizeof(nal);
    memcpy(plaintext + 4U, nal, sizeof(nal));

    uint8_t header[AIRPLAY_SCREEN_HEADER_BYTES] = {0};
    write_le32(header, (uint32_t)(sizeof(plaintext) +
                                  AIRPLAY_SCREEN_TAG_BYTES));
    header[4] = 0U; /* video */
    static const uint64_t ntp = UINT64_C(0x0123456789abcdef);
    write_le64(header + 8U, ntp);

    mbedtls_chachapoly_context cipher;
    mbedtls_chachapoly_init(&cipher);
    assert(mbedtls_chachapoly_setkey(&cipher, output_key) == 0);
    for (unsigned frame_index = 0U; frame_index < 2U; ++frame_index) {
        uint8_t nonce[12] = {0};
        write_le64(nonce + 4U, frame_index);
        uint8_t frame[sizeof(plaintext) + AIRPLAY_SCREEN_TAG_BYTES];
        assert(mbedtls_chachapoly_encrypt_and_tag(
                   &cipher, sizeof(plaintext), nonce, header, sizeof(header),
                   plaintext, frame, frame + sizeof(plaintext)) == 0);
        assert(rtcp_net_linux.send(screen_fd, header, sizeof(header)) ==
               (int)sizeof(header));
        assert(rtcp_net_linux.send(screen_fd, frame, sizeof(frame)) ==
               (int)sizeof(frame));

        uint64_t start = rtcp_os_linux.now_ms();
        while (screen_frames == frame_index &&
               rtcp_os_linux.now_ms() - start < WAIT_LIMIT_MS) {
            airplay_rtsp_server_poll(server, 10);
        }
        assert(screen_frames == frame_index + 1U);
    }
    mbedtls_chachapoly_free(&cipher);

    static const uint8_t start_code[4] = {0U, 0U, 0U, 1U};
    assert(screen_frame_len == 4U + sizeof(nal));
    assert(memcmp(screen_frame, start_code, sizeof(start_code)) == 0);
    assert(memcmp(screen_frame + 4U, nal, sizeof(nal)) == 0);
    uint64_t expected_ns =
        (ntp >> 32U) * UINT64_C(1000000000) +
        (((ntp & UINT64_C(0xffffffff)) * UINT64_C(1000000000)) >> 32U);
    assert(screen_sender_time_ns == expected_ns);

    rtcp_net_linux.close(screen_fd);
    mbedtls_platform_zeroize(output_key, sizeof(output_key));
}


/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    fixture_t fixture = {0};
    assert(airplay_ed25519_keypair_from_seed(ACCESSORY_SEED,
                                             &fixture.accessory));
    assert(airplay_ed25519_keypair_from_seed(CONTROLLER_SEED,
                                             &fixture.controller));

    uint8_t info_plist[INFO_PLIST_BYTES];
    airplay_info_plist_config_t info = {0};
    info.device_id = ACCESSORY_ID;
    info.name = "rt-carplay e2e";
    info.model = "AppleTV6,2";
    info.width_pixels = 1920U;
    info.height_pixels = 1080U;
    info.max_fps = 30U;
    size_t info_len = 0U;
    assert(airplay_info_plist_build(&info, info_plist, sizeof(info_plist),
                                    &info_len));

    airplay_rtsp_server_config_t config = {0};
    config.os = &rtcp_os_linux;
    config.net = &rtcp_net_linux;
    fill_pairing(&fixture, &config.pairing);
    config.info_plist = info_plist;
    config.info_plist_len = info_len;
    config.on_screen_message = capture_screen;

    airplay_rtsp_server_t *server = NULL;
    assert(airplay_rtsp_server_start(&config, &server) == RTCP_OK);
    assert(server != NULL);

    client_t client;
    client_connect(server, &client);
    run_pair_setup(server, &client, &fixture);
    uint8_t shared_secret[KEY_BYTES];
    run_pair_verify(server, &client, &fixture, shared_secret);
    run_encrypted_info(server, &client, info_plist, info_len);
    uint32_t cseq = 30U;
    run_screen_stream(server, &client, shared_secret, &cseq);
    mbedtls_platform_zeroize(shared_secret, sizeof(shared_secret));
    run_tampered_frame(server, &client);

    if (client.cipher != NULL) airplay_control_cipher_destroy(client.cipher);
    rtcp_net_linux.close(client.fd);
    airplay_rtsp_server_stop(server);
    airplay_ed25519_keypair_clear(&fixture.accessory);
    airplay_ed25519_keypair_clear(&fixture.controller);
    mbedtls_platform_zeroize(&fixture, sizeof(fixture));

    assert(screen_messages == 2U);
    puts("airplay pairing end-to-end host test passed");
    return 0;
}
