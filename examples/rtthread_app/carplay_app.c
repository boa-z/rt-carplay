/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * RT-Thread + Luban-Lite receiver composition.
 *
 * Boot order (start): entropy + Ed25519 identity, pairing store, SoftAP and
 * vendor IE, DHCP, /info plist, AirPlay RTSP server, mDNS, input bridge,
 * on-board video sink (RTCARPLAY_VIDEO), poll thread.  Everything is torn
 * down by that thread when it observes the stop flag, so the RTSP server and
 * the event cipher stay single-threaded.
 */
#include "carplay_app.h"

#include <rtthread.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "airplay_ed25519.h"
#include "airplay_info_plist.h"
#include "airplay_pairing_store.h"
#include "pairing_files.h"
#include "airplay_rtsp_server.h"
#include "airplay_txt.h"
#include "airplay_vendor_ie.h"
#include "carplay_ctrl.h"
#include "input_bridge.h"
#include "mdns_browser.h"
#include "mdns_responder.h"
#include "rtcp/input.h"
#include "rtcp/rtcp.h"
#include "rtcp_lubanlite.h"
#include "rtcp_rtthread.h"

#if defined(RTCARPLAY_VIDEO)
#include "video_sink_lubanlite.h"
#endif

#if defined(RTCARPLAY_AUDIO)
#include "audio_sink_lubanlite.h"
#endif

#if defined(RTCARPLAY_TOUCH_PANEL)
#include "touch_panel_lubanlite.h"
#endif

#if defined(RTCARPLAY_IAP2)
#include "iap2_link_lubanlite.h"
#endif

#define APP_LOG(fmt, ...) rt_kprintf("[carplay] " fmt "\n", ##__VA_ARGS__)

/* carplay_start uses the same geometry as the SDK autostart path.  In app
 * mode the board's Kconfig values (which must match the panel) win; a caller
 * that links the library directly gets the 1280x480 widescreen fallback. */
#ifndef CARPLAY_APP_DEFAULT_WIDTH
#if defined(RTCARPLAY_SCREEN_WIDTH)
#define CARPLAY_APP_DEFAULT_WIDTH RTCARPLAY_SCREEN_WIDTH
#else
#define CARPLAY_APP_DEFAULT_WIDTH 1280U
#endif
#endif
#ifndef CARPLAY_APP_DEFAULT_HEIGHT
#if defined(RTCARPLAY_SCREEN_HEIGHT)
#define CARPLAY_APP_DEFAULT_HEIGHT RTCARPLAY_SCREEN_HEIGHT
#else
#define CARPLAY_APP_DEFAULT_HEIGHT 480U
#endif
#endif
#ifndef CARPLAY_APP_DEFAULT_ROTATION
#if defined(RTCARPLAY_ROTATION)
#define CARPLAY_APP_DEFAULT_ROTATION RTCARPLAY_ROTATION
#else
#define CARPLAY_APP_DEFAULT_ROTATION 0U
#endif
#endif

/* Advertised PCM sample rate.  The sink configures the codec from the format
 * the SETUP actually negotiates; this only selects which capability mask the
 * /info plist publishes (44.1 kHz or 48 kHz). */
#ifndef CARPLAY_APP_AUDIO_SAMPLE_RATE
#define CARPLAY_APP_AUDIO_SAMPLE_RATE 44100U
#endif

#define DEFAULT_NAME "rt-carplay"
#define DEFAULT_MODEL "ArtInChip"
#define DEFAULT_SSID "ArtInChip CarPlay"
#define DEFAULT_PASSPHRASE "88888888"
/* The vendor firmware runs the CarPlay SoftAP on 5 GHz channel 36, which the
 * RTL8733BS supports on the reference board. */
#define DEFAULT_CHANNEL 36U
#define DEFAULT_SEED_PATH "/data/rt-carplay.seed"
#define DEFAULT_STORE_PATH "/data/rt-carplay-pairings.bin"
/* iOS CarPlay runs AirPlay Pair-Setup with this fixed setup code; there is
 * no PIN entry UI.  DiPlay and LIVI hardcode the same value. */
#define DEFAULT_PAIR_CODE "3939"

#define POLL_THREAD_STACK_BYTES (32U * 1024U)
#define POLL_THREAD_PRIORITY 15U
#define POLL_THREAD_TICK 20U

/* CarPlay Control reverse connection.
 *
 * The iPhone advertises _carplay-ctrl._tcp once it commits to CarPlay; the
 * receiver browses it and sends GET /ctrl-int/1/connect.  The vendor reference
 * stack on this board does exactly this (mdnsd_start_carplay_client_connect),
 * as do DiPlay, LIVI, catplay and carplayd's rx_connect.  carplayd's session
 * notes call the connect-out what makes the phone open the RTSP session.
 *
 * Kept switchable (carplay_ctrl on|off) so an A/B run can isolate its effect
 * from the rest of the session establishment. */
#define CARPLAY_CTRL_SERVICE "_carplay-ctrl._tcp.local"
#define CARPLAY_CTRL_USER_AGENT "AirPlay/" AIRPLAY_INFO_DEFAULT_SOURCE_VERSION
#define CARPLAY_CTRL_RETRY_MS 2000U

#define PAIR_CODE_DIGITS 4U
#define TOUCH_QUEUE_LENGTH 32U
#define TOUCH_QUEUE_MASK (TOUCH_QUEUE_LENGTH - 1U)
#define TOUCH_BATCH 8U

#define IP_WAIT_STEP_MS 100U
#define IP_WAIT_TRIES 100U

typedef struct {
    uint8_t phase;
    uint16_t pointer_id;
    uint16_t x;
    uint16_t y;
} touch_event_t;

typedef struct {
    struct rtcp_os os;
    airplay_ed25519_keypair_t keypair;

    char name[33];
    char model[33];
    char ssid[33];
    char passphrase[65];
    uint8_t channel;
    char pairing_code[PAIR_CODE_DIGITS + 1U];

    char device_id[18];
    char host_name[32];
    char seed_path[64];
    char store_path[80];
    char public_key_hex[AIRPLAY_ED25519_PUBLIC_KEY_BYTES * 2U + 1U];

    uint8_t wifi_mac[6];
    bool wifi_mac_valid;

    uint8_t accessory_identifier[64];
    size_t accessory_identifier_len;

    uint16_t width;
    uint16_t height;
    uint16_t rotation;
    bool wired;

    uint8_t ipv4[4];

    uint8_t info_plist[AIRPLAY_INFO_PLIST_MAX_BYTES];
    size_t info_plist_len;
    uint8_t txt[512];
    size_t txt_len;

    mdns_responder_t *mdns;
    mdns_browser_t *browser;
    airplay_rtsp_server_t *server;
    input_bridge_t *bridge;
    rt_mutex_t lock;
    rt_thread_t thread;

    uint16_t airplay_port;
    volatile bool airplay_connected;
    volatile int pairing_storage_error;

    /* CarPlay Control browse/connect state (owned by the poll thread). */
    volatile bool ctrl_enabled;
    uint64_t ctrl_device_id;
    uint32_t ctrl_last_attempt_ms;
    unsigned ctrl_attempts;
    unsigned ctrl_responses;
    int ctrl_last_status;
    /* Last IPv4 peer that opened an RTSP connection: the phone on our AP.
     * Used for a unicast mDNS query as well as the multicast one. */
    uint8_t ctrl_peer_ipv4[4];
    bool ctrl_peer_valid;
    unsigned browse_queries;
    unsigned browse_tx_errors;
    unsigned browse_packets;
    unsigned browse_updates;
    char ctrl_last_endpoint[MDNS_MAX_NAME];

    carplay_app_screen_cb_t on_screen;
    void *screen_context;

    airplay_screen_server_stats_t screen_stats;
#if defined(RTCARPLAY_VIDEO)
    uint32_t perf_ms;
    airplay_screen_server_stats_t perf_screen;
    carplay_video_perf_t perf_video;
#endif
    uint32_t screen_sample_ms;
    uint32_t screen_report_ms;
    uint32_t screen_report_frames;
    unsigned touch_queued;
    unsigned touch_queue_full;
    unsigned hid_reports_sent;
    unsigned hid_reports_failed;
    touch_event_t touch[TOUCH_QUEUE_LENGTH];
    unsigned touch_head;
    unsigned touch_tail;

    unsigned screen_frames;
    unsigned mdns_errors;

    volatile bool stop;
    volatile bool thread_running;
    volatile bool running;
    volatile bool shutting_down;
    bool ap_started;
    bool store_initialized;
#if defined(RTCARPLAY_VIDEO)
    bool video_started;
#endif
#if defined(RTCARPLAY_AUDIO)
    bool audio_started;
    bool touch_panel_started;
    unsigned audio_packets;
#endif
} app_state_t;

static app_state_t s_app;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static bool copy_string(char *destination, size_t capacity, const char *source)
{
    if (source == NULL) return false;
    size_t length = strlen(source);
    if (length == 0U || length >= capacity) return false;
    memcpy(destination, source, length + 1U);
    return true;
}

static void hex_encode(const uint8_t *bytes, size_t len, char *output)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0U; i < len; ++i) {
        output[i * 2U] = digits[bytes[i] >> 4U];
        output[i * 2U + 1U] = digits[bytes[i] & 0x0fU];
    }
    output[len * 2U] = '\0';
}

