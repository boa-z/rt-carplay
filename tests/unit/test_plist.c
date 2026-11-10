/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rtcp/plist.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rtcp/rtcp.h"

static size_t build_get_value(uint8_t *buffer, size_t capacity)
{
    rtcp_plist_writer_t writer;
    rtcp_plist_writer_init(&writer, buffer, capacity);
    assert(rtcp_plist_begin_dict(&writer));
    assert(rtcp_plist_key(&writer, "Label"));
    assert(rtcp_plist_string(&writer, "rt-carplay"));
    assert(rtcp_plist_key(&writer, "Request"));
    assert(rtcp_plist_string(&writer, "GetValue"));
    assert(rtcp_plist_key(&writer, "Key"));
    assert(rtcp_plist_string(&writer, "ProductVersion"));
    assert(rtcp_plist_end_dict(&writer));
    return rtcp_plist_finish(&writer);
}

static void test_writer_roundtrip(void)
{
    uint8_t document[1024];
    size_t length = build_get_value(document, sizeof(document));
    assert(length > 0U);
    assert(document[length - 1U] == '\n');

    char label[32];
    char request[32];
    char key[32];
    assert(rtcp_plist_get_string(document, length, "Label", label,
                                 sizeof(label)) == RTCP_OK);
    assert(strcmp(label, "rt-carplay") == 0);
    assert(rtcp_plist_get_string(document, length, "Request", request,
                                 sizeof(request)) == RTCP_OK);
    assert(strcmp(request, "GetValue") == 0);
    assert(rtcp_plist_get_string(document, length, "Key", key,
                                 sizeof(key)) == RTCP_OK);
    assert(strcmp(key, "ProductVersion") == 0);
    assert(rtcp_plist_get_string(document, length, "Missing", label,
                                 sizeof(label)) == RTCP_ENOENT);
    assert(rtcp_plist_get_integer(document, length, "Label", &(int64_t){0}) ==
           RTCP_EINVAL);
}

static void test_writer_nested_and_types(void)
{
    uint8_t document[1024];
    rtcp_plist_writer_t writer;
    rtcp_plist_writer_init(&writer, document, sizeof(document));
    assert(rtcp_plist_begin_dict(&writer));
    assert(rtcp_plist_key(&writer, "Integer"));
    assert(rtcp_plist_integer(&writer, -42));
    assert(rtcp_plist_key(&writer, "Big"));
    assert(rtcp_plist_integer(&writer, INT64_MAX));
    assert(rtcp_plist_key(&writer, "Yes"));
    assert(rtcp_plist_boolean(&writer, true));
    assert(rtcp_plist_key(&writer, "No"));
    assert(rtcp_plist_boolean(&writer, false));
    assert(rtcp_plist_key(&writer, "Options"));
    assert(rtcp_plist_begin_dict(&writer));
    assert(rtcp_plist_key(&writer, "ExtendedPairingErrors"));
    assert(rtcp_plist_boolean(&writer, true));
    assert(rtcp_plist_end_dict(&writer));
    assert(rtcp_plist_end_dict(&writer));
    size_t length = rtcp_plist_finish(&writer);
    assert(length > 0U);

    int64_t integer = 0;
    assert(rtcp_plist_get_integer(document, length, "Integer", &integer) ==
           RTCP_OK);
    assert(integer == -42);
    assert(rtcp_plist_get_integer(document, length, "Big", &integer) ==
           RTCP_OK);
    assert(integer == INT64_MAX);
    bool flag = false;
    assert(rtcp_plist_get_boolean(document, length, "Yes", &flag) == RTCP_OK);
    assert(flag);
    assert(rtcp_plist_get_boolean(document, length, "No", &flag) == RTCP_OK);
    assert(!flag);
    /* The nested dictionary is not a root-level scalar. */
    assert(rtcp_plist_get_boolean(document, length, "Options", &flag) ==
           RTCP_EINVAL);
}

