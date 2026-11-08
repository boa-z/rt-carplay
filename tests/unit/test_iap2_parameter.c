/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "iap2_parameter.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    uint8_t body[64];
    size_t used = 0U;
    static const uint8_t payload[] = {0xde, 0xad, 0xbe, 0xef};

    assert(iap2_param_encode(body, sizeof(body), 7, payload, sizeof(payload)) == 8U);
    assert(body[0] == 0 && body[1] == 8 && body[2] == 0 && body[3] == 7);
    assert(memcmp(body + 4, payload, sizeof(payload)) == 0);

    /* No room for the header or payload. */
    assert(iap2_param_encode(body, 3, 7, payload, sizeof(payload)) == 0U);
    assert(iap2_param_encode(body, sizeof(body), 7, NULL, 5) == 0U);

    assert(iap2_param_append(body, sizeof(body), &used, 1, NULL, 0));
    assert(iap2_param_append(body, sizeof(body), &used, 2, payload, sizeof(payload)));
    assert(iap2_param_append_void(body, sizeof(body), &used, 3));
    assert(used == 4U + 8U + 4U);
    assert(iap2_param_validate(body, used));
    assert(iap2_param_validate(NULL, 0));
    assert(!iap2_param_validate(NULL, 1));
    assert(!iap2_param_validate(body, 2));
    assert(!iap2_param_validate(body, 5)); /* Truncated first parameter. */
    assert(!iap2_param_validate(body, used - 1U));

    const uint8_t *found = NULL;
    size_t found_len = 0U;
    assert(iap2_param_find_single(body, used, 2, &found, &found_len));
    assert(found_len == sizeof(payload) && memcmp(found, payload, found_len) == 0);

    found = NULL;
    found_len = 99U;
    assert(iap2_param_find_single(body, used, 1, &found, &found_len));
    assert(found_len == 0U);

    assert(!iap2_param_find_single(body, used, 4, &found, &found_len));

    /* Duplicate ids are ambiguous, not "last one wins". */
    uint8_t duplicate[16];
    size_t duplicate_len = 0U;
    assert(iap2_param_append_void(duplicate, sizeof(duplicate), &duplicate_len, 5));
    assert(iap2_param_append_void(duplicate, sizeof(duplicate), &duplicate_len, 5));
    assert(!iap2_param_find_single(duplicate, duplicate_len, 5, &found, &found_len));

    /* Appending into a body that is already full must fail cleanly. */
    used = sizeof(body);
    assert(!iap2_param_append_void(body, sizeof(body), &used, 9));

    puts("iap2 parameter host test passed");
    return 0;
}
