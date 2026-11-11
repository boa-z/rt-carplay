/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L

#include "lockdown_session.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "rtcp/plist.h"
#include "rtcp/rtcp.h"

#define LOCKDOWN_PORT 62078U
#define SERVICE_PORT 7000U
#define MAX_CONNECTIONS 8U

/* The phone's device public key, in the format GetValue DevicePublicKey
 * returns.  Neither the client nor the fake phone needs its private key. */
static const char device_public_key_pem[] =
    "-----BEGIN RSA PUBLIC KEY-----\n"
    "MIIBCgKCAQEA4jtAqhC/gkeGKQjAPhpvmNb3yQx/qj8I2JXViuwwuZVecE5pAoYn\n"
    "wO58YS1014QeEID5xcEHQD3m8m4zl64d2B1sDhegRhZozvR2yf1fSXMnTu7FQbL0\n"
    "B0ZSNRUOCWpdCLKvWd36ZykydZmbZnm4hGlN/CYhFM8QaaH3GGJ6D9ug3PPexM94\n"
    "/NUqmvcbDdY5qku+92kRQKL06AdfU6NUhQc2tBZL4f+dSHgLuHtOwZRaG3oiesAH\n"
    "fWpRCGGDmoccmNjiiXT8WrD5iaa8m3Re0Gb3QiXmrDGt3HiBqzcgeiocc3/wxQpS\n"
    "dbYW3XzLF6e0S6Dultfvu9zJAnY4vDidHwIDAQAB\n"
    "-----END RSA PUBLIC KEY-----\n";

static uint32_t rng_state = 0x5eed1234U;

static int test_rng(void *context, uint8_t *output, size_t length)
{
    (void)context;
    for (size_t i = 0U; i < length; ++i) {
        rng_state ^= rng_state << 13;
        rng_state ^= rng_state >> 17;
        rng_state ^= rng_state << 5;
        output[i] = (uint8_t)rng_state;
    }
    return 0;
}

static int test_rng_ssl(void *context, unsigned char *output, size_t length)
{
    return test_rng(context, output, length);
}

/* ------------------------------------------------------------------ */
/* Pipes and duplex streams                                            */
/* ------------------------------------------------------------------ */

#define PIPE_CAPACITY 65536U

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    uint8_t data[PIPE_CAPACITY];
    size_t read_pos;
    size_t write_pos;
    size_t used;
    int writer_closed;
} pipe_t;

static void pipe_init(pipe_t *pipe)
{
    memset(pipe, 0, sizeof(*pipe));
    assert(pthread_mutex_init(&pipe->lock, NULL) == 0);
    assert(pthread_cond_init(&pipe->cond, NULL) == 0);
}

static void pipe_destroy(pipe_t *pipe)
{
    pthread_cond_destroy(&pipe->cond);
    pthread_mutex_destroy(&pipe->lock);
}

static int pipe_read(void *context, void *buffer, size_t length,
                     uint32_t timeout_ms)
{
    pipe_t *pipe = context;
    uint8_t *output = buffer;
    size_t count;
    int rc;

    pthread_mutex_lock(&pipe->lock);
    while (pipe->used == 0U && !pipe->writer_closed) {
        if (timeout_ms == 0U) {
            pthread_mutex_unlock(&pipe->lock);
            return RTCP_ETIMEDOUT;
        }
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += (time_t)(timeout_ms / 1000U);
        deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }
        rc = pthread_cond_timedwait(&pipe->cond, &pipe->lock, &deadline);
        if (rc == ETIMEDOUT && pipe->used == 0U && !pipe->writer_closed) {
            pthread_mutex_unlock(&pipe->lock);
            return RTCP_ETIMEDOUT;
        }
    }
    if (pipe->used == 0U) {
        pthread_mutex_unlock(&pipe->lock);
        return 0;
    }
    count = length < pipe->used ? length : pipe->used;
    for (size_t i = 0U; i < count; ++i) {
        output[i] = pipe->data[pipe->read_pos];
        pipe->read_pos = (pipe->read_pos + 1U) % PIPE_CAPACITY;
    }
    pipe->used -= count;
    pthread_cond_broadcast(&pipe->cond);
    pthread_mutex_unlock(&pipe->lock);
    return (int)count;
}