static bool load_seed(const char *path,
                      uint8_t seed[AIRPLAY_ED25519_SEED_BYTES])
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    uint8_t bytes[AIRPLAY_ED25519_SEED_BYTES];
    size_t got = fread(bytes, 1U, sizeof(bytes), file);
    bool extra = fgetc(file) != EOF;
    fclose(file);
    bool ok = got == sizeof(bytes) && !extra;
    if (ok) memcpy(seed, bytes, sizeof(bytes));
    memset(bytes, 0, sizeof(bytes));
    return ok;
}

static bool write_seed(const char *path,
                       const uint8_t seed[AIRPLAY_ED25519_SEED_BYTES])
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) return false;
    bool ok = fwrite(seed, 1U, AIRPLAY_ED25519_SEED_BYTES, file) ==
              AIRPLAY_ED25519_SEED_BYTES;
    if (fclose(file) != 0) ok = false;
    return ok;
}

/* ------------------------------------------------------------------ */
/* Pairing persistence (files on the data partition)                   */
/* ------------------------------------------------------------------ */

static int store_load(uint8_t *blob, size_t capacity, size_t *len,
                      void *context)
{
    app_state_t *app = context;
    return carplay_file_load(app->store_path, blob, capacity, len);
}

static int store_save(const uint8_t *blob, size_t len, void *context)
{
    app_state_t *app = context;
    int ret = carplay_file_save(app->store_path, blob, len);
    app->pairing_storage_error = ret;
    return ret;
}

/* ------------------------------------------------------------------ */
/* Pairing providers                                                   */
/* ------------------------------------------------------------------ */

static bool provider_random(uint8_t *output, size_t len, void *context)
{
    app_state_t *app = context;
    if (app->os.random == NULL) return false;
    return app->os.random(output, len);
}

static bool provider_save_controller(const uint8_t *id, size_t length,
                                     const uint8_t public_key[32], void *context)
{
    app_state_t *app = context;
    bool ok = airplay_pairing_store_save_pairing(id, length, public_key, context);
    if (!ok && app->pairing_storage_error == RTCP_OK) app->pairing_storage_error = RTCP_EIO;
    APP_LOG("AirPlay pairing persistence=%s error=%d", ok ? "saved" : "failed", app->pairing_storage_error);
    return ok;
}

static bool provider_sign(const uint8_t *message, size_t message_len,
                          uint8_t signature[AIRPLAY_ED25519_SIGNATURE_BYTES],
                          void *context)
{
    app_state_t *app = context;
    return airplay_ed25519_sign(&app->keypair, message, message_len,
                                signature);
}

static bool provider_verify(
    const uint8_t public_key[AIRPLAY_ED25519_PUBLIC_KEY_BYTES],
    const uint8_t *message, size_t message_len,
    const uint8_t signature[AIRPLAY_ED25519_SIGNATURE_BYTES], void *context)
{
    (void)context;
    return airplay_ed25519_verify(public_key, message, message_len, signature);
}

static void fill_pairing(app_state_t *app,
                         airplay_rtsp_pairing_config_t *pairing)
{
    memset(pairing, 0, sizeof(*pairing));

    pairing->pair_setup.pairing_code = app->pairing_code;
    pairing->pair_setup.accessory_identifier = app->accessory_identifier;
    pairing->pair_setup.accessory_identifier_len =
        app->accessory_identifier_len;
    pairing->pair_setup.accessory_public_key = app->keypair.public_key;
    pairing->pair_setup.random = provider_random;
    pairing->pair_setup.sign_accessory = provider_sign;
    pairing->pair_setup.verify_controller = provider_verify;
    pairing->pair_setup.save_pairing = provider_save_controller;
    pairing->pair_setup.provider_context = app;

    pairing->pair_verify.accessory_identifier = app->accessory_identifier;
    pairing->pair_verify.accessory_identifier_len =
        app->accessory_identifier_len;
    pairing->pair_verify.random = provider_random;
    pairing->pair_verify.sign_accessory = provider_sign;
    pairing->pair_verify.verify_controller = provider_verify;
    pairing->pair_verify.lookup_controller =
        airplay_pairing_store_lookup_controller;
    pairing->pair_verify.provider_context = app;
}

/* ------------------------------------------------------------------ */
/* Server callbacks (poll thread)                                      */
/* ------------------------------------------------------------------ */

static int on_screen_message(airplay_screen_message_type_t type,
                             uint8_t *annex_b, size_t annex_b_len,
                             size_t prefix_capacity, uint64_t sender_time_ns,
                             uint64_t arrival_time_us, void *context)
{
    (void)prefix_capacity;
    (void)arrival_time_us;
    app_state_t *app = context;
    bool configuration = type == AIRPLAY_SCREEN_MESSAGE_CONFIG_H264;
    if (!configuration) {
        ++app->screen_frames;
        if (app->screen_frames == 1U || app->screen_frames % 120U == 0U) {
            APP_LOG("screen frames=%u last=%u bytes", app->screen_frames,
                    (unsigned)annex_b_len);
        }
    } else {
        APP_LOG("screen H.264 configuration: %u bytes", (unsigned)annex_b_len);
    }
    if (app->on_screen != NULL) {
        app->on_screen(configuration, annex_b, annex_b_len, sender_time_ns,
                       app->screen_context);
    }
    return RTCP_OK;
}

#if defined(RTCARPLAY_AUDIO)
static int on_audio_pcm(uint16_t stream_type, const uint8_t *data, size_t len,
                        const airplay_audio_format_t *format,
                        uint32_t rtp_timestamp, uint64_t arrival_us,
                        void *context)
{
    (void)rtp_timestamp;
    (void)arrival_us;
    app_state_t *app = context;
    if (!app->audio_started || app->shutting_down) return RTCP_OK;
    if (format == NULL) return RTCP_EINVAL;
    if (app->audio_packets == 0U) {
        APP_LOG("audio stream=%u %u Hz %u ch first packet %u bytes",
                (unsigned)stream_type, (unsigned)format->sample_rate,
                (unsigned)format->channels, (unsigned)len);
    }
    ++app->audio_packets;
    return carplay_audio_sink_pcm(format->sample_rate, format->channels, data,
                                  len);
}

static void on_volume(float volume_db, void *context)
{
    app_state_t *app = context;
    if (!app->audio_started || app->shutting_down) return;
    carplay_audio_sink_set_volume_db(volume_db);
}
#endif

static void on_event_state(bool connected, void *context)
{
    app_state_t *app = context;
    if (app->shutting_down) return;
    app->airplay_connected = connected;
    APP_LOG("event channel %s", connected ? "connected" : "closed");
    if (!connected && app->bridge != NULL) {
        /* The controller is gone; drop any held contacts so a reconnect does
         * not inherit a stuck finger. */
        input_bridge_release_all(app->bridge);
    }
}

static bool send_hid_report(uint32_t hid_uid, const uint8_t *report,
                            size_t report_len, void *context)
{
    app_state_t *app = context;
    bool sent = app->server != NULL &&
        airplay_rtsp_server_send_hid_report(app->server, hid_uid, report,
                                           report_len);
    if (sent) ++app->hid_reports_sent;
    else ++app->hid_reports_failed;
    return sent;
}

#if defined(RTCARPLAY_TOUCH_PANEL) && !defined(RTCARPLAY_DEMO_LAUNCHER)
/* Panel reader thread -> touch queue; the poll thread turns the queue into
 * HID reports, so the panel thread never touches the event channel. */
static void on_panel_touch(uint8_t phase, uint16_t pointer_id, uint16_t x,
                           uint16_t y, void *context)
{
    (void)context;
    (void)carplay_app_touch(phase, pointer_id, x, y);
}
#endif

/* ------------------------------------------------------------------ */
/* Touch queue: any thread -> poll thread                              */
/* ------------------------------------------------------------------ */

static int enqueue_touch(const touch_event_t *event)
{
    app_state_t *app = &s_app;
    if (!app->running || app->lock == NULL) return RTCP_ESTATE;
    if (rt_mutex_take(app->lock, RT_WAITING_NO) != RT_EOK) return RTCP_ESTATE;
    unsigned next = (app->touch_head + 1U) & TOUCH_QUEUE_MASK;
    if (next == app->touch_tail) {
        ++app->touch_queue_full;
        (void)rt_mutex_release(app->lock);
        return RTCP_ENOMEM;
    }
    ++app->touch_queued;
    app->touch[app->touch_head] = *event;
    app->touch_head = next;
    (void)rt_mutex_release(app->lock);
    return RTCP_OK;
}

static void drain_touch(app_state_t *app)
{
    touch_event_t batch[TOUCH_BATCH];
    bool pending;
    do {
        size_t count = 0U;
        pending = false;
        if (rt_mutex_take(app->lock, RT_WAITING_NO) != RT_EOK) return;
        while (count < TOUCH_BATCH && app->touch_tail != app->touch_head) {
            batch[count++] = app->touch[app->touch_tail];
            app->touch_tail = (app->touch_tail + 1U) & TOUCH_QUEUE_MASK;
        }
        pending = app->touch_tail != app->touch_head;
        (void)rt_mutex_release(app->lock);

        for (size_t i = 0U; i < count; ++i) {
            input_bridge_phase_t phase;
            switch (batch[i].phase) {
            case RTCP_TOUCH_DOWN: phase = INPUT_BRIDGE_TOUCH_DOWN; break;
            case RTCP_TOUCH_MOVE: phase = INPUT_BRIDGE_TOUCH_MOVE; break;
            case RTCP_TOUCH_UP: phase = INPUT_BRIDGE_TOUCH_UP; break;
            default: continue;
            }
            double x = (double)batch[i].x / (double)app->width;
            double y = (double)batch[i].y / (double)app->height;
            if (x > 1.0) x = 1.0;
            if (y > 1.0) y = 1.0;
            (void)input_bridge_touch(app->bridge, phase, batch[i].pointer_id,
                                     x, y, app->os.now_ms());
        }
    } while (pending);
}

