/* SPDX-License-Identifier: GPL-3.0-or-later */
/** @file vehicle.h
 * Experimental Vehicle State characteristic and control model. */
#ifndef RTCP_VEHICLE_H
#define RTCP_VEHICLE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "rtcp/data_stream.h"
#ifdef __cplusplus
extern "C"
{
#endif

/* Bounded foundation, not a complete vehicle catalogue or Ultra certification.
 * Host assigns the actual accessory/service/characteristic IDs and types.
 * All calls belong to one serialized owner; callbacks must not reenter. */
#define RTCP_VEHICLE_MAX_CHARACTERISTICS  8U
#define RTCP_VEHICLE_MAX_VALUE_BYTES      256U
#define RTCP_VEHICLE_DATASTREAM_UUID      RTCP_DATA_STREAM_VEHICLE_UUID
#define RTCP_VEHICLE_HIGH_DATASTREAM_UUID RTCP_DATA_STREAM_VEHICLE_HIGH_UUID

/** Wire format IDs for characteristic and control values. */
typedef enum
{
    RTCP_VEHICLE_BOOL = 0,
    RTCP_VEHICLE_UINT8 = 1,
    RTCP_VEHICLE_UINT32 = 3,
    RTCP_VEHICLE_UINT64 = 4,
    RTCP_VEHICLE_INT32 = 7,
    RTCP_VEHICLE_FLOAT = 9,
    RTCP_VEHICLE_STRING = 10,
    RTCP_VEHICLE_DATA = 11
} rtcp_vehicle_format_t;
/** Endpoint ownership declared for a characteristic. */
typedef enum
{
    RTCP_VEHICLE_PHONE,
    RTCP_VEHICLE_CAR,
    RTCP_VEHICLE_SHARED
} rtcp_vehicle_owner_t;
/** Logical Vehicle State transport lane. */
typedef enum
{
    RTCP_VEHICLE_NORMAL,
    RTCP_VEHICLE_HIGH
} rtcp_vehicle_lane_t;

/** Strings are UTF-8 bytes with explicit length, without a trailing NUL.
 * Uint64 is limited to INT64_MAX by this OPACK foundation; larger values must
 * not be advertised. A missing value is represented by has_value=false. */
typedef struct
{
    rtcp_vehicle_format_t format;
    union
    {
        bool boolean;
        int64_t integer;
        double real;
        struct
        {
            size_t length;
            uint8_t bytes[RTCP_VEHICLE_MAX_VALUE_BYTES];
        } blob;
    } as;
} rtcp_vehicle_value_t;

/** Characteristic descriptor with host-assigned IDs and optional constraints. */
typedef struct
{
    int64_t accessory_iid, accessory_type, service_iid, service_type, iid, type;
    rtcp_vehicle_format_t format;
    rtcp_vehicle_owner_t owner;
    bool mutable_value;
    bool has_range;
    double minimum, maximum, step;
    bool has_units;
    int64_t units;
    size_t maximum_length; /* Required for STRING/DATA; at most 256. */
    bool has_initial;
    rtcp_vehicle_value_t initial;
} rtcp_vehicle_characteristic_t;

/* Optional service-level controls, described using LIVI's sender/hasResponse
 * and named parameter arrays. IDs/types belong to the actual host, not SDK.
 * Parameters are required, ordered by the descriptor and copied on delivery. */
#define RTCP_VEHICLE_MAX_CONTROLS           4U
#define RTCP_VEHICLE_MAX_CONTROL_PARAMETERS 4U
#define RTCP_VEHICLE_MAX_PARAMETER_NAME     32U
/** Named parameter in an ordered, typed service-control descriptor. */
typedef struct
{
    const char *name;
    rtcp_vehicle_format_t format;
    size_t maximum_length; /* STRING/DATA only, 1..MAX_VALUE_BYTES. */
} rtcp_vehicle_control_parameter_t;
/** Service control and its declared request and response parameters. */
typedef struct
{
    int64_t service_iid, iid, type;
    bool urgent;
    const rtcp_vehicle_control_parameter_t *request, *response;
    size_t request_count, response_count;
} rtcp_vehicle_control_t;
/** Bounded values in the order declared by the control descriptor. */
typedef struct
{
    size_t count;
    rtcp_vehicle_value_t values[RTCP_VEHICLE_MAX_CONTROL_PARAMETERS];
} rtcp_vehicle_control_values_t;

/** validate_write is a pure policy check; return false to refuse a phone write.
 * did_write runs only after the response fits and the new value is committed.
 * Neither callback implies actual vehicle actuation or feedback. */
typedef struct
{
    size_t struct_size;
    const char *plugin_name;
    int64_t plugin_id;
    const rtcp_vehicle_characteristic_t *characteristics;
    size_t count;
    bool (*validate_write)(void *, int64_t, const rtcp_vehicle_value_t *);
    void (*did_write)(void *, int64_t, const rtcp_vehicle_value_t *);
    void *context;
    const rtcp_vehicle_control_t *controls;
    size_t control_count;
    /* Required with controls: pure preparation, no actuation/reentry. Receives
     * copied typed arguments and fills response values in descriptor order.
     * false refuses locally (ESTATE), without inventing a wire error code.
     * May run again after output exhaustion; must not produce side effects. */
    bool (*prepare_control)(void *, int64_t, const rtcp_vehicle_control_values_t *,
                            rtcp_vehicle_control_values_t *);
    /* Optional post-serialization commit; parameters borrowed until return.
     * No callback on malformed input, refusal or encoding failure. A response
     * proves no actuation, phone receipt or duplicate suppression. */
    void (*did_control)(void *, int64_t, const rtcp_vehicle_control_values_t *);
} rtcp_vehicle_config_t;

/* Local diagnostics, not wire command IDs or proof of phone receipt. */
typedef enum
{
    RTCP_VEHICLE_METHOD_UNKNOWN = 0,
    RTCP_VEHICLE_METHOD_CONFIG,
    RTCP_VEHICLE_METHOD_READ,
    RTCP_VEHICLE_METHOD_REGISTER,
    RTCP_VEHICLE_METHOD_WRITE,
    RTCP_VEHICLE_METHOD_CONTROL,
    RTCP_VEHICLE_METHOD_COUNT
} rtcp_vehicle_method_t;

/** Opaque Vehicle State processor. Serialize all calls on one owner. */
typedef struct rtcp_vehicle rtcp_vehicle_t;
typedef struct
{
    size_t characteristics, subscribed, pending_notifications;
    uint32_t requests, rejected_messages, committed_writes, refused_writes;
    uint32_t notifications;
    size_t controls;
    uint32_t committed_controls;
    /* Cumulative until destroy, including failed attempts. UNKNOWN covers
     * malformed/unrecognized envelopes. Responses count local serialization,
     * including replies containing per-IID errors; no delivery is implied.
     * Invalid API arguments do not change request diagnostics. Last fields
     * are meaningful only when requests is nonzero and survive session reset. */
    uint32_t method_requests[RTCP_VEHICLE_METHOD_COUNT];
    uint32_t method_responses[RTCP_VEHICLE_METHOD_COUNT];
    rtcp_vehicle_method_t last_method;
    rtcp_vehicle_lane_t last_lane;
    int last_result;
} rtcp_vehicle_status_t;

/** Receiver-owned delivery workspace; counters are cumulative until destroy.
 * Queued means copied by transport, not acknowledged or applied by the phone. */
typedef struct
{
    bool attached;
    size_t pending_reply_bytes[2], pending_notification_bytes;
    uint64_t queued_messages, queue_retries, rejected_requests;
    uint64_t abandoned_replies, abandoned_notifications;
    int last_error;
} rtcp_vehicle_delivery_status_t;

/** Create an owned Vehicle State model from caller configuration.
 * Returns RTCP_OK or a negative error. The caller owns destroy. */
int rtcp_vehicle_create(const rtcp_vehicle_config_t *, rtcp_vehicle_t **);
/** Free a detached model. Stop and destroy any borrowing Receiver first. */
void rtcp_vehicle_destroy(rtcp_vehicle_t *);
/** Clear session subscriptions/pending events, retain current vehicle values. */
void rtcp_vehicle_reset_session(rtcp_vehicle_t *);
/** Copy counters and current subscription status. Returns RTCP_EINVAL
 * for NULL input or output. */
int rtcp_vehicle_status(const rtcp_vehicle_t *, rtcp_vehicle_status_t *);
/** Copy a characteristic value and its presence flag. An unknown IID
 * returns RTCP_ENOENT. */
int rtcp_vehicle_get(const rtcp_vehicle_t *, int64_t iid, rtcp_vehicle_value_t *, bool *has_value);
/** Publish a local characteristic value and mark subscribed changes pending.
 * Returns a negative RTCP error for an invalid value or unknown IID. */
int rtcp_vehicle_set(rtcp_vehicle_t *, int64_t iid, const rtcp_vehicle_value_t *);

/** Serialize OPACK vehicleStateProtocolInfo into caller storage for /info.
 * This API does not advertise or negotiate any capability by itself. */
int rtcp_vehicle_info(rtcp_vehicle_t *, uint8_t *, size_t, size_t *);
/** Process one complete decrypted Type 130 body.
 * Caller retains the request buffer until return. Output must not alias it.
 * RTCP_ENOMEM means bounded workspace/output exhaustion. No state commits on
 * malformed input or serialization failure. Replies stay on the input lane. */
int rtcp_vehicle_handle(rtcp_vehicle_t *, rtcp_vehicle_lane_t,
                        const uint8_t *, size_t, uint8_t *, size_t, size_t *);
/** Coalesce changed subscribed values into updateNotify on the HIGH lane.
 * RTCP_ENOENT means no pending changes. Failed encoding retains pending data.
 * Successful encoding consumes those changes; caller then owns queuing/retry
 * of the encoded message until delivered or the authenticated session ends. */
int rtcp_vehicle_notification(rtcp_vehicle_t *, rtcp_vehicle_lane_t *,
                              uint8_t *, size_t, size_t *);
#ifdef __cplusplus
}
#endif
#endif