static int pipe_write(void *context, const void *data, size_t length)
{
    pipe_t *pipe = context;
    const uint8_t *input = data;

    pthread_mutex_lock(&pipe->lock);
    for (size_t i = 0U; i < length; ++i) {
        while (pipe->used == PIPE_CAPACITY) {
            pthread_cond_wait(&pipe->cond, &pipe->lock);
        }
        pipe->data[pipe->write_pos] = input[i];
        pipe->write_pos = (pipe->write_pos + 1U) % PIPE_CAPACITY;
        ++pipe->used;
        pthread_cond_broadcast(&pipe->cond);
    }
    pthread_mutex_unlock(&pipe->lock);
    return RTCP_OK;
}

static const rtcp_stream_ops_t pipe_ops = {
    .read = pipe_read,
    .write = pipe_write,
};

typedef struct {
    rtcp_stream_t in;
    rtcp_stream_t out;
} duplex_t;

static int duplex_read(void *context, void *buffer, size_t length,
                       uint32_t timeout_ms)
{
    duplex_t *duplex = context;

    return duplex->in.ops->read(duplex->in.context, buffer, length,
                                timeout_ms);
}

static int duplex_write(void *context, const void *data, size_t length)
{
    duplex_t *duplex = context;

    return duplex->out.ops->write(duplex->out.context, data, length);
}

static const rtcp_stream_ops_t duplex_ops = {
    .read = duplex_read,
    .write = duplex_write,
};

/* mbedTLS server callbacks over an rtcp_stream_t.  They must report byte
 * counts, not the transport status. */
static int tls_send(void *context, const unsigned char *data, size_t length)
{
    rtcp_stream_t *stream = context;
    int ret = stream->ops->write(stream->context, data, length);

    if (ret != RTCP_OK) {
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    return (int)length;
}

static int tls_recv(void *context, unsigned char *buffer, size_t length)
{
    rtcp_stream_t *stream = context;
    int ret = stream->ops->read(stream->context, buffer, length, 5000U);

    if (ret == RTCP_ETIMEDOUT) {
        return MBEDTLS_ERR_SSL_TIMEOUT;
    }
    if (ret < 0) {
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    return ret;
}

/* An established mbedTLS server session presented as an rtcp_stream_t so the
 * plist helpers can read requests from it. */
typedef struct {
    mbedtls_ssl_context *ssl;
} tls_stream_context_t;

static int tls_stream_read(void *context, void *buffer, size_t length,
                           uint32_t timeout_ms)
{
    tls_stream_context_t *tls = context;
    int retries = 0;

    (void)timeout_ms;
    for (;;) {
        int ret = mbedtls_ssl_read(tls->ssl, buffer, length);
        if (ret > 0) {
            return ret;
        }
        if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            return 0;
        }
        if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
            ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (++retries <= 16) {
                continue;
            }
        }
        return RTCP_EIO;
    }
}

static int tls_stream_write(void *context, const void *data, size_t length)
{
    tls_stream_context_t *tls = context;
    const uint8_t *cursor = data;
    size_t written = 0U;

    while (written < length) {
        int ret = mbedtls_ssl_write(tls->ssl, cursor + written,
                                    length - written);
        if (ret > 0) {
            written += (size_t)ret;
            continue;
        }
        if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
            ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            return RTCP_EIO;
        }
    }
    return RTCP_OK;
}

static const rtcp_stream_ops_t tls_stream_ops = {
    .read = tls_stream_read,
    .write = tls_stream_write,
};

/* ------------------------------------------------------------------ */
/* Fake phone                                                          */
/* ------------------------------------------------------------------ */

typedef struct phone phone_t;

typedef struct {
    phone_t *phone;
    pipe_t client_to_server;
    pipe_t server_to_client;
    duplex_t duplex;
    pthread_t thread;
    bool used;
    uint16_t port;
} connection_t;

struct phone {
    pthread_mutex_t lock;
    unsigned pair_requests;
    unsigned pair_pending_answers;
    bool deny_pair;
    bool service_ssl;
    uint16_t service_port;

    rtcp_pair_record_t identity; /* server-side TLS certificate and key */
};

static phone_t s_phone;

static int read_exact(rtcp_stream_t *stream, void *buffer, size_t length)
{
    uint8_t *cursor = buffer;

    while (length != 0U) {
        int received = stream->ops->read(stream->context, cursor, length,
                                         5000U);
        if (received < 0) {
            return received;
        }
        if (received == 0) {
            return RTCP_EIO;
        }
        cursor += (size_t)received;
        length -= (size_t)received;
    }
    return RTCP_OK;
}