/* ------------------------------------------------------------------ */
/* Thread and teardown                                                 */
/* ------------------------------------------------------------------ */

static void shutdown_stack(app_state_t *app)
{
    app->shutting_down = true;
#if defined(RTCARPLAY_TOUCH_PANEL)
    if (app->touch_panel_started) {
        carplay_touch_panel_stop();
        app->touch_panel_started = false;
    }
#endif
#if defined(RTCARPLAY_VIDEO)
    if (app->video_started) {
        carplay_video_sink_stop();
        app->video_started = false;
    }
#endif
    if (app->bridge != NULL) {
        input_bridge_release_all(app->bridge);
        input_bridge_destroy(app->bridge);
        app->bridge = NULL;
    }
    if (app->mdns != NULL) {
        mdns_responder_close(app->mdns, &rtcp_net_rtthread);
        mdns_responder_destroy(app->mdns);
        app->mdns = NULL;
    }
    if (app->browser != NULL) {
        mdns_browser_destroy(app->browser);
        app->browser = NULL;
    }
    rtcp_net_rtthread_multicast_interface(NULL);
    if (app->server != NULL) {
        airplay_rtsp_server_stop(app->server);
        app->server = NULL;
    }
#if defined(RTCARPLAY_AUDIO)
    /* After the server: no PCM callback can run once it is stopped. */
    if (app->audio_started) {
        carplay_audio_sink_stop();
        app->audio_started = false;
    }
#endif
    if (app->store_initialized) {
        airplay_pairing_store_deinit();
        app->store_initialized = false;
    }
    airplay_ed25519_keypair_clear(&app->keypair);
    if (app->ap_started) {
#if defined(LPKG_RT_CARPLAY_LUBANLITE_WIFI)
        (void)rtcp_wifi_lubanlite.ap_stop();
#endif
        app->ap_started = false;
    }
    if (app->lock != NULL) {
        (void)rt_mutex_delete(app->lock);
        app->lock = NULL;
    }
    memset(app, 0, sizeof(*app));
}

static bool bytes_contain(const uint8_t *bytes, size_t length,
                          const char *needle)
{
    size_t needle_len = strlen(needle);
    if (needle_len == 0U || length < needle_len) return false;
    for (size_t i = 0U; i + needle_len <= length; ++i) {
        if (memcmp(bytes + i, needle, needle_len) == 0) return true;
    }
    return false;
}

/* Every packet received on the shared mDNS socket also feeds the browser. */
static void mdns_packet_sink(const uint8_t *packet, size_t packet_len,
                             const rtcp_addr_t *from, void *context)
{
    app_state_t *app = context;
    if (app->browser == NULL || !app->ctrl_enabled) return;
    const uint8_t *ipv4 = NULL;
    if (from != NULL && from->family == RTCP_AF_INET) ipv4 = from->addr;
    bool control = bytes_contain(packet, packet_len, "_carplay-ctrl");
    bool airplay = bytes_contain(packet, packet_len, "_airplay");
    bool response =
        packet_len >= 3U && (packet[2] & 0x80U) != 0U; /* DNS QR bit */
    int updates = mdns_browser_handle_packet(app->browser, packet, packet_len,
                                             ipv4,
                                             (uint32_t)app->os.now_ms());
    ++app->browse_packets;
    if (updates > 0) app->browse_updates += (unsigned)updates;
    if (app->browse_packets <= 10U || (app->browse_packets % 64U) == 0U ||
        (response && control)) {
        APP_LOG("mdns_rx #%u from=%u.%u.%u.%u bytes=%u response=%u ctrl=%u "
                "airplay=%u updates=%d",
                app->browse_packets, ipv4 != NULL ? ipv4[0] : 0U,
                ipv4 != NULL ? ipv4[1] : 0U, ipv4 != NULL ? ipv4[2] : 0U,
                ipv4 != NULL ? ipv4[3] : 0U, (unsigned)packet_len,
                response ? 1U : 0U, control ? 1U : 0U, airplay ? 1U : 0U,
                updates);
    }
}

/*
 * Browses _carplay-ctrl._tcp and performs the reverse connect-out.  Runs in
 * the poll thread; carplay_ctrl_connect() bounds the handshake and the status
 * read at CARPLAY_CTRL_TIMEOUT_MS each, so a silent endpoint cannot stall
 * mDNS/RTSP for long.  Once the phone's event channel is up the nudge is no
 * longer needed and is suppressed for the rest of the session.
 */
static void carplay_ctrl_tick(app_state_t *app, uint32_t now_ms)
{
    if (app->browser == NULL || !app->ctrl_enabled) return;

    /* Learn the phone's Wi-Fi address from its RTSP connection, then query it
     * directly as well as via multicast.  The unicast query does not depend
     * on multicast delivery through the soft-AP. */
    rtcp_addr_t peer;
    if (airplay_rtsp_server_get_peer(app->server, &peer)) {
        memcpy(app->ctrl_peer_ipv4, peer.addr, 4U);
        app->ctrl_peer_valid = true;
    }

    if (mdns_browser_query_due(app->browser, now_ms)) {
        uint8_t query[512];
        size_t query_len = mdns_browser_build_query(app->browser, query,
                                                    sizeof(query));
        if (query_len != 0U) {
            ++app->browse_queries;
            int multicast_rc = mdns_responder_send(
                app->mdns, &rtcp_net_rtthread, query, query_len);
            int unicast_rc = RTCP_ESTATE;
            if (app->ctrl_peer_valid) {
                rtcp_addr_t destination;
                memset(&destination, 0, sizeof(destination));
                destination.family = RTCP_AF_INET;
                memcpy(destination.addr, app->ctrl_peer_ipv4, 4U);
                destination.port = MDNS_PORT;
                unicast_rc = mdns_responder_send_to(
                    app->mdns, &rtcp_net_rtthread, &destination, query,
                    query_len);
            }
            if (multicast_rc < 0) ++app->browse_tx_errors;
            if (unicast_rc < 0 && app->ctrl_peer_valid) ++app->browse_tx_errors;
            if (app->browse_queries <= 3U ||
                (app->browse_queries % 15U) == 0U) {
                if (app->ctrl_peer_valid) {
                    APP_LOG("carplay_ctrl=query #%u multicast_rc=%d "
                            "unicast_rc=%d peer=%u.%u.%u.%u",
                            app->browse_queries, multicast_rc, unicast_rc,
                            app->ctrl_peer_ipv4[0], app->ctrl_peer_ipv4[1],
                            app->ctrl_peer_ipv4[2], app->ctrl_peer_ipv4[3]);
                } else {
                    APP_LOG("carplay_ctrl=query #%u multicast_rc=%d "
                            "unicast=none",
                            app->browse_queries, multicast_rc);
                }
            }
        }
        mdns_browser_mark_queried(app->browser, now_ms);
    }

    if (app->airplay_connected) return;
    const mdns_browser_endpoint_t *endpoint =
        mdns_browser_best(app->browser, now_ms);
    if (endpoint == NULL) return;
    if (strcmp(app->ctrl_last_endpoint, endpoint->instance) != 0) {
        strncpy(app->ctrl_last_endpoint, endpoint->instance,
                sizeof(app->ctrl_last_endpoint) - 1U);
        APP_LOG("carplay_ctrl=endpoint ip=%u.%u.%u.%u port=%u instance=%s",
                endpoint->ipv4[0], endpoint->ipv4[1], endpoint->ipv4[2],
                endpoint->ipv4[3], (unsigned)endpoint->port,
                endpoint->instance);
    }
    if (app->ctrl_last_attempt_ms != 0U &&
        (uint32_t)(now_ms - app->ctrl_last_attempt_ms) <
            CARPLAY_CTRL_RETRY_MS) {
        return;
    }
    app->ctrl_last_attempt_ms = now_ms;
    ++app->ctrl_attempts;
    APP_LOG("carplay_ctrl=connect_out ip=%u.%u.%u.%u port=%u attempt=%u",
            endpoint->ipv4[0], endpoint->ipv4[1], endpoint->ipv4[2],
            endpoint->ipv4[3], (unsigned)endpoint->port, app->ctrl_attempts);
    int status = 0;
    int result = carplay_ctrl_connect(
        &rtcp_net_rtthread, endpoint->ipv4, endpoint->port, app->ctrl_device_id,
        CARPLAY_CTRL_USER_AGENT, CARPLAY_CTRL_TIMEOUT_MS, &status);
    uint32_t elapsed = (uint32_t)((uint32_t)app->os.now_ms() - now_ms);
    if (result == RTCP_OK) {
        ++app->ctrl_responses;
        app->ctrl_last_status = status;
        APP_LOG("carplay_ctrl=connected status=%d elapsed_ms=%u responses=%u",
                status, (unsigned)elapsed, app->ctrl_responses);
    } else {
        APP_LOG("carplay_ctrl=connect_failed rc=%d elapsed_ms=%u", result,
                (unsigned)elapsed);
    }
}

