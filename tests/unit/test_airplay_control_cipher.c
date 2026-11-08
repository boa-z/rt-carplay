/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_control_cipher.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define FRAME_BYTES (AIRPLAY_CONTROL_CIPHER_MAX_FRAME_BYTES + \
                     AIRPLAY_CONTROL_CIPHER_OVERHEAD_BYTES)

static void make_keys(uint8_t read_key[32], uint8_t write_key[32])
{
    for (size_t i = 0U; i < 32U; ++i) {
        read_key[i] = (uint8_t)(0x10U + i);
        write_key[i] = (uint8_t)(0x80U + i);
    }
}

static size_t encrypt_one(airplay_control_cipher_t *cipher,
                          const void *plaintext,
                          size_t plaintext_len,
                          uint8_t *frame)
{
    size_t frame_len = 0U;
    assert(airplay_control_cipher_encrypt(cipher, plaintext, plaintext_len,
                                          frame, FRAME_BYTES,
                                          &frame_len) ==
           AIRPLAY_CONTROL_CIPHER_OK);
    assert(frame_len == plaintext_len + AIRPLAY_CONTROL_CIPHER_OVERHEAD_BYTES);
    assert(((size_t)frame[0] | ((size_t)frame[1] << 8U)) == plaintext_len);
    return frame_len;
}

static void test_round_trip(void)
{
    uint8_t read_key[32];
    uint8_t write_key[32];
    make_keys(read_key, write_key);

    /* The receiver reads with read_key and writes with write_key; the peer
     * holds the mirror image. */
    airplay_control_cipher_t *receiver = NULL;
    airplay_control_cipher_t *peer = NULL;
    assert(airplay_control_cipher_create(read_key, write_key, &receiver) ==
           AIRPLAY_CONTROL_CIPHER_OK);
    assert(airplay_control_cipher_create(write_key, read_key, &peer) ==
           AIRPLAY_CONTROL_CIPHER_OK);

    static const uint8_t message[] = "RTSP/1.0 200 OK\r\nCSeq: 9\r\n\r\n";
    uint8_t frame[FRAME_BYTES];
    size_t frame_len = encrypt_one(peer, message, sizeof(message) - 1U, frame);

    uint8_t plaintext[256];
    size_t consumed = 0U;
    size_t plaintext_len = 0U;
    assert(airplay_control_cipher_decrypt(receiver, frame, frame_len, &consumed,
                                          plaintext, sizeof(plaintext),
                                          &plaintext_len) ==
           AIRPLAY_CONTROL_CIPHER_OK);
    assert(consumed == frame_len);
    assert(plaintext_len == sizeof(message) - 1U);
    assert(memcmp(plaintext, message, plaintext_len) == 0);

    /* The reply direction uses the mirrored write key. */
    frame_len = encrypt_one(receiver, message, sizeof(message) - 1U, frame);
    assert(airplay_control_cipher_decrypt(peer, frame, frame_len, &consumed,
                                          plaintext, sizeof(plaintext),
                                          &plaintext_len) ==
           AIRPLAY_CONTROL_CIPHER_OK);
    assert(plaintext_len == sizeof(message) - 1U);
    assert(memcmp(plaintext, message, plaintext_len) == 0);

    airplay_control_cipher_destroy(peer);
    airplay_control_cipher_destroy(receiver);
}

static void test_fragmented_input(void)
{
    uint8_t read_key[32];
    uint8_t write_key[32];
    make_keys(read_key, write_key);
    airplay_control_cipher_t *receiver = NULL;
    airplay_control_cipher_t *peer = NULL;
    assert(airplay_control_cipher_create(read_key, write_key, &receiver) ==
           AIRPLAY_CONTROL_CIPHER_OK);
    assert(airplay_control_cipher_create(write_key, read_key, &peer) ==
           AIRPLAY_CONTROL_CIPHER_OK);

    static const uint8_t first[] = "first";
    static const uint8_t second[] = "second message";
    uint8_t frame[FRAME_BYTES];
    size_t first_len = encrypt_one(peer, first, sizeof(first) - 1U, frame);
    size_t second_len = encrypt_one(peer, second, sizeof(second) - 1U,
                                    frame + first_len);
    size_t total = first_len + second_len;

    uint8_t plaintext[256];
    size_t produced = 0U;
    for (size_t i = 0U; i < total; ++i) {
        size_t consumed = 0U;
        size_t plaintext_len = 0U;
        airplay_control_cipher_result_t result =
            airplay_control_cipher_decrypt(receiver, frame + i, 1U, &consumed,
                                           plaintext + produced,
                                           sizeof(plaintext) - produced,
                                           &plaintext_len);
        assert(consumed == 1U);
        assert(result == AIRPLAY_CONTROL_CIPHER_OK ||
               result == AIRPLAY_CONTROL_CIPHER_INCOMPLETE);
        produced += plaintext_len;
    }
    assert(produced == (sizeof(first) - 1U) + (sizeof(second) - 1U));
    assert(memcmp(plaintext, "first", sizeof(first) - 1U) == 0);
    assert(memcmp(plaintext + sizeof(first) - 1U, "second message",
                  sizeof(second) - 1U) == 0);

    airplay_control_cipher_destroy(peer);
    airplay_control_cipher_destroy(receiver);
}

