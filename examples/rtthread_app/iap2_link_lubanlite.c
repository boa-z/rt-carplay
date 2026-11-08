/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Wireless iAP2 session over the Zephyr Classic RFCOMM transport.
 *
 * This is the application-side pump: one thread accepts a phone on the iAP2
 * SDP service, feeds its bytes to iap2_session, sends the session's iAP2
 * link frames back, and runs the session timers on a 100 ms tick.  The
 * receiver (carplay_app) must be running; the session borrows its SSID,
 * AirPlay endpoint and Ed25519 identity.
 *
 * With RTCARPLAY_MFI the session also gets the 2.0C coprocessor on the I2C
 * bus, so the MFi certificate/challenge exchange runs and the session can
 * reach Wi-Fi configuration and CarPlayStartSession.  When the probe fails
 * the session is created without MFi and stops after IdentificationInformation
 * (the transport path is still testable).
 */
#include "iap2_link_lubanlite.h"

#include <rtthread.h>

#include <stdio.h>
#include <string.h>

#include "airplay_info_plist.h"
#include "carplay_app.h"
#include "iap2_session.h"
#include "rtcp/rtcp.h"
#include "rtcp_lubanlite.h"
#include "rtcp_rtthread.h"

#define IAP2_LOG(fmt, ...) rt_kprintf("[iap2] " fmt "\n", ##__VA_ARGS__)

/* Apple iAP2 service class UUID (confirmed in the vendor ZABlue library). */
#define IAP2_LINK_UUID "00000000-deca-fade-deca-deafdecacafe"

#define IAP2_LINK_RX_MS 100
#define IAP2_LINK_RX_BYTES 1024U

/* The iAP2 callbacks build up to ~1.6 KB bodies on the stack; leave room for
 * the transport and logging paths. */
#define IAP2_LINK_STACK_BYTES (12U * 1024U)
#define IAP2_LINK_PRIORITY 20U
#define IAP2_LINK_TICK 20U

/* IdentificationInformation strings.  modelIdentifier is the accessory model;
 * the firmware/hardware versions are informational.  Override per product. */
#ifndef RTCARPLAY_IAP2_MODEL
#define RTCARPLAY_IAP2_MODEL "D213"
#endif
#ifndef RTCARPLAY_IAP2_FIRMWARE_VERSION
#define RTCARPLAY_IAP2_FIRMWARE_VERSION "0.1"
#endif
#ifndef RTCARPLAY_IAP2_HARDWARE_VERSION
#define RTCARPLAY_IAP2_HARDWARE_VERSION "ArtInChip demo128"
#endif
#define IAP2_LINK_MANUFACTURER "ArtInChip"
#define IAP2_LINK_LANGUAGE "en"

typedef struct {
    iap2_session_t *session;
    int fd;
    char ip_address[16];

    bool link_up;
    bool airplay_connected;
    volatile bool running;
    volatile bool authenticated;
    volatile bool identification_accepted;
    volatile unsigned rx_bytes;
    volatile unsigned tx_bytes;
    volatile unsigned events;
} iap2_link_t;

static iap2_link_t s_link;
static volatile bool s_serve;

static bool link_tx(const uint8_t *bytes, size_t len, void *context)
{
    iap2_link_t *link = context;
    if (link->fd < 1) return false;
    int sent = rtcp_bt_lubanlite_send(link->fd, bytes, len);
    if (sent != (int)len) {
        IAP2_LOG("tx failed (%d of %u bytes)", sent, (unsigned)len);
        return false;
    }
    link->tx_bytes += (unsigned)len;
    return true;
}

static bool link_get_wifi(iap2_session_wifi_t *out, void *context)
{
    (void)context;
    carplay_app_iap2_info_t info;
    if (carplay_app_iap2_info(&info) != RTCP_OK) return false;

    memset(out, 0, sizeof(*out));
    out->ssid = info.ssid;
    out->passphrase = info.passphrase;
    out->channel = info.channel;
    out->security_type = info.security_type;
    if (info.bssid_valid) memcpy(out->bssid, info.bssid, sizeof(out->bssid));
    return true;
}

