/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Wireless iAP2 session on top of rtcp_bt_lubanlite.
 *
 * carplay_iap2 starts one Bluetooth RFCOMM server for the Apple iAP2 service
 * UUID and drives core/iap2/iap2_session over it: received bytes are fed to
 * the link layer, its writes go back over RFCOMM, and a 100 ms tick runs the
 * retransmit/invite timers.  The receiver must already be running
 * (carplay_start): the session borrows the live SSID/passphrase, AirPlay
 * port, device id and Ed25519 public key from the application.
 *
 * RTCARPLAY_MFI supplies the board's shared authentication provider.
 */
#pragma once

#include <stdbool.h>

/* Starts the Bluetooth host (if needed), registers SDP/RFCOMM and serves one
 * phone at a time.  Safe to call from the MSH thread.  Returns RTCP_OK,
 * RTCP_ESTATE when the receiver is not running or the Bluetooth link is
 * owned by another service, or another negative RTCP error. */
int carplay_iap2_start(void);

/* Asks the link thread to stop, wakes the transport and waits for the
 * session to be destroyed.  Returns RTCP_OK when it is down. */
int carplay_iap2_stop(void);

/* Non-blocking stop request; the session is torn down
 * on the link thread. */
void carplay_iap2_request_stop(void);

bool carplay_iap2_is_running(void);

/* Session progress, mirrored for the shell status command.  All reset when
 * the RFCOMM link drops. */
bool carplay_iap2_identification_accepted(void);
bool carplay_iap2_authenticated(void);

typedef struct {
    bool running, listening, connected, identified, authenticated;
    int error; /* asynchronous startup/session failure; reset on start */
} carplay_iap2_status_t;
void carplay_iap2_get_status(carplay_iap2_status_t *status);