static void poll_thread(void *argument)
{
    app_state_t *app = argument;
    app->thread_running = true;
    APP_LOG("receiver running (PIN %s)", app->pairing_code);

    while (!app->stop) {
        if (app->mdns != NULL) {
            int mdns_result = mdns_responder_poll_ex(
                app->mdns, &rtcp_net_rtthread, 10, mdns_packet_sink, app);
            if (mdns_result < 0 && app->mdns_errors < 3U) {
                ++app->mdns_errors;
                APP_LOG("mDNS poll failed rc=%d", mdns_result);
            }
        }
        carplay_ctrl_tick(app, (uint32_t)app->os.now_ms());
        /* In wired mode wait for socket readiness, not a fixed sleep before
         * each recv. Screen headers and bodies need separate readable polls;
         * pacing every fragment by 10 ms caps throughput and builds latency. */
        (void)airplay_rtsp_server_poll(app->server,
                                      app->mdns == NULL ? 10 : 0);
        /* Snapshot on the owner thread; the shell never dereferences a
         * screen server that the protocol loop may tear down. */
        uint32_t sample_ms = (uint32_t)app->os.now_ms();
        if ((uint32_t)(sample_ms - app->screen_sample_ms) >= 100U &&
            rt_mutex_take(app->lock, RT_WAITING_NO) == RT_EOK) {
            airplay_rtsp_server_get_screen_stats(app->server, &app->screen_stats);
            app->screen_sample_ms = sample_ms;
            (void)rt_mutex_release(app->lock);
        }
        drain_touch(app);
        input_bridge_tick(app->bridge, app->os.now_ms());
    }

    APP_LOG("stopping (%u screen frames)", app->screen_frames);
    shutdown_stack(app);
    app->thread_running = false;
    app->running = false;
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                            */
/* ------------------------------------------------------------------ */

void carplay_app_config_defaults(carplay_app_config_t *config)
{
    if (config == NULL) return;
    memset(config, 0, sizeof(*config));
    config->name = DEFAULT_NAME;
    config->model = DEFAULT_MODEL;
    config->ssid = DEFAULT_SSID;
    config->passphrase = DEFAULT_PASSPHRASE;
    config->channel = DEFAULT_CHANNEL;
    config->seed_path = DEFAULT_SEED_PATH;
    config->store_path = DEFAULT_STORE_PATH;
    config->rotation_degrees = 0U;
}

static bool make_pairing_code(app_state_t *app, const char *requested)
{
    if (requested == NULL || requested[0] == '\0') {
        requested = DEFAULT_PAIR_CODE;
    }
    size_t length = strlen(requested);
    if (length > PAIR_CODE_DIGITS) return false;
    memcpy(app->pairing_code, requested, length + 1U);
    return true;
}

#if defined(LPKG_RT_CARPLAY_LUBANLITE_WIFI)
static bool format_device_id_from_mac(app_state_t *app)
{
    uint8_t mac[6];
    if (rtcp_wifi_lubanlite_get_mac(mac) != RTCP_OK) return false;
    memcpy(app->wifi_mac, mac, sizeof(mac));
    app->wifi_mac_valid = true;
    rt_snprintf(app->device_id, sizeof(app->device_id),
                "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2],
                mac[3], mac[4], mac[5]);
    rt_snprintf(app->host_name, sizeof(app->host_name), "rtcp-%02x%02x%02x",
                mac[3], mac[4], mac[5]);
    return true;
}
#endif /* LPKG_RT_CARPLAY_LUBANLITE_WIFI */

static bool format_device_id_from_seed(app_state_t *app,
                                       const uint8_t seed[32])
{
    /* Fallback identity: a locally administered address derived from the
     * persisted seed, so it stays stable across boots. */
    rt_snprintf(app->device_id, sizeof(app->device_id),
                "02:%02X:%02X:%02X:%02X:%02X", seed[0], seed[1], seed[2],
                seed[3], seed[4]);
    rt_snprintf(app->host_name, sizeof(app->host_name), "rtcp-%02x%02x%02x",
                seed[2], seed[3], seed[4]);
    return true;
}

static int build_identity(app_state_t *app, const char *pair_code,
                          const char *seed_path)
{
    uint8_t seed[AIRPLAY_ED25519_SEED_BYTES];
    bool loaded = load_seed(seed_path, seed);
    if (!loaded) {
        if (app->os.random == NULL ||
            !app->os.random(seed, sizeof(seed))) {
            APP_LOG("no entropy source for the Ed25519 identity");
            return RTCP_ENOTSUP;
        }
        if (!write_seed(seed_path, seed)) {
            APP_LOG("cannot write identity seed %s", seed_path);
            memset(seed, 0, sizeof(seed));
            return RTCP_EIO;
        }
        APP_LOG("generated a new Ed25519 identity in %s", seed_path);
    }
    bool ok = airplay_ed25519_keypair_from_seed(seed, &app->keypair);
    /* The Realtek MAC is only readable after the WLAN device is registered
     * and the AP is up, which happens in start_wifi().  Use the stable
     * seed-derived identity until then and refine it once the AP is
     * active; querying the driver earlier leaves its wext path on a DOWN
     * netif and is not needed. */
    if (ok) ok = format_device_id_from_seed(app, seed);
    memset(seed, 0, sizeof(seed));
    if (!ok) return RTCP_EIO;

    app->accessory_identifier_len = strlen(app->device_id);
    memcpy(app->accessory_identifier, app->device_id,
           app->accessory_identifier_len);
    return make_pairing_code(app, pair_code) ? RTCP_OK : RTCP_EIO;
}

#if defined(LPKG_RT_CARPLAY_LUBANLITE_WIFI)
static int wait_for_ipv4(app_state_t *app)
{
    for (unsigned attempt = 0U; attempt < IP_WAIT_TRIES; ++attempt) {
        if (rtcp_wifi_lubanlite_get_ipv4(app->ipv4) == RTCP_OK) return RTCP_OK;
        rt_thread_mdelay(IP_WAIT_STEP_MS);
    }
    return RTCP_ETIMEDOUT;
}

static int start_wifi(app_state_t *app)
{
    int result = rtcp_wifi_lubanlite.ap_start(app->ssid, app->passphrase,
                                              app->channel);
    if (result != RTCP_OK) {
        APP_LOG("SoftAP start failed rc=%d", result);
        return result;
    }
    app->ap_started = true;

    /* This SDK's dhcpd_start changes the AP address to DHCPD_SERVER_IP.
     * Read/announce the final address only after that operation. */
    result = RTCP_ESTATE;
    for (unsigned attempt = 0; attempt < IP_WAIT_TRIES && result == RTCP_ESTATE; ++attempt) {
        result = rtcp_wifi_lubanlite_start_dhcpd();
        if (result == RTCP_ESTATE) rt_thread_mdelay(IP_WAIT_STEP_MS);
    }
    if (result != RTCP_OK) {
        APP_LOG("DHCP server unavailable rc=%d", result);
        return result;
    }

    size_t ie_len = 0U;
    const uint8_t *ie = airplay_vendor_ie(&ie_len);
    result = rtcp_wifi_lubanlite.set_vendor_ie(ie, ie_len);
    if (result != RTCP_OK) {
        /* Without the IE the iPhone may not offer CarPlay automatically, but
         * the AirPlay service itself is still discoverable over mDNS. */
        APP_LOG("Apple vendor IE not applied rc=%d", result);
    }

    result = wait_for_ipv4(app);
    if (result != RTCP_OK) {
        APP_LOG("AP netif did not get an IPv4 address");
        return result;
    }
    APP_LOG("SoftAP %s ip=%u.%u.%u.%u", app->ssid, app->ipv4[0], app->ipv4[1],
            app->ipv4[2], app->ipv4[3]);

    /* The AP is active now, so the driver can report its MAC.  Prefer it
     * over the seed-derived fallback chosen in build_identity(). */
    if (format_device_id_from_mac(app)) {
        app->accessory_identifier_len = strlen(app->device_id);
        memcpy(app->accessory_identifier, app->device_id,
               app->accessory_identifier_len);
    } else {
        APP_LOG("cannot read the Wi-Fi MAC; keeping the seed-derived device id");
    }

    return RTCP_OK;
}
#endif /* LPKG_RT_CARPLAY_LUBANLITE_WIFI */

static int build_announcement_data(app_state_t *app)
{
    airplay_info_plist_config_t info;
    memset(&info, 0, sizeof(info));
    info.device_id = app->device_id;
    info.name = app->name;
    info.model = app->model;
    info.width_pixels = app->width;
    info.height_pixels = app->height;
    info.max_fps = 30U;
#if defined(RTCARPLAY_AUDIO)
    if (app->audio_started) {
        /* Publish the PCM capability arrays only when a sink can play them,
         * so a controller never negotiates an undecodable audio format. */
        info.audio_output = true;
        info.audio_sample_rate = CARPLAY_APP_AUDIO_SAMPLE_RATE;
    }
#endif
    if (!airplay_info_plist_build(&info, app->info_plist,
                                  sizeof(app->info_plist),
                                  &app->info_plist_len) ||
        app->info_plist_len > AIRPLAY_RTSP_SESSION_INFO_MAX_BYTES) {
        APP_LOG("cannot build the /info plist");
        return RTCP_EIO;
    }

    hex_encode(app->keypair.public_key, AIRPLAY_ED25519_PUBLIC_KEY_BYTES,
               app->public_key_hex);
    airplay_txt_record_t records[AIRPLAY_TXT_MAX_RECORDS];
    airplay_txt_config_t txt;
    memset(&txt, 0, sizeof(txt));
    txt.device_id = app->device_id;
    txt.model = app->model;
    txt.source_version = AIRPLAY_INFO_DEFAULT_SOURCE_VERSION;
    txt.protocol_version = "1.1";
    txt.pairing_id = app->device_id;
    txt.public_key = app->public_key_hex;
    size_t record_count = airplay_txt_build(&txt, records,
                                            AIRPLAY_TXT_MAX_RECORDS);
    app->txt_len = airplay_txt_encode(records, record_count, app->txt,
                                      sizeof(app->txt));
    if (app->txt_len == 0U) {
        APP_LOG("cannot encode the mDNS TXT record");
        return RTCP_EIO;
    }
    return RTCP_OK;
}

int carplay_app_start(const carplay_app_config_t *config)
{
    if (config == NULL || config->width_pixels == 0U ||
        config->height_pixels == 0U) {
        return RTCP_EINVAL;
    }
    if (s_app.running || s_app.thread_running) return RTCP_ESTATE;

    app_state_t *app = &s_app;
    memset(app, 0, sizeof(*app));
    app->os = rtcp_os_rtthread;
    app->os.random = rtcp_lubanlite_random;
    app->width = config->width_pixels;
    app->height = config->height_pixels;
    app->rotation = config->rotation_degrees;
    app->wired = config->wired;
    app->on_screen = config->on_screen;
    app->screen_context = config->screen_context;

    if (config->rotation_degrees != 0U && config->rotation_degrees != 90U &&
        config->rotation_degrees != 180U && config->rotation_degrees != 270U) {
        return RTCP_EINVAL;
    }
    if (!copy_string(app->name, sizeof(app->name),
                     config->name != NULL ? config->name : DEFAULT_NAME) ||
        !copy_string(app->model, sizeof(app->model),
                     config->model != NULL ? config->model : DEFAULT_MODEL) ||
        !copy_string(app->ssid, sizeof(app->ssid),
                     config->ssid != NULL ? config->ssid : DEFAULT_SSID) ||
        !copy_string(app->seed_path, sizeof(app->seed_path),
                     config->seed_path != NULL ? config->seed_path
                                               : DEFAULT_SEED_PATH) ||
        !copy_string(app->store_path, sizeof(app->store_path),
                     config->store_path != NULL ? config->store_path
                                                : DEFAULT_STORE_PATH)) {
        return RTCP_EINVAL;
    }
    const char *passphrase = config->passphrase != NULL
                                 ? config->passphrase
                                 : DEFAULT_PASSPHRASE;
    if (passphrase[0] != '\0' &&
        !copy_string(app->passphrase, sizeof(app->passphrase), passphrase)) {
        return RTCP_EINVAL;
    }
    app->channel = config->channel != 0U ? config->channel : DEFAULT_CHANNEL;

    int result = build_identity(app, config->pair_code, app->seed_path);
    if (result != RTCP_OK) return result;

    if (!app->wired) {
#if defined(LPKG_RT_CARPLAY_LUBANLITE_WIFI)
        result = start_wifi(app);
        if (result != RTCP_OK) {
            shutdown_stack(app);
            return result;
        }
#else
        APP_LOG("wireless mode needs LPKG_RT_CARPLAY_LUBANLITE_WIFI; "
                "use 'carplay_start wired' on this build");
        shutdown_stack(app);
        return RTCP_ENOTSUP;
#endif
    }

    airplay_pairing_store_backend_t backend = {
        .load = store_load,
        .save = store_save,
        .context = app,
    };
    result = airplay_pairing_store_init(&backend);
    if (result != RTCP_OK) {
        APP_LOG("cannot load pairing store %s", app->store_path);
        shutdown_stack(app);
        return result;
    }
    app->store_initialized = true;

#if defined(RTCARPLAY_AUDIO)
    /* Start the sink before /info is built: its presence decides whether the
     * receiver advertises audio output.  The codec itself stays untouched
     * until the first PCM packet. */
    result = carplay_audio_sink_start();
    if (result != RTCP_OK) {
        APP_LOG("audio sink unavailable rc=%d; running without audio", result);
    } else {
        app->audio_started = true;
    }
#endif

    result = build_announcement_data(app);
    if (result != RTCP_OK) {
        shutdown_stack(app);
        return result;
    }

    airplay_rtsp_server_config_t server_config;
    memset(&server_config, 0, sizeof(server_config));
    server_config.os = &app->os;
    server_config.net = &rtcp_net_rtthread;
    fill_pairing(app, &server_config.pairing);
    server_config.info_plist = app->info_plist;
    server_config.info_plist_len = app->info_plist_len;
    server_config.port = AIRPLAY_RTSP_DEFAULT_PORT;
    server_config.family = app->wired ? RTCP_AF_INET6 : RTCP_AF_INET;
    server_config.on_screen_message = on_screen_message;
    server_config.screen_message_context = app;
    server_config.on_event_state = on_event_state;
    server_config.event_state_context = app;
#if defined(RTCARPLAY_AUDIO)
    if (app->audio_started) {
        server_config.on_audio_pcm = on_audio_pcm;
        server_config.audio_pcm_context = app;
        server_config.on_volume = on_volume;
        server_config.volume_context = app;
    }
#endif
    result = airplay_rtsp_server_start(&server_config, &app->server);
    if (result != RTCP_OK) {
        APP_LOG("RTSP server start failed rc=%d", result);
        shutdown_stack(app);
        return result;
    }
    /* Cache the port for the iAP2 session; it must not touch the server. */
    app->airplay_port = airplay_rtsp_server_get_port(app->server);

    mdns_responder_config_t mdns_config;
    memset(&mdns_config, 0, sizeof(mdns_config));
    mdns_config.host_name = app->host_name;
    mdns_config.instance_name = app->name;
    mdns_config.port = app->airplay_port;
    mdns_config.txt = app->txt;
    mdns_config.txt_len = app->txt_len;
    memcpy(mdns_config.ipv4, app->ipv4, sizeof(app->ipv4));
    if (app->wired) {
        /* Wired CarPlay learns the AirPlay endpoint from CarPlayStartSession,
         * so there is nothing to advertise and no IEEE 802.11 link to browse
         * the phone on.  The response arrives over the NCM netif. */
        APP_LOG("wired mode: mDNS/Control browse disabled");
    } else {
        /* IPHETH may still be attached. Join/send mDNS on this AP rather
         * than whichever interface currently owns lwIP's default route. */
        rtcp_net_rtthread_multicast_interface(app->ipv4);
        app->mdns = mdns_responder_create(&mdns_config);
        result = app->mdns == NULL ? RTCP_ENOMEM
                                   : mdns_responder_open(app->mdns,
                                                         &rtcp_net_rtthread);
        if (result != RTCP_OK) {
            APP_LOG("mDNS responder start failed rc=%d", result);
            shutdown_stack(app);
            return result;
        }

        /* Browser state only; it shares the responder's socket. */
        app->browser = mdns_browser_create(CARPLAY_CTRL_SERVICE);
        app->ctrl_enabled = app->browser != NULL;
        if (app->browser == NULL) {
            APP_LOG("carplay_ctrl=disabled reason=no_memory");
        } else if (!carplay_ctrl_parse_device_id(app->device_id,
                                                 &app->ctrl_device_id)) {
            APP_LOG("carplay_ctrl=disabled reason=device_id_parse id=%s",
                    app->device_id);
            app->ctrl_enabled = false;
        }
    }

    app->bridge = input_bridge_create(app->width, app->height, app->rotation,
                                      send_hid_report, app);
    if (app->bridge == NULL) {
        shutdown_stack(app);
        return RTCP_ENOMEM;
    }

#if defined(RTCARPLAY_VIDEO)
    /* A caller-supplied sink wins; otherwise drive the on-board decoder. */
    if (app->on_screen == NULL) {
        result = carplay_video_sink_start(app->width, app->height);
        if (result != RTCP_OK) {
            APP_LOG("video sink start failed rc=%d", result);
            shutdown_stack(app);
            return result;
        }
        app->video_started = true;
        app->on_screen = carplay_video_sink_frame;
        app->screen_context = NULL;
    }
#endif

    app->lock = rt_mutex_create("capp", RT_IPC_FLAG_FIFO);
    if (app->lock == NULL) {
        shutdown_stack(app);
        return RTCP_ENOMEM;
    }

    app->stop = false;
    app->running = true;
    app->thread = rt_thread_create("carplay", poll_thread, app,
                                   POLL_THREAD_STACK_BYTES,
                                   POLL_THREAD_PRIORITY, POLL_THREAD_TICK);
    if (app->thread == NULL) {
        app->running = false;
        shutdown_stack(app);
        return RTCP_ENOMEM;
    }
    (void)rt_thread_startup(app->thread);

#if defined(RTCARPLAY_TOUCH_PANEL) && !defined(RTCARPLAY_DEMO_LAUNCHER)
    /* The touch queue exists now (app->running and app->lock); a board without
     * the panel driver keeps working through carplay_touch. */
    result = carplay_touch_panel_start(app->width, app->height, on_panel_touch,
                                       app);
    if (result != RTCP_OK) {
        APP_LOG("touch panel unavailable rc=%d; use carplay_touch", result);
    } else {
        app->touch_panel_started = true;
    }
#endif

    if (app->wired) {
        APP_LOG("wired AirPlay on IPv6:%u (device %s, PIN %s)",
                (unsigned)airplay_rtsp_server_get_port(app->server),
                app->device_id, app->pairing_code);
    } else {
        APP_LOG("advertising \"%s\" on %u.%u.%u.%u:%u (device %s, PIN %s)",
                app->name, app->ipv4[0], app->ipv4[1], app->ipv4[2],
                app->ipv4[3],
                (unsigned)airplay_rtsp_server_get_port(app->server),
                app->device_id, app->pairing_code);
    }
    return RTCP_OK;
}

int carplay_app_stop(void)
{
    if (!s_app.running) return RTCP_OK;
#if defined(RTCARPLAY_IAP2)
    /* The iAP2 session borrows the receiver state, so retire it before the
     * poll thread clears the stack.  The iAP2 thread owns its teardown. */
    if (carplay_iap2_is_running()) {
        int result = carplay_iap2_stop();
        if (result != RTCP_OK) return result;
    }
#endif
    s_app.stop = true;
    /* The poll thread tears the stack down before clearing running. */
    while (s_app.running) {
        rt_thread_mdelay(10);
    }
    return RTCP_OK;
}

bool carplay_app_is_running(void)
{
    return s_app.running;
}

void carplay_app_display_size(uint16_t *width, uint16_t *height)
{
    if (width != NULL) *width = s_app.width;
    if (height != NULL) *height = s_app.height;
}

const char *carplay_app_pairing_code(void)
{
    return s_app.pairing_code;
}

int carplay_app_iap2_info(carplay_app_iap2_info_t *info)
{
    if (info == NULL) return RTCP_EINVAL;
    app_state_t *app = &s_app;
    if (!app->running || app->stop || app->wired) return RTCP_ESTATE;

    memset(info, 0, sizeof(*info));
    info->name = app->name;
    info->model = app->model;
    info->ssid = app->ssid;
    info->passphrase = app->passphrase;
    info->channel = app->channel;
    info->security_type = app->passphrase[0] != '\0' ? 2U : 0U;
    info->airplay_port = app->airplay_port;
    memcpy(info->ipv4, app->ipv4, sizeof(info->ipv4));
    if (app->wifi_mac_valid) {
        memcpy(info->bssid, app->wifi_mac, sizeof(info->bssid));
        info->bssid_valid = true;
    }
    info->device_id = app->device_id;
    info->public_key_hex = app->public_key_hex;
    info->source_version = AIRPLAY_INFO_DEFAULT_SOURCE_VERSION;
    info->mdns_ready = app->mdns != NULL;
    info->airplay_connected = app->airplay_connected;
    return RTCP_OK;
}

int carplay_app_wired_info(carplay_app_wired_info_t *info)
{
    if (info == NULL) return RTCP_EINVAL;
    app_state_t *app = &s_app;
    if (!app->running || app->stop || !app->wired) return RTCP_ESTATE;
#if !defined(LPKG_RT_CARPLAY_LUBANLITE_USB) || !defined(RT_USING_LWIP)
    return RTCP_ENOTSUP;
#else
    memset(info, 0, sizeof(*info));
    bool valid = false;
    if (!rtcp_lubanlite_wired_ipv6(info->ipv6, sizeof(info->ipv6), &valid) ||
        !valid) {
        /* Tentative addresses have not passed duplicate-address detection;
         * advertising one would race the phone's first connection. */
        return RTCP_ENOENT;
    }
    info->device_id = app->device_id;
    info->public_key_hex = app->public_key_hex;
    info->source_version = AIRPLAY_INFO_DEFAULT_SOURCE_VERSION;
    info->airplay_port = app->airplay_port;
    info->airplay_connected = app->airplay_connected;
    return RTCP_OK;
#endif
}

/* The shell commands run on one MSH thread, so a plain check-and-set is
 * enough; the serving threads only release. */
static volatile int s_bt_owner;

bool carplay_bt_claim(carplay_bt_owner_t owner)
{
    if (owner == CARPLAY_BT_OWNER_NONE) return false;
    if (s_bt_owner != CARPLAY_BT_OWNER_NONE) return false;
    s_bt_owner = owner;
    return true;
}

void carplay_bt_release(carplay_bt_owner_t owner)
{
    if (s_bt_owner == owner) s_bt_owner = CARPLAY_BT_OWNER_NONE;
}

carplay_bt_owner_t carplay_bt_owner(void)
{
    return (carplay_bt_owner_t)s_bt_owner;
}

int carplay_app_touch(uint8_t phase, uint16_t pointer_id,
                      uint16_t x, uint16_t y)
{
    if (phase < RTCP_TOUCH_DOWN || phase > RTCP_TOUCH_UP) {
        return RTCP_EINVAL;
    }
    touch_event_t event = {
        .phase = phase,
        .pointer_id = pointer_id,
        .x = x,
        .y = y,
    };
    return enqueue_touch(&event);
}

static int app_autostart(bool wired)
{
    carplay_app_config_t config;
    carplay_app_config_defaults(&config);
    config.width_pixels = CARPLAY_APP_DEFAULT_WIDTH;
    config.height_pixels = CARPLAY_APP_DEFAULT_HEIGHT;
    config.rotation_degrees = CARPLAY_APP_DEFAULT_ROTATION;
    config.wired = wired;
    return carplay_app_start(&config);
}

int carplay_app_autostart(void)
{
    return app_autostart(false);
}

int carplay_app_autostart_wired(void)
{
    return app_autostart(true);
}

/* ------------------------------------------------------------------ */
/* MSH commands                                                        */
/* ------------------------------------------------------------------ */

static int cmd_carplay_start(int argc, char **argv)
{
#ifdef RTCARPLAY_DEMO_LAUNCHER
    (void)argc; (void)argv;
    rt_kprintf("LVGL demo is active; select USB or Wireless on the display\n");
    return 0;
#endif
    bool wired = argc > 1 && strcmp(argv[1], "wired") == 0;
    if (argc > 1 && !wired) {
        rt_kprintf("usage: carplay_start [wired]\n");
        return 0;
    }
    if (carplay_app_is_running()) {
        rt_kprintf("carplay is already running\n");
        return 0;
    }
    int result = wired ? carplay_app_autostart_wired()
                       : carplay_app_autostart();
    if (result != RTCP_OK) {
        rt_kprintf("carplay start failed: %d\n", result);
        return 0;
    }
    rt_kprintf("carplay started (%s), pairing PIN %s\n",
               wired ? "wired IPv6" : "wireless",
               carplay_app_pairing_code());
    return 0;
}

static int cmd_carplay_stop(int argc, char **argv)
{
    (void)argc;
    (void)argv;
#ifdef RTCARPLAY_DEMO_LAUNCHER
    rt_kprintf("LVGL demo owns the receiver; use the connection selector\n");
    return 0;
#endif
    (void)carplay_app_stop();
    rt_kprintf("carplay stopped\n");
    return 0;
}

static int cmd_carplay_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint16_t width = 0U;
    uint16_t height = 0U;
    carplay_app_display_size(&width, &height);
    rt_kprintf("carplay: %s, display %ux%u, PIN %s\n",
               carplay_app_is_running() ? "running" : "stopped",
               (unsigned)width, (unsigned)height,
               carplay_app_pairing_code()[0] != '\0'
                   ? carplay_app_pairing_code()
                   : "-");
#if defined(RTCARPLAY_AUDIO)
    unsigned packets = 0U, dropped = 0U, errors = 0U, replays = 0U;
    carplay_audio_sink_stats(&packets, &dropped, &errors, &replays);
    rt_kprintf("carplay audio: %s, %u packets, %u replay(s), %u dropped "
               "bytes, %u write errors\n",
               carplay_audio_sink_is_running() ? "ready" : "stopped", packets,
               replays, dropped, errors);
    int volume = carplay_audio_sink_volume_percent();
    if (volume >= 0) {
        rt_kprintf("carplay audio speaker volume: %d%%\n", volume);
    }
#endif
#if defined(RTCARPLAY_TOUCH_PANEL)
    uint16_t points = carplay_touch_panel_points();
    unsigned reads = 0U, events = 0U;
    carplay_touch_panel_stats(&reads, &events);
    rt_kprintf("carplay touch panel: %s, %u point(s), reads=%u events=%u\n",
               carplay_touch_panel_is_running() ? "running" : "stopped",
               (unsigned)points, reads, events);
#endif
    rt_kprintf("carplay touch: queued=%u queue_full=%u HID_sent=%u HID_failed=%u\n",
               s_app.touch_queued, s_app.touch_queue_full,
               s_app.hid_reports_sent, s_app.hid_reports_failed);
    airplay_screen_server_stats_t screen = {0};
    if (s_app.lock != NULL && rt_mutex_take(s_app.lock, RT_WAITING_NO) == RT_EOK) {
        screen = s_app.screen_stats;
        (void)rt_mutex_release(s_app.lock);
    }
    uint32_t now = (uint32_t)rt_tick_get_millisecond();
    uint32_t interval = now - s_app.screen_report_ms;
    uint32_t delta = screen.frames >= s_app.screen_report_frames ?
                         screen.frames - s_app.screen_report_frames : screen.frames;
    unsigned fps10 = interval != 0U ? (unsigned)((uint64_t)delta * 10000U / interval) : 0U;
    rt_kprintf("carplay screen RX: frames=%u wire_KiB=%u recv_calls=%u interval=%ums fps=%u.%u\n",
               (unsigned)screen.frames, (unsigned)(screen.wire_bytes / 1024U),
               (unsigned)screen.recv_calls, (unsigned)interval, fps10 / 10U, fps10 % 10U);
    rt_kprintf("carplay screen process_us avg/max=%u/%u (includes callback/scheduling)\n",
               screen.messages ? (unsigned)(screen.process_us / screen.messages) : 0U,
               (unsigned)screen.process_max_us);
    rt_kprintf("carplay screen gaps_ms avg/max: receive=%u/%u sender=%u/%u assembly=%u/%u\n",
               screen.frame_gap_count ? (unsigned)(screen.frame_gap_us / screen.frame_gap_count / 1000U) : 0U,
               screen.frame_gap_max_us / 1000U,
               screen.sender_gap_count ? (unsigned)(screen.sender_gap_us / screen.sender_gap_count / 1000U) : 0U,
               screen.sender_gap_max_us / 1000U,
               screen.messages ? (unsigned)(screen.assembly_us / screen.messages / 1000U) : 0U,
               screen.assembly_max_us / 1000U);
    rt_kprintf("carplay screen gap counts: <=40ms=%u 40..100ms=%u >100ms=%u\n",
               screen.frame_gap_le40ms, screen.frame_gap_le100ms, screen.frame_gap_gt100ms);
    s_app.screen_report_ms = now;
    s_app.screen_report_frames = screen.frames;
    return 0;
}

