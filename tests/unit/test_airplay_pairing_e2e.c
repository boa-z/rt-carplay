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
#include "rtcp/receiver.h"
#include "rtcp/receiver_compat.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airplay_bplist.h"
#include "airplay_control_cipher.h"
#include "airplay_ed25519.h"
#include "airplay_info_plist.h"
#include "airplay_rtsp_codec.h"
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

#define WAIT_LIMIT_MS    5000U
#define PAIRING_CODE     "3939"
#define CONTROLLER_ID    "rt-carplay-e2e-controller"
#define ACCESSORY_ID     "02:00:00:00:00:01"
#define KEY_BYTES        32U
#define PAIRING_TYPE     "application/pairing+tlv8"
#define PLIST_TYPE       "application/x-apple-binary-plist"
#define INFO_PLIST_BYTES AIRPLAY_RTSP_SESSION_INFO_MAX_BYTES

/* Exercise the public Receiver lifecycle with the real encrypted controller. */
static rtcp_receiver_t *e2e_receiver;
static int receiver_poll(rtcp_receiver_t *server, int timeout)
{
    assert(server == e2e_receiver);
    return rtcp_receiver_poll(e2e_receiver, timeout);
}
static bool receiver_connected(const rtcp_receiver_t *receiver)
{
    rtcp_receiver_state_t state;
    assert(rtcp_receiver_get_state(receiver, &state) == RTCP_OK);
    return state.transport_connected;
}
static bool receiver_authenticated(const rtcp_receiver_t *receiver)
{
    rtcp_receiver_state_t state;
    assert(rtcp_receiver_get_state(receiver, &state) == RTCP_OK);
    return state.authenticated;
}
static void expect_receiver_state(bool transport, bool authenticated,
                                  bool session, bool video, bool audio)
{
    rtcp_receiver_state_t state;
    assert(rtcp_receiver_get_state(e2e_receiver, &state) == RTCP_OK);
    assert(state.transport_connected == transport);
    assert(state.authenticated == authenticated);
    assert(state.session_established == session);
    assert(state.video_ready == video);
    assert(state.audio_ready == audio);
}

/* Ed25519 seeds are test fixtures, not identities. */
static const uint8_t ACCESSORY_SEED[KEY_BYTES] = {
    0x11,
    0x22,
    0x33,
    0x44,
    0x55,
    0x66,
    0x77,
    0x88,
    0x99,
    0xaa,
    0xbb,
    0xcc,
    0xdd,
    0xee,
    0xff,
    0x00,
    0x01,
    0x02,
    0x03,
    0x04,
    0x05,
    0x06,
    0x07,
    0x08,
    0x09,
    0x0a,
    0x0b,
    0x0c,
    0x0d,
    0x0e,
    0x0f,
    0x10,
};
static const uint8_t CONTROLLER_SEED[KEY_BYTES] = {
    0x80,
    0x81,
    0x82,
    0x83,
    0x84,
    0x85,
    0x86,
    0x87,
    0x88,
    0x89,
    0x8a,
    0x8b,
    0x8c,
    0x8d,
    0x8e,
    0x8f,
    0x90,
    0x91,
    0x92,
    0x93,
    0x94,
    0x95,
    0x96,
    0x97,
    0x98,
    0x99,
    0x9a,
    0x9b,
    0x9c,
    0x9d,
    0x9e,
    0x9f,
};

typedef struct
{
    uint8_t counter;
} rng_t;

typedef struct
{
    airplay_ed25519_keypair_t accessory;
    airplay_ed25519_keypair_t controller;
    uint8_t paired_identifier[AIRPLAY_PAIR_SETUP_MAX_IDENTIFIER_BYTES];
    size_t paired_identifier_len;
    uint8_t paired_public_key[KEY_BYTES];
    bool has_pairing;
    rng_t rng;
} fixture_t;

static bool event_connected;
static unsigned event_transitions;
static unsigned video_context, audio_context, volume_context, event_context;

static void capture_event_state(bool connected, void *context)
{
    assert(context == &event_context);
    event_connected = connected;
    ++event_transitions;
}

static unsigned alt_frames, alt_context;
static unsigned extra_main_frames;
static int capture_alt(rtcp_video_message_type_t type, uint8_t *bytes, size_t n,
                       size_t prefix, uint64_t sender, uint64_t arrival, void *context)
{
    (void)prefix;
    (void)sender;
    (void)arrival;
    assert(context == &alt_context && type == RTCP_VIDEO_FRAME_H264);
    assert(n == 6 && !memcmp(bytes, "\0\0\0\1\x65\xbb", 6));
    ++alt_frames;
    return RTCP_OK;
}

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
    assert(context == &video_context);
    ++screen_messages;
    if (type == AIRPLAY_SCREEN_MESSAGE_FRAME_H264)
    {
        assert(annex_b_len <= sizeof(screen_frame));
        memcpy(screen_frame, annex_b, annex_b_len);
        screen_frame_len = annex_b_len;
        screen_sender_time_ns = sender_time_ns;
        ++screen_frames;
    }
    return RTCP_OK;
}

static unsigned audio_packets;
static uint8_t audio_pcm[256];
static size_t audio_pcm_len;
static uint16_t audio_packets_stream_type;
static airplay_audio_format_t audio_packet_format;
static uint32_t audio_packet_timestamp;

static int capture_audio(uint16_t stream_type, const uint8_t *data, size_t len,
                         const airplay_audio_format_t *format,
                         uint32_t rtp_timestamp, uint64_t arrival_us,
                         void *context)
{
    (void)arrival_us;
    assert(context == &audio_context);
    ++audio_packets;
    assert(len <= sizeof(audio_pcm));
    memcpy(audio_pcm, data, len);
    audio_pcm_len = len;
    audio_packets_stream_type = stream_type;
    audio_packet_format = *format;
    audio_packet_timestamp = rtp_timestamp;
    return RTCP_OK;
}

static unsigned volume_events;
static float volume_last_db;