static bool link_get_airplay(iap2_session_airplay_t *out, void *context)
{
    iap2_link_t *link = context;
    carplay_app_iap2_info_t info;
    if (carplay_app_iap2_info(&info) != RTCP_OK) return false;

    int written = rt_snprintf(link->ip_address, sizeof(link->ip_address),
                              "%u.%u.%u.%u", info.ipv4[0], info.ipv4[1],
                              info.ipv4[2], info.ipv4[3]);
    if (written <= 0 || (size_t)written >= sizeof(link->ip_address)) return false;

    memset(out, 0, sizeof(*out));
    out->airplay_port = info.airplay_port;
    out->ip_address = link->ip_address;
    out->device_identifier = info.device_id;
    out->public_key = info.public_key_hex;
    out->source_version = info.source_version;
    out->mdns_ready = info.mdns_ready;
    out->event_connected = info.airplay_connected;
    return true;
}

static void link_event(iap2_session_event_t event, void *context)
{
    iap2_link_t *link = context;
    ++link->events;

    switch (event) {
    case IAP2_SESSION_EVENT_IDENTIFICATION_ACCEPTED:
        link->identification_accepted = true;
        IAP2_LOG("identification accepted");
        break;
    case IAP2_SESSION_EVENT_IDENTIFICATION_REJECTED:
        IAP2_LOG("identification rejected by the phone");
        break;
    case IAP2_SESSION_EVENT_MFI_CERTIFICATE_SENT:
        IAP2_LOG("MFi certificate sent");
        break;
    case IAP2_SESSION_EVENT_MFI_AUTHENTICATED:
        link->authenticated = true;
        IAP2_LOG("MFi authenticated");
        break;
    case IAP2_SESSION_EVENT_MFI_FAILED:
        IAP2_LOG("MFi authentication failed");
        break;
    case IAP2_SESSION_EVENT_SUBSCRIPTIONS_SENT:
        IAP2_LOG("wireless subscriptions sent");
        break;
    case IAP2_SESSION_EVENT_WIFI_CONFIG_SENT:
        IAP2_LOG("Wi-Fi configuration sent");
        break;
    case IAP2_SESSION_EVENT_CARPLAY_INVITE_SENT:
        IAP2_LOG("CarPlayStartSession sent");
        break;
    case IAP2_SESSION_EVENT_CARPLAY_REINVITE_SENT:
        IAP2_LOG("CarPlayStartSession re-sent");
        break;
    case IAP2_SESSION_EVENT_LINK_READY:
        IAP2_LOG("iAP2 link ready");
        break;
    case IAP2_SESSION_EVENT_LINK_DEAD:
        IAP2_LOG("iAP2 link failed (protocol)");
        break;
    default:
        break;
    }
}

/*
 * The coprocessor is optional at runtime: a board with the part absent (or
 * wired to another bus/address) must still run the identify-only flow so the
 * transport can be tested.  The probe touches I2C once, when the session is
 * created.
 */
static const struct rtcp_mfi *link_mfi(void)
{
#if defined(RTCARPLAY_MFI)
    int result = rtcp_mfi_lubanlite_start(RTCARPLAY_MFI_I2C_BUS,
                                          RTCARPLAY_MFI_I2C_ADDR);
    if (result == RTCP_OK) return rtcp_mfi_lubanlite();
    IAP2_LOG("MFi coprocessor unavailable rc=%d; the session will stop after "
             "identification", result);
#else
    IAP2_LOG("MFi support not built in; the session will stop after "
             "identification");
#endif
    return NULL;
}

static int link_session_create(iap2_link_t *link)
{
    carplay_app_iap2_info_t info;
    int result = carplay_app_iap2_info(&info);
    if (result != RTCP_OK) return result;

    uint8_t bdaddr[6];
    result = rtcp_bt_lubanlite_local_bdaddr(bdaddr);
    if (result != RTCP_OK) {
        IAP2_LOG("cannot read the local Bluetooth address rc=%d", result);
        return result;
    }

    iap2_identity_config_t identity;
    memset(&identity, 0, sizeof(identity));
    identity.name = info.name;
    identity.model_identifier = RTCARPLAY_IAP2_MODEL;
    identity.manufacturer = IAP2_LINK_MANUFACTURER;
    identity.serial_number = info.device_id;
    identity.firmware_version = RTCARPLAY_IAP2_FIRMWARE_VERSION;
    identity.hardware_version = RTCARPLAY_IAP2_HARDWARE_VERSION;
    identity.language = IAP2_LINK_LANGUAGE;
    identity.wifi_ssid = info.ssid;
    memcpy(identity.bluetooth_address, bdaddr,
           sizeof(identity.bluetooth_address));

    iap2_session_config_t config;
    memset(&config, 0, sizeof(config));
    config.identity = identity;
    config.mfi = link_mfi();
    config.tx = link_tx;
    config.get_wifi = link_get_wifi;
    config.get_airplay = link_get_airplay;
    config.on_event = link_event;
    config.context = link;

    link->session = iap2_session_create(&config);
    return link->session != NULL ? RTCP_OK : RTCP_ENOMEM;
}

