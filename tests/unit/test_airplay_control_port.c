/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_control_port.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "airplay_control_cipher.h"
#include "rtcp/rtcp.h"
#include "rtcp_linux.h"

#define CAPTURE_BYTES 4096U
#define FRAME_BYTES (AIRPLAY_CONTROL_CIPHER_MAX_FRAME_BYTES + \
                     AIRPLAY_CONTROL_CIPHER_OVERHEAD_BYTES)

typedef struct {
    uint8_t bytes[CAPTURE_BYTES];
    size_t len;
    unsigned calls;
    bool fail_next;
} capture_t;

static bool capture_write(const uint8_t *bytes, size_t len, void *context)
{
    capture_t *capture = context;
    assert(len <= sizeof(capture->bytes));
    memcpy(capture->bytes, bytes, len);
    capture->len = len;
    ++capture->calls;
    return !capture->fail_next;
}

static void make_keys(uint8_t read_key[32], uint8_t write_key[32])
{
    for (size_t i = 0U; i < 32U; ++i) {
        read_key[i] = (uint8_t)(0x20U + i);
        write_key[i] = (uint8_t)(0x90U + i);
    }
}

static airplay_control_cipher_t *make_peer(const uint8_t read_key[32],
                                           const uint8_t write_key[32])
{
    airplay_control_cipher_t *peer = NULL;
    assert(airplay_control_cipher_create(write_key, read_key, &peer) ==
           AIRPLAY_CONTROL_CIPHER_OK);
    return peer;
}

static size_t peer_frame(airplay_control_cipher_t *peer,
                         const void *plaintext,
                         size_t plaintext_len,
                         uint8_t *frame)
{
    size_t frame_len = 0U;
    assert(airplay_control_cipher_encrypt(peer, plaintext, plaintext_len, frame,
                                          FRAME_BYTES, &frame_len) ==
           AIRPLAY_CONTROL_CIPHER_OK);
    return frame_len;
}

static void decrypt_capture(airplay_control_cipher_t *peer,
                            const capture_t *capture,
                            char *plaintext,
                            size_t plaintext_capacity)
{
    assert(capture->len <= FRAME_BYTES);
    assert(plaintext_capacity > 0U);
    size_t consumed = 0U;
    size_t plaintext_len = 0U;
    assert(airplay_control_cipher_decrypt(peer, capture->bytes, capture->len,
                                          &consumed, (uint8_t *)plaintext,
                                          plaintext_capacity - 1U,
                                          &plaintext_len) ==
           AIRPLAY_CONTROL_CIPHER_OK);
    assert(consumed == capture->len);
    plaintext[plaintext_len] = '\0';
}

static void test_event_ack(void)
{
    uint8_t read_key[32];
    uint8_t write_key[32];
    make_keys(read_key, write_key);

    capture_t capture = {0};
    airplay_control_port_t *port = NULL;
    assert(airplay_control_port_create(&rtcp_os_linux, read_key, write_key, 0U,
                                       100U, capture_write, &capture,
                                       &port) == RTCP_OK);
    assert(port != NULL);
    assert(airplay_control_port_is_active(port));

    airplay_control_cipher_t *peer = make_peer(read_key, write_key);
    static const char request[] =
        "POST /command RTSP/1.0\r\n"
        "CSeq: 5\r\n"
        "Content-Type: application/x-apple-binary-plist\r\n"
        "Content-Length: 0\r\n\r\n";
    uint8_t frame[FRAME_BYTES];
    size_t frame_len = peer_frame(peer, request, sizeof(request) - 1U, frame);

    /* Feed the frame one byte at a time: the ack appears only on the last. */
    for (size_t i = 0U; i + 1U < frame_len; ++i) {
        assert(airplay_control_port_receive_encrypted(port, frame + i, 1U) ==
               RTCP_OK);
        assert(capture.calls == 0U);
    }
    assert(airplay_control_port_receive_encrypted(port, frame + frame_len - 1U,
                                                  1U) == RTCP_OK);
    assert(capture.calls == 1U);

    char plaintext[1024];
    decrypt_capture(peer, &capture, plaintext, sizeof(plaintext));
    assert(strncmp(plaintext, "RTSP/1.0 200 OK\r\n", 17U) == 0);
    assert(strstr(plaintext, "CSeq: 5\r\n") != NULL);

    /* HID reports go out as an encrypted POST /command. */
    static const uint8_t report[] = {0x01U, 0x02U, 0x00U};
    capture.len = 0U;
    assert(airplay_control_port_send_hid_report(port, 1U, report,
                                                sizeof(report)));
    assert(capture.calls == 2U);
    decrypt_capture(peer, &capture, plaintext, sizeof(plaintext));
    assert(strncmp(plaintext, "POST /command RTSP/1.0\r\n", 24U) == 0);
    assert(strstr(plaintext, "CSeq: 101\r\n") != NULL);

    /* The forceKeyFrame command is rate limited, not dropped. */
    capture.len = 0U;
    assert(airplay_control_port_request_main_screen_keyframe(port));
    assert(capture.calls == 3U);
    decrypt_capture(peer, &capture, plaintext, sizeof(plaintext));
    assert(strncmp(plaintext, "POST /command RTSP/1.0\r\n", 24U) == 0);

    airplay_control_cipher_destroy(peer);
    airplay_control_port_destroy(port);
}

