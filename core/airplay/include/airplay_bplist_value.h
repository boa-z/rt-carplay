/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef AIRPLAY_BPLIST_VALUE_H
#define AIRPLAY_BPLIST_VALUE_H
#include "airplay_bplist.h"
#include "rtcp/opack.h"
#define AIRPLAY_BPLIST_VALUE_MAX_NODES 128U
#define AIRPLAY_BPLIST_VALUE_MAX_BYTES 16384U
/* Bounded tree writer for protocol-generated nested plist values, reusing the
 * preorder node shape of OPACK. Supports null/bool/int/ASCII string/data and
 * array/dictionary. Strings are explicit-length non-NUL ASCII identifiers;
 * dictionary keys must be strings. Reals, UUID/date and non-ASCII are refused.
 * Output length is zero on any failure; partial bytes must not be consumed.
 * No heap allocation; <=512 bytes of offset workspace plus bounded recursion. */
airplay_bplist_result_t airplay_bplist_encode_nodes(const rtcp_opack_node_t *, size_t,
    uint8_t *, size_t, size_t *);
#endif
