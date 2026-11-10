/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lockdown.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rtcp/rtcp.h"
#include "rtcp/plist.h"

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

/* ------------------------------------------------------------------ */
/* Framed exchange over a fake byte stream                             */
/* ------------------------------------------------------------------ */

#define FAKE_CAPACITY 4096U

typedef struct {
    uint8_t written[FAKE_CAPACITY];
    size_t written_length;
    uint8_t incoming[FAKE_CAPACITY];
    size_t incoming_length;
    size_t incoming_offset;
    size_t max_read_chunk;
    bool closed;
    int write_error;
    unsigned read_calls;
} fake_stream_t;

static int fake_read(void *context, void *buffer, size_t length,
                     uint32_t timeout_ms)
{
    fake_stream_t *fake = context;
    (void)timeout_ms;
    ++fake->read_calls;
    if (fake->closed) {
        return 0;
    }
    if (fake->incoming_offset >= fake->incoming_length) {
        return RTCP_ETIMEDOUT;
    }
    size_t available = fake->incoming_length - fake->incoming_offset;
    size_t chunk = available < length ? available : length;
    if (fake->max_read_chunk != 0U && chunk > fake->max_read_chunk) {
        chunk = fake->max_read_chunk;
    }
    memcpy(buffer, fake->incoming + fake->incoming_offset, chunk);
    fake->incoming_offset += chunk;
    return (int)chunk;
}

static int fake_write(void *context, const void *data, size_t length)
{
    fake_stream_t *fake = context;
    if (fake->write_error != 0) {
        return fake->write_error;
    }
    if (length > FAKE_CAPACITY - fake->written_length) {
        return RTCP_ENOMEM;
    }
    memcpy(fake->written + fake->written_length, data, length);
    fake->written_length += length;
    return RTCP_OK;
}

static void fake_queue_frame(fake_stream_t *fake, const uint8_t *xml,
                             size_t length)
{
    fake->incoming[fake->incoming_length++] = (uint8_t)(length >> 24);
    fake->incoming[fake->incoming_length++] = (uint8_t)(length >> 16);
    fake->incoming[fake->incoming_length++] = (uint8_t)(length >> 8);
    fake->incoming[fake->incoming_length++] = (uint8_t)length;
    memcpy(fake->incoming + fake->incoming_length, xml, length);
    fake->incoming_length += length;
}

static void test_exchange(void)
{
    fake_stream_t fake;
    memset(&fake, 0, sizeof(fake));
    rtcp_stream_t stream = {
        .ops = &(rtcp_stream_ops_t){
            .read = fake_read,
            .write = fake_write,
        },
        .context = &fake,
    };

    uint8_t request[RTCP_LOCKDOWN_REQUEST_MAX];
    size_t request_length = rtcp_lockdown_build_get_value(
        request, sizeof(request), "rt-carplay", "ProductVersion");
    assert(request_length > 0U);

    static const char reply[] =
        "<plist><dict><key>Value</key><string>17.4.1</string>"
        "</dict></plist>";
    fake_queue_frame(&fake, (const uint8_t *)reply, sizeof(reply) - 1U);

    /* Force short reads so read_exact has to loop. */
    fake.max_read_chunk = 3U;
    uint8_t response[256];
    size_t response_length = 0U;
    assert(rtcp_lockdown_exchange(&stream, request, request_length, response,
                                  sizeof(response), &response_length,
                                  1000U) == RTCP_OK);
    assert(response_length == sizeof(reply) - 1U);
    assert(memcmp(response, reply, response_length) == 0);

    /* The four-byte length prefix and the request itself went out. */
    assert(fake.written_length == 4U + request_length);
    assert(fake.written[0] == 0U && fake.written[1] == 0U);
    assert(fake.written[2] == (uint8_t)(request_length >> 8));
    assert(fake.written[3] == (uint8_t)request_length);
    assert(memcmp(fake.written + 4U, request, request_length) == 0);

    char value[32];
    assert(rtcp_plist_get_string(response, response_length, "Value", value,
                                 sizeof(value)) == RTCP_OK);
    assert(strcmp(value, "17.4.1") == 0);
}

static void test_exchange_errors(void)
{
    fake_stream_t fake;
    uint8_t request[RTCP_LOCKDOWN_REQUEST_MAX];
    size_t request_length = rtcp_lockdown_build_get_value(
        request, sizeof(request), "rt-carplay", "ProductVersion");
    assert(request_length > 0U);
    uint8_t response[64];
    size_t response_length = 0U;

    /* Nothing queued: the read times out. */
    memset(&fake, 0, sizeof(fake));
    rtcp_stream_t stream = {
        .ops = &(rtcp_stream_ops_t){.read = fake_read, .write = fake_write},
        .context = &fake,
    };
    assert(rtcp_lockdown_exchange(&stream, request, request_length, response,
                                  sizeof(response), &response_length,
                                  10U) == RTCP_ETIMEDOUT);

    /* Peer closed inside the length prefix. */
    memset(&fake, 0, sizeof(fake));
    fake.closed = true;
    assert(rtcp_lockdown_exchange(&stream, request, request_length, response,
                                  sizeof(response), &response_length,
                                  10U) == RTCP_ESTATE);

    /* Declared reply is larger than the caller buffer. */
    memset(&fake, 0, sizeof(fake));
    static const char reply[] = "<plist><dict></dict></plist>";
    fake_queue_frame(&fake, (const uint8_t *)reply, sizeof(reply) - 1U);
    assert(rtcp_lockdown_exchange(&stream, request, request_length, response,
                                  8U, &response_length, 10U) == RTCP_EINVAL);

    /* Zero-length frame is invalid. */
    memset(&fake, 0, sizeof(fake));
    fake_queue_frame(&fake, (const uint8_t *)reply, 0U);
    assert(rtcp_lockdown_exchange(&stream, request, request_length, response,
                                  sizeof(response), &response_length,
                                  10U) == RTCP_EINVAL);

    /* A write failure is propagated before any read. */
    memset(&fake, 0, sizeof(fake));
    fake.write_error = RTCP_EIO;
    assert(rtcp_lockdown_exchange(&stream, request, request_length, response,
                                  sizeof(response), &response_length,
                                  10U) == RTCP_EIO);

    /* Invalid arguments. */
    assert(rtcp_lockdown_exchange(NULL, request, request_length, response,
                                  sizeof(response), &response_length,
                                  10U) == RTCP_EINVAL);
    assert(rtcp_lockdown_exchange(&stream, request, 0U, response,
                                  sizeof(response), &response_length,
                                  10U) == RTCP_EINVAL);
    assert(rtcp_lockdown_exchange(&stream, request, request_length, NULL,
                                  sizeof(response), &response_length,
                                  10U) == RTCP_EINVAL);
}

