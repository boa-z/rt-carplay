/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_pairing_store.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rtcp/rtcp.h"

#define BLOB_CAPACITY 1024U

typedef struct {
    uint8_t blob[BLOB_CAPACITY];
    size_t len;
    unsigned saves;
} memory_backend_t;

static memory_backend_t g_memory;

static int memory_load(uint8_t *blob, size_t capacity, size_t *len, void *context)
{
    memory_backend_t *memory = context;
    if (capacity < memory->len) return RTCP_EINVAL;
    memcpy(blob, memory->blob, memory->len);
    *len = memory->len;
    return RTCP_OK;
}

static int memory_save(const uint8_t *blob, size_t len, void *context)
{
    memory_backend_t *memory = context;
    if (len > sizeof(memory->blob)) return RTCP_EINVAL;
    memcpy(memory->blob, blob, len);
    memory->len = len;
    ++memory->saves;
    return RTCP_OK;
}

static int memory_erase(void *context)
{
    memory_backend_t *memory = context;
    memset(memory->blob, 0, sizeof(memory->blob));
    memory->len = 0U;
    return RTCP_OK;
}

static airplay_pairing_store_backend_t make_backend(memory_backend_t *memory)
{
    airplay_pairing_store_backend_t backend = {
        .load = memory_load,
        .save = memory_save,
        .erase = memory_erase,
        .context = memory,
    };
    return backend;
}

int main(void)
{
    memset(&g_memory, 0, sizeof(g_memory));
    airplay_pairing_store_backend_t backend = make_backend(&g_memory);

    assert(airplay_pairing_store_init(NULL) == RTCP_EINVAL);
    backend.load = NULL;
    assert(airplay_pairing_store_init(&backend) == RTCP_EINVAL);
    backend = make_backend(&g_memory);
    assert(airplay_pairing_store_init(&backend) == RTCP_OK);

    size_t count = 99U;
    assert(airplay_pairing_store_count(&count) == RTCP_OK && count == 0U);
    assert(airplay_pairing_store_count(NULL) == RTCP_EINVAL);

    static const uint8_t id_a[] = "controller-a";
    static const uint8_t id_b[] = "controller-b";
    uint8_t key_a[32];
    uint8_t key_b[32];
    memset(key_a, 0xa1, sizeof(key_a));
    memset(key_b, 0xb2, sizeof(key_b));

    uint8_t out[32];
    assert(!airplay_pairing_store_lookup_controller(id_a, sizeof(id_a) - 1U,
                                                    out, NULL));
    assert(airplay_pairing_store_save_pairing(id_a, sizeof(id_a) - 1U,
                                              key_a, NULL));
    assert(g_memory.len > 0U && g_memory.saves == 1U);
    assert(airplay_pairing_store_lookup_controller(id_a, sizeof(id_a) - 1U,
                                                   out, NULL));
    assert(memcmp(out, key_a, sizeof(key_a)) == 0);
    assert(airplay_pairing_store_count(&count) == RTCP_OK && count == 1U);

    /* Updating an existing controller replaces its key without growing. */
    memset(key_b, 0xc3, sizeof(key_b));
    assert(airplay_pairing_store_save_pairing(id_a, sizeof(id_a) - 1U,
                                              key_b, NULL));
    assert(airplay_pairing_store_lookup_controller(id_a, sizeof(id_a) - 1U,
                                                   out, NULL));
    assert(memcmp(out, key_b, sizeof(key_b)) == 0);

    /* Persistence survives a deinit/init cycle. */
    airplay_pairing_store_deinit();
    assert(airplay_pairing_store_init(&backend) == RTCP_OK);
    assert(airplay_pairing_store_lookup_controller(id_a, sizeof(id_a) - 1U,
                                                   out, NULL));
    assert(memcmp(out, key_b, sizeof(key_b)) == 0);

    /* Fill the table, then reject the ninth controller. */
    assert(airplay_pairing_store_save_pairing(id_b, sizeof(id_b) - 1U,
                                              key_a, NULL));
    for (unsigned i = 2U; i < AIRPLAY_PAIRING_STORE_MAX_CONTROLLERS; ++i) {
        uint8_t id[8] = {'c', 't', 'r', 'l', (uint8_t)('0' + i), 0, 0, 0};
        assert(airplay_pairing_store_save_pairing(id, 5U, key_a, NULL));
    }
    assert(airplay_pairing_store_count(&count) == RTCP_OK &&
           count == AIRPLAY_PAIRING_STORE_MAX_CONTROLLERS);
    static const uint8_t ninth[] = "controller-nine";
    assert(!airplay_pairing_store_save_pairing(ninth, sizeof(ninth) - 1U,
                                               key_a, NULL));
    assert(airplay_pairing_store_count(&count) == RTCP_OK &&
           count == AIRPLAY_PAIRING_STORE_MAX_CONTROLLERS);

    /* Invalid arguments. */
    assert(!airplay_pairing_store_save_pairing(NULL, 1U, key_a, NULL));
    assert(!airplay_pairing_store_save_pairing(id_a, 0U, key_a, NULL));
    assert(!airplay_pairing_store_save_pairing(id_a, 65U, key_a, NULL));
    assert(!airplay_pairing_store_save_pairing(id_a, 1U, NULL, NULL));
    assert(!airplay_pairing_store_lookup_controller(id_a, 0U, out, NULL));

    assert(airplay_pairing_store_remove(id_a, sizeof(id_a) - 1U) == RTCP_OK);
    assert(!airplay_pairing_store_lookup_controller(id_a, sizeof(id_a) - 1U,
                                                    out, NULL));
    assert(airplay_pairing_store_remove(id_a, sizeof(id_a) - 1U) == RTCP_ENOENT);
    assert(airplay_pairing_store_remove(NULL, 1U) == RTCP_EINVAL);

    assert(airplay_pairing_store_clear() == RTCP_OK);
    assert(airplay_pairing_store_count(&count) == RTCP_OK && count == 0U);

    /* A corrupt blob fails init and leaves the store unusable. */
    airplay_pairing_store_deinit();
    assert(!airplay_pairing_store_lookup_controller(id_b, sizeof(id_b) - 1U,
                                                    out, NULL));
    assert(g_memory.len > 0U);
    g_memory.blob[0] ^= 0xffU;
    assert(airplay_pairing_store_init(&backend) == RTCP_EIO);
    assert(airplay_pairing_store_count(&count) == RTCP_ESTATE);

    puts("airplay pairing store host test passed");
    return 0;
}