static int cmd_carplay_touch(int argc, char **argv)
{
    if (argc < 4) {
        rt_kprintf("usage: carplay_touch down|move|up X Y [pointer]\n");
        return 0;
    }
    uint8_t phase;
    if (strcmp(argv[1], "down") == 0) {
        phase = RTCP_TOUCH_DOWN;
    } else if (strcmp(argv[1], "move") == 0) {
        phase = RTCP_TOUCH_MOVE;
    } else if (strcmp(argv[1], "up") == 0) {
        phase = RTCP_TOUCH_UP;
    } else {
        rt_kprintf("unknown touch phase: %s\n", argv[1]);
        return 0;
    }
    unsigned x = (unsigned)atoi(argv[2]);
    unsigned y = (unsigned)atoi(argv[3]);
    unsigned pointer = argc >= 5 ? (unsigned)atoi(argv[4]) : 0U;
    int result = carplay_app_touch(phase, (uint16_t)pointer, (uint16_t)x,
                                   (uint16_t)y);
    if (result != RTCP_OK) {
        rt_kprintf("touch rejected: %d\n", result);
    }
    return 0;
}

static int cmd_carplay_randtest(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint8_t first[32] = {0};
    uint8_t second[32] = {0};
    bool ok_first = rtcp_lubanlite_random(first, sizeof(first));
    bool ok_second = rtcp_lubanlite_random(second, sizeof(second));
    for (size_t i = 0U; i < sizeof(first); ++i) {
        rt_kprintf("%02x", first[i]);
    }
    rt_kprintf("\n");
    for (size_t i = 0U; i < sizeof(second); ++i) {
        rt_kprintf("%02x", second[i]);
    }
    rt_kprintf("\n");
    if (!ok_first || !ok_second) {
        rt_kprintf("carplay_randtest: entropy provider unavailable "
                   "(check the CE TRNG error log)\n");
    } else if (memcmp(first, second, sizeof(first)) == 0) {
        rt_kprintf("carplay_randtest: FAIL, two calls returned identical "
                   "bytes\n");
    } else {
        rt_kprintf("carplay_randtest: OK, distinct output\n");
    }
    memset(first, 0, sizeof(first));
    memset(second, 0, sizeof(second));
    return 0;
}