static int read_frame(rtcp_stream_t *stream, uint8_t *buffer, size_t capacity,
                      size_t *length)
{
    uint8_t header[4];
    uint32_t frame_length;
    int ret = read_exact(stream, header, sizeof(header));

    if (ret != RTCP_OK) {
        return ret;
    }
    frame_length = ((uint32_t)header[0] << 24) | ((uint32_t)header[1] << 16) |
                   ((uint32_t)header[2] << 8) | header[3];
    if (frame_length == 0U || frame_length >= capacity) {
        return RTCP_EIO;
    }
    ret = read_exact(stream, buffer, frame_length);
    if (ret != RTCP_OK) {
        return ret;
    }
    buffer[frame_length] = '\0';
    *length = frame_length;
    return RTCP_OK;
}

static int write_frame(rtcp_stream_t *stream, const uint8_t *document,
                       size_t length)
{
    uint8_t header[4];
    int ret;

    header[0] = (uint8_t)(length >> 24);
    header[1] = (uint8_t)(length >> 16);
    header[2] = (uint8_t)(length >> 8);
    header[3] = (uint8_t)length;
    ret = stream->ops->write(stream->context, header, sizeof(header));
    if (ret != RTCP_OK) {
        return ret;
    }
    return stream->ops->write(stream->context, document, length);
}

typedef struct {
    uint8_t buffer[4096];
    rtcp_plist_writer_t writer;
} reply_t;

static void reply_init(reply_t *reply)
{
    rtcp_plist_writer_init(&reply->writer, reply->buffer,
                           sizeof(reply->buffer));
    assert(rtcp_plist_begin_dict(&reply->writer));
}

static int send_reply(rtcp_stream_t *stream, reply_t *reply)
{
    size_t length;

    assert(rtcp_plist_end_dict(&reply->writer));
    length = rtcp_plist_finish(&reply->writer);
    assert(length != 0U);
    return write_frame(stream, reply->buffer, length);
}

static int send_request_ok(rtcp_stream_t *stream, const char *request)
{
    reply_t reply;
    reply_init(&reply);
    assert(rtcp_plist_key(&reply.writer, "Request"));
    assert(rtcp_plist_string(&reply.writer, request));
    return send_reply(stream, &reply);
}

static int send_error(rtcp_stream_t *stream, const char *error)
{
    reply_t reply;
    reply_init(&reply);
    assert(rtcp_plist_key(&reply.writer, "Error"));
    assert(rtcp_plist_string(&reply.writer, error));
    return send_reply(stream, &reply);
}

static int send_get_value_data(rtcp_stream_t *stream, const uint8_t *data,
                               size_t length)
{
    reply_t reply;
    reply_init(&reply);
    assert(rtcp_plist_key(&reply.writer, "Request"));
    assert(rtcp_plist_string(&reply.writer, "GetValue"));
    assert(rtcp_plist_key(&reply.writer, "Value"));
    assert(rtcp_plist_data(&reply.writer, data, length));
    return send_reply(stream, &reply);
}

static int send_get_value_string(rtcp_stream_t *stream, const char *value)
{
    reply_t reply;
    reply_init(&reply);
    assert(rtcp_plist_key(&reply.writer, "Request"));
    assert(rtcp_plist_string(&reply.writer, "GetValue"));
    assert(rtcp_plist_key(&reply.writer, "Value"));
    assert(rtcp_plist_string(&reply.writer, value));
    return send_reply(stream, &reply);
}