static void test_writer_escaping(void)
{
    uint8_t document[1024];
    rtcp_plist_writer_t writer;
    rtcp_plist_writer_init(&writer, document, sizeof(document));
    assert(rtcp_plist_begin_dict(&writer));
    assert(rtcp_plist_key(&writer, "Text"));
    assert(rtcp_plist_string(&writer, "a<b>c&d\"e'f"));
    assert(rtcp_plist_end_dict(&writer));
    size_t length = rtcp_plist_finish(&writer);
    assert(length > 0U);

    char text[64];
    assert(rtcp_plist_get_string(document, length, "Text", text,
                                 sizeof(text)) == RTCP_OK);
    assert(strcmp(text, "a<b>c&d\"e'f") == 0);
}

static void test_data_roundtrip(void)
{
    static const uint8_t sample[] = {0x00, 0x01, 0x02, 0xfe, 0xff};
    static const size_t sizes[] = {0U, 1U, 2U, 3U, 4U, 5U};
    for (size_t s = 0U; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
        uint8_t document[1024];
        rtcp_plist_writer_t writer;
        rtcp_plist_writer_init(&writer, document, sizeof(document));
        assert(rtcp_plist_begin_dict(&writer));
        assert(rtcp_plist_key(&writer, "Blob"));
        assert(rtcp_plist_data(&writer, sample, sizes[s]));
        assert(rtcp_plist_end_dict(&writer));
        size_t length = rtcp_plist_finish(&writer);
        assert(length > 0U);

        uint8_t decoded[16];
        size_t decoded_length = 0U;
        assert(rtcp_plist_get_data(document, length, "Blob", decoded,
                                   sizeof(decoded),
                                   &decoded_length) == RTCP_OK);
        assert(decoded_length == sizes[s]);
        assert(memcmp(decoded, sample, sizes[s]) == 0);
    }

    /* Decoding tolerates line breaks and whitespace. */
    static const uint8_t spaced[] =
        "<plist><dict><key>Blob</key><data>AAEC\n/v8=</data></dict></plist>";
    uint8_t decoded[8];
    size_t decoded_length = 0U;
    assert(rtcp_plist_get_data(spaced, sizeof(spaced) - 1U, "Blob", decoded,
                               sizeof(decoded),
                               &decoded_length) == RTCP_OK);
    assert(decoded_length == 5U);
    assert(decoded[0] == 0x00 && decoded[4] == 0xff);

    static const uint8_t bad[] =
        "<plist><dict><key>Blob</key><data>!!!!</data></dict></plist>";
    assert(rtcp_plist_get_data(bad, sizeof(bad) - 1U, "Blob", decoded,
                               sizeof(decoded), &decoded_length) ==
           RTCP_EINVAL);
}

static void test_writer_errors(void)
{
    uint8_t buffer[512];
    rtcp_plist_writer_t writer;

    /* A scalar before the root dictionary is invalid. */
    rtcp_plist_writer_init(&writer, buffer, sizeof(buffer));
    assert(!rtcp_plist_string(&writer, "x"));
    assert(rtcp_plist_finish(&writer) == 0U);

    /* A key without a value cannot close the dictionary. */
    rtcp_plist_writer_init(&writer, buffer, sizeof(buffer));
    assert(rtcp_plist_begin_dict(&writer));
    assert(rtcp_plist_key(&writer, "K"));
    assert(!rtcp_plist_end_dict(&writer));
    assert(rtcp_plist_finish(&writer) == 0U);

    /* Capacity is enforced without overrunning the buffer. */
    char too_long[600];
    memset(too_long, 'x', sizeof(too_long) - 1U);
    too_long[sizeof(too_long) - 1U] = '\0';
    rtcp_plist_writer_init(&writer, buffer, sizeof(buffer));
    assert(rtcp_plist_begin_dict(&writer));
    assert(rtcp_plist_key(&writer, "Key"));
    assert(!rtcp_plist_string(&writer, too_long));
    assert(rtcp_plist_finish(&writer) == 0U);

    /* A document must be closed exactly once. */
    rtcp_plist_writer_init(&writer, buffer, sizeof(buffer));
    assert(rtcp_plist_begin_dict(&writer));
    assert(rtcp_plist_end_dict(&writer));
    assert(rtcp_plist_finish(&writer) != 0U);
}