#if defined(RTCARPLAY_BT)
/*
 * Bluetooth RFCOMM bring-up.  This is a transport check, not CarPlay: it
 * advertises the iAP2 service class UUID over SDP, accepts one phone
 * connection and echoes bytes back.  A Bluetooth serial terminal that
 * browses SDP for the iAP2 UUID (or an iAP2 probe) exercises the path.
 *
 * The UUID is the accessory-side iAP2 service class (...CAFF), the same
 * value the vendor reference Bluetooth library registers with its RFCOMM server
 * (iap2_uuid_srv).  ...CAFE is the iPhone-side service and must not be
 * used here: advertising it made iOS see no accessory iAP2 service and
 * drop the link (diag7).
 */
#define BT_BRINGUP_UUID "00000000-deca-fade-deca-deafdecacaff"
#define BT_BRINGUP_STACK (8U * 1024U)
#define BT_BRINGUP_PRIORITY 20U
#define BT_BRINGUP_TICK 20U

static volatile bool s_bt_serve;
static volatile bool s_bt_thread_running;
static rt_thread_t s_bt_thread;

static void bt_event_cb(int ev, void *user)
{
    (void)user;
    APP_LOG("bt event: %s", ev == RTCP_BT_EV_CONNECTED ? "connected"
                              : ev == RTCP_BT_EV_DISCONNECTED ? "disconnected"
                                                              : "other");
}