static void test_request_builders(void)
{
    uint8_t document[2048];

    size_t length = rtcp_lockdown_build_set_value(
        document, sizeof(document), "rt-carplay", "UntrustedHostBUID",
        "5E1C0D0A-0000-0000-0000-000000000001");
    assert(length > 0U);
    char value[64];
    assert(rtcp_plist_get_string(document, length, "Request", value,
                                 sizeof(value)) == RTCP_OK);
    assert(strcmp(value, "SetValue") == 0);
    assert(rtcp_plist_get_string(document, length, "Key", value,
                                 sizeof(value)) == RTCP_OK);
    assert(strcmp(value, "UntrustedHostBUID") == 0);
    assert(rtcp_plist_get_string(document, length, "Value", value,
                                 sizeof(value)) == RTCP_OK);
    assert(strcmp(value, "5E1C0D0A-0000-0000-0000-000000000001") == 0);

    length = rtcp_lockdown_build_start_session(
        document, sizeof(document), "rt-carplay", "HOST-ID", "SYSTEM-BUID");
    assert(length > 0U);
    assert(rtcp_plist_get_string(document, length, "Request", value,
                                 sizeof(value)) == RTCP_OK);
    assert(strcmp(value, "StartSession") == 0);
    assert(rtcp_plist_get_string(document, length, "HostID", value,
                                 sizeof(value)) == RTCP_OK);
    assert(strcmp(value, "HOST-ID") == 0);
    assert(rtcp_plist_get_string(document, length, "SystemBUID", value,
                                 sizeof(value)) == RTCP_OK);
    assert(strcmp(value, "SYSTEM-BUID") == 0);

    length = rtcp_lockdown_build_start_service(
        document, sizeof(document), "com.apple.carkit.service");
    assert(length > 0U);
    assert(rtcp_plist_get_string(document, length, "Service", value,
                                 sizeof(value)) == RTCP_OK);
    assert(strcmp(value, "com.apple.carkit.service") == 0);

    /* Arguments outside the bare-token set are refused. */
    assert(rtcp_lockdown_build_set_value(document, sizeof(document), "x",
                                         "K", NULL) == 0U);
    assert(rtcp_lockdown_build_start_service(document, sizeof(document),
                                             "bad service") == 0U);
}

static void test_pair_builder(void)
{
    static const uint8_t device_cert[] = "device-pem";
    static const uint8_t host_cert[] = "host-pem";
    static const uint8_t root_cert[] = "root-pem";
    uint8_t document[4096];
    size_t length = rtcp_lockdown_build_pair(
        document, sizeof(document), "rt-carplay", "HOST-ID", "SYSTEM-BUID",
        device_cert, sizeof(device_cert) - 1U, host_cert,
        sizeof(host_cert) - 1U, root_cert, sizeof(root_cert) - 1U);
    assert(length > 0U);
    document[length] = '\0';

    assert(strstr((const char *)document, "<key>PairRecord</key><dict>") !=
           NULL);
    assert(strstr((const char *)document,
                  "<key>DeviceCertificate</key><data>ZGV2aWNlLXBlbQ==</data>") !=
           NULL);
    assert(strstr((const char *)document,
                  "<key>HostCertificate</key><data>aG9zdC1wZW0=</data>") !=
           NULL);
    assert(strstr((const char *)document,
                  "<key>RootCertificate</key><data>cm9vdC1wZW0=</data>") !=
           NULL);
    assert(strstr((const char *)document,
                  "<key>Request</key><string>Pair</string>") != NULL);
    assert(strstr((const char *)document,
                  "<key>ProtocolVersion</key><string>2</string>") != NULL);
    assert(strstr((const char *)document,
                  "<key>ExtendedPairingErrors</key><true/>") != NULL);

    char value[32];
    assert(rtcp_plist_get_string(document, length, "HostID", value,
                                 sizeof(value)) == RTCP_OK);
    assert(strcmp(value, "HOST-ID") == 0);

    /* Missing certificate material is refused. */
    assert(rtcp_lockdown_build_pair(
               document, sizeof(document), "rt-carplay", "HOST-ID",
               "SYSTEM-BUID", NULL, 0U, host_cert, sizeof(host_cert) - 1U,
               root_cert, sizeof(root_cert) - 1U) == 0U);
}

int main(void)
{
    test_build_get_value();
    test_extract_string();
    test_exchange();
    test_exchange_errors();
    test_request_builders();
    test_pair_builder();
    puts("lockdown host test passed");
    return 0;
}
