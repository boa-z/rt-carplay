/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L

#include "lockdown_tls.h"

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
#include "rtcp/rtcp.h"

/* The same fixed phone public key as test_lockdown_pair, so a pair record
 * can be generated without a phone. */
static const char device_public_key_pem[] =
    "-----BEGIN RSA PUBLIC KEY-----\n"
    "MIIBCgKCAQEA4jtAqhC/gkeGKQjAPhpvmNb3yQx/qj8I2JXViuwwuZVecE5pAoYn\n"
    "wO58YS1014QeEID5xcEHQD3m8m4zl64d2B1sDhegRhZozvR2yf1fSXMnTu7FQbL0\n"
    "B0ZSNRUOCWpdCLKvWd36ZykydZmbZnm4hGlN/CYhFM8QaaH3GGJ6D9ug3PPexM94\n"
    "/NUqmvcbDdY5qku+92kRQKL06AdfU6NUhQc2tBZL4f+dSHgLuHtOwZRaG3oiesAH\n"
    "fWpRCGGDmoccmNjiiXT8WrD5iaa8m3Re0Gb3QiXmrDGt3HiBqzcgeiocc3/wxQpS\n"
    "dbYW3XzLF6e0S6Dultfvu9zJAnY4vDidHwIDAQAB\n"
    "-----END RSA PUBLIC KEY-----\n";

static uint32_t rng_state = 0x2468ace0U;

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

/* One duplex stream built from two unidirectional pipes. */
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

typedef struct {
    pipe_t client_to_server;
    pipe_t server_to_client;
    rtcp_pair_record_t *record;

    int handshake_ret;
    int read_ret;
    int write_ret;
    int close_ret;
    char request[8];
} server_t;

static int server_send(void *context, const unsigned char *data,
                       size_t length)
{
    duplex_t *duplex = context;
    int ret;

    ret = duplex->out.ops->write(duplex->out.context, data, length);
    if (ret != RTCP_OK) {
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    return (int)length;
}

static int server_recv(void *context, unsigned char *buffer, size_t length)
{
    duplex_t *duplex = context;
    int ret = duplex->in.ops->read(duplex->in.context, buffer, length, 5000U);

    if (ret == RTCP_ETIMEDOUT) {
        return MBEDTLS_ERR_SSL_TIMEOUT;
    }
    if (ret < 0) {
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    return ret;
}

static void *server_thread(void *argument)
{
    server_t *server = argument;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt certificate;
    mbedtls_pk_context key;
    duplex_t transport;
    uint8_t scratch[64];

    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&config);
    mbedtls_x509_crt_init(&certificate);
    mbedtls_pk_init(&key);

    assert(mbedtls_x509_crt_parse(&certificate,
                                  server->record->root_certificate,
                                  server->record->root_certificate_length +
                                      1U) == 0);
    assert(mbedtls_pk_parse_key(&key, server->record->root_private_key,
                                server->record->root_private_key_length + 1U,
                                NULL, 0U) == 0);
    assert(mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_SERVER,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT) == 0);
    mbedtls_ssl_conf_rng(&config, test_rng_ssl, NULL);
    assert(mbedtls_ssl_conf_own_cert(&config, &certificate, &key) == 0);
    /* Demand the client certificate and trust only the pair-record root;
     * a successful handshake proves the client presented that identity. */
    mbedtls_ssl_conf_ca_chain(&config, &certificate, NULL);
    mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_min_version(&config, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_max_version(&config, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);
    assert(mbedtls_ssl_setup(&ssl, &config) == 0);

    transport.in.ops = &pipe_ops;
    transport.in.context = &server->client_to_server;
    transport.out.ops = &pipe_ops;
    transport.out.context = &server->server_to_client;
    mbedtls_ssl_set_bio(&ssl, &transport, server_send, server_recv, NULL);

    server->handshake_ret = mbedtls_ssl_handshake(&ssl);
    if (server->handshake_ret == 0) {
        server->read_ret =
            mbedtls_ssl_read(&ssl, (unsigned char *)server->request, 4U);
        if (server->read_ret == 4) {
            /* Hold the reply back so the client exercises a read timeout
             * and then a successful retry on the same session. */
            struct timespec delay = { 0, 200 * 1000 * 1000L };
            nanosleep(&delay, NULL);
            server->write_ret = mbedtls_ssl_write(
                &ssl, (const unsigned char *)"pong", 4U);
        }
        server->close_ret = mbedtls_ssl_read(&ssl, scratch, sizeof(scratch));
    }

    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&config);
    mbedtls_x509_crt_free(&certificate);
    mbedtls_pk_free(&key);
    return NULL;
}

static int closed_read(void *context, void *buffer, size_t length,
                       uint32_t timeout_ms)
{
    (void)context;
    (void)buffer;
    (void)length;
    (void)timeout_ms;
    return 0;
}