static void bt_serve_thread(void *parameter)
{
    (void)parameter;
    s_bt_thread_running = true;

    int result = rtcp_bt_lubanlite.enable("rt-carplay", NULL);
    if (result != RTCP_OK) {
        APP_LOG("bt enable failed rc=%d", result);
        goto done;
    }

    uint8_t channel = 0U;
    result = rtcp_bt_lubanlite.rfcomm_listen(0U, "rt-carplay",
                                             BT_BRINGUP_UUID, &channel);
    if (result != RTCP_OK) {
        APP_LOG("bt listen failed rc=%d", result);
        goto done;
    }
    APP_LOG("bt RFCOMM listening on channel %u", (unsigned)channel);

    while (s_bt_serve) {
        int fd = -1;
        result = rtcp_bt_lubanlite_rfcomm_accept(1, &fd);
        if (result != RTCP_OK) {
            APP_LOG("bt accept stopped rc=%d", result);
            break;
        }
        APP_LOG("bt link up (fd %d)", fd);
        while (s_bt_serve) {
            uint8_t buffer[256];
            int received = rtcp_bt_lubanlite_recv(fd, buffer, sizeof(buffer));
            if (received == RTCP_ETIMEDOUT) continue;
            if (received <= 0) break;
            APP_LOG("bt rx %d byte(s)", received);
            if (rtcp_bt_lubanlite_send(fd, buffer, (size_t)received) < 0) {
                break;
            }
        }
        APP_LOG("bt link down");
    }

done:
    s_bt_thread_running = false;
    carplay_bt_release(CARPLAY_BT_OWNER_ECHO);
}

static int cmd_carplay_bt(int argc, char **argv)
{
    const char *sub = argc >= 2 ? argv[1] : "status";
#ifdef RTCARPLAY_DEMO_LAUNCHER
    if (strcmp(sub, "status") != 0) {
        rt_kprintf("LVGL demo owns Bluetooth; use the connection selector\n");
        return 0;
    }
#endif
    if (strcmp(sub, "start") == 0) {
        if (s_bt_thread_running) {
            rt_kprintf("carplay_bt already running\n");
            return 0;
        }
        if (!carplay_bt_claim(CARPLAY_BT_OWNER_ECHO)) {
            rt_kprintf("carplay_bt: Bluetooth link is in use by %s\n",
                       carplay_bt_owner() == CARPLAY_BT_OWNER_IAP2
                           ? "carplay_iap2"
                           : "another service");
            return 0;
        }
        rtcp_bt_lubanlite_set_event_cb(bt_event_cb, NULL);
        s_bt_serve = true;
        s_bt_thread = rt_thread_create("carplay-bt", bt_serve_thread, NULL,
                                       BT_BRINGUP_STACK, BT_BRINGUP_PRIORITY,
                                       BT_BRINGUP_TICK);
        if (s_bt_thread == NULL) {
            s_bt_serve = false;
            carplay_bt_release(CARPLAY_BT_OWNER_ECHO);
            rt_kprintf("carplay_bt: cannot create thread\n");
            return 0;
        }
        (void)rt_thread_startup(s_bt_thread);
        return 0;
    }
    if (strcmp(sub, "stop") == 0) {
        s_bt_serve = false;
        rtcp_bt_lubanlite_disconnect();
        while (s_bt_thread_running) {
            rt_thread_mdelay(10);
        }
        rt_kprintf("carplay_bt stopped\n");
        return 0;
    }
    if (strcmp(sub, "status") == 0) {
        rt_kprintf("carplay_bt: %s, channel %u, link %s, rx drops %u\n",
                   !rtcp_bt_lubanlite_is_up()
                       ? "off"
                       : (s_bt_thread_running ? "serving" : "idle"),
                   (unsigned)rtcp_bt_lubanlite_channel(),
                   rtcp_bt_lubanlite_link_up() ? "up" : "down",
                   rtcp_bt_lubanlite_rx_dropped());
        return 0;
    }
    rt_kprintf("usage: carplay_bt start|stop|status\n");
    return 0;
}
#endif /* RTCARPLAY_BT */

