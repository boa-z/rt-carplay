/* SPDX-License-Identifier: GPL-3.0-or-later; ControlCipher framing reference: DiPlay */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airplay_control_cipher.h"
#include "airplay_rtsp_codec.h"
#include "rtcp/os.h"

#define AIRPLAY_CONTROL_KEY_BYTES 32U

typedef bool (*airplay_control_write_cb_t)(const uint8_t *bytes,
                                           size_t len,
                                           void *context);

/*
 * Authenticated AirPlay event channel: decrypts incoming ControlCipher
 * frames, answers RTSP requests with encrypted 200 responses, and sends HID
 * reports / keyframe commands.  The owner feeds it encrypted TCP bytes from
 * its poll loop; the write callback sends encrypted frames back.
 *
 * The instance is not thread-safe on its own; the mutex created from the os
 * seam serializes concurrent senders.
 */
typedef struct airplay_control_port airplay_control_port_t;

int airplay_control_port_create(const struct rtcp_os *os,
                                const uint8_t read_key[AIRPLAY_CONTROL_KEY_BYTES],
                                const uint8_t write_key[AIRPLAY_CONTROL_KEY_BYTES],
                                uint32_t stream_id,
                                uint32_t initial_cseq,
                                airplay_control_write_cb_t write_cb,
                                void *context,
                                airplay_control_port_t **out_port);

/* Destroys the cipher and keys.  The caller must stop using it afterwards. */
void airplay_control_port_destroy(airplay_control_port_t *port);

/* False after a decrypt/format failure has poisoned the channel. */
bool airplay_control_port_is_active(const airplay_control_port_t *port);

/* Feeds TCP bytes.  Complete RTSP requests are answered with encrypted 200
 * responses; RTSP/HTTP responses are consumed.  Returns RTCP_OK, or an error
 * after which the channel is inactive. */
int airplay_control_port_receive_encrypted(airplay_control_port_t *port,
                                           const uint8_t *bytes,
                                           size_t len);

bool airplay_control_port_send_hid_report(airplay_control_port_t *port,
                                          uint32_t hid_uid,
                                          const uint8_t *report,
                                          size_t report_len);

/* DiPlay's main-screen forceKeyFrame command, rate limited per session. */
bool airplay_control_port_request_main_screen_keyframe(
    airplay_control_port_t *port);