static void test_output_too_small(void)
{
    uint8_t read_key[32];
    uint8_t write_key[32];
    make_keys(read_key, write_key);
    airplay_control_cipher_t *receiver = NULL;
    airplay_control_cipher_t *peer = NULL;
    assert(airplay_control_cipher_create(read_key, write_key, &receiver) ==
           AIRPLAY_CONTROL_CIPHER_OK);
    assert(airplay_control_cipher_create(write_key, read_key, &peer) ==
           AIRPLAY_CONTROL_CIPHER_OK);

    uint8_t frame[FRAME_BYTES];
    size_t frame_len = encrypt_one(peer, "twelve bytes", 12U, frame);

    uint8_t plaintext[4];
    size_t consumed = 0U;
    size_t plaintext_len = 0U;
    assert(airplay_control_cipher_decrypt(receiver, frame, frame_len, &consumed,
                                          plaintext, sizeof(plaintext),
                                          &plaintext_len) ==
           AIRPLAY_CONTROL_CIPHER_OUTPUT_TOO_SMALL);
    assert(plaintext_len == 0U);
    /* The frame stayed buffered: retrying with room succeeds. */
    uint8_t full[64];
    assert(airplay_control_cipher_decrypt(receiver, frame + consumed,
                                          frame_len - consumed, &consumed,
                                          full, sizeof(full),
                                          &plaintext_len) ==
           AIRPLAY_CONTROL_CIPHER_OK);
    assert(plaintext_len == 12U);
    assert(memcmp(full, "twelve bytes", 12U) == 0);

    airplay_control_cipher_destroy(peer);
    airplay_control_cipher_destroy(receiver);
}

static void test_tampering_poisons(void)
{
    uint8_t read_key[32];
    uint8_t write_key[32];
    make_keys(read_key, write_key);
    airplay_control_cipher_t *receiver = NULL;
    airplay_control_cipher_t *peer = NULL;
    assert(airplay_control_cipher_create(read_key, write_key, &receiver) ==
           AIRPLAY_CONTROL_CIPHER_OK);
    assert(airplay_control_cipher_create(write_key, read_key, &peer) ==
           AIRPLAY_CONTROL_CIPHER_OK);

    uint8_t frame[FRAME_BYTES];
    size_t frame_len = encrypt_one(peer, "command", 7U, frame);
    frame[2U] ^= 0x01U;

    uint8_t plaintext[64];
    size_t consumed = 0U;
    size_t plaintext_len = 0U;
    assert(airplay_control_cipher_decrypt(receiver, frame, frame_len, &consumed,
                                          plaintext, sizeof(plaintext),
                                          &plaintext_len) ==
           AIRPLAY_CONTROL_CIPHER_AUTH_FAILED);
    assert(plaintext_len == 0U);

    /* A poisoned session must not resume, even for its own outgoing frames. */
    static const uint8_t probe[] = {'x'};
    assert(airplay_control_cipher_encrypt(receiver, probe, sizeof(probe),
                                          plaintext, sizeof(plaintext),
                                          &plaintext_len) ==
           AIRPLAY_CONTROL_CIPHER_FAILED_STATE);
    assert(airplay_control_cipher_decrypt(receiver, frame, frame_len, &consumed,
                                          plaintext, sizeof(plaintext),
                                          &plaintext_len) ==
           AIRPLAY_CONTROL_CIPHER_FAILED_STATE);

    airplay_control_cipher_destroy(peer);
    airplay_control_cipher_destroy(receiver);
}

