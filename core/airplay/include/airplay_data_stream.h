/* SPDX-License-Identifier: GPL-3.0-or-later
 * Based on LIVI by Lasse Heitgres (https://github.com/f-io/LIVI).
 * APTransportPackage and duplex behavior: LIVI iap_tunnel.rs; see NOTICE.
 */
#ifndef AIRPLAY_DATA_STREAM_H
#define AIRPLAY_DATA_STREAM_H

#include "rtcp/net.h"

#define AIRPLAY_DATA_STREAM_MAX_BODY       65536U
#define AIRPLAY_DATA_STREAM_QUEUE_PACKAGES 8U
#define AIRPLAY_DATA_STREAM_QUEUE_BYTES    131072U

typedef struct airplay_data_stream airplay_data_stream_t;
typedef enum
{
    AIRPLAY_DATA_STREAM_LISTENING,
    AIRPLAY_DATA_STREAM_CONNECTED,
    AIRPLAY_DATA_STREAM_FAILED,
} airplay_data_stream_state_t;
typedef struct
{
    airplay_data_stream_state_t state;
    int error;
    uint64_t received, delivered, ignored, sent, sync_replies, rejected_peers;
    size_t queued_packages, queued_bytes;
} airplay_data_stream_status_t;

/* One serialized owner. Body storage is borrowed only for this callback.
 * The callback may queue a response, but must not poll/stop/reenter otherwise.
 * False fails the stream. Transport replies acknowledge receipt, not execution. */
typedef bool (*airplay_data_stream_body_cb_t)(const uint8_t *, size_t, void *);

/* Each instance accepts one RTSP peer once. Caller derives independent read
 * (DataStream-Output) and write (DataStream-Input) keys from Pair-Verify plus
 * a fresh connection ID, and prevents reuse across failed/reopened streams.
 * This primitive does not negotiate, derive keys or advertise any capability.
 * The port must provide bounded socket operations; call poll only with ready
 * events returned by the owner's net->poll. No automatic reconnect/retry. */
int airplay_data_stream_start(const struct rtcp_net *, const rtcp_addr_t *,
                              const uint8_t read_key[32], const uint8_t write_key[32],
                              airplay_data_stream_body_cb_t, void *, airplay_data_stream_t **);
int airplay_data_stream_fd(const airplay_data_stream_t *);
int airplay_data_stream_events(const airplay_data_stream_t *);
uint16_t airplay_data_stream_port(const airplay_data_stream_t *);
int airplay_data_stream_poll(airplay_data_stream_t *, int ready_events);
/* Copies an async/cmnd body while connected. ENOMEM leaves the queue and
 * cipher counters unchanged. One queue slot is reserved for sync replies.
 * Success transfers delivery ownership until send or terminal failure. */
int airplay_data_stream_send(airplay_data_stream_t *, const uint8_t *, size_t);
int airplay_data_stream_status(const airplay_data_stream_t *, airplay_data_stream_status_t *);
void airplay_data_stream_stop(airplay_data_stream_t *);

#endif
