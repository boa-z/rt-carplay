/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lockdown.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rtcp/rtcp.h"

static void test_build_get_value(void)
{
    uint8_t request[RTCP_LOCKDOWN_REQUEST_MAX];
    size_t length = rtcp_lockdown_build_get_value(
        request, sizeof(request), "rt-carplay", "ProductVersion");
    assert(length > 0U);
    assert(length < sizeof(request));
    assert(request[length - 1U] == '\n');
    assert(strstr((const char *)request, "<key>Request</key>") != NULL);
    assert(strstr((const char *)request, "<string>GetValue</string>") !=
           NULL);
    assert(strstr((const char *)request, "<string>ProductVersion</string>") !=
           NULL);
    assert(strstr((const char *)request, "<string>rt-carplay</string>") !=
           NULL);

    /* No XML escaping: anything outside the bare-token set is rejected. */
    assert(rtcp_lockdown_build_get_value(request, sizeof(request),
                                         "bad<label", "ProductVersion") == 0U);
    assert(rtcp_lockdown_build_get_value(request, sizeof(request), "ok",
                                         "bad&key") == 0U);
    assert(rtcp_lockdown_build_get_value(request, 32U, "ok",
                                         "ProductVersion") == 0U);
    assert(rtcp_lockdown_build_get_value(NULL, sizeof(request), "ok",
                                         "ProductVersion") == 0U);
}

static void test_extract_string(void)
{
    static const char reply[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "<key>Key</key><string>ProductVersion</string>\n"
        "<key>Value</key><string>17.4.1</string>\n"
        "<key>Request</key><string>GetValue</string>\n"
        "</dict>\n</plist>\n";
    char value[32];
    assert(rtcp_lockdown_extract_string((const uint8_t *)reply,
                                        sizeof(reply) - 1U, "Value", value,
                                        sizeof(value)) == RTCP_OK);
    assert(strcmp(value, "17.4.1") == 0);
    assert(rtcp_lockdown_extract_string((const uint8_t *)reply,
                                        sizeof(reply) - 1U, "Key", value,
                                        sizeof(value)) == RTCP_OK);
    assert(strcmp(value, "ProductVersion") == 0);

    assert(rtcp_lockdown_extract_string((const uint8_t *)reply,
                                        sizeof(reply) - 1U, "Missing", value,
                                        sizeof(value)) == RTCP_ENOENT);
    /* The value does not fit. */
    assert(rtcp_lockdown_extract_string((const uint8_t *)reply,
                                        sizeof(reply) - 1U, "Value", value,
                                        4U) == RTCP_EINVAL);

    static const char truncated[] =
        "<key>Value</key><string>17.4.1";
    assert(rtcp_lockdown_extract_string((const uint8_t *)truncated,
                                        sizeof(truncated) - 1U, "Value",
                                        value, sizeof(value)) == RTCP_EINVAL);

    static const char not_a_string[] = "<key>Value</key><data>AAAA</data>";
    assert(rtcp_lockdown_extract_string((const uint8_t *)not_a_string,
                                        sizeof(not_a_string) - 1U, "Value",
                                        value, sizeof(value)) == RTCP_EINVAL);
}

int main(void)
{
    test_build_get_value();
    test_extract_string();
    puts("lockdown host test passed");
    return 0;
}