static void test_authentication_failure(void)
{
    uint8_t read_key[32];
    uint8_t write_key[32];
    make_keys(read_key, write_key);

    capture_t capture = {0};
    airplay_control_port_t *port = NULL;
    assert(airplay_control_port_create(&rtcp_os_linux, read_key, write_key, 7U,
                                       1U, capture_write, &capture,
                                       &port) == RTCP_OK);

    airplay_control_cipher_t *peer = make_peer(read_key, write_key);
    static const char request[] =
        "POST /command RTSP/1.0\r\nCSeq: 2\r\nContent-Length: 0\r\n\r\n";
    uint8_t frame[FRAME_BYTES];
    size_t frame_len = peer_frame(peer, request, sizeof(request) - 1U, frame);
    frame[frame_len - 1U] ^= 0xffU;

    assert(airplay_control_port_receive_encrypted(port, frame, frame_len) ==
           RTCP_EIO);
    assert(!airplay_control_port_is_active(port));

    /* A dead channel refuses to send. */
    assert(!airplay_control_port_send_hid_report(port, 1U, frame, 1U));
    assert(!airplay_control_port_request_main_screen_keyframe(port));

    airplay_control_cipher_destroy(peer);
    airplay_control_port_destroy(port);
}

static void test_write_failure_destroys_channel(void)
{
    uint8_t read_key[32];
    uint8_t write_key[32];
    make_keys(read_key, write_key);

    capture_t capture = {0};
    capture.fail_next = true;
    airplay_control_port_t *port = NULL;
    assert(airplay_control_port_create(&rtcp_os_linux, read_key, write_key, 0U,
                                       1U, capture_write, &capture,
                                       &port) == RTCP_OK);

    airplay_control_cipher_t *peer = make_peer(read_key, write_key);
    static const char request[] =
        "POST /command RTSP/1.0\r\nCSeq: 2\r\nContent-Length: 0\r\n\r\n";
    uint8_t frame[FRAME_BYTES];
    size_t frame_len = peer_frame(peer, request, sizeof(request) - 1U, frame);

    assert(airplay_control_port_receive_encrypted(port, frame, frame_len) ==
           RTCP_EIO);
    assert(!airplay_control_port_is_active(port));

    airplay_control_cipher_destroy(peer);
    airplay_control_port_destroy(port);
}

static void test_argument_checks(void)
{
    uint8_t key[32] = {0};
    capture_t capture = {0};
    airplay_control_port_t *port = NULL;
    assert(airplay_control_port_create(NULL, key, key, 0U, 0U, capture_write,
                                       &capture, &port) == RTCP_EINVAL);
    assert(airplay_control_port_create(&rtcp_os_linux, key, key, 0U, 0U, NULL,
                                       &capture, &port) == RTCP_EINVAL);
    assert(airplay_control_port_create(&rtcp_os_linux, key, key, 0U, 0U,
                                       capture_write, &capture, NULL) ==
           RTCP_EINVAL);
    assert(!airplay_control_port_is_active(NULL));
    assert(!airplay_control_port_send_hid_report(NULL, 1U, key, 1U));
    assert(!airplay_control_port_request_main_screen_keyframe(NULL));
    assert(airplay_control_port_receive_encrypted(NULL, key, 1U) ==
           RTCP_EINVAL);
    airplay_control_port_destroy(NULL);
}

int main(void)
{
    test_event_ack();
    test_authentication_failure();
    test_write_failure_destroys_channel();
    test_argument_checks();
    puts("airplay control port host test passed");
    return 0;
}