static int cmd_carplay_ctrl(int argc, char **argv)
{
    const char *sub = argc >= 2 ? argv[1] : "status";
    uint32_t now_ms = s_app.os.now_ms != NULL
                          ? (uint32_t)s_app.os.now_ms()
                          : 0U;
    if (strcmp(sub, "on") == 0) {
        s_app.ctrl_enabled = s_app.browser != NULL;
        rt_kprintf("carplay_ctrl: %s\n",
                   s_app.ctrl_enabled ? "enabled"
                                      : "unavailable (receiver not running)");
        return 0;
    }
    if (strcmp(sub, "off") == 0) {
        s_app.ctrl_enabled = false;
        rt_kprintf("carplay_ctrl: disabled\n");
        return 0;
    }
    if (strcmp(sub, "connect") == 0) {
        /* Force the next poll-thread tick to attempt the connect-out. */
        s_app.ctrl_last_attempt_ms = now_ms - CARPLAY_CTRL_RETRY_MS;
        rt_kprintf("carplay_ctrl: connect requested\n");
        return 0;
    }
    if (strcmp(sub, "status") == 0) {
        const mdns_browser_endpoint_t *endpoint =
            s_app.browser != NULL
                ? mdns_browser_best(s_app.browser, now_ms)
                : NULL;
        rt_kprintf("carplay_ctrl: %s, %u endpoint(s), id %llu, attempts %u, "
                   "responses %u, last status %d\n",
                   s_app.ctrl_enabled ? "enabled" : "disabled",
                   (unsigned)mdns_browser_endpoint_count(s_app.browser),
                   (unsigned long long)s_app.ctrl_device_id,
                   s_app.ctrl_attempts, s_app.ctrl_responses,
                   s_app.ctrl_last_status);
        rt_kprintf("  browse: queries %u, tx errors %u, packets %u, "
                   "updates %u\n",
                   s_app.browse_queries, s_app.browse_tx_errors,
                   s_app.browse_packets, s_app.browse_updates);
        if (s_app.ctrl_peer_valid) {
            rt_kprintf("  unicast peer %u.%u.%u.%u\n",
                       s_app.ctrl_peer_ipv4[0], s_app.ctrl_peer_ipv4[1],
                       s_app.ctrl_peer_ipv4[2], s_app.ctrl_peer_ipv4[3]);
        } else {
            rt_kprintf("  unicast peer: none\n");
        }
        if (endpoint != NULL) {
            rt_kprintf("  endpoint %u.%u.%u.%u:%u instance=%s\n",
                       endpoint->ipv4[0], endpoint->ipv4[1],
                       endpoint->ipv4[2], endpoint->ipv4[3],
                       (unsigned)endpoint->port, endpoint->instance);
        }
        return 0;
    }
    rt_kprintf("usage: carplay_ctrl on|off|connect|status\n");
    return 0;
}

/* Wireless A/B knob for the auth-setup close: Apple's transport chunks a
 * reply at 1024 plaintext bytes, but the observed controller closes right
 * after the 1166-byte auth-setup reply.  Flip framing here and retest without
 * reflashing; the default (chunked) is unchanged. */
static int cmd_carplay_ab(int argc, char **argv)
{
    if (!s_app.running || s_app.server == NULL) {
        rt_kprintf("carplay_ab: receiver is not running\n");
        return 0;
    }
    if (argc > 1) {
        if (strcmp(argv[1], "single") == 0) {
            airplay_rtsp_server_set_single_cipher_frame(s_app.server, true);
        } else if (strcmp(argv[1], "chunked") == 0) {
            airplay_rtsp_server_set_single_cipher_frame(s_app.server, false);
        } else {
            rt_kprintf("usage: carplay_ab [single|chunked]\n");
            return 0;
        }
    }
    rt_kprintf("carplay_ab: ControlCipher framing %s\n",
               airplay_rtsp_server_single_cipher_frame(s_app.server)
                   ? "single-frame"
                   : "chunked (1024 B)");
    return 0;
}

MSH_CMD_EXPORT_ALIAS(cmd_carplay_start, carplay_start,
                     Start the rt-carplay receiver);
MSH_CMD_EXPORT_ALIAS(cmd_carplay_stop, carplay_stop,
                     Stop the rt-carplay receiver);
MSH_CMD_EXPORT_ALIAS(cmd_carplay_status, carplay_status,
                     Show rt-carplay receiver status);
MSH_CMD_EXPORT_ALIAS(cmd_carplay_touch, carplay_touch,
                     Inject a touch: carplay_touch down|move|up X Y [pointer]);
MSH_CMD_EXPORT_ALIAS(cmd_carplay_randtest, carplay_randtest,
                     Verify the CE TRNG: two calls must differ);
MSH_CMD_EXPORT_ALIAS(cmd_carplay_ctrl, carplay_ctrl,
                     CarPlay Control browse/connect: on|off|connect|status);
MSH_CMD_EXPORT_ALIAS(cmd_carplay_ab, carplay_ab,
                     ControlCipher A/B framing: single|chunked);
#if defined(RTCARPLAY_BT)
MSH_CMD_EXPORT_ALIAS(cmd_carplay_bt, carplay_bt,
                     Bluetooth RFCOMM bring-up: start|stop|status);
#endif

int carplay_app_pairing_storage_error(void) { return s_app.pairing_storage_error; }

void carplay_app_debug_counters(unsigned *received, unsigned *hid_sent, unsigned *hid_failed)
{
    if (received) {
        *received = 0;
        if (s_app.lock && rt_mutex_take(s_app.lock, RT_WAITING_NO) == RT_EOK) {
            *received = s_app.screen_stats.frames;
            (void)rt_mutex_release(s_app.lock);
        }
    }
    if (hid_sent) *hid_sent = s_app.hid_reports_sent;
    if (hid_failed) *hid_failed = s_app.hid_reports_failed;
}

/* Called by the low-priority demo owner, never from the network/decoder path.
 * Print common-window deltas automatically, so a cadence report does not
 * require two shell commands with different startup/static intervals. */
extern void ipheth_performance_report(void) __attribute__((weak));
extern void rtcp_lubanlite_usb_probe_report(void) __attribute__((weak));
extern void carplay_usb_mux_probe_report(void) __attribute__((weak));
void carplay_app_performance_report(void)
{
    if (ipheth_performance_report) ipheth_performance_report();
    if (rtcp_lubanlite_usb_probe_report) rtcp_lubanlite_usb_probe_report();
    if (carplay_usb_mux_probe_report) carplay_usb_mux_probe_report();
#if defined(RTCARPLAY_VIDEO)
    app_state_t *app = &s_app;
    if (!app->running || !app->lock) return;
    airplay_screen_server_stats_t rx;
    if (rt_mutex_take(app->lock, RT_WAITING_NO) != RT_EOK) return;
    rx = app->screen_stats;
    (void)rt_mutex_release(app->lock);
    if (!rx.frames && !rx.owner_calls) return;
    uint32_t now = (uint32_t)rt_tick_get_millisecond();
    carplay_video_perf_t video;
    carplay_video_sink_perf(&video);
    if (!app->perf_ms) {
        app->perf_ms = now;
        app->perf_screen = rx; app->perf_video = video;
        return;
    }
    uint32_t dt = now - app->perf_ms;
    if (dt < 5000U) return;
    const airplay_screen_server_stats_t *old = &app->perf_screen;
    const carplay_video_perf_t *prev = &app->perf_video;
    unsigned rx10 = (unsigned)((uint64_t)(rx.frames - old->frames) * 10000U / dt);
    unsigned show10 = (unsigned)((uint64_t)(video.frames - prev->frames) * 10000U / dt);
    unsigned messages = rx.messages - old->messages;
    unsigned gaps = rx.frame_gap_count - old->frame_gap_count;
    unsigned sender = rx.sender_gap_count - old->sender_gap_count;
    unsigned dequeued = video.dequeued - prev->dequeued;
    unsigned decodes = video.decode_calls - prev->decode_calls;
    unsigned presents = video.present_calls - prev->present_calls;
    /* Keep each call below the SDK's 128-byte console buffer in ordinary
     * operation. Long multi-metric calls truncate diagnostic fields. */
    rt_kprintf("[carplay-perf] dt=%ums rx=%u.%u show=%u.%u KiB/s=%u\n",
        (unsigned)dt, rx10 / 10, rx10 % 10, show10 / 10, show10 % 10,
        (unsigned)((rx.wire_bytes - old->wire_bytes) * 1000U / dt / 1024U));
    rt_kprintf("[carplay-perf] reads=%u batches=%u budget_yields=%u queue=%u peak_total=%u drop=%u err=%u\n",
        rx.recv_calls - old->recv_calls, rx.read_batches - old->read_batches,
        rx.read_budget_yields - old->read_budget_yields, video.pending, video.queue_peak,
        video.drops - prev->drops, video.errors - prev->errors);
    rt_kprintf("[carplay-perf] avg_us rx_gap=%u sender_gap=%u assembly=%u process=%u\n",
        gaps ? (unsigned)((rx.frame_gap_us - old->frame_gap_us) / gaps) : 0,
        sender ? (unsigned)((rx.sender_gap_us - old->sender_gap_us) / sender) : 0,
        messages ? (unsigned)((rx.assembly_us - old->assembly_us) / messages) : 0,
        messages ? (unsigned)((rx.process_us - old->process_us) / messages) : 0);
    rt_kprintf("[carplay-perf] avg_ms queue=%u decode=%u present=%u\n",
        dequeued ? (unsigned)((video.queue_ms - prev->queue_ms) / dequeued) : 0,
        decodes ? (unsigned)((video.decode_ms - prev->decode_ms) / decodes) : 0,
        presents ? (unsigned)((video.present_ms - prev->present_ms) / presents) : 0);
    rt_kprintf("[screen-wait] owner_calls=%u ge100ms=%u max_total_us=%u\n",
        rx.owner_calls - old->owner_calls, rx.owner_gap_over100ms - old->owner_gap_over100ms,
        rx.owner_gap_max_us);
    rt_kprintf("[screen-wait] partial_gaps=%u ge100ms=%u max_total_us=%u\n",
        rx.partial_gaps - old->partial_gaps, rx.partial_gap_over100ms - old->partial_gap_over100ms,
        rx.partial_gap_max_us);
    rt_kprintf("[screen-wait] recv_ge10ms=%u max_total_us=%u pending_header=%u body_left=%u age_ms=%u\n",
        rx.recv_block_over10ms - old->recv_block_over10ms, rx.recv_block_max_us,
        rx.pending_header_bytes, rx.pending_body_bytes, rx.pending_age_ms);
    app->perf_ms = now; app->perf_screen = rx; app->perf_video = video;
#endif
}
