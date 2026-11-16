/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "headless.h"
#include "airplay_ed25519.h"
#include "airplay_info_plist.h"
#include "airplay_pairing_store.h"
#include "airplay_txt.h"
#include "mdns_responder.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_flag active = ATOMIC_FLAG_INIT;
typedef struct
{
    const rtcp_headless_config_t *config;
    airplay_ed25519_keypair_t key;
    rtcp_headless_result_t *result;
    int storage_error;
    uint8_t info[AIRPLAY_INFO_PLIST_MAX_BYTES];
    uint8_t txt[512];
    char public_key_hex[65];
} headless_t;

/* Same backup/commit algorithm used by the host's pairing_files.c. DFS
 * ports may expose negative errno; FatFS rename cannot replace a target. */
static bool missing_file(void)
{
    return errno == ENOENT || errno == -ENOENT;
}
static int path_suffix(char output[256], const char *path, const char *ending)
{
    int n = snprintf(output, 256, "%s%s", path, ending);
    return n > 0 && n < 256 ? RTCP_OK : RTCP_EINVAL;
}
static FILE *open_saved_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL && missing_file())
    {
        char backup[256];
        if (path_suffix(backup, path, ".bak") != RTCP_OK)
        {
            errno = EINVAL;
            return NULL;
        }
        file = fopen(backup, "rb");
    }
    return file;
}
static int write_file(const char *path, const uint8_t *bytes, size_t length)
{
    char temporary[256], backup[256];
    if (path_suffix(temporary, path, ".tmp") != RTCP_OK ||
        path_suffix(backup, path, ".bak") != RTCP_OK)
        return RTCP_EINVAL;
    FILE *file = fopen(temporary, "wb");
    if (file == NULL)
        return RTCP_EIO;
    bool ok = fwrite(bytes, 1, length, file) == length;
    if (fclose(file) != 0)
        ok = false;
    if (!ok)
        goto fail;
    file = fopen(path, "rb");
    bool had_primary = file != NULL;
    if (file != NULL)
    {
        if (fclose(file) != 0)
            goto fail;
    }
    else if (!missing_file())
        goto fail;
    if (had_primary)
    {
        if (remove(backup) != 0 && !missing_file())
            goto fail;
        if (rename(path, backup) != 0)
            goto fail;
    }
    if (rename(temporary, path) == 0)
        return RTCP_OK;
    if (had_primary)
        (void)rename(backup, path);
fail:
    (void)remove(temporary);
    return RTCP_EIO;
}

/* Never replace corrupt/inaccessible existing identity with a new one. */
static int load_identity(headless_t *app)
{
    uint8_t seed[32] = { 0 };
    int result = RTCP_OK;
    /* Probe the real entropy source even when reusing an existing identity. */
    if (!app->config->os->random(seed, sizeof(seed)))
    {
        memset(seed, 0, sizeof(seed));
        return RTCP_EIO;
    }
    FILE *file = open_saved_file(app->config->seed_path);
    if (file != NULL)
    {
        size_t length = fread(seed, 1, sizeof(seed), file);
        bool ok = length == sizeof(seed) && fgetc(file) == EOF && !ferror(file);
        if (fclose(file) != 0)
            ok = false;
        if (!ok)
            result = RTCP_EIO;
    }
    else if (!missing_file())
    {
        result = RTCP_EIO;
    }
    else
    {
        result = write_file(app->config->seed_path, seed, sizeof(seed));
    }
    if (result == RTCP_OK && !airplay_ed25519_keypair_from_seed(seed, &app->key))
        result = RTCP_EIO;
    memset(seed, 0, sizeof(seed));
    return result;
}

