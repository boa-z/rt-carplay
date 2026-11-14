/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Ported from esp32-carplay-web-relay components/airplay_pairing_store
 * (GPL-3.0-or-later).  The ESP-IDF NVS/mutex layer is replaced by a
 * whole-blob backend supplied by the platform port.
 */
#include "airplay_pairing_store.h"

#include <string.h>

#include "rtcp/rtcp.h"

#define STORE_MAGIC "APS1"
#define STORE_VERSION 1U
#define STORE_HEADER_BYTES 6U
#define STORE_RECORD_BYTES (1U + AIRPLAY_PAIRING_STORE_MAX_IDENTIFIER_BYTES + \
                            AIRPLAY_PAIRING_STORE_PUBLIC_KEY_BYTES)
#define STORE_BLOB_BYTES (STORE_HEADER_BYTES + \
                          AIRPLAY_PAIRING_STORE_MAX_CONTROLLERS * STORE_RECORD_BYTES)

typedef struct {
    uint8_t identifier_len;
    uint8_t identifier[AIRPLAY_PAIRING_STORE_MAX_IDENTIFIER_BYTES];
    uint8_t public_key[AIRPLAY_PAIRING_STORE_PUBLIC_KEY_BYTES];
} controller_record_t;

typedef struct {
    uint8_t count;
    controller_record_t records[AIRPLAY_PAIRING_STORE_MAX_CONTROLLERS];
} controller_table_t;

static airplay_pairing_store_backend_t s_backend;
static controller_table_t s_table;
static bool s_initialized;

static bool identifier_valid(const uint8_t *identifier, size_t identifier_len)
{
    return identifier != NULL && identifier_len != 0U &&
           identifier_len <= AIRPLAY_PAIRING_STORE_MAX_IDENTIFIER_BYTES;
}

static bool identifier_equal(const controller_record_t *record,
                             const uint8_t *identifier,
                             size_t identifier_len)
{
    return record->identifier_len == identifier_len &&
           memcmp(record->identifier, identifier, identifier_len) == 0;
}

static void encode_table(const controller_table_t *table,
                         uint8_t blob[STORE_BLOB_BYTES])
{
    memset(blob, 0, STORE_BLOB_BYTES);
    memcpy(blob, STORE_MAGIC, 4U);
    blob[4] = STORE_VERSION;
    blob[5] = table->count;
    for (size_t i = 0U; i < table->count; ++i) {
        size_t offset = STORE_HEADER_BYTES + i * STORE_RECORD_BYTES;
        const controller_record_t *record = &table->records[i];
        blob[offset] = record->identifier_len;
        memcpy(blob + offset + 1U, record->identifier,
               AIRPLAY_PAIRING_STORE_MAX_IDENTIFIER_BYTES);
        memcpy(blob + offset + 1U + AIRPLAY_PAIRING_STORE_MAX_IDENTIFIER_BYTES,
               record->public_key, AIRPLAY_PAIRING_STORE_PUBLIC_KEY_BYTES);
    }
}

static int decode_table(const uint8_t blob[STORE_BLOB_BYTES],
                        controller_table_t *table)
{
    if (memcmp(blob, STORE_MAGIC, 4U) != 0 || blob[4] != STORE_VERSION ||
        blob[5] > AIRPLAY_PAIRING_STORE_MAX_CONTROLLERS) {
        return RTCP_EIO;
    }
    memset(table, 0, sizeof(*table));
    table->count = blob[5];
    for (size_t i = 0U; i < AIRPLAY_PAIRING_STORE_MAX_CONTROLLERS; ++i) {
        size_t offset = STORE_HEADER_BYTES + i * STORE_RECORD_BYTES;
        uint8_t identifier_len = blob[offset];
        if (i >= table->count) {
            for (size_t j = 0U; j < STORE_RECORD_BYTES; ++j) {
                if (blob[offset + j] != 0U) return RTCP_EIO;
            }
            continue;
        }
        if (identifier_len == 0U ||
            identifier_len > AIRPLAY_PAIRING_STORE_MAX_IDENTIFIER_BYTES) {
            return RTCP_EIO;
        }
        controller_record_t *record = &table->records[i];
        record->identifier_len = identifier_len;
        memcpy(record->identifier, blob + offset + 1U,
               AIRPLAY_PAIRING_STORE_MAX_IDENTIFIER_BYTES);
        memcpy(record->public_key,
               blob + offset + 1U + AIRPLAY_PAIRING_STORE_MAX_IDENTIFIER_BYTES,
               AIRPLAY_PAIRING_STORE_PUBLIC_KEY_BYTES);
        for (size_t j = identifier_len;
             j < AIRPLAY_PAIRING_STORE_MAX_IDENTIFIER_BYTES; ++j) {
            if (record->identifier[j] != 0U) return RTCP_EIO;
        }
        for (size_t j = 0U; j < i; ++j) {
            if (identifier_equal(&table->records[j], record->identifier,
                                 record->identifier_len)) {
                return RTCP_EIO;
            }
        }
    }
    return RTCP_OK;
}