static void link_reset_progress(iap2_link_t *link)
{
    link->authenticated = false;
    link->identification_accepted = false;
}

static void link_thread(void *parameter)
{
    iap2_link_t *link = parameter;

    int result = rtcp_bt_lubanlite.enable("rt-carplay", NULL);
    if (result != RTCP_OK) {
        IAP2_LOG("Bluetooth enable failed rc=%d", result);
        goto done;
    }

    result = link_session_create(link);
    if (result != RTCP_OK) {
        IAP2_LOG("iAP2 session create failed rc=%d", result);
        goto done;
    }

    uint8_t channel = 0U;
    result = rtcp_bt_lubanlite_rfcomm_listen(0U, "rt-carplay", IAP2_LINK_UUID,
                                             &channel);
    if (result != RTCP_OK) {
        IAP2_LOG("iAP2 SDP/RFCOMM registration failed rc=%d", result);
        goto done;
    }
    IAP2_LOG("RFCOMM iAP2 service on channel %u", (unsigned)channel);

    while (s_serve) {
        if (!carplay_app_is_running()) {
            IAP2_LOG("receiver stopped, ending the iAP2 service");
            break;
        }

        int fd = -1;
        result = rtcp_bt_lubanlite_rfcomm_accept(1, &fd);
        if (result != RTCP_OK) {
            if (s_serve) IAP2_LOG("accept stopped rc=%d", result);
            break;
        }

        link->fd = fd;
        link->link_up = true;
        link_reset_progress(link);
        link->rx_bytes = 0U;
        link->tx_bytes = 0U;

        carplay_app_iap2_info_t info;
        link->airplay_connected = carplay_app_iap2_info(&info) == RTCP_OK &&
                                  info.airplay_connected;
        iap2_session_link_up(link->session, rtcp_os_rtthread.now_ms());
        IAP2_LOG("link up (fd %d)", fd);

        while (s_serve) {
            if (!carplay_app_is_running()) break;

            uint8_t buffer[IAP2_LINK_RX_BYTES];
            int received = rtcp_bt_lubanlite_recv_timeout(fd, buffer,
                                                          sizeof(buffer),
                                                          IAP2_LINK_RX_MS);
            uint64_t now = rtcp_os_rtthread.now_ms();

            if (received > 0) {
                link->rx_bytes += (unsigned)received;
                iap2_session_feed(link->session, buffer, (size_t)received, now);
            } else if (received == 0) {
                break;
            } else if (received != RTCP_ETIMEDOUT) {
                IAP2_LOG("recv failed rc=%d", received);
                rtcp_bt_lubanlite_drop_link(fd);
                break;
            }

            /* Notify only on transitions: the session restarts its invite
             * timer on every "disconnected" notification. */
            if (carplay_app_iap2_info(&info) == RTCP_OK &&
                info.airplay_connected != link->airplay_connected) {
                link->airplay_connected = info.airplay_connected;
                IAP2_LOG("AirPlay event channel %s",
                         info.airplay_connected ? "connected" : "closed");
                iap2_session_notify_airplay_event(link->session,
                                                  info.airplay_connected, now);
            }

            iap2_session_advance(link->session, now);
        }

        if (link->session != NULL) iap2_session_link_down(link->session);
        link->fd = -1;
        link->link_up = false;
        link_reset_progress(link);
        IAP2_LOG("link down (rx %u, tx %u, events %u)",
                 link->rx_bytes, link->tx_bytes, link->events);
    }

done:
    if (link->session != NULL && link->link_up) {
        iap2_session_link_down(link->session);
    }
    link->fd = -1;
    link->link_up = false;
    link_reset_progress(link);
    if (link->session != NULL) {
        iap2_session_destroy(link->session);
        link->session = NULL;
    }
    link->running = false;
    s_serve = false;
    carplay_bt_release(CARPLAY_BT_OWNER_IAP2);
    IAP2_LOG("service stopped");
}