static int tls_server_handshake(rtcp_stream_t *raw, mbedtls_ssl_context *ssl,
                                mbedtls_ssl_config *config,
                                mbedtls_x509_crt *certificate,
                                mbedtls_pk_context *key)
{
    mbedtls_ssl_init(ssl);
    mbedtls_ssl_config_init(config);
    mbedtls_x509_crt_init(certificate);
    mbedtls_pk_init(key);

    if (mbedtls_x509_crt_parse(certificate,
                               s_phone.identity.root_certificate,
                               s_phone.identity.root_certificate_length +
                                   1U) != 0 ||
        mbedtls_pk_parse_key(key, s_phone.identity.root_private_key,
                             s_phone.identity.root_private_key_length + 1U,
                             NULL, 0U) != 0 ||
        mbedtls_ssl_config_defaults(config, MBEDTLS_SSL_IS_SERVER,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        return RTCP_EIO;
    }
    mbedtls_ssl_conf_rng(config, test_rng_ssl, NULL);
    mbedtls_ssl_conf_authmode(config, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_min_version(config, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_max_version(config, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);
    if (mbedtls_ssl_conf_own_cert(config, certificate, key) != 0 ||
        mbedtls_ssl_setup(ssl, config) != 0) {
        return RTCP_EIO;
    }
    mbedtls_ssl_set_bio(ssl, raw, tls_send, tls_recv, NULL);
    return mbedtls_ssl_handshake(ssl) == 0 ? RTCP_OK : RTCP_EIO;
}

static void tls_server_free(mbedtls_ssl_context *ssl,
                            mbedtls_ssl_config *config,
                            mbedtls_x509_crt *certificate,
                            mbedtls_pk_context *key)
{
    mbedtls_ssl_free(ssl);
    mbedtls_ssl_config_free(config);
    mbedtls_x509_crt_free(certificate);
    mbedtls_pk_free(key);
}

/* StartService over the TLS lockdown session, then wait for the client's
 * close_notify so the handler can join. */
static void serve_lockdown_tls(rtcp_stream_t *raw)
{
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt certificate;
    mbedtls_pk_context key;
    tls_stream_context_t tls_context;
    rtcp_stream_t tls;
    uint8_t buffer[4096];
    size_t length;

    if (tls_server_handshake(raw, &ssl, &config, &certificate, &key) !=
        RTCP_OK) {
        tls_server_free(&ssl, &config, &certificate, &key);
        return;
    }
    tls_context.ssl = &ssl;
    tls.ops = &tls_stream_ops;
    tls.context = &tls_context;

    if (read_frame(&tls, buffer, sizeof(buffer), &length) == RTCP_OK &&
        strstr((const char *)buffer, "<string>StartService</string>") !=
            NULL) {
        reply_t reply;
        reply_init(&reply);
        assert(rtcp_plist_key(&reply.writer, "Request"));
        assert(rtcp_plist_string(&reply.writer, "StartService"));
        assert(rtcp_plist_key(&reply.writer, "Port"));
        assert(rtcp_plist_integer(&reply.writer, s_phone.service_port));
        assert(rtcp_plist_key(&reply.writer, "EnableServiceSSL"));
        assert(rtcp_plist_boolean(&reply.writer, s_phone.service_ssl));
        (void)send_reply(&tls, &reply);

        while (tls_stream_read(&tls_context, buffer, sizeof(buffer), 0) > 0) {
        }
    }
    tls_server_free(&ssl, &config, &certificate, &key);
}

static void serve_lockdown(rtcp_stream_t *raw)
{
    uint8_t buffer[16U * 1024U];
    size_t length;

    for (;;) {
        if (read_frame(raw, buffer, sizeof(buffer), &length) != RTCP_OK) {
            return;
        }
        const char *document = (const char *)buffer;
        if (strstr(document, "<string>SetValue</string>") != NULL) {
            if (send_request_ok(raw, "SetValue") != RTCP_OK) {
                return;
            }
        } else if (strstr(document, "DevicePublicKey") != NULL) {
            if (send_get_value_data(raw, (const uint8_t *)device_public_key_pem,
                                    strlen(device_public_key_pem)) !=
                RTCP_OK) {
                return;
            }
        } else if (strstr(document, "WiFiAddress") != NULL) {
            if (send_get_value_string(raw, "00:11:22:33:44:55") != RTCP_OK) {
                return;
            }
        } else if (strstr(document, "<string>Pair</string>") != NULL) {
            unsigned seen;
            pthread_mutex_lock(&s_phone.lock);
            seen = ++s_phone.pair_requests;
            pthread_mutex_unlock(&s_phone.lock);
            if (seen <= s_phone.pair_pending_answers) {
                if (send_error(raw, "PairingDialogResponsePending") !=
                    RTCP_OK) {
                    return;
                }
            } else if (s_phone.deny_pair) {
                if (send_error(raw, "UserDeniedPairing") != RTCP_OK) {
                    return;
                }
            } else if (send_request_ok(raw, "Pair") != RTCP_OK) {
                return;
            }
        } else if (strstr(document, "<string>StartSession</string>") !=
                   NULL) {
            reply_t reply;
            reply_init(&reply);
            assert(rtcp_plist_key(&reply.writer, "Request"));
            assert(rtcp_plist_string(&reply.writer, "StartSession"));
            assert(rtcp_plist_key(&reply.writer, "EnableSessionSSL"));
            assert(rtcp_plist_boolean(&reply.writer, true));
            if (send_reply(raw, &reply) != RTCP_OK) {
                return;
            }
            serve_lockdown_tls(raw);
            return;
        } else if (send_error(raw, "InvalidRequest") != RTCP_OK) {
            return;
        }
    }
}

/* The carkit service: optionally TLS, then echo bytes until the peer
 * closes. */
static void serve_service(rtcp_stream_t *raw)
{
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt certificate;
    mbedtls_pk_context key;
    tls_stream_context_t tls_context;
    rtcp_stream_t stream = *raw;
    uint8_t buffer[128];
    int received;

    if (s_phone.service_ssl) {
        if (tls_server_handshake(raw, &ssl, &config, &certificate, &key) !=
            RTCP_OK) {
            tls_server_free(&ssl, &config, &certificate, &key);
            return;
        }
        tls_context.ssl = &ssl;
        stream.ops = &tls_stream_ops;
        stream.context = &tls_context;
    }

    do {
        received = stream.ops->read(stream.context, buffer, sizeof(buffer),
                                    5000U);
        if (received > 0) {
            if (stream.ops->write(stream.context, buffer,
                                  (size_t)received) != RTCP_OK) {
                break;
            }
        }
    } while (received > 0);

    if (s_phone.service_ssl) {
        tls_server_free(&ssl, &config, &certificate, &key);
    }
}

static void *connection_thread(void *argument)
{
    connection_t *connection = argument;
    rtcp_stream_t raw = { &duplex_ops, &connection->duplex };

    if (connection->port == LOCKDOWN_PORT) {
        serve_lockdown(&raw);
    } else {
        serve_service(&raw);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Session callbacks                                                   */
/* ------------------------------------------------------------------ */

static connection_t s_connections[MAX_CONNECTIONS];
static bool fail_service_open, fail_service_tls_read;

static int connection_read(void *context, void *buffer, size_t length,
                           uint32_t timeout_ms)
{
    connection_t *connection = context;

    if (connection->port == SERVICE_PORT && fail_service_tls_read) return RTCP_ETIMEDOUT;
    return pipe_read(&connection->server_to_client, buffer, length,
                     timeout_ms);
}

static int connection_write(void *context, const void *data, size_t length)
{
    connection_t *connection = context;

    return pipe_write(&connection->client_to_server, data, length);
}

static const rtcp_stream_ops_t connection_ops = {
    .read = connection_read,
    .write = connection_write,
};

static int fake_open(void *user, uint16_t port, rtcp_stream_t *stream)
{
    (void)user;
    if (port == SERVICE_PORT && fail_service_open) return RTCP_ETIMEDOUT;
    for (size_t i = 0U; i < MAX_CONNECTIONS; ++i) {
        connection_t *connection = &s_connections[i];
        if (connection->used) {
            continue;
        }
        memset(connection, 0, sizeof(*connection));
        connection->phone = &s_phone;
        connection->port = port;
        connection->used = true;
        pipe_init(&connection->client_to_server);
        pipe_init(&connection->server_to_client);
        connection->duplex.in.ops = &pipe_ops;
        connection->duplex.in.context = &connection->client_to_server;
        connection->duplex.out.ops = &pipe_ops;
        connection->duplex.out.context = &connection->server_to_client;
        assert(pthread_create(&connection->thread, NULL, connection_thread,
                              connection) == 0);
        stream->ops = &connection_ops;
        stream->context = connection;
        return RTCP_OK;
    }
    return RTCP_ENOMEM;
}

static void fake_close(void *user, const rtcp_stream_t *stream)
{
    connection_t *connection = stream->context;

    (void)user;
    pthread_mutex_lock(&connection->client_to_server.lock);
    connection->client_to_server.writer_closed = 1;
    pthread_cond_broadcast(&connection->client_to_server.cond);
    pthread_mutex_unlock(&connection->client_to_server.lock);

    assert(pthread_join(connection->thread, NULL) == 0);
    pipe_destroy(&connection->server_to_client);
    pipe_destroy(&connection->client_to_server);
    connection->used = false;
}

static unsigned s_pair_sleeps;

static void fake_sleep(void *user, uint32_t ms)
{
    (void)user;
    (void)ms;
    ++s_pair_sleeps;
}

static void reset_phone(unsigned pending_answers, bool deny, bool service_ssl)
{
    pthread_mutex_lock(&s_phone.lock);
    s_phone.pair_requests = 0;
    s_phone.pair_pending_answers = pending_answers;
    s_phone.deny_pair = deny;
    s_phone.service_ssl = service_ssl;
    pthread_mutex_unlock(&s_phone.lock);
}

static void make_config(rtcp_lockdown_session_config_t *config,
                        rtcp_pair_record_t *record)
{
    memset(config, 0, sizeof(*config));
    config->open_connection = fake_open;
    config->close_connection = fake_close;
    config->connection_user = &s_phone;
    config->sleep_ms = fake_sleep;
    config->rng = test_rng;
    config->label = "rt-carplay";
    config->record = record;
    config->step_timeout_ms = 5000U;
    config->pair_retry_ms = 1U;
    config->pair_attempts = 5U;
}

static void assert_connections_closed(void)
{
    for (size_t i = 0U; i < MAX_CONNECTIONS; ++i) {
        assert(!s_connections[i].used);
    }
}

static void test_full_session(void)
{
    static rtcp_pair_record_t record;
    static rtcp_lockdown_session_t session;
    rtcp_lockdown_session_config_t config;
    rtcp_stream_t service;
    uint8_t buffer[8];
    int received;

    reset_phone(1U, false, true);
    memset(&record, 0, sizeof(record));
    memset(&session, 0, sizeof(session));
    make_config(&config, &record);
    s_pair_sleeps = 0U;

    assert(rtcp_lockdown_session_open(&session, &config) == RTCP_OK);
    assert(s_phone.pair_requests == 2U);
    assert(s_pair_sleeps == 1U);
    assert(session.paired);
    assert(session.service_port == SERVICE_PORT);
    assert(session.service_ssl);
    assert(session.control_tls != NULL);
    assert(session.service_tls != NULL);
    assert(session.remote_error[0] == '\0');
    assert(session.stage == RTCP_LOCKDOWN_STAGE_SERVICE);
    assert(strlen(session.config.host_id) == 36U);
    assert(strlen(session.config.system_buid) == 36U);

    service = rtcp_lockdown_session_service(&session);
    assert(service.ops != NULL);
    assert(rtcp_lockdown_session_service(NULL).ops == NULL);

    assert(service.ops->write(service.context, "ping", 4U) == RTCP_OK);
    received = service.ops->read(service.context, buffer, sizeof(buffer),
                                 5000U);
    assert(received == 4);
    /* The fake service echoes what it receives. */
    assert(memcmp(buffer, "ping", 4U) == 0);

    rtcp_lockdown_session_close(&session);
    assert(session.scratch == NULL);
    assert(session.control_tls == NULL);
    assert(session.service_tls == NULL);
    assert(rtcp_lockdown_session_service(&session).ops == NULL);
    assert_connections_closed();

    /* Closing twice is harmless. */
    rtcp_lockdown_session_close(&session);
}

static void test_service_connect_timeout(void)
{
    static rtcp_pair_record_t record;
    static rtcp_lockdown_session_t session;
    rtcp_lockdown_session_config_t config;
    reset_phone(0U, false, true);
    memset(&record, 0, sizeof(record));
    make_config(&config, &record);
    fail_service_open = true;
    assert(rtcp_lockdown_session_open(&session, &config) == RTCP_ETIMEDOUT);
    fail_service_open = false;
    assert(session.paired && session.stage == RTCP_LOCKDOWN_STAGE_SERVICE);
    assert(!strcmp(session.operation, "OpenServiceConnection"));
    assert(session.last_exchange_result == RTCP_OK && session.last_reply_length > 0);
    assert(session.service_port == SERVICE_PORT && session.service_ssl);
    assert_connections_closed();
}

static void test_service_tls_timeout(void)
{
    static rtcp_pair_record_t record;
    static rtcp_lockdown_session_t session;
    rtcp_lockdown_session_config_t config;
    reset_phone(0U, false, true);
    memset(&record, 0, sizeof(record));
    make_config(&config, &record);
    fail_service_tls_read = true;
    assert(rtcp_lockdown_session_open(&session, &config) == RTCP_ETIMEDOUT);
    fail_service_tls_read = false;
    assert(session.paired && session.stage == RTCP_LOCKDOWN_STAGE_SERVICE);
    assert(!strcmp(session.operation, "ServiceTLS"));
    assert(session.last_exchange_result == RTCP_OK && session.last_reply_length > 0);
    assert(session.service_raw.ops == NULL && session.service_tls == NULL);
    assert_connections_closed();
}

static void test_denied_pairing(void)
{
    static rtcp_pair_record_t record;
    static rtcp_lockdown_session_t session;
    rtcp_lockdown_session_config_t config;

    reset_phone(0U, true, false);
    memset(&record, 0, sizeof(record));
    memset(&session, 0, sizeof(session));
    make_config(&config, &record);

    assert(rtcp_lockdown_session_open(&session, &config) == RTCP_EIO);
    assert(s_phone.pair_requests == 1U);
    assert(strcmp(session.remote_error, "UserDeniedPairing") == 0);
    assert(session.operation && !strcmp(session.operation, "Pair"));
    assert(session.exchange_count >= 4 && session.last_request_length > 0);
    assert(session.last_reply_length > 0 && session.last_exchange_result == RTCP_OK);
    assert(session.stage == RTCP_LOCKDOWN_STAGE_PAIR);
    assert(!session.paired);
    assert(session.scratch == NULL);
    assert_connections_closed();
}

static unsigned timeout_closes;
static int diagnostic_read(void *context, void *buffer, size_t length, uint32_t ms)
{
    (void)context; (void)buffer; (void)length;
    assert(ms == 25U);
    return RTCP_ETIMEDOUT;
}
static int diagnostic_write(void *context, const void *data, size_t length)
{
    (void)context; assert(data && length); return RTCP_OK;
}
static int diagnostic_open(void *user, uint16_t port, rtcp_stream_t *stream)
{
    (void)user; assert(port == LOCKDOWN_PORT);
    static const rtcp_stream_ops_t ops = {diagnostic_read, diagnostic_write};
    stream->ops = &ops; stream->context = NULL; return RTCP_OK;
}
static void diagnostic_close(void *user, const rtcp_stream_t *stream)
{
    (void)user; assert(stream->ops); ++timeout_closes;
}
static void test_timeout_diagnostic_survives_cleanup(void)
{
    static rtcp_pair_record_t record;
    static rtcp_lockdown_session_t session;
    rtcp_lockdown_session_config_t config;
    memset(&record, 0, sizeof(record));
    make_config(&config, &record);
    config.open_connection = diagnostic_open;
    config.close_connection = diagnostic_close;
    config.step_timeout_ms = 25U;
    timeout_closes = 0;
    assert(rtcp_lockdown_session_open(&session, &config) == RTCP_ETIMEDOUT);
    assert(session.stage == RTCP_LOCKDOWN_STAGE_PAIR);
    assert(!strcmp(session.operation, "SetValue/UntrustedHostBUID"));
    assert(session.exchange_count == 1 && session.last_request_length > 0);
    assert(session.last_reply_length == 0 && session.last_exchange_result == RTCP_ETIMEDOUT);
    assert(!session.paired && !session.scratch && timeout_closes == 1);
}

int main(void)
{
    memset(&s_phone, 0, sizeof(s_phone));
    assert(pthread_mutex_init(&s_phone.lock, NULL) == 0);

    /* The fake phone needs a TLS identity; it is not the client's pair
     * record and the client does not verify it. */
    assert(rtcp_pair_record_generate(
               &s_phone.identity, (const uint8_t *)device_public_key_pem,
               strlen(device_public_key_pem), "00:11:22:33:44:55",
               "11111111-2222-4333-8444-555555555555",
               "66666666-7777-4888-8999-AAAAAAAAAAAA", test_rng, NULL) ==
           RTCP_OK);
    s_phone.service_port = SERVICE_PORT;

    test_full_session();
    test_denied_pairing();
    test_service_connect_timeout();
    test_service_tls_timeout();
    test_timeout_diagnostic_survives_cleanup();

    pthread_mutex_destroy(&s_phone.lock);
    puts("lockdown session host test passed");
    return 0;
}