static int store_load(uint8_t *bytes, size_t capacity, size_t *length, void *context)
{
    headless_t *app = context;
    *length = 0;
    FILE *file = open_saved_file(app->config->store_path);
    if (file == NULL)
        return missing_file() ? RTCP_OK : RTCP_EIO;
    *length = fread(bytes, 1, capacity, file);
    bool ok = !ferror(file) && fgetc(file) == EOF && !ferror(file);
    if (fclose(file) != 0)
        ok = false;
    return ok ? RTCP_OK : RTCP_EIO;
}
static int store_save(const uint8_t *bytes, size_t length, void *context)
{
    headless_t *app = context;
    app->storage_error = write_file(app->config->store_path, bytes, length);
    return app->storage_error;
}
static bool random_bytes(uint8_t *bytes, size_t length, void *context)
{
    headless_t *app = context;
    return app->config->os->random(bytes, length);
}
static bool sign_bytes(const uint8_t *bytes, size_t length, uint8_t signature[64], void *context)
{
    headless_t *app = context;
    return airplay_ed25519_sign(&app->key, bytes, length, signature);
}
static bool verify_bytes(const uint8_t key[32], const uint8_t *bytes, size_t length,
                         const uint8_t signature[64], void *context)
{
    (void)context;
    return airplay_ed25519_verify(key, bytes, length, signature);
}
static int discard_video(rtcp_video_message_type_t type, uint8_t *bytes, size_t length,
                         size_t prefix, uint64_t sender_ns, uint64_t arrival_us, void *context)
{
    (void)bytes;
    (void)prefix;
    (void)sender_ns;
    (void)arrival_us;
    headless_t *app = context;
    if (type == RTCP_VIDEO_CONFIG_H264)
        ++app->result->video_configs;
    else
        ++app->result->video_frames;
    app->result->video_bytes += length;
    return RTCP_OK;
}

static mdns_responder_t *create_discovery(headless_t *app, uint16_t port)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i)
    {
        app->public_key_hex[2 * i] = hex[app->key.public_key[i] >> 4];
        app->public_key_hex[2 * i + 1] = hex[app->key.public_key[i] & 15];
    }
    char features[32];
    if (!airplay_txt_format_features(AIRPLAY_INFO_DEFAULT_FEATURES, features, sizeof(features)))
        return NULL;
    airplay_txt_config_t txt = { .features = features, .device_id = app->config->device_id, .model = "rt-carplay-headless", .source_version = AIRPLAY_INFO_DEFAULT_SOURCE_VERSION, .protocol_version = "1.1", .pairing_id = app->config->device_id, .public_key = app->public_key_hex };
    airplay_txt_record_t records[AIRPLAY_TXT_MAX_RECORDS];
    size_t count = airplay_txt_build(&txt, records, AIRPLAY_TXT_MAX_RECORDS);
    size_t length = airplay_txt_encode(records, count, app->txt, sizeof(app->txt));
    if (count == 0 || length == 0)
        return NULL;
    mdns_responder_config_t discovery = { .host_name = app->config->host_name,
                                          .instance_name = app->config->name,
                                          .port = port,
                                          .txt = app->txt,
                                          .txt_len = length };
    memcpy(discovery.ipv4, app->config->ipv4, 4);
    return mdns_responder_create(&discovery);
}

static bool valid_device_id(const char *id)
{
    if (id == NULL || strlen(id) != 17)
        return false;
    for (size_t i = 0; i < 17; ++i)
    {
        if (i % 3 == 2)
        {
            if (id[i] != ':')
                return false;
        }
        else if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f') ||
                   (id[i] >= 'A' && id[i] <= 'F')))
            return false;
    }
    return true;
}