int carplay_iap2_start(void)
{
    if (s_link.running) return RTCP_ESTATE;
    if (!carplay_app_is_running()) {
        IAP2_LOG("start the receiver (carplay_start) first");
        return RTCP_ESTATE;
    }
    if (!carplay_bt_claim(CARPLAY_BT_OWNER_IAP2)) {
        IAP2_LOG("Bluetooth link is already owned by another service");
        return RTCP_ESTATE;
    }

    memset(&s_link, 0, sizeof(s_link));
    s_link.fd = -1;
    s_link.running = true;
    s_serve = true;
    rt_thread_t thread = rt_thread_create("carplay-iap2", link_thread, &s_link,
                                          IAP2_LINK_STACK_BYTES,
                                          IAP2_LINK_PRIORITY, IAP2_LINK_TICK);
    if (thread == NULL) {
        s_serve = false;
        s_link.running = false;
        carplay_bt_release(CARPLAY_BT_OWNER_IAP2);
        return RTCP_ENOMEM;
    }
    (void)rt_thread_startup(thread);
    return RTCP_OK;
}

void carplay_iap2_request_stop(void)
{
    if (!s_link.running) return;
    s_serve = false;
    /* Wakes a blocked accept()/recv(); the thread owns the teardown. */
    rtcp_bt_lubanlite_disconnect();
}

int carplay_iap2_stop(void)
{
    carplay_iap2_request_stop();
    for (unsigned waited = 0U; s_link.running && waited < 3000U;
         waited += 10U) {
        rt_thread_mdelay(10);
    }
    return s_link.running ? RTCP_ETIMEDOUT : RTCP_OK;
}

bool carplay_iap2_is_running(void)
{
    return s_link.running;
}

bool carplay_iap2_identification_accepted(void)
{
    return s_link.running && s_link.link_up && s_link.identification_accepted;
}

bool carplay_iap2_authenticated(void)
{
    return s_link.running && s_link.link_up && s_link.authenticated;
}

/* ------------------------------------------------------------------ */
/* MSH command                                                         */
/* ------------------------------------------------------------------ */

static int cmd_carplay_iap2(int argc, char **argv)
{
    const char *sub = argc >= 2 ? argv[1] : "status";

    if (strcmp(sub, "start") == 0) {
        int result = carplay_iap2_start();
        if (result == RTCP_ESTATE) {
            rt_kprintf("carplay_iap2: start carplay_start first, and stop "
                       "carplay_bt if it is running\n");
        } else if (result != RTCP_OK) {
            rt_kprintf("carplay_iap2 start failed: %d\n", result);
        }
        return 0;
    }
    if (strcmp(sub, "stop") == 0) {
        int result = carplay_iap2_stop();
        rt_kprintf("carplay_iap2 %s\n",
                   result == RTCP_OK ? "stopped" : "stop timed out");
        return 0;
    }
    if (strcmp(sub, "status") == 0) {
        const char *coproc = "not built in";
#if defined(RTCARPLAY_MFI)
        coproc = rtcp_mfi_lubanlite_ready() ? "ready" : "absent";
#endif
        rt_kprintf("carplay_iap2: %s, link %s, BT channel %u, "
                   "coprocessor %s, identification %s, MFi %s, "
                   "rx %u tx %u, events %u\n",
                   s_link.running ? "serving" : "off",
                   s_link.link_up ? "up" : "down",
                   (unsigned)rtcp_bt_lubanlite_channel(), coproc,
                   carplay_iap2_identification_accepted() ? "accepted"
                                                         : "pending",
                   carplay_iap2_authenticated() ? "authenticated"
                       : (s_link.link_up ? "not authenticated" : "-"),
                   s_link.rx_bytes, s_link.tx_bytes, s_link.events);
        return 0;
    }
    rt_kprintf("usage: carplay_iap2 start|stop|status\n");
    return 0;
}

MSH_CMD_EXPORT_ALIAS(cmd_carplay_iap2, carplay_iap2,
                     Wireless iAP2 over RFCOMM: start|stop|status);