static void test_wrong_key(void)
{
    uint8_t read_key[32];
    uint8_t write_key[32];
    make_keys(read_key, write_key);
    airplay_control_cipher_t *receiver = NULL;
    airplay_control_cipher_t *stranger = NULL;
    assert(airplay_control_cipher_create(read_key, write_key, &receiver) ==
           AIRPLAY_CONTROL_CIPHER_OK);
    uint8_t other_read[32];
    uint8_t other_write[32];
    memset(other_read, 0x5a, sizeof(other_read));
    memset(other_write, 0xa5, sizeof(other_write));
    assert(airplay_control_cipher_create(other_read, other_write, &stranger) ==
           AIRPLAY_CONTROL_CIPHER_OK);

    uint8_t frame[FRAME_BYTES];
    size_t frame_len = encrypt_one(stranger, "secret", 6U, frame);
    uint8_t plaintext[64];
    size_t consumed = 0U;
    size_t plaintext_len = 0U;
    assert(airplay_control_cipher_decrypt(receiver, frame, frame_len, &consumed,
                                          plaintext, sizeof(plaintext),
                                          &plaintext_len) ==
           AIRPLAY_CONTROL_CIPHER_AUTH_FAILED);

    airplay_control_cipher_destroy(stranger);
    airplay_control_cipher_destroy(receiver);
}

static void test_limits(void)
{
    uint8_t read_key[32];
    uint8_t write_key[32];
    make_keys(read_key, write_key);
    airplay_control_cipher_t *cipher = NULL;
    assert(airplay_control_cipher_create(read_key, write_key, &cipher) ==
           AIRPLAY_CONTROL_CIPHER_OK);

    static uint8_t scratch[AIRPLAY_CONTROL_CIPHER_MAX_FRAME_BYTES + 1U];
    uint8_t frame[FRAME_BYTES];
    size_t frame_len = 0U;
    assert(airplay_control_cipher_encrypt(cipher, scratch, sizeof(scratch),
                                          frame, sizeof(frame), &frame_len) ==
           AIRPLAY_CONTROL_CIPHER_FRAME_TOO_LARGE);
    assert(airplay_control_cipher_encrypt(cipher, scratch, 4U, frame, 8U,
                                          &frame_len) ==
           AIRPLAY_CONTROL_CIPHER_OUTPUT_TOO_SMALL);

    /* An impossible 16-bit length is rejected before any large allocation. */
    uint8_t poison[2] = {0xffU, 0xffU};
    uint8_t plaintext[32];
    size_t consumed = 0U;
    size_t plaintext_len = 0U;
    assert(airplay_control_cipher_decrypt(cipher, poison, sizeof(poison),
                                          &consumed, plaintext,
                                          sizeof(plaintext),
                                          &plaintext_len) ==
           AIRPLAY_CONTROL_CIPHER_FRAME_TOO_LARGE);
    assert(consumed == sizeof(poison));
    assert(airplay_control_cipher_decrypt(cipher, poison, sizeof(poison),
                                          &consumed, plaintext,
                                          sizeof(plaintext),
                                          &plaintext_len) ==
           AIRPLAY_CONTROL_CIPHER_FAILED_STATE);

    airplay_control_cipher_destroy(cipher);
}

static void test_argument_checks(void)
{
    uint8_t key[32] = {0};
    airplay_control_cipher_t *cipher = NULL;
    assert(airplay_control_cipher_create(NULL, key, &cipher) ==
           AIRPLAY_CONTROL_CIPHER_INVALID_ARGUMENT);
    assert(airplay_control_cipher_create(key, NULL, &cipher) ==
           AIRPLAY_CONTROL_CIPHER_INVALID_ARGUMENT);
    assert(airplay_control_cipher_create(key, key, NULL) ==
           AIRPLAY_CONTROL_CIPHER_INVALID_ARGUMENT);

    size_t consumed = 0U;
    size_t written = 0U;
    uint8_t output[32];
    assert(airplay_control_cipher_decrypt(NULL, output, sizeof(output),
                                          &consumed, output, sizeof(output),
                                          &written) ==
           AIRPLAY_CONTROL_CIPHER_INVALID_ARGUMENT);
    airplay_control_cipher_destroy(NULL);
}

int main(void)
{
    test_round_trip();
    test_fragmented_input();
    test_output_too_small();
    test_tampering_poisons();
    test_wrong_key();
    test_limits();
    test_argument_checks();
    puts("airplay control cipher host test passed");
    return 0;
}