int rtcp_headless_run(const rtcp_headless_config_t *config,
                      const atomic_bool *stop_requested, rtcp_headless_result_t *result)
{
    if (config == NULL || stop_requested == NULL || result == NULL ||
        config->os == NULL || config->net == NULL || config->os->random == NULL ||
        config->name == NULL || !valid_device_id(config->device_id) ||
        config->seed_path == NULL || config->store_path == NULL ||
        config->seed_path[0] == '\0' || config->store_path[0] == '\0' ||
        strcmp(config->seed_path, config->store_path) == 0 || !config->width || !config->height ||
        (config->advertise && (config->host_name == NULL || !config->ipv4[0])))
        return RTCP_EINVAL;
    if (atomic_flag_test_and_set_explicit(&active, memory_order_acquire))
        return RTCP_ESTATE;
    memset(result, 0, sizeof(*result));
    if (atomic_load_explicit(stop_requested, memory_order_acquire))
    {
        atomic_flag_clear_explicit(&active, memory_order_release);
        return RTCP_OK;
    }
    headless_t *app = calloc(1, sizeof(*app));
    if (app == NULL)
    {
        atomic_flag_clear_explicit(&active, memory_order_release);
        return RTCP_ENOMEM;
    }
    app->config = config;
    app->result = result;
    rtcp_receiver_t *receiver = NULL;
    mdns_responder_t *mdns = NULL;
    bool store_initialized = false;
    int rc = load_identity(app);
    if (rc != RTCP_OK)
        goto done;
    airplay_pairing_store_backend_t backend = { .load = store_load, .save = store_save, .context = app };
    rc = airplay_pairing_store_init(&backend);
    if (rc != RTCP_OK)
        goto done;
    store_initialized = true;
    airplay_info_plist_config_t info = { .name = config->name, .device_id = config->device_id, .model = "rt-carplay-headless", .width_pixels = config->width, .height_pixels = config->height };
    size_t info_length = 0;
    if (!airplay_info_plist_build(&info, app->info, sizeof(app->info), &info_length))
    {
        rc = RTCP_EINVAL;
        goto done;
    }
    const uint8_t *identifier = (const uint8_t *)config->device_id;
    rtcp_receiver_config_t receiver_config = { .port = config->port,
                                               .on_state = config->on_state,
                                               .context = config->context };
    receiver_config.platform.os = config->os;
    receiver_config.platform.net = config->net;
    receiver_config.platform.info_plist = app->info;
    receiver_config.platform.info_plist_len = info_length;
    receiver_config.platform.pairing.pair_setup = (rtcp_pair_setup_provider_t){
        .pairing_code = "3939",
        .accessory_identifier = identifier,
        .accessory_identifier_len = 17,
        .accessory_public_key = app->key.public_key,
        .random = random_bytes,
        .sign_accessory = sign_bytes,
        .verify_controller = verify_bytes,
        .save_pairing = airplay_pairing_store_save_pairing,
        .provider_context = app
    };
    receiver_config.platform.pairing.pair_verify = (rtcp_pair_verify_provider_t){
        .accessory_identifier = identifier,
        .accessory_identifier_len = 17,
        .random = random_bytes,
        .sign_accessory = sign_bytes,
        .verify_controller = verify_bytes,
        .lookup_controller = airplay_pairing_store_lookup_controller,
        .provider_context = app
    };
    receiver_config.media.on_video = discard_video;
    receiver_config.media.video_context = app;
    rc = rtcp_receiver_create(&receiver_config, &receiver);
    if (rc == RTCP_OK)
        rc = rtcp_receiver_start(receiver);
    if (rc != RTCP_OK)
        goto done;
    if (config->advertise)
    {
        mdns = create_discovery(app, rtcp_receiver_port(receiver));
        rc = mdns == NULL ? RTCP_ENOMEM : mdns_responder_open(mdns, config->net);
        if (rc != RTCP_OK)
            goto done;
    }
    if (config->on_ready != NULL)
        config->on_ready(rtcp_receiver_port(receiver), app->key.public_key, config->context);
    while (!atomic_load_explicit(stop_requested, memory_order_acquire))
    {
        if (mdns != NULL)
        {
            rc = mdns_responder_poll(mdns, config->net, 10);
            if (rc < 0)
                break;
        }
        rc = rtcp_receiver_poll(receiver, mdns == NULL ? 10 : 0);
        if (rc != RTCP_OK)
            break;
        if (app->storage_error != RTCP_OK)
        {
            rc = app->storage_error;
            break;
        }
    }
    (void)rtcp_receiver_get_diagnostics(receiver, &result->diagnostics);
done:
    if (rc == RTCP_OK && app->storage_error != RTCP_OK)
        rc = app->storage_error;
    if (mdns != NULL)
    {
        mdns_responder_close(mdns, config->net);
        mdns_responder_destroy(mdns);
    }
    (void)rtcp_receiver_destroy(receiver);
    if (store_initialized)
        airplay_pairing_store_deinit();
    airplay_ed25519_keypair_clear(&app->key);
    free(app);
    atomic_flag_clear_explicit(&active, memory_order_release);
    return rc;
}