static void test_accessor_errors(void)
{
    char text[32];
    int64_t integer = 0;
    bool flag = false;

    static const uint8_t truncated[] =
        "<plist><dict><key>Text</key><string>abc";
    assert(rtcp_plist_get_string(truncated, sizeof(truncated) - 1U, "Text",
                                 text, sizeof(text)) == RTCP_EINVAL);

    static const uint8_t not_text[] =
        "<plist><dict><key>Text</key><integer>1</integer></dict></plist>";
    assert(rtcp_plist_get_string(not_text, sizeof(not_text) - 1U, "Text",
                                 text, sizeof(text)) == RTCP_EINVAL);
    assert(rtcp_plist_get_integer(not_text, sizeof(not_text) - 1U, "Text",
                                  &integer) == RTCP_OK);
    assert(integer == 1);

    static const uint8_t bad_integer[] =
        "<plist><dict><key>N</key><integer>12x</integer></dict></plist>";
    assert(rtcp_plist_get_integer(bad_integer, sizeof(bad_integer) - 1U, "N",
                                  &integer) == RTCP_EINVAL);
    static const uint8_t empty_integer[] =
        "<plist><dict><key>N</key><integer></integer></dict></plist>";
    assert(rtcp_plist_get_integer(empty_integer, sizeof(empty_integer) - 1U,
                                  "N", &integer) == RTCP_EINVAL);

    static const uint8_t bad_bool[] =
        "<plist><dict><key>B</key><string>true</string></dict></plist>";
    assert(rtcp_plist_get_boolean(bad_bool, sizeof(bad_bool) - 1U, "B",
                                  &flag) == RTCP_EINVAL);

    /* A value must not be a nested element when a scalar is requested. */
    static const uint8_t nested[] =
        "<plist><dict><key>V</key><dict><key>X</key><string>y</string>"
        "</dict></dict></plist>";
    assert(rtcp_plist_get_string(nested, sizeof(nested) - 1U, "V", text,
                                 sizeof(text)) == RTCP_EINVAL);

    /* Output buffers are bounded. */
    static const uint8_t long_text[] =
        "<plist><dict><key>V</key><string>abcdef</string></dict></plist>";
    assert(rtcp_plist_get_string(long_text, sizeof(long_text) - 1U, "V", text,
                                 4U) == RTCP_EINVAL);
}

static void test_error_value(void)
{
    char error[64];

    static const uint8_t as_string[] =
        "<plist><dict><key>Error</key><string>UserDeniedPairing</string>"
        "</dict></plist>";
    assert(rtcp_plist_get_error(as_string, sizeof(as_string) - 1U, error,
                                sizeof(error)) == RTCP_OK);
    assert(strcmp(error, "UserDeniedPairing") == 0);

    static const uint8_t as_integer[] =
        "<plist><dict><key>Error</key><integer>7</integer></dict></plist>";
    assert(rtcp_plist_get_error(as_integer, sizeof(as_integer) - 1U, error,
                                sizeof(error)) == RTCP_OK);
    assert(strcmp(error, "7") == 0);

    static const uint8_t with_string[] =
        "<plist><dict><key>Error</key><integer>7</integer>"
        "<key>ErrorString</key><string>InvalidHostID</string></dict></plist>";
    assert(rtcp_plist_get_error(with_string, sizeof(with_string) - 1U, error,
                                sizeof(error)) == RTCP_OK);
    assert(strcmp(error, "InvalidHostID") == 0);

    static const uint8_t none[] =
        "<plist><dict><key>Request</key><string>GetValue</string>"
        "</dict></plist>";
    assert(rtcp_plist_get_error(none, sizeof(none) - 1U, error,
                                sizeof(error)) == RTCP_ENOENT);
}

int main(void)
{
    test_writer_roundtrip();
    test_writer_nested_and_types();
    test_writer_escaping();
    test_data_roundtrip();
    test_writer_errors();
    test_accessor_errors();
    test_error_value();
    puts("plist host test passed");
    return 0;
}