static int closed_write(void *context, const void *data, size_t length)
{
    (void)context;
    (void)data;
    (void)length;
    return RTCP_OK;
}

static const rtcp_stream_ops_t closed_ops = {
    .read = closed_read,
    .write = closed_write,
};

static void test_open_errors(void)
{
    static rtcp_pair_record_t record;
    rtcp_lockdown_tls_t *tls = (rtcp_lockdown_tls_t *)1;
    rtcp_stream_t transport = { &closed_ops, NULL };

    assert(rtcp_lockdown_tls_open(NULL, &transport, &record, test_rng, NULL,
                                  1000U) == RTCP_EINVAL);
    assert(rtcp_lockdown_tls_open(&tls, NULL, &record, test_rng, NULL,
                                  1000U) == RTCP_EINVAL);
    assert(rtcp_lockdown_tls_open(&tls, &transport, NULL, test_rng, NULL,
                                  1000U) == RTCP_EINVAL);
    assert(rtcp_lockdown_tls_open(&tls, &transport, &record, NULL, NULL,
                                  1000U) == RTCP_EINVAL);
    /* A record without its root identity cannot be used at all. */
    memset(&record, 0, sizeof(record));
    assert(rtcp_lockdown_tls_open(&tls, &transport, &record, test_rng, NULL,
                                  1000U) == RTCP_EINVAL);
    assert(tls == NULL);

    /* A closed transport fails the handshake rather than hanging. */
    memset(&record, 0, sizeof(record));
    record.root_certificate_length = 1U;
    record.root_private_key_length = 1U;
    assert(rtcp_lockdown_tls_open(&tls, &transport, &record, test_rng, NULL,
                                  1000U) == RTCP_EIO);
    assert(tls == NULL);

    rtcp_stream_t none = rtcp_lockdown_tls_stream(NULL);
    assert(none.ops == NULL && none.context == NULL);
}

static void test_handshake_and_io(void)
{
    static rtcp_pair_record_t record;
    static server_t server;
    static rtcp_lockdown_tls_t *tls;
    pthread_t thread;
    uint8_t buffer[8];
    rtcp_stream_t client_transport;
    rtcp_stream_t stream;
    duplex_t client_duplex;

    memset(&record, 0, sizeof(record));
    assert(rtcp_pair_record_generate(
               &record, (const uint8_t *)device_public_key_pem,
               sizeof(device_public_key_pem) - 1U, "00:11:22:33:44:55",
               "11111111-2222-4333-8444-555555555555",
               "66666666-7777-4888-8999-AAAAAAAAAAAA", test_rng, NULL) ==
           RTCP_OK);

    memset(&server, 0, sizeof(server));
    server.record = &record;
    server.handshake_ret = -1;
    server.read_ret = -1;
    server.write_ret = -1;
    server.close_ret = -1;
    pipe_init(&server.client_to_server);
    pipe_init(&server.server_to_client);

    client_duplex.in.ops = &pipe_ops;
    client_duplex.in.context = &server.server_to_client;
    client_duplex.out.ops = &pipe_ops;
    client_duplex.out.context = &server.client_to_server;
    client_transport.ops = &duplex_ops;
    client_transport.context = &client_duplex;

    assert(pthread_create(&thread, NULL, server_thread, &server) == 0);
    assert(rtcp_lockdown_tls_open(&tls, &client_transport, &record, test_rng,
                                  NULL, 5000U) == RTCP_OK);
    assert(tls != NULL);

    stream = rtcp_lockdown_tls_stream(tls);
    assert(stream.ops != NULL && stream.context == tls);

    assert(stream.ops->write(stream.context, "ping", 4U) == RTCP_OK);

    /* The server is deliberately silent for 200 ms; a 100 ms read reports
     * a timeout and leaves the session usable. */
    assert(stream.ops->read(stream.context, buffer, sizeof(buffer), 100U) ==
           RTCP_ETIMEDOUT);
    assert(stream.ops->read(stream.context, buffer, sizeof(buffer), 2000U) ==
           4);
    assert(memcmp(buffer, "pong", 4U) == 0);

    rtcp_lockdown_tls_close(tls);
    assert(pthread_join(thread, NULL) == 0);

    assert(server.handshake_ret == 0);
    assert(server.read_ret == 4);
    assert(memcmp(server.request, "ping", 4U) == 0);
    assert(server.write_ret == 4);
    /* 0 or a close_notify error both end the session cleanly. */
    assert(server.close_ret == 0 ||
           server.close_ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY);

    pipe_destroy(&server.server_to_client);
    pipe_destroy(&server.client_to_server);
}

int main(void)
{
    test_open_errors();
    test_handshake_and_io();
    puts("lockdown TLS host test passed");
    return 0;
}