static int load_table(controller_table_t *table)
{
    uint8_t blob[STORE_BLOB_BYTES];
    size_t blob_len = 0U;
    int rc = s_backend.load(blob, sizeof(blob), &blob_len, s_backend.context);
    if (rc != RTCP_OK) {
        memset(blob, 0, sizeof(blob));
        return rc;
    }
    if (blob_len == 0U) {
        memset(table, 0, sizeof(*table));
        rc = RTCP_OK;
    } else if (blob_len != STORE_BLOB_BYTES) {
        rc = RTCP_EIO;
    } else {
        rc = decode_table(blob, table);
    }
    memset(blob, 0, sizeof(blob));
    return rc;
}

static int save_table(const controller_table_t *table)
{
    uint8_t blob[STORE_BLOB_BYTES];
    encode_table(table, blob);
    int rc = s_backend.save(blob, sizeof(blob), s_backend.context);
    memset(blob, 0, sizeof(blob));
    return rc;
}

int airplay_pairing_store_init(const airplay_pairing_store_backend_t *backend)
{
    if (backend == NULL || backend->load == NULL || backend->save == NULL) {
        return RTCP_EINVAL;
    }
    s_backend = *backend;
    int rc = load_table(&s_table);
    if (rc != RTCP_OK) {
        memset(&s_table, 0, sizeof(s_table));
        memset(&s_backend, 0, sizeof(s_backend));
        return rc;
    }
    s_initialized = true;
    return RTCP_OK;
}

void airplay_pairing_store_deinit(void)
{
    memset(&s_table, 0, sizeof(s_table));
    memset(&s_backend, 0, sizeof(s_backend));
    s_initialized = false;
}

bool airplay_pairing_store_save_pairing(
    const uint8_t *identifier,
    size_t identifier_len,
    const uint8_t public_key[AIRPLAY_PAIRING_STORE_PUBLIC_KEY_BYTES],
    void *context)
{
    (void)context;
    if (!s_initialized || !identifier_valid(identifier, identifier_len) ||
        public_key == NULL) {
        return false;
    }

    controller_table_t table = s_table;
    size_t index = 0U;
    while (index < table.count &&
           !identifier_equal(&table.records[index], identifier, identifier_len)) {
        ++index;
    }
    if (index == table.count) {
        if (table.count == AIRPLAY_PAIRING_STORE_MAX_CONTROLLERS) return false;
        ++table.count;
    }
    if (index < s_table.count && memcmp(s_table.records[index].public_key,
                                        public_key, AIRPLAY_PAIRING_STORE_PUBLIC_KEY_BYTES) == 0)
        return true; /* already durable: avoid needless flash writes */
    controller_record_t *record = &table.records[index];
    memset(record, 0, sizeof(*record));
    record->identifier_len = (uint8_t)identifier_len;
    memcpy(record->identifier, identifier, identifier_len);
    memcpy(record->public_key, public_key,
           AIRPLAY_PAIRING_STORE_PUBLIC_KEY_BYTES);

    int rc = save_table(&table);
    if (rc == RTCP_OK) s_table = table;
    memset(&table, 0, sizeof(table));
    return rc == RTCP_OK;
}

bool airplay_pairing_store_lookup_controller(
    const uint8_t *identifier,
    size_t identifier_len,
    uint8_t public_key[AIRPLAY_PAIRING_STORE_PUBLIC_KEY_BYTES],
    void *context)
{
    (void)context;
    if (!s_initialized || !identifier_valid(identifier, identifier_len) ||
        public_key == NULL) {
        return false;
    }
    for (size_t i = 0U; i < s_table.count; ++i) {
        if (identifier_equal(&s_table.records[i], identifier, identifier_len)) {
            memcpy(public_key, s_table.records[i].public_key,
                   AIRPLAY_PAIRING_STORE_PUBLIC_KEY_BYTES);
            return true;
        }
    }
    return false;
}

int airplay_pairing_store_remove(const uint8_t *identifier, size_t identifier_len)
{
    if (!s_initialized) return RTCP_ESTATE;
    if (!identifier_valid(identifier, identifier_len)) return RTCP_EINVAL;

    controller_table_t table = s_table;
    size_t index = 0U;
    while (index < table.count &&
           !identifier_equal(&table.records[index], identifier, identifier_len)) {
        ++index;
    }
    if (index == table.count) {
        memset(&table, 0, sizeof(table));
        return RTCP_ENOENT;
    }
    for (size_t i = index; i + 1U < table.count; ++i) {
        table.records[i] = table.records[i + 1U];
    }
    --table.count;
    memset(&table.records[table.count], 0, sizeof(table.records[0]));

    int rc = save_table(&table);
    if (rc == RTCP_OK) s_table = table;
    memset(&table, 0, sizeof(table));
    return rc;
}

int airplay_pairing_store_clear(void)
{
    if (!s_initialized) return RTCP_ESTATE;
    controller_table_t table;
    memset(&table, 0, sizeof(table));
    int rc = save_table(&table);
    if (rc == RTCP_OK) s_table = table;
    return rc;
}

int airplay_pairing_store_count(size_t *out_count)
{
    if (!s_initialized) return RTCP_ESTATE;
    if (out_count == NULL) return RTCP_EINVAL;
    *out_count = s_table.count;
    return RTCP_OK;
}
