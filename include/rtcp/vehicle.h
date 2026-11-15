/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RTCP_VEHICLE_H
#define RTCP_VEHICLE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Bounded foundation, not a complete vehicle catalogue or Ultra certification.
 * Host assigns the actual accessory/service/characteristic IDs and types.
 * All calls belong to one serialized owner; callbacks must not reenter. */
#define RTCP_VEHICLE_MAX_CHARACTERISTICS 8U
#define RTCP_VEHICLE_MAX_VALUE_BYTES 256U
#define RTCP_VEHICLE_DATASTREAM_UUID "3E2F3C61-AAD0-42CB-A8AA-BF22186DA62E"
#define RTCP_VEHICLE_HIGH_DATASTREAM_UUID "FF4A6720-F2BE-4F56-A3E1-DB3B4E37D634"

typedef enum {
    RTCP_VEHICLE_BOOL = 0, RTCP_VEHICLE_UINT8 = 1,
    RTCP_VEHICLE_UINT32 = 3, RTCP_VEHICLE_UINT64 = 4,
    RTCP_VEHICLE_INT32 = 7, RTCP_VEHICLE_FLOAT = 9,
    RTCP_VEHICLE_STRING = 10, RTCP_VEHICLE_DATA = 11
} rtcp_vehicle_format_t;
typedef enum { RTCP_VEHICLE_PHONE, RTCP_VEHICLE_CAR, RTCP_VEHICLE_SHARED } rtcp_vehicle_owner_t;
typedef enum { RTCP_VEHICLE_NORMAL, RTCP_VEHICLE_HIGH } rtcp_vehicle_lane_t;

/* Strings are UTF-8 bytes with explicit length, without a trailing NUL.
 * Uint64 is limited to INT64_MAX by this OPACK foundation; larger values must
 * not be advertised. A missing value is represented by has_value=false. */
typedef struct {
    rtcp_vehicle_format_t format;
    union {
        bool boolean;
        int64_t integer;
        double real;
        struct { size_t length; uint8_t bytes[RTCP_VEHICLE_MAX_VALUE_BYTES]; } blob;
    } as;
} rtcp_vehicle_value_t;

typedef struct {
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

/* validate_write is a pure policy check; return false to refuse a phone write.
 * did_write runs only after the response fits and the new value is committed.
 * Neither callback implies actual vehicle actuation or feedback. */
typedef struct {
    size_t struct_size;
    const char *plugin_name;
    int64_t plugin_id;
    const rtcp_vehicle_characteristic_t *characteristics;
    size_t count;
    bool (*validate_write)(void *, int64_t, const rtcp_vehicle_value_t *);
    void (*did_write)(void *, int64_t, const rtcp_vehicle_value_t *);
    void *context;
} rtcp_vehicle_config_t;

typedef struct rtcp_vehicle rtcp_vehicle_t;
typedef struct {
    size_t characteristics, subscribed, pending_notifications;
    uint32_t requests, rejected_messages, committed_writes, refused_writes;
    uint32_t notifications;
} rtcp_vehicle_status_t;

int rtcp_vehicle_create(const rtcp_vehicle_config_t *, rtcp_vehicle_t **);
void rtcp_vehicle_destroy(rtcp_vehicle_t *);
/* Clear session subscriptions/pending events, retain current vehicle values. */
void rtcp_vehicle_reset_session(rtcp_vehicle_t *);
int rtcp_vehicle_status(const rtcp_vehicle_t *, rtcp_vehicle_status_t *);
int rtcp_vehicle_get(const rtcp_vehicle_t *, int64_t iid, rtcp_vehicle_value_t *, bool *has_value);
int rtcp_vehicle_set(rtcp_vehicle_t *, int64_t iid, const rtcp_vehicle_value_t *);

/* OPACK vehicleStateProtocolInfo value, for a future host /info composition.
 * This API does not advertise or negotiate any capability by itself. */
int rtcp_vehicle_info(rtcp_vehicle_t *, uint8_t *, size_t, size_t *);
/* Complete decrypted Type 130 comm body, not an arbitrary TCP fragment.
 * Caller retains the request buffer until return. Output must not alias it.
 * RTCP_ENOMEM means bounded workspace/output exhaustion. No state commits on
 * malformed input or serialization failure. Replies stay on the input lane. */
int rtcp_vehicle_handle(rtcp_vehicle_t *, rtcp_vehicle_lane_t,
                        const uint8_t *, size_t, uint8_t *, size_t, size_t *);
/* Coalesces changed subscribed values into updateNotify on the HIGH lane.
 * RTCP_ENOENT means no pending changes. Failed encoding retains pending data.
 * Successful encoding consumes those changes; caller then owns queuing/retry
 * of the encoded message until delivered or the authenticated session ends. */
int rtcp_vehicle_notification(rtcp_vehicle_t *, rtcp_vehicle_lane_t *,
                              uint8_t *, size_t, size_t *);
#ifdef __cplusplus
}
#endif
#endif
