/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RTCP_OPACK_H
#define RTCP_OPACK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define RTCP_OPACK_MAX_NODES 512U
#define RTCP_OPACK_MAX_DEPTH 16U
#define RTCP_OPACK_MAX_BYTES 65536U
typedef enum {
    RTCP_OPACK_NULL, RTCP_OPACK_BOOL, RTCP_OPACK_INT, RTCP_OPACK_REAL,
    RTCP_OPACK_STRING, RTCP_OPACK_DATA, RTCP_OPACK_UUID, RTCP_OPACK_DATE,
    RTCP_OPACK_ARRAY, RTCP_OPACK_DICT
} rtcp_opack_type_t;

/* Nodes form a preorder arena. end is the exclusive subtree boundary.
 * Container count is array items or dictionary pairs; first child is index+1.
 * String/data/UUID bytes borrow the input buffer. No allocation or recursion
 * into external references. References decode to independent scalar nodes. */
typedef struct {
    rtcp_opack_type_t type;
    uint16_t end, count;
    union {
        bool boolean;
        int64_t integer;
        double real;
        struct { const uint8_t *bytes; size_t length; } blob;
    } value;
} rtcp_opack_node_t;

typedef enum {
    RTCP_OPACK_OK, RTCP_OPACK_INVALID, RTCP_OPACK_LIMIT,
    RTCP_OPACK_OUTPUT_TOO_SMALL, RTCP_OPACK_UNSUPPORTED
} rtcp_opack_result_t;

/* Both count/length outputs are zero on error. Caller storage may be partially
 * written on failure and must not be consumed. UTF-8 must be valid, floats
 * finite, containers complete; trailing bytes and unknown tags are rejected. */
rtcp_opack_result_t rtcp_opack_decode(const uint8_t *, size_t,
                                      rtcp_opack_node_t *, size_t capacity,
                                      size_t *node_count);
rtcp_opack_result_t rtcp_opack_encode(const rtcp_opack_node_t *, size_t count,
                                      uint8_t *, size_t capacity, size_t *length);
/* Returns SIZE_MAX for absent keys or invalid arenas. Numeric dictionary keys
 * remain available by traversing the preorder nodes (Vehicle State uses them). */
size_t rtcp_opack_dict_get(const rtcp_opack_node_t *, size_t count,
                            size_t dictionary, const char *key);

#ifdef __cplusplus
}
#endif
#endif