static void capture_volume(float volume_db, void *context)
{
    assert(context == &volume_context);
    volume_last_db = volume_db;
    ++volume_events;
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
        identifier_len > sizeof(fixture->paired_identifier))
    {
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
        memcmp(identifier, fixture->paired_identifier, identifier_len) != 0)
    {
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

typedef struct
{
    int fd;
    uint8_t raw[4096];
    uint8_t plain[32768];
    size_t plain_len;
    airplay_control_cipher_t *cipher;
} client_t;

typedef struct
{
    const uint8_t *headers;
    size_t headers_len;
    const uint8_t *body;
    size_t body_len;
} response_t;

static void client_send(client_t *client, const uint8_t *bytes, size_t len)
{
    size_t offset = 0U;
    while (offset < len)
    {
        int sent = rtcp_net_linux.send(client->fd, bytes + offset,
                                       len - offset);
        assert(sent > 0);
        offset += (size_t)sent;
    }
}

static bool client_has_data(int fd)
{
    struct rtcp_pollfd pfd = { .fd = fd, .events = RTCP_POLLIN };
    int ready = rtcp_net_linux.poll(&pfd, 1, 0);
    return ready > 0 && (pfd.revents & RTCP_POLLIN) != 0;
}

static bool contains(const uint8_t *bytes, size_t len, const char *needle)
{
    size_t needle_len = strlen(needle);
    if (needle_len > len)
        return false;
    for (size_t i = 0U; i <= len - needle_len; ++i)
    {
        if (memcmp(bytes + i, needle, needle_len) == 0)
            return true;
    }
    return false;
}

static void client_pump(rtcp_receiver_t *server, client_t *client)
{
    (void)receiver_poll(server, 5);
    while (client_has_data(client->fd))
    {
        int received = rtcp_net_linux.recv(client->fd, client->raw,
                                           sizeof(client->raw));
        assert(received > 0);
        if (client->cipher == NULL)
        {
            assert(client->plain_len + (size_t)received <=
                   sizeof(client->plain));
            memcpy(client->plain + client->plain_len, client->raw,
                   (size_t)received);
            client->plain_len += (size_t)received;
            continue;
        }
        size_t offset = 0U;
        while (offset < (size_t)received)
        {
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
            if (result == AIRPLAY_CONTROL_CIPHER_INCOMPLETE)
                break;
            if (result != AIRPLAY_CONTROL_CIPHER_OK)
            {
                assert(0 && "server response failed to decrypt");
            }
            if (consumed == 0U && plaintext_len == 0U)
                break;
        }
    }
}

static bool parse_response(const client_t *client, response_t *response)
{
    static const char separator[] = "\r\n\r\n";
    size_t separator_at = 0U;
    bool found = false;
    for (size_t i = 0U; i + sizeof(separator) - 1U <= client->plain_len; ++i)
    {
        if (memcmp(client->plain + i, separator, sizeof(separator) - 1U) == 0)
        {
            separator_at = i;
            found = true;
            break;
        }
    }
    if (!found)
        return false;
    size_t headers_len = separator_at + sizeof(separator) - 1U;
    size_t content_length = 0U;
    size_t cursor = 0U;
    while (cursor < separator_at)
    {
        size_t line_end = cursor;
        while (line_end < separator_at &&
               !(client->plain[line_end] == '\r' &&
                 client->plain[line_end + 1U] == '\n'))
        {
            ++line_end;
        }
        static const char length_header[] = "Content-Length:";
        size_t length_header_len = sizeof(length_header) - 1U;
        if (line_end - cursor >= length_header_len &&
            memcmp(client->plain + cursor, length_header,
                   length_header_len) == 0)
        {
            size_t value_at = cursor + length_header_len;
            while (value_at < line_end && client->plain[value_at] == ' ')
            {
                ++value_at;
            }
            while (value_at < line_end &&
                   client->plain[value_at] >= '0' &&
                   client->plain[value_at] <= '9')
            {
                content_length =
                    content_length * 10U + (size_t)(client->plain[value_at] - '0');
                ++value_at;
            }
        }
        cursor = line_end + 2U;
    }
    if (client->plain_len < headers_len + content_length)
        return false;
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
    if (body_len != 0U)
        memcpy(output + written, body, body_len);
    return (size_t)written + body_len;
}

static void client_exchange_raw(rtcp_receiver_t *server, client_t *client,
                                const uint8_t *request, size_t request_len,
                                response_t *response)
{
    client->plain_len = 0U;
    if (client->cipher == NULL)
    {
        client_send(client, request, request_len);
    }
    else
    {
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
    while (!parse_response(client, response))
    {
        assert(rtcp_os_linux.now_ms() - start < WAIT_LIMIT_MS);
        client_pump(server, client);
    }
}

static void client_exchange(rtcp_receiver_t *server, client_t *client,
                            const uint8_t *request, size_t request_len, response_t *response)
{
    client_exchange_raw(server, client, request, request_len, response);
    assert(contains(response->headers, response->headers_len, "200 OK"));
}

static void client_connect(rtcp_receiver_t *server, client_t *client)
{
    uint16_t port = rtcp_receiver_port(server);
    assert(port != 0U);
    rtcp_addr_t address = { 0 };
    address.family = RTCP_AF_INET;
    address.addr[0] = 127U;
    address.addr[3] = 1U;
    address.port = port;
    memset(client, 0, sizeof(*client));
    client->fd = rtcp_net_linux.connect(&address);
    assert(client->fd >= 0);

    uint64_t start = rtcp_os_linux.now_ms();
    while (rtcp_os_linux.now_ms() - start < WAIT_LIMIT_MS &&
           !receiver_connected(server))
    {
        receiver_poll(server, 10);
    }
    assert(receiver_connected(server));
    assert(!receiver_authenticated(server));
}

/* ------------------------------------------------------------------ */
/* Pair-Setup                                                         */
/* ------------------------------------------------------------------ */

static void run_pair_setup(rtcp_receiver_t *server, client_t *client,
                           fixture_t *fixture)
{
    uint8_t request[4096];
    uint8_t body[1024];
    response_t response;
    uint32_t cseq = 1U;

    /* M1: start SRP. */
    uint8_t state1 = 1U;
    const airplay_tlv8_item_t m1_items[] = {
        { .type = 0x06U, .value = &state1, .value_len = 1U },
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
        { .type = 0x06U, .value = &state3, .value_len = 1U },
        { .type = 0x03U, .value = public_a, .value_len = sizeof(public_a) },
        { .type = 0x04U, .value = proof_m1, .value_len = sizeof(proof_m1) },
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
    assert(!receiver_authenticated(server));

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
        { .type = 0x01U, .value = (const uint8_t *)CONTROLLER_ID, .value_len = sizeof(CONTROLLER_ID) - 1U },
        { .type = 0x03U, .value = fixture->controller.public_key, .value_len = KEY_BYTES },
        { .type = 0x0aU, .value = controller_signature, .value_len = sizeof(controller_signature) },
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
        { .type = 0x06U, .value = &state5, .value_len = 1U },
        { .type = 0x05U, .value = encrypted, .value_len = encrypted_len },
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

static void run_pair_verify(rtcp_receiver_t *server, client_t *client,
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
    rng_t client_rng = { .counter = 0x40 };
    assert(mbedtls_ecp_gen_keypair(&group, &private_key, &public_key,
                                   mbed_rng, &client_rng) == 0);
    assert(mbedtls_ecp_point_write_binary(
               &group, &public_key, MBEDTLS_ECP_PF_UNCOMPRESSED, &public_len,
               controller_public, sizeof(controller_public)) == 0);
    assert(public_len == KEY_BYTES);

    /* M1: controller ephemeral key. */
    uint8_t state1 = 1U;
    const airplay_tlv8_item_t m1_items[] = {
        { .type = 0x06U, .value = &state1, .value_len = 1U },
        { .type = 0x03U, .value = controller_public, .value_len = sizeof(controller_public) },
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
        { .type = 0x01U, .value = (const uint8_t *)CONTROLLER_ID, .value_len = sizeof(CONTROLLER_ID) - 1U },
        { .type = 0x0aU, .value = controller_signature, .value_len = sizeof(controller_signature) },
    };
    submessage_len = tlv_encode(controller_items, 2U, submessage,
                                sizeof(submessage));
    encrypted_len = chacha_encrypt(verify_key, "PV-Msg03", submessage,
                                   submessage_len, encrypted,
                                   sizeof(encrypted));

    uint8_t state3 = 3U;
    const airplay_tlv8_item_t m3_items[] = {
        { .type = 0x06U, .value = &state3, .value_len = 1U },
        { .type = 0x05U, .value = encrypted, .value_len = encrypted_len },
    };
    body_len = tlv_encode(m3_items, 2U, body, sizeof(body));
    request_len = build_request(request, sizeof(request), "POST",
                                "/pair-verify", PAIRING_TYPE, cseq++, body,
                                body_len);
    client_exchange(server, client, request, request_len, &response);
    assert(tlv_state(response.body, response.body_len) == 4U);
    assert(receiver_authenticated(server));

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

static void run_encrypted_info(rtcp_receiver_t *server, client_t *client,
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

/* The sender's SET_PARAMETER volume reaches the receiver callback and the
 * session acknowledges it with 200 OK. */
static void expect_volume_query(rtcp_receiver_t *server, client_t *client,
                                uint32_t *cseq, const char *parameter, const char *expected)
{
    uint8_t request[256];
    response_t response;
    size_t length = build_request(request, sizeof(request), "GET_PARAMETER", "/stream",
                                  "text/parameters", (*cseq)++, (const uint8_t *)parameter, strlen(parameter));
    client_exchange(server, client, request, length, &response);
    assert(contains(response.headers, response.headers_len, "200"));
    assert(response.body_len == strlen(expected));
    assert(memcmp(response.body, expected, response.body_len) == 0);
    if (response.body_len)
        assert(contains(response.headers, response.headers_len, "text/parameters"));
}

static void run_set_parameter_volume(rtcp_receiver_t *server,
                                     client_t *client, uint32_t *cseq)
{
    static const char body[] = "volume: -12.000000\r\n";
    uint8_t request[256];
    response_t response;
    unsigned before = volume_events;
    size_t request_len = build_request(
        request, sizeof(request), "SET_PARAMETER", "/stream",
        "text/parameters", (*cseq)++, (const uint8_t *)body,
        sizeof(body) - 1U);
    client_exchange(server, client, request, request_len, &response);
    assert(contains(response.headers, response.headers_len, "200"));
    assert(volume_events == before + 1U);
    assert(volume_last_db > -12.001f && volume_last_db < -11.999f);
    expect_volume_query(server, client, cseq, "volume\r\n", "");
    assert(rtcp_receiver_set_volume_db(server, -12.375f) == RTCP_OK);
    assert(rtcp_receiver_set_volume_db(server, 1.0f) == RTCP_EINVAL);
    expect_volume_query(server, client, cseq, "volume\r\n", "volume: -12.375\r\n");
    expect_volume_query(server, client, cseq, "volume-other", "");
    assert(rtcp_receiver_set_volume_db(server, -144.0f) == RTCP_OK);
    expect_volume_query(server, client, cseq, "volume", "volume: -144.000\r\n");
}

static void capture_now_playing(const rtcp_now_playing_t *update, void *context)
{
    unsigned *events = context;
    if ((*events)++ == 0)
    {
        assert(update->title && !strcmp(update->title, "\xe5\xa4\x9c\xe8\x88\xaa\xf0\x9f\x8e\xb5"));
        assert(update->artist && !strcmp(update->artist, "Artist"));
        assert(!update->album && !update->has_playing);
    }
    else
        assert(!update->title && !update->artist && !update->album && update->has_playing && update->playing);
}

static void run_now_playing(rtcp_receiver_t *server, client_t *client, uint32_t *cseq)
{
    static const uint8_t track[] = {
        0x62,
        0x70,
        0x6c,
        0x69,
        0x73,
        0x74,
        0x30,
        0x30,
        0xd2,
        0x01,
        0x02,
        0x03,
        0x09,
        0x56,
        0x70,
        0x61,
        0x72,
        0x61,
        0x6d,
        0x73,
        0x54,
        0x74,
        0x79,
        0x70,
        0x65,
        0xd1,
        0x01,
        0x04,
        0xd2,
        0x05,
        0x06,
        0x07,
        0x08,
        0x5f,
        0x10,
        0x22,
        0x6b,
        0x4d,
        0x52,
        0x4d,
        0x65,
        0x64,
        0x69,
        0x61,
        0x52,
        0x65,
        0x6d,
        0x6f,
        0x74,
        0x65,
        0x4e,
        0x6f,
        0x77,
        0x50,
        0x6c,
        0x61,
        0x79,
        0x69,
        0x6e,
        0x67,
        0x49,
        0x6e,
        0x66,
        0x6f,
        0x41,
        0x72,
        0x74,
        0x69,
        0x73,
        0x74,
        0x5f,
        0x10,
        0x21,
        0x6b,
        0x4d,
        0x52,
        0x4d,
        0x65,
        0x64,
        0x69,
        0x61,
        0x52,
        0x65,
        0x6d,
        0x6f,
        0x74,
        0x65,
        0x4e,
        0x6f,
        0x77,
        0x50,
        0x6c,
        0x61,
        0x79,
        0x69,
        0x6e,
        0x67,
        0x49,
        0x6e,
        0x66,
        0x6f,
        0x54,
        0x69,
        0x74,
        0x6c,
        0x65,
        0x56,
        0x41,
        0x72,
        0x74,
        0x69,
        0x73,
        0x74,
        0x64,
        0x59,
        0x1c,
        0x82,
        0x2a,
        0xd8,
        0x3c,
        0xdf,
        0xb5,
        0x5f,
        0x10,
        0x16,
        0x75,
        0x70,
        0x64,
        0x61,
        0x74,
        0x65,
        0x4d,
        0x52,
        0x4e,
        0x6f,
        0x77,
        0x50,
        0x6c,
        0x61,
        0x79,
        0x69,
        0x6e,
        0x67,
        0x49,
        0x6e,
        0x66,
        0x6f,
        0x08,
        0x0d,
        0x14,
        0x19,
        0x1c,
        0x21,
        0x46,
        0x6a,
        0x71,
        0x7a,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x01,
        0x01,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x0a,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x93,
    };
    static const uint8_t playback[] = {
        0x62,
        0x70,
        0x6c,
        0x69,
        0x73,
        0x74,
        0x30,
        0x30,
        0xd2,
        0x01,
        0x02,
        0x03,
        0x06,
        0x56,
        0x70,
        0x61,
        0x72,
        0x61,
        0x6d,
        0x73,
        0x54,
        0x74,
        0x79,
        0x70,
        0x65,
        0xd1,
        0x04,
        0x05,
        0x5f,
        0x10,
        0x0f,
        0x6d,
        0x72,
        0x50,
        0x6c,
        0x61,
        0x79,
        0x62,
        0x61,
        0x63,
        0x6b,
        0x53,
        0x74,
        0x61,
        0x74,
        0x65,
        0x10,
        0x01,
        0x5f,
        0x10,
        0x15,
        0x75,
        0x70,
        0x64,
        0x61,
        0x74,
        0x65,
        0x4d,
        0x52,
        0x50,
        0x6c,
        0x61,
        0x79,
        0x62,
        0x61,
        0x63,
        0x6b,
        0x53,
        0x74,
        0x61,
        0x74,
        0x65,
        0x08,
        0x0d,
        0x14,
        0x19,
        0x1c,
        0x2e,
        0x30,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x01,
        0x01,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x07,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x48,
    };
    unsigned events = 0;
    assert(rtcp_receiver_set_now_playing_callback(server, capture_now_playing, &events) == RTCP_OK);
    uint8_t request[512];
    response_t response;
    size_t length = build_request(request, sizeof(request), "POST", "/command", PLIST_TYPE,
                                  (*cseq)++, track, sizeof(track));
    client_exchange(server, client, request, length, &response);
    assert(contains(response.headers, response.headers_len, "200") && events == 1);
    length = build_request(request, sizeof(request), "POST", "/command", PLIST_TYPE,
                           (*cseq)++, playback, sizeof(playback));
    client_exchange(server, client, request, length, &response);
    assert(contains(response.headers, response.headers_len, "200") && events == 2);
    assert(rtcp_receiver_set_now_playing_callback(server, NULL, NULL) == RTCP_OK);
}

static void run_tampered_frame(rtcp_receiver_t *server,
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
    while (receiver_connected(server) &&
           rtcp_os_linux.now_ms() - start < WAIT_LIMIT_MS)
    {
        receiver_poll(server, 10);
    }
    assert(!receiver_connected(server));
}

/* ------------------------------------------------------------------ */
/* Screen DataStream                                                  */
/* ------------------------------------------------------------------ */

static void write_le32(uint8_t *output, uint32_t value)
{
    for (unsigned i = 0U; i < 4U; ++i)
    {
        output[i] = (uint8_t)(value >> (8U * i));
    }
}

static void write_le64(uint8_t *output, uint64_t value)
{
    for (unsigned i = 0U; i < 8U; ++i)
    {
        output[i] = (uint8_t)(value >> (8U * i));
    }
}

static void write_be64(uint8_t *output, uint64_t value)
{
    for (unsigned i = 0U; i < 8U; ++i)
    {
        output[7U - i] = (uint8_t)(value >> (8U * i));
    }
}

/* Closes a binary plist: offset table followed by the 32-byte trailer. */
static size_t bplist_finish(uint8_t *output, size_t objects_len,
                            const size_t *offsets, size_t object_count,
                            size_t offset_int_size)
{
    size_t table_at = objects_len;
    for (size_t i = 0U; i < object_count; ++i)
    {
        for (size_t byte = 0U; byte < offset_int_size; ++byte)
        {
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
    size_t offsets[1] = { 8U };
    output[8] = 0xd0U; /* empty dictionary */
    return bplist_finish(output, 9U, offsets, 1U, 1U);
}

/* Independent test writer: protocol requests are not built by the receiver's
 * SETUP response serializer. All object references fit in one byte. */
static size_t plist_string(uint8_t *out, size_t at, const char *text)
{
    size_t n = strlen(text);
    if (n < 15)
        out[at++] = (uint8_t)(0x50 + n);
    else
    {
        assert(n < 256);
        out[at++] = 0x5f;
        out[at++] = 0x10;
        out[at++] = (uint8_t)n;
    }
    memcpy(out + at, text, n);
    return at + n;
}
static void write_be32(uint8_t *output, uint32_t value);
static size_t build_data_setup(uint8_t out[512], const char *uuid, uint64_t seed, uint64_t connection)
{
    size_t offsets[12], at = 8;
    memcpy(out, "bplist00", 8);
    offsets[0] = at;
    out[at++] = 0xd1;
    out[at++] = 1;
    out[at++] = 2;
    offsets[1] = at;
    at = plist_string(out, at, "streams");
    offsets[2] = at;
    out[at++] = 0xa1;
    out[at++] = 3;
    offsets[3] = at;
    out[at++] = 0xd4;
    for (uint8_t i = 4; i < 12; ++i) out[at++] = i;
    const char *keys[] = { "type", "clientTypeUUID", "seed", "streamConnectionID" };
    for (unsigned i = 0; i < 4; ++i)
    {
        offsets[4 + i] = at;
        at = plist_string(out, at, keys[i]);
    }
    offsets[8] = at;
    out[at++] = 0x10;
    out[at++] = 130;
    offsets[9] = at;
    at = plist_string(out, at, uuid);
    offsets[10] = at;
    out[at++] = 0x13;
    write_be64(out + at, seed);
    at += 8;
    offsets[11] = at;
    out[at++] = 0x13;
    write_be64(out + at, connection);
    at += 8;
    assert(at + 24 + 32 <= 512);
    return bplist_finish(out, at, offsets, 12, 2);
}
static size_t build_feature_offer(uint8_t out[256], bool invalid)
{
    size_t offsets[5], at = 8;
    memcpy(out, "bplist00", 8);
    offsets[0] = at;
    out[at++] = 0xd1;
    out[at++] = 1;
    out[at++] = 2;
    offsets[1] = at;
    at = plist_string(out, at, "features");
    offsets[2] = at;
    if (invalid)
        out[at++] = 9; /* a bool, not a feature array */
    else
    {
        out[at++] = 0xa2;
        out[at++] = 3;
        out[at++] = 4;
    }
    offsets[3] = at;
    at = plist_string(out, at, "vehicleStateProtocol");
    offsets[4] = at;
    at = plist_string(out, at, "uiSync");
    return bplist_finish(out, at, offsets, 5, 2);
}

/* {"streams": [{"type": 110, "streamConnectionID": id}]} */
static size_t build_bplist_video_setup(uint8_t *output, size_t capacity,
                                       uint64_t connection_id, uint8_t type)
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
    output[at++] = type;

    offsets[7] = at;
    output[at++] = 0x13U; /* eight-byte integer */
    write_be64(output + at, connection_id);
    at += 8U;

    return bplist_finish(output, at, offsets, 8U, 2U);
}

static size_t build_bplist_screen_setup(uint8_t *output, size_t capacity, uint64_t id)
{
    return build_bplist_video_setup(output, capacity, id, 110);
}

/* {"streams": [{"type": 100, "streamConnectionID": id,
 *               "audioFormat": bits}]} */
static size_t build_bplist_audio_setup(uint8_t *output, size_t capacity,
                                       uint64_t connection_id,
                                       uint64_t format_bits, bool media)
{
    assert(capacity >= 192U);
    memcpy(output, "bplist00", 8U);
    size_t offsets[12];
    size_t at = 8U;

    offsets[0] = at;
    output[at++] = 0xd1U; /* dictionary, one entry */
    output[at++] = 0x01U;
    output[at++] = 0x02U;

    offsets[1] = at;
    output[at++] = 0x57U;
    memcpy(output + at, "streams", 7U);
    at += 7U;

    offsets[2] = at;
    output[at++] = 0xa1U; /* array, one element */
    output[at++] = 0x03U;

    offsets[3] = at;
    output[at++] = media ? 0xd4U : 0xd3U; /* optional audioType */
    output[at++] = 0x04U;
    output[at++] = 0x05U;
    output[at++] = 0x06U;
    if (media)
        output[at++] = 10U;
    output[at++] = 0x07U;
    output[at++] = 0x08U;
    output[at++] = 0x09U;
    if (media)
        output[at++] = 11U;

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
    output[at++] = 0x5bU;
    memcpy(output + at, "audioFormat", 11U);
    at += 11U;

    offsets[7] = at;
    output[at++] = 0x10U;
    output[at++] = 100U;

    offsets[8] = at;
    output[at++] = 0x13U; /* eight-byte integer */
    write_be64(output + at, connection_id);
    at += 8U;

    offsets[9] = at;
    if (format_bits <= 0xffU)
    {
        output[at++] = 0x10U;
        output[at++] = (uint8_t)format_bits;
    }
    else if (format_bits <= 0xffffU)
    {
        output[at++] = 0x11U;
        output[at++] = (uint8_t)(format_bits >> 8U);
        output[at++] = (uint8_t)format_bits;
    }
    else
    {
        output[at++] = 0x12U;
        output[at++] = (uint8_t)(format_bits >> 24U);
        output[at++] = (uint8_t)(format_bits >> 16U);
        output[at++] = (uint8_t)(format_bits >> 8U);
        output[at++] = (uint8_t)format_bits;
    }

    if (media)
    {
        offsets[10] = at;
        output[at++] = 0x59;
        memcpy(output + at, "audioType", 9);
        at += 9;
        offsets[11] = at;
        output[at++] = 0x55;
        memcpy(output + at, "media", 5);
        at += 5;
    }
    return bplist_finish(output, at, offsets, media ? 12U : 10U, 2U);
}

static uint16_t response_port(const response_t *response, const char *key)
{
    airplay_bplist_doc_t doc;
    airplay_bplist_object_t root;
    airplay_bplist_object_t object;
    uint64_t value = 0U;
    assert(airplay_bplist_open(response->body, response->body_len, &doc) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc, root, key, &object) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_uint(&doc, object, &value) == AIRPLAY_BPLIST_OK);
    assert(value != 0U && value <= UINT16_MAX);
    return (uint16_t)value;
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

static uint64_t response_stream_uint(const response_t *response, uint64_t index,
                                     const char *key)
{
    airplay_bplist_doc_t doc;
    airplay_bplist_object_t root;
    airplay_bplist_object_t streams;
    airplay_bplist_object_t entry;
    airplay_bplist_object_t object;
    uint64_t value = 0U;
    assert(airplay_bplist_open(response->body, response->body_len, &doc) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc, root, "streams", &streams) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_array_get(&doc, streams, index, &entry) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc, entry, key, &object) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_uint(&doc, object, &value) == AIRPLAY_BPLIST_OK);
    return value;
}

static rtcp_data_stream_status_t data_status(rtcp_data_stream_kind_t kind);
static uint16_t run_initial_setup(rtcp_receiver_t *server,
                                  client_t *client, uint32_t *cseq, bool offer)
{
    uint8_t request[512];
    uint8_t body[256];
    response_t response;
    size_t body_len = offer ? build_feature_offer(body, false) : build_bplist_empty_dict(body, sizeof(body));
    size_t request_len = build_request(request, sizeof(request), "SETUP",
                                       "/stream", PLIST_TYPE, (*cseq)++, body,
                                       body_len);
    client_exchange(server, client, request, request_len, &response);
    airplay_bplist_doc_t doc;
    airplay_bplist_object_t root, features, item;
    uint64_t count;
    airplay_bplist_type_t type;
    assert(airplay_bplist_open(response.body, response.body_len, &doc) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc, root, "enabledFeatures", &features) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_type(&doc, features, &type, &count) == AIRPLAY_BPLIST_OK && type == AIRPLAY_BPLIST_ARRAY);
    bool enabled = data_status(RTCP_DATA_STREAM_VEHICLE).enabled;
    bool ui_enabled = data_status(RTCP_DATA_STREAM_UI_SYNC).enabled;
    assert(count == 2U + (enabled ? 1U : 0U) + (ui_enabled ? 1U : 0U));
    const char *expected[4] = { "iAPChannel", "viewAreas" };
    size_t nfeatures = 2;
    if (enabled)
        expected[nfeatures++] = "vehicleStateProtocol";
    if (ui_enabled)
        expected[nfeatures++] = "uiSync";
    for (uint64_t i = 0; i < count; ++i)
    {
        char text[64];
        size_t n;
        assert(airplay_bplist_array_get(&doc, features, i, &item) == AIRPLAY_BPLIST_OK);
        assert(airplay_bplist_get_string(&doc, item, text, sizeof(text), &n) == AIRPLAY_BPLIST_OK);
        assert(!strcmp(text, expected[i]));
    }
    return response_port(&response, "eventPort");
}

static size_t receive_decrypted(rtcp_receiver_t *server, int fd,
                                airplay_control_cipher_t *cipher,
                                uint8_t *plain, size_t capacity)
{
    size_t plain_len = 0U;
    uint64_t start = rtcp_os_linux.now_ms();
    while (plain_len == 0U && rtcp_os_linux.now_ms() - start < WAIT_LIMIT_MS)
    {
        receiver_poll(server, 10);
        if (!client_has_data(fd))
            continue;
        uint8_t raw[4096];
        int received = rtcp_net_linux.recv(fd, raw, sizeof(raw));
        assert(received > 0);
        size_t offset = 0U;
        while (offset < (size_t)received)
        {
            size_t consumed = 0U;
            size_t produced = 0U;
            airplay_control_cipher_result_t result =
                airplay_control_cipher_decrypt(
                    cipher, raw + offset, (size_t)received - offset, &consumed,
                    plain + plain_len, capacity - plain_len, &produced);
            offset += consumed;
            plain_len += produced;
            if (result == AIRPLAY_CONTROL_CIPHER_INCOMPLETE)
                break;
            assert(result == AIRPLAY_CONTROL_CIPHER_OK);
            if (consumed == 0U && produced == 0U)
                break;
        }
    }
    return plain_len;
}

static unsigned data_messages[RTCP_DATA_STREAM_COUNT], data_closes[RTCP_DATA_STREAM_COUNT];
static bool fail_next_listener;
static uint16_t fail_poll_port;
static int data_poll(struct rtcp_pollfd *fds, int count, int timeout)
{
    int ready = rtcp_net_linux.poll(fds, count, timeout);
    if (ready < 0)
        return ready;
    for (int i = 0; fail_poll_port && i < count; ++i)
    {
        if (rtcp_net_linux.local_port(fds[i].fd) != fail_poll_port)
            continue;
        if (!fds[i].revents)
            ++ready;
        fds[i].revents = RTCP_POLLERR;
        fail_poll_port = 0;
    }
    return ready;
}

static int data_listen(uint8_t family, uint16_t port)
{
    if (fail_next_listener)
    {
        fail_next_listener = false;
        return RTCP_EIO;
    }
    return rtcp_net_linux.listen(family, port == 7000U ? 0U : port);
}
#include "vehicle_test_wire.h"
#include "vehicle_control_test_model.h"
#include "ui_sync_test_fixtures.h"
static bool block_server_send;
static int data_send(int fd, const void *bytes, size_t length)
{
    return block_server_send ? RTCP_ETIMEDOUT : rtcp_net_linux.send(fd, bytes, length);
}
static bool capture_data(rtcp_data_stream_kind_t kind, const uint8_t *body, size_t length, void *context)
{
    assert(context == data_messages && (unsigned)kind < RTCP_DATA_STREAM_COUNT);
    assert(length == 3 && body[0] == kind && body[1] == 0xab && body[2] == 0xcd);
    ++data_messages[kind];
    uint8_t reply[] = { (uint8_t)kind, 0xc7, 0xe1 };
    assert(rtcp_receiver_send_data_stream(e2e_receiver, kind, reply, sizeof(reply)) == RTCP_OK);
    return true;
}
static void capture_data_closed(rtcp_data_stream_kind_t kind, int error, void *context)
{
    assert(context == data_messages && (unsigned)kind < RTCP_DATA_STREAM_COUNT);
    assert(error == 0 || error == RTCP_EIO);
    ++data_closes[kind];
    assert(rtcp_receiver_send_data_stream(e2e_receiver, kind, NULL, 0) == RTCP_ESTATE);
}
static rtcp_data_stream_status_t data_status(rtcp_data_stream_kind_t kind)
{
    rtcp_data_stream_status_t status;
    assert(rtcp_receiver_get_data_stream_status(e2e_receiver, kind, &status) == RTCP_OK);
    return status;
}
static void response_status_is(const response_t *r, unsigned code)
{
    char prefix[32];
    int n = snprintf(prefix, sizeof(prefix), "RTSP/1.0 %u ", code);
    assert(n > 0 && r->headers_len >= (size_t)n && !memcmp(r->headers, prefix, (size_t)n));
}
static const char *data_uuids[] = { RTCP_DATA_STREAM_VEHICLE_UUID, RTCP_DATA_STREAM_VEHICLE_HIGH_UUID,
                                    RTCP_DATA_STREAM_UI_SYNC_UUID };
static uint16_t request_data(client_t *client, uint32_t *cseq, rtcp_data_stream_kind_t kind,
                             uint64_t seed, unsigned expected_status, uint32_t *id)
{
    uint8_t body[512], request[768];
    response_t response;
    size_t length = build_data_setup(body, data_uuids[kind], seed, 9000 + kind);
    length = build_request(request, sizeof(request), "SETUP", "/stream", PLIST_TYPE,
                           (*cseq)++, body, length);
    client_exchange_raw(e2e_receiver, client, request, length, &response);
    response_status_is(&response, expected_status);
    if (expected_status != 200)
        return 0;
    assert(response_stream_uint(&response, 0, "streamConnectionID") == 9000 + (unsigned)kind);
    assert(response_stream_uint(&response, 0, "type") == 130);
    *id = (uint32_t)response_stream_uint(&response, 0, "streamID");
    assert(*id > 1);
    return response_data_port(&response);
}
static void request_data_teardown(client_t *client, uint32_t *cseq, uint32_t id)
{
    uint8_t body[256], request[512];
    response_t response;
    size_t length;
    airplay_bplist_stream_entry_t entry = { .stream_type = 130, .stream_id = id, .data_port = 1 };
    assert(airplay_bplist_build_streams_setup_response(&entry, 1, body, sizeof(body), &length) == AIRPLAY_BPLIST_OK);
    length = build_request(request, sizeof(request), "TEARDOWN", "/stream", PLIST_TYPE, (*cseq)++, body, length);
    client_exchange(e2e_receiver, client, request, length, &response);
    response_status_is(&response, 200);
}
static airplay_control_cipher_t *data_phone_cipher(const uint8_t shared[32], uint64_t seed)
{
    uint8_t read_key[32], write_key[32];
    char salt[64];
    snprintf(salt, sizeof(salt), "DataStream-Salt%llu", (unsigned long long)seed);
    hkdf_key(shared, 32, salt, "DataStream-Input-Encryption-Key", read_key);
    hkdf_key(shared, 32, salt, "DataStream-Output-Encryption-Key", write_key);
    airplay_control_cipher_t *cipher = NULL;
    assert(airplay_control_cipher_create(read_key, write_key, &cipher) == AIRPLAY_CONTROL_CIPHER_OK);
    mbedtls_platform_zeroize(read_key, sizeof(read_key));
    mbedtls_platform_zeroize(write_key, sizeof(write_key));
    return cipher;
}
static int connect_data(uint16_t port, rtcp_data_stream_kind_t kind)
{
    rtcp_addr_t address = { .family = RTCP_AF_INET, .addr = { 127, 0, 0, 1 }, .port = port };
    int fd = rtcp_net_linux.connect(&address);
    assert(fd >= 0);
    uint64_t start = rtcp_os_linux.now_ms();
    while (data_status(kind).state != RTCP_DATA_STREAM_CONNECTED &&
           rtcp_os_linux.now_ms() - start < WAIT_LIMIT_MS) assert(receiver_poll(e2e_receiver, 10) == RTCP_OK);
    assert(data_status(kind).state == RTCP_DATA_STREAM_CONNECTED);
    return fd;
}
static void data_roundtrip(int fd, airplay_control_cipher_t *cipher, rtcp_data_stream_kind_t kind, bool bad_tag)
{
    uint8_t packet[35] = { 0 }, sealed[80], plain[128];
    size_t length = 0;
    write_be32(packet, sizeof(packet));
    memcpy(packet + 4, "sync", 4);
    memcpy(packet + 16, "comm", 4);
    write_be64(packet + 20, UINT64_C(0x8000000000000011) + kind);
    packet[32] = (uint8_t)kind;
    packet[33] = 0xab;
    packet[34] = 0xcd;
    assert(airplay_control_cipher_encrypt(cipher, packet, sizeof(packet), sealed, sizeof(sealed), &length) == AIRPLAY_CONTROL_CIPHER_OK);
    if (bad_tag)
        sealed[length - 1] ^= 1;
    /* Fragment the AEAD header as well as the APTransportPackage. */
    assert(rtcp_net_linux.send(fd, sealed, 1) == 1);
    assert(receiver_poll(e2e_receiver, 0) == RTCP_OK);
    assert(rtcp_net_linux.send(fd, sealed + 1, length - 1) == (int)length - 1);
    if (bad_tag)
    {
        uint64_t start = rtcp_os_linux.now_ms();
        while (data_status(kind).state != RTCP_DATA_STREAM_FAILED &&
               rtcp_os_linux.now_ms() - start < WAIT_LIMIT_MS) receiver_poll(e2e_receiver, 10);
        assert(data_status(kind).state == RTCP_DATA_STREAM_FAILED);
        return;
    }
    size_t received = 0;
    while (received < 67)
    {
        size_t n = receive_decrypted(e2e_receiver, fd, cipher, plain + received, sizeof(plain) - received);
        assert(n);
        received += n;
    }
    assert(received == 67 && !memcmp(plain + 4, "rply", 4) && !memcmp(plain + 20, packet + 20, 8));
    assert(!memcmp(plain + 32 + 4, "asyn", 4) && !memcmp(plain + 32 + 16, "cmnd", 4));
    assert(plain[64] == kind && plain[65] == 0xc7 && plain[66] == 0xe1);
}
static void run_duplex_registry(client_t *client, const uint8_t shared[32], uint32_t *cseq)
{
    int fd[3];
    uint32_t id[3];
    uint16_t port[3];
    airplay_control_cipher_t *cipher[3];
    for (unsigned k = 0; k < RTCP_DATA_STREAM_COUNT; ++k)
    {
        rtcp_data_stream_kind_t kind = (rtcp_data_stream_kind_t)k;
        assert(data_status(kind).registered && data_status(kind).offered);
        port[k] = request_data(client, cseq, kind, 7000 + k, 200, &id[k]);
        assert(data_status(kind).state == RTCP_DATA_STREAM_LISTENING);
        fd[k] = connect_data(port[k], kind);
        cipher[k] = data_phone_cipher(shared, 7000 + k);
        data_roundtrip(fd[k], cipher[k], kind, false);
        assert(data_messages[k] == 1 && data_status(kind).sent == 2);
        for (unsigned i = 0; i < k; ++i) assert(port[k] != port[i] && id[k] != id[i]);
    }
    uint32_t duplicate;
    assert(request_data(client, cseq, RTCP_DATA_STREAM_VEHICLE, 7000, 200, &duplicate) == port[0] && duplicate == id[0]);
    request_data(client, cseq, RTCP_DATA_STREAM_VEHICLE, 7010, 503, &duplicate);
    /* Salt uniqueness spans transport kinds, not just the Type 130 registry. */
    uint8_t screen_body[256], screen_request[512];
    response_t screen_response;
    size_t screen_length = build_bplist_screen_setup(screen_body, sizeof(screen_body), 7000);
    screen_length = build_request(screen_request, sizeof(screen_request), "SETUP", "/stream",
                                  PLIST_TYPE, (*cseq)++, screen_body, screen_length);
    client_exchange_raw(e2e_receiver, client, screen_request, screen_length, &screen_response);
    response_status_is(&screen_response, 503);
    request_data_teardown(client, cseq, id[1]);
    assert(data_status(RTCP_DATA_STREAM_VEHICLE_HIGH).state == RTCP_DATA_STREAM_CLOSED);
    assert(data_status(RTCP_DATA_STREAM_VEHICLE).state == RTCP_DATA_STREAM_CONNECTED);
    assert(data_status(RTCP_DATA_STREAM_UI_SYNC).state == RTCP_DATA_STREAM_CONNECTED);
    request_data(client, cseq, RTCP_DATA_STREAM_VEHICLE_HIGH, 7000, 503, &duplicate);
    /* Even a listener creation failure consumes its salt and stream ID. */
    fail_next_listener = true;
    request_data(client, cseq, RTCP_DATA_STREAM_VEHICLE_HIGH, 7100, 503, &duplicate);
    assert(!fail_next_listener && data_status(RTCP_DATA_STREAM_VEHICLE_HIGH).error == RTCP_EIO);
    request_data(client, cseq, RTCP_DATA_STREAM_VEHICLE_HIGH, 7100, 503, &duplicate);
    rtcp_net_linux.close(fd[1]);
    airplay_control_cipher_destroy(cipher[1]);
    port[1] = request_data(client, cseq, RTCP_DATA_STREAM_VEHICLE_HIGH, 7101, 200, &duplicate);
    assert(duplicate > id[2]);
    id[1] = duplicate;
    fd[1] = connect_data(port[1], RTCP_DATA_STREAM_VEHICLE_HIGH);
    cipher[1] = data_phone_cipher(shared, 7101);
    data_roundtrip(fd[1], cipher[1], RTCP_DATA_STREAM_VEHICLE_HIGH, false);
    request_data_teardown(client, cseq, 3); /* stale ID cannot retire its replacement */
    assert(data_status(RTCP_DATA_STREAM_VEHICLE_HIGH).state == RTCP_DATA_STREAM_CONNECTED);
    data_roundtrip(fd[0], cipher[0], RTCP_DATA_STREAM_VEHICLE, true);
    assert(data_messages[0] == 1 && data_closes[0] == 1);
    assert(data_status(RTCP_DATA_STREAM_VEHICLE_HIGH).state == RTCP_DATA_STREAM_CONNECTED);
    request_data(client, cseq, RTCP_DATA_STREAM_VEHICLE, 7000, 503, &duplicate);
    rtcp_net_linux.close(fd[0]);
    airplay_control_cipher_destroy(cipher[0]);
    port[0] = request_data(client, cseq, RTCP_DATA_STREAM_VEHICLE, 7200, 200, &duplicate);
    assert(duplicate > id[1]);
    id[0] = duplicate;
    fd[0] = connect_data(port[0], RTCP_DATA_STREAM_VEHICLE);
    cipher[0] = data_phone_cipher(shared, 7200);
    data_roundtrip(fd[0], cipher[0], RTCP_DATA_STREAM_VEHICLE, false);
    request_data_teardown(client, cseq, 0); /* all type-130 channels, not video/event */
    for (unsigned k = 0; k < RTCP_DATA_STREAM_COUNT; ++k)
    {
        assert(data_status((rtcp_data_stream_kind_t)k).state == RTCP_DATA_STREAM_CLOSED);
        rtcp_net_linux.close(fd[k]);
        airplay_control_cipher_destroy(cipher[k]);
    }
    assert(data_closes[0] == 2 && data_closes[1] == 2 && data_closes[2] == 1);
    assert(receiver_connected(e2e_receiver) && receiver_authenticated(e2e_receiver));
    /* Leave one never-connected listener owned by this RTSP session. Screen
     * shutdown below must close it exactly once; the next controller sees idle. */
    request_data(client, cseq, RTCP_DATA_STREAM_UI_SYNC, 7300, 200, &duplicate);
    assert(data_status(RTCP_DATA_STREAM_UI_SYNC).state == RTCP_DATA_STREAM_LISTENING);
}

static void vehicle_exchange(int fd, airplay_control_cipher_t *cipher,
                             const char *request_command, bool write, int64_t value, const char *response_command,
                             int64_t expected_value)
{
    uint8_t packet[1024] = { 0 }, sealed[1200], plain[4096];
    size_t length;
    bool control = !strcmp(request_command, "controlRequest");
    size_t body;
    if (control)
    {
        body = sizeof(control_mixed);
        memcpy(packet + 32, control_mixed, body);
    }
    else
        body = vehicle_test_request(request_command, write, value, packet + 32, sizeof(packet) - 32);
    write_be32(packet, (uint32_t)(body + 32));
    memcpy(packet + 4, "sync", 4);
    memcpy(packet + 16, "comm", 4);
    write_be64(packet + 20, 0x9988776655443322ULL);
    assert(airplay_control_cipher_encrypt(cipher, packet, body + 32, sealed, sizeof(sealed), &length) == AIRPLAY_CONTROL_CIPHER_OK);
    assert(rtcp_net_linux.send(fd, sealed, length) == (int)length);
    size_t received = 0, needed = 64;
    while (received < needed)
    {
        size_t n = receive_decrypted(e2e_receiver, fd, cipher, plain + received, sizeof(plain) - received);
        assert(n);
        received += n;
        if (received >= 36)
        {
            needed = 32 + ((size_t)plain[32] << 24 | (size_t)plain[33] << 16 |
                           (size_t)plain[34] << 8 | plain[35]);
            assert(needed <= sizeof(plain));
        }
    }
    assert(received == needed && !memcmp(plain + 4, "rply", 4) && !memcmp(plain + 20, packet + 20, 8));
    assert(!memcmp(plain + 36, "asyn", 4) && !memcmp(plain + 48, "cmnd", 4));
    vehicle_test_reply(plain + 64, received - 64, response_command, true, expected_value);
    if (control)
        vehicle_control_test_reply(plain + 64, received - 64);
}
static void run_vehicle_processor(client_t *client, const uint8_t shared[32], uint32_t *cseq,
                                  rtcp_vehicle_t *vehicle, bool raw_ui)
{
    uint32_t id[2];
    int fd[2];
    airplay_control_cipher_t *cipher[2];
    for (unsigned k = 0; k < 2; ++k)
    {
        uint16_t port = request_data(client, cseq, (rtcp_data_stream_kind_t)k, 7400 + k, 200, &id[k]);
        fd[k] = connect_data(port, (rtcp_data_stream_kind_t)k);
        cipher[k] = data_phone_cipher(shared, 7400 + k);
    }
    vehicle_exchange(fd[0], cipher[0], "configRequest", false, 0, "configResponse", -1);
    rtcp_vehicle_status_t model_status;
    assert(rtcp_vehicle_status(vehicle, &model_status) == RTCP_OK);
    if (model_status.controls)
    {
        unsigned before = control_commits;
        vehicle_exchange(fd[0], cipher[0], "controlRequest", false, 0, "controlResponse", -1);
        vehicle_exchange(fd[1], cipher[1], "controlRequest", false, 0, "controlResponse", -1);
        assert(control_commits == before + 4);
    }
    vehicle_exchange(fd[0], cipher[0], "readRequest", false, 0, "readResponse", 7);
    vehicle_exchange(fd[1], cipher[1], "writeRequest", true, 41, "writeResponse", -1);
    vehicle_exchange(fd[0], cipher[0], "registerRequest", false, 0, "registerResponse", 41);
    request_data_teardown(client, cseq, id[1]);
    rtcp_net_linux.close(fd[1]);
    airplay_control_cipher_destroy(cipher[1]);
    rtcp_vehicle_value_t value = { .format = RTCP_VEHICLE_UINT32, .as.integer = 42 };
    assert(rtcp_vehicle_set(vehicle, 3, &value) == RTCP_OK);
    assert(receiver_poll(e2e_receiver, 0) == RTCP_OK);
    rtcp_vehicle_delivery_status_t status;
    assert(rtcp_receiver_get_vehicle_delivery_status(e2e_receiver, &status) == RTCP_OK);
    assert(status.attached && status.pending_notification_bytes);
    uint16_t port = request_data(client, cseq, RTCP_DATA_STREAM_VEHICLE_HIGH, 7410, 200, &id[1]);
    fd[1] = connect_data(port, RTCP_DATA_STREAM_VEHICLE_HIGH);
    cipher[1] = data_phone_cipher(shared, 7410);
    uint8_t plain[1024];
    size_t received = 0, needed = 32;
    while (received < needed)
    {
        size_t n = receive_decrypted(e2e_receiver, fd[1], cipher[1], plain + received, sizeof(plain) - received);
        assert(n);
        received += n;
        if (received >= 4)
            needed = (size_t)plain[0] << 24 | (size_t)plain[1] << 16 | (size_t)plain[2] << 8 | plain[3];
        assert(needed >= 32 && needed <= sizeof(plain));
    }
    assert(received == needed && !memcmp(plain + 4, "asyn", 4));
    vehicle_test_reply(plain + 32, received - 32, "updateNotify", false, 42);
    vehicle_exchange(fd[0], cipher[0], "readRequest", false, 0, "readResponse", 42);
    /* An independently registered raw UI backend still routes through its own
     * callback/context while the vehicle processor owns the two vehicle lanes. */
    if (raw_ui)
    {
        uint32_t ui_id;
        uint16_t ui_port = request_data(client, cseq, RTCP_DATA_STREAM_UI_SYNC, 7420, 200, &ui_id);
        int ui_fd = connect_data(ui_port, RTCP_DATA_STREAM_UI_SYNC);
        airplay_control_cipher_t *ui_cipher = data_phone_cipher(shared, 7420);
        data_roundtrip(ui_fd, ui_cipher, RTCP_DATA_STREAM_UI_SYNC, false);
        request_data_teardown(client, cseq, ui_id);
        rtcp_net_linux.close(ui_fd);
        airplay_control_cipher_destroy(ui_cipher);
        assert(data_messages[2] == 1 && data_closes[2] == 1);
    }
    if (model_status.controls)
    {
        uint8_t packet[512] = { 0 }, sealed[600];
        size_t bytes;
        write_be32(packet, sizeof(control_late_invalid) + 32);
        memcpy(packet + 4, "asyn", 4);
        memcpy(packet + 16, "comm", 4);
        memcpy(packet + 32, control_late_invalid, sizeof(control_late_invalid));
        unsigned before = control_commits;
        assert(airplay_control_cipher_encrypt(cipher[0], packet, sizeof(control_late_invalid) + 32, sealed, sizeof(sealed), &bytes) == AIRPLAY_CONTROL_CIPHER_OK);
        assert(rtcp_net_linux.send(fd[0], sealed, bytes) == (int)bytes);
        uint64_t start = rtcp_os_linux.now_ms();
        while (data_status(RTCP_DATA_STREAM_VEHICLE).state != RTCP_DATA_STREAM_FAILED && rtcp_os_linux.now_ms() - start < WAIT_LIMIT_MS)
            receiver_poll(e2e_receiver, 10);
        assert(data_status(RTCP_DATA_STREAM_VEHICLE).state == RTCP_DATA_STREAM_FAILED && control_commits == before);
        vehicle_exchange(fd[1], cipher[1], "readRequest", false, 0, "readResponse", 42);
        assert(receiver_connected(e2e_receiver) && receiver_authenticated(e2e_receiver));
        rtcp_net_linux.close(fd[0]);
        airplay_control_cipher_destroy(cipher[0]);
        uint16_t reopened = request_data(client, cseq, RTCP_DATA_STREAM_VEHICLE, 7450, 200, &id[0]);
        fd[0] = connect_data(reopened, RTCP_DATA_STREAM_VEHICLE);
        cipher[0] = data_phone_cipher(shared, 7450);
        vehicle_exchange(fd[0], cipher[0], "controlRequest", false, 0, "controlResponse", -1);
        assert(control_commits == before + 2);
    }
    request_data_teardown(client, cseq, 0);
    for (unsigned k = 0; k < 2; ++k)
    {
        rtcp_net_linux.close(fd[k]);
        airplay_control_cipher_destroy(cipher[k]);
    }
    rtcp_vehicle_status_t vs;
    assert(rtcp_vehicle_status(vehicle, &vs) == RTCP_OK && vs.subscribed == 1 && vs.committed_writes == 1);
}

static uint32_t ui_package_size(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void send_ui_fixture(int fd, airplay_control_cipher_t *cipher, unsigned fixture, bool sync, bool bad_tag)
{
    uint8_t packet[RTCP_UI_SYNC_MAX_MESSAGE + 32] = { 0 }, sealed[9000];
    size_t length;
    assert(fixture < sizeof(ui_fixtures) / sizeof(ui_fixtures[0]) && ui_fixtures[fixture].length <= RTCP_UI_SYNC_MAX_MESSAGE);
    rtcp_opack_node_t nodes[128];
    size_t count, body;
    assert(rtcp_opack_decode(ui_fixtures[fixture].bytes, ui_fixtures[fixture].length, nodes, 128, &count) == RTCP_OPACK_OK);
    size_t display = rtcp_opack_dict_get(nodes, count, 0, "displayUUID");
    assert(display != SIZE_MAX);
    const char *uuid = "b7e6c5a0-1111-4000-8000-000000000001";
    nodes[display].value.blob.bytes = (const uint8_t *)uuid;
    nodes[display].value.blob.length = strlen(uuid);
    assert(rtcp_opack_encode(nodes, count, packet + 32, sizeof(packet) - 32, &body) == RTCP_OPACK_OK);
    write_be32(packet, (uint32_t)(body + 32));
    memcpy(packet + 4, sync ? "sync" : "asyn", 4);
    memcpy(packet + 16, "comm", 4);
    write_be64(packet + 20, UINT64_C(0x8877665544332211));
    assert(airplay_control_cipher_encrypt(cipher, packet, body + 32, sealed, sizeof(sealed), &length) == AIRPLAY_CONTROL_CIPHER_OK);
    if (bad_tag)
        sealed[length - 1] ^= 1;
    assert(rtcp_net_linux.send(fd, sealed, length) == (int)length);
}
static void ui_exchange(int fd, airplay_control_cipher_t *cipher, unsigned fixture, const unsigned *types, unsigned outputs)
{
    static uint8_t plain[RTCP_UI_SYNC_MAX_OUTPUTS * (RTCP_UI_SYNC_MAX_MESSAGE + 32) + 32];
    static rtcp_opack_node_t frame[512];
    send_ui_fixture(fd, cipher, fixture, true, false);
    size_t received = 0, at = 0;
    unsigned packages = 0;
    while (packages < outputs + 1)
    {
        size_t n = receive_decrypted(e2e_receiver, fd, cipher, plain + received, sizeof(plain) - received);
        assert(n);
        received += n;
        while (received - at >= 32)
        {
            size_t size = ui_package_size(plain + at);
            assert(size >= 32 && size <= RTCP_UI_SYNC_MAX_MESSAGE + 32);
            if (received - at < size)
                break;
            if (!packages)
            {
                assert(size == 32 && !memcmp(plain + at + 4, "rply", 4));
                const uint8_t token[] = { 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 };
                assert(!memcmp(plain + at + 20, token, sizeof(token)));
            }
            else
            {
                assert(packages <= outputs && !memcmp(plain + at + 4, "asyn", 4) && !memcmp(plain + at + 16, "cmnd", 4));
                size_t count;
                assert(rtcp_opack_decode(plain + at + 32, size - 32, frame, 512, &count) == RTCP_OPACK_OK);
                size_t data = rtcp_opack_dict_get(frame, count, 0, "data");
                assert(data != SIZE_MAX);
                airplay_bplist_doc_t d;
                airplay_bplist_object_t root, type;
                uint64_t value;
                assert(airplay_bplist_open(frame[data].value.blob.bytes, frame[data].value.blob.length, &d) == AIRPLAY_BPLIST_OK);
                assert(airplay_bplist_root(&d, &root) == AIRPLAY_BPLIST_OK);
                assert(airplay_bplist_dict_get_unique(&d, root, "type", &type) == AIRPLAY_BPLIST_OK);
                assert(airplay_bplist_get_uint(&d, type, &value) == AIRPLAY_BPLIST_OK && value == types[packages - 1]);
            }
            at += size;
            ++packages;
        }
    }
    assert(received == at);
}
static rtcp_ui_sync_status_t ui_status(rtcp_ui_sync_t *ui)
{
    rtcp_ui_sync_status_t st;
    assert(rtcp_ui_sync_status(ui, &st) == RTCP_OK);
    return st;
}
static void ui_handshake(int fd, airplay_control_cipher_t *cipher, rtcp_ui_sync_t *ui)
{
    const unsigned reset[] = { 0 }, complete[] = { 1, 2, 2, 2 };
    ui_exchange(fd, cipher, UI_FIXTURE_reset, reset, 1);
    ui_exchange(fd, cipher, UI_FIXTURE_complete, complete, 4);
    assert(ui_status(ui).state == RTCP_UI_SYNC_READY && ui_status(ui).version == 2 && !ui_status(ui).pending_outputs);
}
static void run_ui_processor(client_t *client, const uint8_t shared[32], uint32_t *cseq, rtcp_ui_sync_t *ui)
{
    uint32_t vehicle_id, id;
    uint16_t vehicle_port = request_data(client, cseq, RTCP_DATA_STREAM_VEHICLE, 7600, 200, &vehicle_id);
    int vehicle_fd = connect_data(vehicle_port, RTCP_DATA_STREAM_VEHICLE);
    airplay_control_cipher_t *vehicle_cipher = data_phone_cipher(shared, 7600);
    uint16_t port = request_data(client, cseq, RTCP_DATA_STREAM_UI_SYNC, 7500, 200, &id);
    int fd = connect_data(port, RTCP_DATA_STREAM_UI_SYNC);
    airplay_control_cipher_t *cipher = data_phone_cipher(shared, 7500);
    assert(rtcp_receiver_send_data_stream(e2e_receiver, RTCP_DATA_STREAM_UI_SYNC, NULL, 0) == RTCP_ESTATE);
    ui_handshake(fd, cipher, ui);
    const unsigned ack[] = { 3 };
    ui_exchange(fd, cipher, UI_FIXTURE_basic, ack, 1);
    assert(ui_status(ui).last_rx == 2);
    vehicle_exchange(vehicle_fd, vehicle_cipher, "readRequest", false, 0, "readResponse", 42);

    /* With outgoing TCP writes temporarily blocked, seven application messages
     * occupy the transport FIFO and the eighth remains in the model. A ninth
     * request must not apply a layout choice before rejecting this UI lane. */
    block_server_send = true;
    for (unsigned i = 0; i < 8; ++i)
    {
        uint64_t before = ui_status(ui).received;
        send_ui_fixture(fd, cipher, UI_FIXTURE_basic, false, false);
        uint64_t start = rtcp_os_linux.now_ms();
        while (ui_status(ui).received == before && rtcp_os_linux.now_ms() - start < WAIT_LIMIT_MS)
            assert(receiver_poll(e2e_receiver, 10) == RTCP_OK);
        assert(ui_status(ui).received == before + 1 && data_status(RTCP_DATA_STREAM_UI_SYNC).state == RTCP_DATA_STREAM_CONNECTED);
    }
    assert(ui_status(ui).pending_outputs == 1 && data_status(RTCP_DATA_STREAM_UI_SYNC).queued_packages == 7);
    uint64_t before = ui_status(ui).received;
    send_ui_fixture(fd, cipher, UI_FIXTURE_layout, false, false);
    uint64_t start = rtcp_os_linux.now_ms();
    while (data_status(RTCP_DATA_STREAM_UI_SYNC).state != RTCP_DATA_STREAM_FAILED && rtcp_os_linux.now_ms() - start < WAIT_LIMIT_MS)
        assert(receiver_poll(e2e_receiver, 10) == RTCP_OK);
    block_server_send = false;
    assert(data_status(RTCP_DATA_STREAM_UI_SYNC).state == RTCP_DATA_STREAM_FAILED);
    assert(ui_status(ui).state == RTCP_UI_SYNC_IDLE && ui_status(ui).received == before && !ui_status(ui).phone_layout[0]);
    assert(ui_status(ui).abandoned_messages == 1 && !ui_status(ui).pending_outputs);
    rtcp_ui_sync_delivery_status_t delivery;
    assert(rtcp_receiver_get_ui_sync_delivery_status(e2e_receiver, &delivery) == RTCP_OK);
    assert(delivery.attached && delivery.rejected_requests == 1 && delivery.queue_retries > 0);
    assert(delivery.last_rejection == RTCP_ENOMEM);
    assert(data_status(RTCP_DATA_STREAM_VEHICLE).state == RTCP_DATA_STREAM_CONNECTED);
    vehicle_exchange(vehicle_fd, vehicle_cipher, "readRequest", false, 0, "readResponse", 42);
    rtcp_net_linux.close(fd);
    airplay_control_cipher_destroy(cipher);

    port = request_data(client, cseq, RTCP_DATA_STREAM_UI_SYNC, 7510, 200, &id);
    fd = connect_data(port, RTCP_DATA_STREAM_UI_SYNC);
    cipher = data_phone_cipher(shared, 7510);
    ui_handshake(fd, cipher, ui);
    ui_exchange(fd, cipher, UI_FIXTURE_no_ack, NULL, 0);
    assert(ui_status(ui).phone_appearance == RTCP_UI_SYNC_DARK && ui_status(ui).pending_choices == 1);
    rtcp_ui_sync_choice_t choice;
    assert(rtcp_ui_sync_peek_choice(ui, &choice) == RTCP_OK && choice.kind == RTCP_UI_SYNC_CHOICE_APPEARANCE);
    assert(choice.appearance == RTCP_UI_SYNC_DARK && choice.sequence == 7 && choice.session == 1);
    assert(choice.generation == ui_status(ui).choice_generation && rtcp_ui_sync_consume_choice(ui) == RTCP_OK);
    uint64_t consumed_generation = choice.generation;
    send_ui_fixture(fd, cipher, UI_FIXTURE_basic, true, true);
    start = rtcp_os_linux.now_ms();
    while (data_status(RTCP_DATA_STREAM_UI_SYNC).state != RTCP_DATA_STREAM_FAILED && rtcp_os_linux.now_ms() - start < WAIT_LIMIT_MS)
        assert(receiver_poll(e2e_receiver, 10) == RTCP_OK);
    assert(data_status(RTCP_DATA_STREAM_UI_SYNC).state == RTCP_DATA_STREAM_FAILED && ui_status(ui).state == RTCP_UI_SYNC_IDLE);
    assert(data_status(RTCP_DATA_STREAM_VEHICLE).state == RTCP_DATA_STREAM_CONNECTED);
    rtcp_net_linux.close(fd);
    airplay_control_cipher_destroy(cipher);
    vehicle_exchange(vehicle_fd, vehicle_cipher, "readRequest", false, 0, "readResponse", 42);
    /* Authenticated commands must also respect the installed host inventory.
     * A valid envelope carrying an unsupported layout retires only UI. */
    port = request_data(client, cseq, RTCP_DATA_STREAM_UI_SYNC, 7530, 200, &id);
    fd = connect_data(port, RTCP_DATA_STREAM_UI_SYNC);
    cipher = data_phone_cipher(shared, 7530);
    ui_handshake(fd, cipher, ui);
    assert(rtcp_ui_sync_get_inventory(ui) && rtcp_ui_sync_get_inventory(ui)->layout_count == 2);
    send_ui_fixture(fd, cipher, UI_FIXTURE_inventory_unknown_layout, false, false);
    start = rtcp_os_linux.now_ms();
    while (data_status(RTCP_DATA_STREAM_UI_SYNC).state != RTCP_DATA_STREAM_FAILED && rtcp_os_linux.now_ms() - start < WAIT_LIMIT_MS)
        assert(receiver_poll(e2e_receiver, 10) == RTCP_OK);
    assert(data_status(RTCP_DATA_STREAM_UI_SYNC).state == RTCP_DATA_STREAM_FAILED);
    assert(rtcp_receiver_get_ui_sync_delivery_status(e2e_receiver, &delivery) == RTCP_OK);
    assert(delivery.last_rejection == RTCP_ENOTSUP && delivery.rejected_requests == 2);
    assert(ui_status(ui).state == RTCP_UI_SYNC_IDLE && !ui_status(ui).phone_layout[0]);
    assert(rtcp_ui_sync_get_inventory(ui) && data_status(RTCP_DATA_STREAM_VEHICLE).state == RTCP_DATA_STREAM_CONNECTED);
    rtcp_net_linux.close(fd);
    airplay_control_cipher_destroy(cipher);
    vehicle_exchange(vehicle_fd, vehicle_cipher, "readRequest", false, 0, "readResponse", 42);
    port = request_data(client, cseq, RTCP_DATA_STREAM_UI_SYNC, 7531, 200, &id);
    fd = connect_data(port, RTCP_DATA_STREAM_UI_SYNC);
    cipher = data_phone_cipher(shared, 7531);
    ui_handshake(fd, cipher, ui);
    ui_exchange(fd, cipher, UI_FIXTURE_layout, ack, 1);
    ui_exchange(fd, cipher, UI_FIXTURE_palette, ack, 1);
    assert(!strcmp(ui_status(ui).phone_layout, "DB7C6944") && !strcmp(ui_status(ui).phone_palette, "red"));
    assert(ui_status(ui).choice_generation > consumed_generation && ui_status(ui).pending_choices == 3);
    for (unsigned i = 0; i < 3; ++i)
    {
        assert(rtcp_ui_sync_peek_choice(ui, &choice) == RTCP_OK && !strcmp(choice.layout_id, "DB7C6944"));
        assert(choice.kind == (i ? RTCP_UI_SYNC_CHOICE_PALETTE : RTCP_UI_SYNC_CHOICE_LAYOUT));
        assert(choice.sequence == (i == 2 ? 4U : 3U) && choice.session == 1);
        assert(choice.generation == ui_status(ui).choice_generation);
        if (i)
            assert(!strcmp(choice.palette_id, i == 2 ? "red" : "dark"));
        assert(rtcp_ui_sync_consume_choice(ui) == RTCP_OK);
    }
    consumed_generation = choice.generation;
    request_data_teardown(client, cseq, id);
    rtcp_net_linux.close(fd);
    airplay_control_cipher_destroy(cipher);

    /* A slow host renderer can fill its copied-choice FIFO even while the
     * transport drains normally. Refuse the next command before application
     * state/ACK commit, retire only UI, and count the undelivered choices. */
    port = request_data(client, cseq, RTCP_DATA_STREAM_UI_SYNC, 7532, 200, &id);
    fd = connect_data(port, RTCP_DATA_STREAM_UI_SYNC);
    cipher = data_phone_cipher(shared, 7532);
    ui_handshake(fd, cipher, ui);
    assert(ui_status(ui).choice_generation > consumed_generation);
    for (unsigned i = 0; i < RTCP_UI_SYNC_MAX_CHOICES; ++i)
        ui_exchange(fd, cipher, UI_FIXTURE_choice_burst_00 + i, ack, i % 2 ? 0 : 1);
    rtcp_ui_sync_status_t queued = ui_status(ui);
    assert(queued.pending_choices == RTCP_UI_SYNC_MAX_CHOICES && !queued.pending_outputs);
    send_ui_fixture(fd, cipher, UI_FIXTURE_choice_burst_16, false, false);
    start = rtcp_os_linux.now_ms();
    while (data_status(RTCP_DATA_STREAM_UI_SYNC).state != RTCP_DATA_STREAM_FAILED && rtcp_os_linux.now_ms() - start < WAIT_LIMIT_MS)
        assert(receiver_poll(e2e_receiver, 10) == RTCP_OK);
    assert(data_status(RTCP_DATA_STREAM_UI_SYNC).state == RTCP_DATA_STREAM_FAILED);
    assert(rtcp_receiver_get_ui_sync_delivery_status(e2e_receiver, &delivery) == RTCP_OK);
    assert(delivery.last_rejection == RTCP_ENOMEM && delivery.rejected_requests == 3);
    assert(!ui_status(ui).pending_choices && ui_status(ui).abandoned_choices == queued.abandoned_choices + RTCP_UI_SYNC_MAX_CHOICES);
    assert(ui_status(ui).phone_choices == queued.phone_choices && ui_status(ui).choice_generation > queued.choice_generation);
    assert(rtcp_ui_sync_peek_choice(ui, &choice) == RTCP_ENOENT);
    assert(data_status(RTCP_DATA_STREAM_VEHICLE).state == RTCP_DATA_STREAM_CONNECTED);
    vehicle_exchange(vehicle_fd, vehicle_cipher, "readRequest", false, 0, "readResponse", 42);
    rtcp_net_linux.close(fd);
    airplay_control_cipher_destroy(cipher);
    request_data_teardown(client, cseq, vehicle_id);
    rtcp_net_linux.close(vehicle_fd);
    airplay_control_cipher_destroy(vehicle_cipher);
    /* Session destruction must also reset an unconnected fresh UI listener. */
    request_data(client, cseq, RTCP_DATA_STREAM_UI_SYNC, 7520, 200, &id);
}

static void verify_appearance(rtcp_receiver_t *server, int fd, airplay_control_cipher_t *peer,
                              rtcp_appearance_setting_t setting, bool ui_night, bool map_night,
                              bool expected_ui, bool expected_map)
{
    assert(rtcp_receiver_set_appearance(server, setting, ui_night, map_night) == RTCP_OK);
    uint8_t plain[1024];
    size_t available = 0;
    for (unsigned command = 0; command < 2; ++command)
    {
        airplay_rtsp_request_t request;
        while (airplay_rtsp_parse_request(plain, available, &request) != AIRPLAY_RTSP_PARSE_COMPLETE)
        {
            size_t received = receive_decrypted(server, fd, peer, plain + available, sizeof(plain) - available);
            assert(received > 0);
            available += received;
        }
        assert(!strcmp(request.method, "POST") && !strcmp(request.path, "/command"));

        airplay_bplist_doc_t doc;
        airplay_bplist_object_t root, field, params;
        char text[40];
        size_t length;
        assert(airplay_bplist_open(request.body, request.body_len, &doc) == AIRPLAY_BPLIST_OK);
        assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);
        assert(airplay_bplist_dict_get(&doc, root, "type", &field) == AIRPLAY_BPLIST_OK);
        assert(airplay_bplist_get_string(&doc, field, text, sizeof(text), &length) == AIRPLAY_BPLIST_OK);
        assert(!strcmp(text, command == 0 ? "uiAppearanceUpdate" : "setNightMode"));
        assert(airplay_bplist_dict_get(&doc, root, "params", &params) == AIRPLAY_BPLIST_OK);
        if (command == 0)
        {
            uint64_t value;
            assert(airplay_bplist_dict_get(&doc, params, "uuid", &field) == AIRPLAY_BPLIST_OK);
            assert(airplay_bplist_get_string(&doc, field, text, sizeof(text), &length) == AIRPLAY_BPLIST_OK);
            assert(!strcmp(text, "b7e6c5a0-1111-4000-8000-000000000001"));
            assert(airplay_bplist_dict_get(&doc, params, "appearanceMode", &field) == AIRPLAY_BPLIST_OK);
            assert(airplay_bplist_get_uint(&doc, field, &value) == AIRPLAY_BPLIST_OK && value == expected_ui);
            assert(airplay_bplist_dict_get(&doc, params, "appearanceSetting", &field) == AIRPLAY_BPLIST_OK);
            assert(airplay_bplist_get_uint(&doc, field, &value) == AIRPLAY_BPLIST_OK && value == (unsigned)setting);
        }
        else
        {
            bool value;
            assert(airplay_bplist_dict_get(&doc, params, "nightMode", &field) == AIRPLAY_BPLIST_OK);
            assert(airplay_bplist_get_bool(&doc, field, &value) == AIRPLAY_BPLIST_OK && value == expected_map);
        }
        available -= request.consumed;
        memmove(plain, plain + request.consumed, available);
    }
    assert(available == 0);
}

static void expect_hid_report(rtcp_receiver_t *server, int fd, airplay_control_cipher_t *peer,
                              const char *expected_uid, const uint8_t *expected_report, size_t expected_len)
{
    uint8_t plain[512];
    size_t plain_len = receive_decrypted(server, fd, peer, plain, sizeof(plain));
    airplay_rtsp_request_t request;
    assert(airplay_rtsp_parse_request(plain, plain_len, &request) == AIRPLAY_RTSP_PARSE_COMPLETE);
    assert(!strcmp(request.method, "POST") && !strcmp(request.path, "/command") && request.consumed == plain_len);

    airplay_bplist_doc_t doc;
    airplay_bplist_object_t root, field;
    char uid[16];
    size_t length;
    const uint8_t *report;
    assert(airplay_bplist_open(request.body, request.body_len, &doc) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&doc, &root) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&doc, root, "uuid", &field) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_string(&doc, field, uid, sizeof(uid), &length) == AIRPLAY_BPLIST_OK);
    assert(!strcmp(uid, expected_uid));
    assert(airplay_bplist_dict_get(&doc, root, "hidReport", &field) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_data(&doc, field, &report, &length) == AIRPLAY_BPLIST_OK);
    assert(length == expected_len && memcmp(report, expected_report, length) == 0);
}

static void run_event_channel(rtcp_receiver_t *server,
                              const uint8_t shared_secret[KEY_BYTES],
                              uint16_t event_port, bool poll_error)
{
    static const uint8_t hid_report[12] = {
        0xde,
        0xad,
        0xbe,
        0xef,
        0x01,
        0x02,
        0x03,
        0x04,
        0x05,
        0x06,
        0x07,
        0x08,
    };
    /* Before the controller attaches there is no event channel to write. */
    assert(rtcp_receiver_set_appearance(server, RTCP_APPEARANCE_LIGHT, false, false) == RTCP_ESTATE);
    assert(!rtcp_receiver_send_hid_report(e2e_receiver, 0x2a2a2a2aU,
                                          hid_report,
                                          sizeof(hid_report)));
    assert(!rtcp_receiver_send_hid_report(NULL, 0x2a2a2a2aU, hid_report,
                                          sizeof(hid_report)));

    rtcp_addr_t address = { 0 };
    address.family = RTCP_AF_INET;
    address.addr[0] = 127U;
    address.addr[3] = 1U;
    address.port = event_port;
    int event_fd = rtcp_net_linux.connect(&address);
    assert(event_fd >= 0);

    uint64_t start = rtcp_os_linux.now_ms();
    while (!event_connected &&
           rtcp_os_linux.now_ms() - start < WAIT_LIMIT_MS)
    {
        receiver_poll(server, 10);
    }
    assert(event_connected);

    /* The peer reads what the accessory writes (Events-Write) and writes
     * with Events-Read. */
    uint8_t peer_read_key[KEY_BYTES];
    uint8_t peer_write_key[KEY_BYTES];
    hkdf_key(shared_secret, KEY_BYTES, "Events-Salt",
             "Events-Write-Encryption-Key", peer_read_key);
    hkdf_key(shared_secret, KEY_BYTES, "Events-Salt",
             "Events-Read-Encryption-Key", peer_write_key);
    airplay_control_cipher_t *peer = NULL;
    assert(airplay_control_cipher_create(peer_read_key, peer_write_key,
                                         &peer) ==
           AIRPLAY_CONTROL_CIPHER_OK);

    assert(rtcp_receiver_send_hid_report(e2e_receiver, 0x2a2a2a2aU, hid_report,
                                         sizeof(hid_report)));

    uint8_t plain[2048];
    size_t plain_len = receive_decrypted(server, event_fd, peer, plain,
                                         sizeof(plain));
    assert(plain_len > 0U);
    assert(contains(plain, plain_len, "POST /command"));
    assert(contains(plain, plain_len, "hidSendReport"));
    assert(contains(plain, plain_len, "hidReport"));

    const rtcp_touch_contact_t contacts[] = { { 300, 150, true }, { 401, 257, true } };
    const uint8_t touch[] = { 0, 1, 44, 1, 150, 0, 1, 1, 145, 1, 1, 1 };
    assert(rtcp_receiver_send_touch(server, contacts));
    expect_hid_report(server, event_fd, peer, "2a2a2a2a", touch, sizeof(touch));
    const rtcp_knob_report_t invalid_knob = { .wheel = INT8_MIN };
    assert(!rtcp_receiver_send_knob(server, &invalid_knob));
    const rtcp_knob_report_t knob = { .select = true, .back = true, .x = -1, .y = 1, .wheel = -2 };
    const uint8_t knob_report[] = { 5, 255, 1, 254 };
    assert(rtcp_receiver_send_knob(server, &knob));
    expect_hid_report(server, event_fd, peer, "2a2a2a2b", knob_report, sizeof(knob_report));
    assert(!rtcp_receiver_send_media_button(server, (rtcp_media_button_t)7));
    const uint8_t next[] = { 4 }, release[] = { 0 };
    assert(rtcp_receiver_send_media_button(server, RTCP_MEDIA_NEXT));
    expect_hid_report(server, event_fd, peer, "2a2a2a2c", next, sizeof(next));
    assert(rtcp_receiver_send_media_button(server, RTCP_MEDIA_RELEASE));
    expect_hid_report(server, event_fd, peer, "2a2a2a2c", release, sizeof(release));

    assert(rtcp_receiver_set_appearance(server, (rtcp_appearance_setting_t)3, false, false) == RTCP_EINVAL);
    verify_appearance(server, event_fd, peer, RTCP_APPEARANCE_AUTOMATIC, true, false, true, false);
    verify_appearance(server, event_fd, peer, RTCP_APPEARANCE_LIGHT, true, true, false, false);
    verify_appearance(server, event_fd, peer, RTCP_APPEARANCE_DARK, false, false, true, true);

    if (poll_error)
        fail_poll_port = event_port;
    else
        rtcp_net_linux.close(event_fd);
    start = rtcp_os_linux.now_ms();
    while (event_connected && rtcp_os_linux.now_ms() - start < WAIT_LIMIT_MS)
    {
        receiver_poll(server, 10);
    }
    assert(!event_connected);
    if (poll_error)
    {
        assert(fail_poll_port == 0 && receiver_connected(server));
        rtcp_net_linux.close(event_fd);
    }
    assert(rtcp_receiver_set_appearance(server, RTCP_APPEARANCE_DARK, false, false) == RTCP_ESTATE);

    airplay_control_cipher_destroy(peer);
    mbedtls_platform_zeroize(peer_read_key, sizeof(peer_read_key));
    mbedtls_platform_zeroize(peer_write_key, sizeof(peer_write_key));
}

static uint32_t run_screen_stream(rtcp_receiver_t *server, client_t *client,
                              const uint8_t shared_secret[KEY_BYTES],
                              uint32_t *cseq)
{
    uint8_t request[512];
    uint8_t body[256];
    response_t response;
    size_t request_len;
    size_t body_len;

    assert(rtcp_receiver_get_projection_stream_generation(server) == 0U);
    static const uint64_t connection_id = UINT64_C(0x1122334455667788);
    body_len = build_bplist_screen_setup(body, sizeof(body), connection_id);
    request_len = build_request(request, sizeof(request), "SETUP", "/stream",
                                PLIST_TYPE, (*cseq)++, body, body_len);
    client_exchange(server, client, request, request_len, &response);
    uint16_t data_port = response_data_port(&response);

    request_len = build_request(request, sizeof(request), "RECORD", "/stream",
                                PLIST_TYPE, (*cseq)++, NULL, 0U);
    client_exchange(server, client, request, request_len, &response);

    expect_receiver_state(true, true, true, true, true);
    uint32_t generation = rtcp_receiver_get_projection_stream_generation(server);
    assert(generation != 0U);
    uint8_t output_key[KEY_BYTES];
    assert(airplay_screen_derive_output_key(shared_secret, KEY_BYTES,
                                            connection_id, output_key) ==
           RTCP_OK);

    rtcp_addr_t address = { 0 };
    address.family = RTCP_AF_INET;
    address.addr[0] = 127U;
    address.addr[3] = 1U;
    address.port = data_port;
    int screen_fd = rtcp_net_linux.connect(&address);
    assert(screen_fd >= 0);

    static const uint8_t nal[] = { 0x65U, 0x11U, 0x22U, 0x33U, 0x44U };
    uint8_t plaintext[4U + sizeof(nal)];
    plaintext[0] = 0U;
    plaintext[1] = 0U;
    plaintext[2] = 0U;
    plaintext[3] = (uint8_t)sizeof(nal);
    memcpy(plaintext + 4U, nal, sizeof(nal));

    uint8_t header[AIRPLAY_SCREEN_HEADER_BYTES] = { 0 };
    write_le32(header, (uint32_t)(sizeof(plaintext) +
                                  AIRPLAY_SCREEN_TAG_BYTES));
    header[4] = 0U; /* video */
    static const uint64_t ntp = UINT64_C(0x0123456789abcdef);
    write_le64(header + 8U, ntp);

    mbedtls_chachapoly_context cipher;
    mbedtls_chachapoly_init(&cipher);
    assert(mbedtls_chachapoly_setkey(&cipher, output_key) == 0);
    unsigned frames_before = screen_frames;
    for (unsigned frame_index = 0U; frame_index < 2U; ++frame_index)
    {
        uint8_t nonce[12] = { 0 };
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
        while (screen_frames == frames_before + frame_index &&
               rtcp_os_linux.now_ms() - start < WAIT_LIMIT_MS)
        {
            receiver_poll(server, 10);
        }
        assert(screen_frames == frames_before + frame_index + 1U);
    }
    mbedtls_chachapoly_free(&cipher);

    static const uint8_t start_code[4] = { 0U, 0U, 0U, 1U };
    assert(screen_frame_len == 4U + sizeof(nal));
    assert(memcmp(screen_frame, start_code, sizeof(start_code)) == 0);
    assert(memcmp(screen_frame + 4U, nal, sizeof(nal)) == 0);
    uint64_t expected_ns =
        (ntp >> 32U) * UINT64_C(1000000000) +
        (((ntp & UINT64_C(0xffffffff)) * UINT64_C(1000000000)) >> 32U);
    assert(screen_sender_time_ns == expected_ns);
    assert(rtcp_receiver_get_projection_stream_generation(server) == generation);

    rtcp_net_linux.close(screen_fd);
    mbedtls_platform_zeroize(output_key, sizeof(output_key));
    return generation;
}

/* Public Receiver bench: genuine authenticated control and concurrent encrypted
 * TCP streams. No second-display metadata or altScreen negotiation is supplied. */
static rtcp_alt_video_status_t alt_status(rtcp_receiver_t *r)
{
    rtcp_alt_video_status_t out;
    assert(rtcp_receiver_get_alt_video_status(r, &out) == RTCP_OK);
    return out;
}
static uint16_t video_request(rtcp_receiver_t *r, client_t *c, uint32_t *seq,
                              const char *method, uint8_t type, uint64_t id, unsigned expected)
{
    uint8_t body[256], request[512];
    response_t response;
    size_t n = build_bplist_video_setup(body, sizeof(body), id, type);
    n = build_request(request, sizeof(request), method, "/stream", PLIST_TYPE, (*seq)++, body, n);
    client_exchange_raw(r, c, request, n, &response);
    response_status_is(&response, expected);
    if (expected != 200 || strcmp(method, "SETUP"))
        return 0;
    assert(response_stream_uint(&response, 0, "type") == type);
    return response_data_port(&response);
}
static int video_connect(uint16_t port)
{
    rtcp_addr_t addr = { .family = RTCP_AF_INET, .port = port, .addr = { 127, 0, 0, 1 } };
    int fd = rtcp_net_linux.connect(&addr);
    assert(fd >= 0);
    return fd;
}
static void video_send(int fd, const uint8_t *secret, uint64_t id,
                       uint64_t counter, uint8_t marker, bool tamper)
{
    uint8_t key[32], nonce[12] = { 0 }, header[AIRPLAY_SCREEN_HEADER_BYTES] = { 0 };
    uint8_t plain[] = { 0, 0, 0, 2, 0x65, marker }, wire[sizeof(plain) + AIRPLAY_SCREEN_TAG_BYTES];
    assert(airplay_screen_derive_output_key(secret, KEY_BYTES, id, key) == RTCP_OK);
    write_le32(header, sizeof(wire));
    write_le64(nonce + 4, counter);
    mbedtls_chachapoly_context cipher;
    mbedtls_chachapoly_init(&cipher);
    assert(!mbedtls_chachapoly_setkey(&cipher, key));
    assert(!mbedtls_chachapoly_encrypt_and_tag(&cipher, sizeof(plain), nonce, header, sizeof(header), plain, wire, wire + sizeof(plain)));
    if (tamper)
        wire[sizeof(wire) - 1] ^= 1;
    assert(rtcp_net_linux.send(fd, header, sizeof(header)) == (int)sizeof(header));
    assert(rtcp_net_linux.send(fd, wire, sizeof(wire)) == (int)sizeof(wire));
    mbedtls_chachapoly_free(&cipher);
    mbedtls_platform_zeroize(key, sizeof(key));
}
static void video_wait(rtcp_receiver_t *r, unsigned main_frames, unsigned alternate_frames, bool ready)
{
    uint64_t start = rtcp_os_linux.now_ms();
    while ((screen_frames != main_frames || alt_status(r).screen.frames != alternate_frames ||
            alt_status(r).ready != ready) &&
           rtcp_os_linux.now_ms() - start < WAIT_LIMIT_MS) receiver_poll(r, 10);
    assert(screen_frames == main_frames);
    if (alt_status(r).screen.frames != alternate_frames || alt_status(r).ready != ready)
        fprintf(stderr, "alternate wait: frames=%u expected=%u ready=%d expected=%d opened=%u closed=%u\n",
                alt_status(r).screen.frames, alternate_frames, alt_status(r).ready, ready,
                alt_status(r).opened, alt_status(r).closed);
    assert(alt_status(r).screen.frames == alternate_frames && alt_status(r).ready == ready);
    assert(receiver_connected(r));
}
static void run_alt_video(rtcp_receiver_t *r, client_t *c, const uint8_t *secret, uint32_t *seq, bool null_sink)
{
    unsigned base = screen_frames;
    uint16_t main_port = video_request(r, c, seq, "SETUP", 110, 9001, 200);
    uint16_t alt_port = video_request(r, c, seq, "SETUP", 111, 9002, 200);
    assert(main_port != alt_port && alt_status(r).configured && alt_status(r).opened == 1);
    assert(video_request(r, c, seq, "SETUP", 111, 9002, 200) == alt_port); /* idempotent */
    int main_fd = video_connect(main_port), alt_fd = video_connect(alt_port);
    for (unsigned i = 0; i < 3; ++i)
    {
        video_send(alt_fd, secret, 9002, i, 0xbb, false);
        video_send(main_fd, secret, 9001, i, 0xaa, false);
        video_wait(r, base + i + 1, i + 1, true);
        assert(screen_frame_len == 6 && screen_frame[5] == 0xaa);
    }
    assert(alt_frames == (null_sink ? 0U : 3U));
    rtcp_receiver_diagnostics_t diag;
    assert(!rtcp_receiver_get_diagnostics(r, &diag) && diag.screen.frames == 3);
    assert(alt_status(r).screen.wire_bytes == diag.screen.wire_bytes);
    /* A stale selective teardown cannot close this active key. */
    video_request(r, c, seq, "TEARDOWN", 111, 8999, 200);
    assert(alt_status(r).ready);
    video_request(r, c, seq, "TEARDOWN", 111, 9002, 200);
    assert(!alt_status(r).ready && alt_status(r).closed == 1 && alt_status(r).screen.frames == 3);
    rtcp_net_linux.close(alt_fd);
    video_request(r, c, seq, "SETUP", 111, 9002, 503); /* retired ID */
    fail_next_listener = true;
    video_request(r, c, seq, "SETUP", 111, 9003, 503);
    assert(alt_status(r).last_error == RTCP_EIO && !alt_status(r).ready);
    video_request(r, c, seq, "SETUP", 111, 9003, 503); /* failed open also retired */
    alt_port = video_request(r, c, seq, "SETUP", 111, 9004, 200);
    alt_fd = video_connect(alt_port);
    video_send(alt_fd, secret, 9001, 0, 0xbb, false); /* Main key on alternate must fail */
    video_send(main_fd, secret, 9001, 3, 0xaa, false);
    video_wait(r, base + 4, 0, false);
    assert(alt_status(r).last_error == RTCP_EIO && alt_status(r).closed == 2);
    rtcp_net_linux.close(alt_fd);
    video_request(r, c, seq, "SETUP", 111, 9004, 503);
    alt_port = video_request(r, c, seq, "SETUP", 111, 9005, 200);
    alt_fd = video_connect(alt_port);
    video_send(alt_fd, secret, 9005, 0, 0xbb, true); /* bad authentication tag */
    video_wait(r, base + 4, 0, false);
    rtcp_net_linux.close(alt_fd);
    alt_port = video_request(r, c, seq, "SETUP", 111, 9006, 200);
    alt_fd = video_connect(alt_port);
    video_send(alt_fd, secret, 9006, 0, 0xbb, false);
    video_wait(r, base + 4, 1, true);
    rtcp_net_linux.close(alt_fd);
    video_wait(r, base + 4, 1, false); /* EOF isolated */
    assert(alt_status(r).opened == 4 && alt_status(r).closed == 4);
    assert(alt_frames == (null_sink ? 0U : 4U));
    alt_port = video_request(r, c, seq, "SETUP", 111, 9007, 200);
    fail_poll_port = alt_port; /* error-only listener readiness must never accept */
    receiver_poll(r, 0);
    assert(!fail_poll_port && !alt_status(r).ready && alt_status(r).last_error == RTCP_EIO);
    assert(alt_status(r).opened == 5 && alt_status(r).closed == 5);
    video_send(main_fd, secret, 9001, 4, 0xaa, false);
    video_wait(r, base + 5, 0, false);
    video_request(r, c, seq, "TEARDOWN", 110, 9001, 200);
    rtcp_net_linux.close(main_fd);
    extra_main_frames += 5;
}

static void write_be32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

/* One SETUP plus one encrypted RTP datagram through the real server poll
 * loop, proving the audio endpoint is bound, keyed and wired to the sink. */
static void run_audio_stream(rtcp_receiver_t *server, client_t *client,
                             const uint8_t shared_secret[KEY_BYTES],
                             uint32_t *cseq, bool media)
{
    uint64_t connection_id = UINT64_C(0x0102030405060708) + (media ? 1U : 0U);
    static const uint64_t format_bits = 0x800U; /* 44.1 kHz stereo LPCM */
    uint8_t request[512];
    uint8_t body[256];
    response_t response;
    size_t body_len = build_bplist_audio_setup(body, sizeof(body), connection_id,
                                               format_bits, media);
    size_t request_len = build_request(request, sizeof(request), "SETUP",
                                       "/stream", PLIST_TYPE, (*cseq)++, body,
                                       body_len);
    client_exchange(server, client, request, request_len, &response);
    uint16_t data_port = (uint16_t)response_stream_uint(&response, 0U, "dataPort");
    uint16_t control_port =
        (uint16_t)response_stream_uint(&response, 0U, "controlPort");
    assert(data_port != 0U && control_port != 0U);
    assert(response_stream_uint(&response, 0U, "streamConnectionID") ==
           connection_id);

    expect_receiver_state(true, true, false, false, true);
    uint8_t output_key[KEY_BYTES];
    assert(airplay_screen_derive_output_key(shared_secret, KEY_BYTES,
                                            connection_id, output_key) ==
           RTCP_OK);

    int fd = rtcp_net_linux.udp_open(RTCP_AF_INET, 0U, false);
    assert(fd >= 0);
    rtcp_addr_t address = { 0 };
    address.family = RTCP_AF_INET;
    address.addr[0] = 127U;
    address.addr[3] = 1U;
    address.port = data_port;

    static const uint8_t pcm[] = {
        0x10,
        0x20,
        0x30,
        0x40,
        0x50,
        0x60,
        0x70,
        0x80,
    };
    uint8_t header[AIRPLAY_AUDIO_RTP_HEADER_BYTES] = { 0x80U, 0x60U, 0x00U, 0x01U };
    write_be32(header + 4U, 4800U);
    write_be32(header + 8U, 0x11223344U);
    uint8_t nonce[12] = { 0 };
    nonce[4] = (uint8_t)(audio_packets + 1U); /* fresh nonce on duplicate SETUP */

    mbedtls_chachapoly_context cipher;
    mbedtls_chachapoly_init(&cipher);
    assert(mbedtls_chachapoly_setkey(&cipher, output_key) == 0);
    uint8_t wire[AIRPLAY_AUDIO_OVERHEAD_BYTES + sizeof(pcm)];
    memcpy(wire, header, sizeof(header));
    assert(mbedtls_chachapoly_encrypt_and_tag(
               &cipher, sizeof(pcm), nonce, header + 4U, 8U, pcm,
               wire + AIRPLAY_AUDIO_RTP_HEADER_BYTES,
               wire + AIRPLAY_AUDIO_RTP_HEADER_BYTES + sizeof(pcm)) == 0);
    mbedtls_chachapoly_free(&cipher);
    const size_t nonce_at = sizeof(wire) - AIRPLAY_AUDIO_NONCE_BYTES;
    memcpy(wire + nonce_at, nonce + 4U, AIRPLAY_AUDIO_NONCE_BYTES);

    assert(rtcp_net_linux.udp_sendto(fd, &address, wire, sizeof(wire)) ==
           (int)sizeof(wire));
    unsigned packets_before = audio_packets;
    uint64_t start = rtcp_os_linux.now_ms();
    while (audio_packets == packets_before &&
           rtcp_os_linux.now_ms() - start < WAIT_LIMIT_MS)
    {
        receiver_poll(server, 10);
    }
    assert(audio_packets == packets_before + 1U);
    assert(audio_packets_stream_type == 100U);
    assert(audio_pcm_len == sizeof(pcm));
    for (size_t i = 0; i < sizeof(pcm); i += 2)
    {
        assert(audio_pcm[i] == pcm[i + 1]);
        assert(audio_pcm[i + 1] == pcm[i]);
    }
    assert(audio_packet_format.sample_rate == 44100U);
    assert(audio_packet_format.channels == 2U);
    assert(audio_packet_format.stream_slot_valid && audio_packet_format.stream_slot == (media ? 7U : 0U));
    assert(audio_packet_timestamp == 4800U);
    rtcp_receiver_diagnostics_t diagnostics;
    assert(rtcp_receiver_get_diagnostics(server, &diagnostics) == RTCP_OK);
    assert(diagnostics.audio.active_streams >= 1 && diagnostics.audio.datagrams >= 1 &&
           !diagnostics.audio.auth_failures);

    rtcp_net_linux.close(fd);
    mbedtls_platform_zeroize(output_key, sizeof(output_key));
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

typedef struct
{
    airplay_info_plist_config_t config;
    uint8_t *bytes;
    size_t *length;
    bool omit, omit_ui;
    unsigned calls;
} vehicle_info_refresh_t;
static bool refresh_vehicle_info(const uint8_t **bytes, size_t *length, void *context)
{
    vehicle_info_refresh_t *r = context;
    ++r->calls;
    airplay_info_plist_config_t config = r->config;
    if (r->omit)
        config.vehicle = NULL;
    if (r->omit_ui)
        config.ui_sync = NULL;
    if (!airplay_info_plist_build(&config, r->bytes, AIRPLAY_INFO_PLIST_MAX_BYTES, r->length))
        return false;
    *bytes = r->bytes;
    *length = *r->length;
    return true;
}

int main(int argc, char **argv)
{
    bool null_alt = argc == 2 && !strcmp(argv[1], "--alt-video-null");
    bool alt_mode = null_alt || (argc == 2 && !strcmp(argv[1], "--alt-video"));
    bool controls_mode = argc == 2 && !strcmp(argv[1], "--vehicle-controls");
    bool ui_mode = argc == 2 && !strcmp(argv[1], "--ultra-apps");
    bool vehicle_mode = controls_mode || ui_mode || (argc == 2 && !strcmp(argv[1], "--vehicle"));
    rtcp_vehicle_t *vehicle = controls_mode ? vehicle_control_test_create() : vehicle_mode ? vehicle_test_create()
                                                                                           : NULL;
    rtcp_ui_sync_t *ui = NULL;
    if (ui_mode)
    {
        rtcp_ui_sync_config_t config = { .struct_size = sizeof(config), .display_uuid = "b7e6c5a0-1111-4000-8000-000000000001", .allow_layout_change = true, .give_focus = true, .focus_token = 12345, .queue_phone_choices = true };
        assert(rtcp_ui_sync_create(&config, &ui) == RTCP_OK);
        const char *palettes[] = { "blue" }, *remote_palettes[] = { "dark", "red" };
        rtcp_ui_sync_inventory_layout_t installed[] = {
            { "0D38B526", palettes, 1, NULL, 0 },
            { "DB7C6944", remote_palettes, 2, NULL, 0 }
        };
        rtcp_ui_sync_inventory_t inventory = { .struct_size = sizeof(inventory), .theme_id = "synthetic-e2e:1", .panel_uid = "synthetic-panel", .display_uuid = "b7e6c5a0-1111-4000-8000-000000000001", .width_pixels = 1920, .height_pixels = 1080, .width_physical_mm = 200, .height_physical_mm = 120, .layouts = installed, .layout_count = 2 };
        assert(rtcp_ui_sync_bind_inventory(ui, &inventory) == RTCP_OK);
        rtcp_ui_sync_layout_t layout = { .id = "0D38B526", .palette = "blue" };
        assert(rtcp_ui_sync_select(ui, &layout) == RTCP_OK);
    }
    fixture_t fixture = { 0 };
    assert(airplay_ed25519_keypair_from_seed(ACCESSORY_SEED,
                                             &fixture.accessory));
    assert(airplay_ed25519_keypair_from_seed(CONTROLLER_SEED,
                                             &fixture.controller));

    uint8_t info_plist[INFO_PLIST_BYTES];
    airplay_info_plist_config_t info = { 0 };
    info.hid_knob = true;
    info.hid_media = true;
    info.device_id = ACCESSORY_ID;
    info.name = "rt-carplay e2e";
    info.model = "AppleTV6,2";
    info.width_pixels = 1920U;
    info.height_pixels = 1080U;
    info.max_fps = 30U;
    info.vehicle = vehicle;
    info.ui_sync = ui;
    size_t info_len = 0U;
    assert(airplay_info_plist_build(&info, info_plist, sizeof(info_plist),
                                    &info_len));

    vehicle_info_refresh_t info_refresh = { .config = info, .bytes = info_plist, .length = &info_len };
    rtcp_receiver_config_t receiver_config = { 0 };
    struct rtcp_net registry_net = rtcp_net_linux;
    registry_net.listen = data_listen;
    registry_net.poll = data_poll;
    registry_net.send = data_send;
    receiver_config.platform.os = &rtcp_os_linux;
    receiver_config.platform.net = &registry_net;
    fill_pairing(&fixture, &receiver_config.platform.pairing);
    receiver_config.platform.info_plist = info_plist;
    receiver_config.platform.info_plist_len = info_len;
    receiver_config.experimental_alt_video = alt_mode;
    receiver_config.media.on_alt_video = null_alt ? NULL : capture_alt;
    receiver_config.media.alt_video_context = &alt_context;
    receiver_config.media.on_video = capture_screen;
    receiver_config.media.video_context = &video_context;
    receiver_config.media.on_audio = capture_audio;
    receiver_config.media.audio_context = &audio_context;
    receiver_config.media.on_volume = capture_volume;
    receiver_config.media.volume_context = &volume_context;
    receiver_config.on_event_state = capture_event_state;
    receiver_config.event_state_context = &event_context;
    receiver_config.data_stream = (rtcp_data_stream_backend_t){
        .channel_mask = RTCP_DATA_STREAM_ALL_MASK,
        .on_body = capture_data,
        .on_closed = capture_data_closed,
        .context = data_messages
    };

    if (vehicle_mode)
    {
        receiver_config.vehicle = vehicle;
        receiver_config.negotiate_vehicle = true;
        receiver_config.platform.get_info_plist = refresh_vehicle_info;
        receiver_config.platform.info_context = &info_refresh;
        receiver_config.data_stream.channel_mask = RTCP_DATA_STREAM_MASK(RTCP_DATA_STREAM_UI_SYNC);
        if (ui_mode)
        {
            receiver_config.ui_sync = ui;
            receiver_config.negotiate_ui_sync = true;
            receiver_config.data_stream.channel_mask = 0;
        }
    }
    else if (argc > 1 && !alt_mode)
    {
        assert(argc == 2 && strcmp(argv[1], "--legacy-config") == 0);
        /* Execute the same encrypted exchanges through the migration API.
         * Populate the original contract independently of the native one. */
        airplay_rtsp_server_config_t legacy = {
            .os = &rtcp_os_linux,
            .net = &registry_net,
            .info_plist = info_plist,
            .info_plist_len = info_len,
            .on_screen_message = capture_screen,
            .screen_message_context = &video_context,
            .on_audio_pcm = capture_audio,
            .audio_pcm_context = &audio_context,
            .on_volume = capture_volume,
            .volume_context = &volume_context,
            .on_event_state = capture_event_state,
            .event_state_context = &event_context,
            .data_stream = { .channel_mask = RTCP_DATA_STREAM_ALL_MASK, .on_body = capture_data, .on_closed = capture_data_closed, .context = data_messages },
        };
        fill_pairing(&fixture, &legacy.pairing);
        assert(rtcp_receiver_config_from_airplay(&legacy, &receiver_config) == RTCP_OK);
    }

    rtcp_receiver_t *server = NULL;
    assert(rtcp_receiver_create(&receiver_config, &e2e_receiver) == RTCP_OK);
    memset(&receiver_config, 0, sizeof(receiver_config)); /* Descriptors are copied. */
    fail_next_listener = true;
    assert(rtcp_receiver_start(e2e_receiver) == RTCP_EIO);
    assert(rtcp_receiver_port(e2e_receiver) == 0);
    assert(rtcp_receiver_stop(e2e_receiver) == RTCP_OK);
    assert(rtcp_receiver_start(e2e_receiver) == RTCP_OK);
    server = e2e_receiver;
    assert(server != NULL);
    assert(alt_status(server).configured == alt_mode && !alt_status(server).ready);

    client_t client;
    client_connect(server, &client);
    expect_receiver_state(true, false, false, false, false);
    uint32_t guard_cseq = 900, guard_id = 0;
    if (alt_mode)
    {
        video_request(server, &client, &guard_cseq, "SETUP", 111, 8990, 503);
        assert(!alt_status(server).ready && !alt_status(server).opened);
    }
    request_data(&client, &guard_cseq, RTCP_DATA_STREAM_VEHICLE, 7000, 503, &guard_id);
    run_pair_setup(server, &client, &fixture);
    expect_receiver_state(true, false, false, false, false);
    uint8_t shared_secret[KEY_BYTES];
    run_pair_verify(server, &client, &fixture, shared_secret);
    expect_receiver_state(true, true, false, false, false);
    run_encrypted_info(server, &client, info_plist, info_len);
    request_data(&client, &guard_cseq, RTCP_DATA_STREAM_VEHICLE, 7000, 503, &guard_id);
    /* A malformed feature array must not create event/timing/data endpoints. */
    uint8_t invalid_offer[256], invalid_request[512];
    response_t invalid_response;
    size_t invalid_length = build_feature_offer(invalid_offer, true);
    invalid_length = build_request(invalid_request, sizeof(invalid_request), "SETUP", "/stream",
                                   PLIST_TYPE, guard_cseq++, invalid_offer, invalid_length);
    client_exchange_raw(server, &client, invalid_request, invalid_length, &invalid_response);
    response_status_is(&invalid_response, 400);
    if (alt_mode)
    {
        video_request(server, &client, &guard_cseq, "SETUP", 111, 8991, 503);
        assert(!alt_status(server).opened);
    }
    uint32_t cseq = 30U;
    uint16_t event_port = run_initial_setup(server, &client, &cseq, true);
    assert(data_status(RTCP_DATA_STREAM_VEHICLE).enabled == vehicle_mode);
    assert(data_status(RTCP_DATA_STREAM_VEHICLE_HIGH).enabled == vehicle_mode);
    assert(data_status(RTCP_DATA_STREAM_UI_SYNC).enabled == ui_mode);
    if (vehicle_mode)
        assert(run_initial_setup(server, &client, &cseq, false) == event_port);
    run_event_channel(server, shared_secret, event_port, false);
    if (vehicle_mode)
        run_vehicle_processor(&client, shared_secret, &cseq, vehicle, !ui_mode);
    else
        run_duplex_registry(&client, shared_secret, &cseq);
    if (ui_mode)
        run_ui_processor(&client, shared_secret, &cseq, ui);
    if (alt_mode)
        run_alt_video(server, &client, shared_secret, &cseq, null_alt);
    /* Audio runs before the screen stream: closing the screen socket ends the
     * session, so the RTSP connection must still be up for the audio SETUP. */
    run_audio_stream(server, &client, shared_secret, &cseq, false);
    expect_receiver_state(true, true, false, false, true);
    run_audio_stream(server, &client, shared_secret, &cseq, true);
    /* Reusing the existing default endpoint still works alongside media. */
    run_audio_stream(server, &client, shared_secret, &cseq, false);
    uint32_t first_screen_generation = run_screen_stream(server, &client, shared_secret, &cseq);
    mbedtls_platform_zeroize(shared_secret, sizeof(shared_secret));
    run_tampered_frame(server, &client);
    expect_receiver_state(false, false, false, false, false);
    assert(rtcp_receiver_get_projection_stream_generation(server) == 0U);
    if (vehicle_mode)
    {
        rtcp_vehicle_status_t status;
        rtcp_vehicle_value_t value;
        bool present;
        assert(rtcp_vehicle_status(vehicle, &status) == RTCP_OK && !status.subscribed && !status.pending_notifications);
        assert(rtcp_vehicle_get(vehicle, 3, &value, &present) == RTCP_OK && present && value.as.integer == 42);
    }
    if (ui_mode)
        assert(ui_status(ui).state == RTCP_UI_SYNC_IDLE && !ui_status(ui).pending_outputs);

    /* The malformed frame dropped only that connection.  A reconnecting
     * controller must Pair-Verify against the pairing stored by the first
     * Pair-Setup and run a complete second session on the same server. */
    if (vehicle_mode)
    {
        /* A refreshed Classic-only document must reject the new controller,
         * rather than silently negotiate with stale Vehicle metadata. */
        info_refresh.omit = true;
        unsigned calls = info_refresh.calls;
        rtcp_addr_t address = { .family = RTCP_AF_INET, .port = rtcp_receiver_port(server), .addr = { 127, 0, 0, 1 } };
        int fd = rtcp_net_linux.connect(&address);
        assert(fd >= 0);
        uint64_t start = rtcp_os_linux.now_ms();
        while (info_refresh.calls == calls && rtcp_os_linux.now_ms() - start < WAIT_LIMIT_MS)
            receiver_poll(server, 10);
        assert(info_refresh.calls > calls && !receiver_connected(server));
        rtcp_net_linux.close(fd);
        info_refresh.omit = false;
    }
    if (ui_mode)
    {
        /* Vehicle metadata still matches; missing UI metadata alone must refuse
         * this new controller before authentication or endpoint allocation. */
        info_refresh.omit_ui = true;
        unsigned calls = info_refresh.calls;
        rtcp_addr_t address = { .family = RTCP_AF_INET, .port = rtcp_receiver_port(server), .addr = { 127, 0, 0, 1 } };
        int fd = rtcp_net_linux.connect(&address);
        assert(fd >= 0);
        uint64_t start = rtcp_os_linux.now_ms();
        while (info_refresh.calls == calls && rtcp_os_linux.now_ms() - start < WAIT_LIMIT_MS)
            receiver_poll(server, 10);
        assert(info_refresh.calls > calls && !receiver_connected(server));
        rtcp_net_linux.close(fd);
        info_refresh.omit_ui = false;
    }
    client_t client2;
    client_connect(server, &client2);
    uint8_t shared_secret2[KEY_BYTES];
    run_pair_verify(server, &client2, &fixture, shared_secret2);
    run_encrypted_info(server, &client2, info_plist, info_len);
    cseq = 60U;
    uint16_t event_port2 = run_initial_setup(server, &client2, &cseq, false);
    assert(!data_status(RTCP_DATA_STREAM_VEHICLE).enabled);
    assert(!data_status(RTCP_DATA_STREAM_UI_SYNC).enabled);
    request_data(&client2, &guard_cseq, RTCP_DATA_STREAM_UI_SYNC, 8000, 503, &guard_id);
    assert(data_status(RTCP_DATA_STREAM_UI_SYNC).registered && !data_status(RTCP_DATA_STREAM_UI_SYNC).offered);
    assert(data_status(RTCP_DATA_STREAM_UI_SYNC).state == RTCP_DATA_STREAM_IDLE);
    run_event_channel(server, shared_secret2, event_port2, true);
    run_set_parameter_volume(server, &client2, &cseq);
    run_now_playing(server, &client2, &cseq);
    run_audio_stream(server, &client2, shared_secret2, &cseq, false);
    if (alt_mode)
    {
        video_request(server, &client2, &cseq, "SETUP", 111, 9010, 200);
        assert(alt_status(server).ready);
    }
    uint32_t second_screen_generation = run_screen_stream(server, &client2, shared_secret2, &cseq);
    assert(second_screen_generation != first_screen_generation);
    if (alt_mode)
    {
        uint64_t start = rtcp_os_linux.now_ms();
        while (receiver_connected(server) && rtcp_os_linux.now_ms() - start < WAIT_LIMIT_MS) receiver_poll(server, 10);
        assert(!receiver_connected(server) && !alt_status(server).ready);
        assert(alt_status(server).opened == 6 && alt_status(server).closed == 6);
        assert(rtcp_receiver_stop(server) == RTCP_OK);
        assert(alt_status(server).configured && !alt_status(server).opened && !alt_status(server).closed);
        assert(rtcp_receiver_start(server) == RTCP_OK);
        assert(alt_status(server).configured && !alt_status(server).ready && !alt_status(server).opened);
    }
    mbedtls_platform_zeroize(shared_secret2, sizeof(shared_secret2));

    if (client2.cipher != NULL)
        airplay_control_cipher_destroy(client2.cipher);
    rtcp_net_linux.close(client2.fd);
    if (client.cipher != NULL)
        airplay_control_cipher_destroy(client.cipher);
    rtcp_net_linux.close(client.fd);
    assert(rtcp_receiver_destroy(e2e_receiver) == RTCP_OK);
    e2e_receiver = NULL;
    rtcp_vehicle_destroy(vehicle);
    rtcp_ui_sync_destroy(ui);
    airplay_ed25519_keypair_clear(&fixture.accessory);
    airplay_ed25519_keypair_clear(&fixture.controller);
    mbedtls_platform_zeroize(&fixture, sizeof(fixture));

    assert(screen_messages == 4U + extra_main_frames);
    assert(screen_frames == 4U + extra_main_frames);
    assert(audio_packets == 4U);
    assert(volume_events == 1U);
    assert(event_transitions == 4U);
    if (!vehicle_mode)
        assert(data_closes[0] == 2 && data_closes[1] == 2 && data_closes[2] == 2);
    if (controls_mode)
        puts("Vehicle controls: authenticated normal/HIGH commands, response values, invalid-batch isolation, fresh-lane recovery and Classic media PASS");
    if (alt_mode)
        puts("alternate video: authenticated concurrent independent H.264 TCP, teardown/reuse, listener failure, wrong key, bad tag, EOF, Main-loss and stop/start passed");
    puts(ui_mode ? "airplay Ultra applications: authenticated Vehicle State and UI Sync, four-message negotiation, queue exhaustion/reopen, channel isolation and Classic media passed" : vehicle_mode ? "airplay vehicle end-to-end: authenticated config/read/write/register, HIGH notification/reopen, session reset and Classic media passed"
                                                                                                                                                                                                       : "airplay pairing end-to-end host test passed (three duplex Type 130 channels, offer/auth gates, salt retirement, independent teardown)");
    return 0;
}
