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
#include "carplay_mfi_provider.h"
#include "iap2_session.h"
#include "rtcp/rtcp.h"
#include "rtcp_lubanlite.h"
#include "rtcp_rtthread.h"

#if defined(RTCARPLAY_MFI)
/* Used only by the carplay_mfi self-test to check that the coprocessor's
 * private key belongs to the certificate it returns. */
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/x509_crt.h"
#endif

#define IAP2_LOG(fmt, ...) rt_kprintf("[iap2] " fmt "\n", ##__VA_ARGS__)

/* iAP2 service class UUID of the *accessory* RFCOMM server (...CAFF).
 *
 * The two DECAFADE UUIDs have distinct roles.  The accessory publishes its
 * own service under ...CAFF and the phone connects to that; ...CAFE is the
 * iPhone-side service an accessory queries when it initiates the connection
 * itself.  The working reference stack registers iap2_uuid_srv = ...CAFF and
 * ios_dev_uuid_srv = ...CAFE in exactly those roles.
 *
 * diag7 registered this server as ...CAFE, so iOS found no accessory iAP2
 * service: after the ACL connect it ran only the generic SDP walk
 * (0x0100/0x1200/0x1000) and dropped the link with reason 0x13. */
#define IAP2_LINK_UUID "00000000-deca-fade-deca-deafdecacaff"

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
    volatile bool listening;
    volatile int error;
    volatile bool authenticated;
    volatile bool mfi_completed;
    volatile unsigned wifi_configs, availability, invites, reinvites, invite_acks;
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
        link->error = RTCP_ESTATE;
        IAP2_LOG("identification rejected by the phone");
        break;
    case IAP2_SESSION_EVENT_MFI_CERTIFICATE_SENT:
        IAP2_LOG("MFi certificate sent");
        break;
    case IAP2_SESSION_EVENT_MFI_AUTHENTICATED:
        link->authenticated = true;
        link->mfi_completed = true;
        IAP2_LOG("MFi authenticated");
        break;
    case IAP2_SESSION_EVENT_MFI_FAILED:
        link->error = RTCP_EIO;
        IAP2_LOG("MFi authentication failed");
        break;
    case IAP2_SESSION_EVENT_MFI_CERT_FAILED:
        IAP2_LOG("MFi reason: certificate read or 0xAA01 send failed");
        break;
    case IAP2_SESSION_EVENT_MFI_CHALLENGE_INVALID:
        IAP2_LOG("MFi reason: 0xAA02 challenge missing or length out of range");
        break;
    case IAP2_SESSION_EVENT_MFI_SIGN_FAILED:
        IAP2_LOG("MFi reason: coprocessor sign failed or returned no signature");
        break;
    case IAP2_SESSION_EVENT_MFI_STATE_INVALID:
        IAP2_LOG("MFi reason: 0xAA02/0xAA05 arrived out of sequence");
        break;
    case IAP2_SESSION_EVENT_MFI_ACK_FAILED:
        IAP2_LOG("MFi reason: 0xAA03 challenge response was not link-acked");
        break;
    case IAP2_SESSION_EVENT_MFI_PHONE_REJECTED:
        IAP2_LOG("MFi reason: phone sent 0xAA04 MFiAuthFailed");
        break;
    case IAP2_SESSION_EVENT_MFI_RESPONSE_SEND_FAILED:
        IAP2_LOG("MFi reason: 0xAA03 challenge response could not be queued");
        break;
    case IAP2_SESSION_EVENT_SUBSCRIPTIONS_SENT:
        IAP2_LOG("wireless subscriptions sent");
        break;
    case IAP2_SESSION_EVENT_WIFI_CONFIG_SENT:
        ++link->wifi_configs;
        IAP2_LOG("Wi-Fi configuration sent");
        break;
    case IAP2_SESSION_EVENT_CARPLAY_INVITE_SENT:
        ++link->invites;
        IAP2_LOG("CarPlayStartSession sent");
        break;
    case IAP2_SESSION_EVENT_CARPLAY_REINVITE_SENT:
        ++link->reinvites;
        IAP2_LOG("CarPlayStartSession re-sent");
        break;
    case IAP2_SESSION_EVENT_CARPLAY_AVAILABILITY_RECEIVED:
        ++link->availability;
        IAP2_LOG("CarPlayAvailability received");
        break;
    case IAP2_SESSION_EVENT_CARPLAY_INVITE_ACKED:
        ++link->invite_acks;
        IAP2_LOG("CarPlayStartSession link-acked");
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
 * created; on failure the session gets a lazy handle that re-probes at
 * certificate time.
 */
static const struct rtcp_mfi *link_mfi(void)
{
#if defined(RTCARPLAY_MFI)
    int result = RTCP_EIO;

    /* Two attempts at start time only; the coprocessor may still be
     * settling (it answered 0x32 to every register right after boot even
     * with the Bluetooth controller untouched).  If both fail the session
     * still gets a lazy handle: the first certificate request re-probes,
     * by which time the phone has connected and the part has recovered. */
    for (int attempt = 0; attempt < 2; attempt++) {
        result = rtcp_mfi_lubanlite_start(RTCARPLAY_MFI_I2C_BUS,
                                          RTCARPLAY_MFI_I2C_ADDR);
        if (result == RTCP_OK) return rtcp_mfi_lubanlite();
        if (attempt == 0) rt_thread_mdelay(100);
    }
    IAP2_LOG("MFi coprocessor not ready at start rc=%d; retrying when the "
             "phone requests the certificate", result);
    const struct rtcp_mfi *lazy = rtcp_mfi_lubanlite_lazy(
        RTCARPLAY_MFI_I2C_BUS, RTCARPLAY_MFI_I2C_ADDR);
    if (lazy != NULL) return lazy;
    IAP2_LOG("MFi lazy handle unavailable; the session will stop after "
             "identification");
#else
    IAP2_LOG("MFi support not built in; the session will stop after "
             "identification");
#endif
    return NULL;
}

static int link_session_create(iap2_link_t *link, const struct rtcp_mfi *mfi)
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
    config.mfi = mfi;
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

/*
 * After AirPlay pair-verify the phone proves the accessory is MFi licensed
 * with POST /auth-setup: a 33-byte MFiSAP request (X25519 public key) that
 * the accessory answers with its certificate and an AES-CTR-encrypted
 * coprocessor signature.  core/airplay reads a process-wide provider for
 * this; carplay_mfi_provider.c owns its lifecycle so the wired USB transport
 * installs the same provider (without it the handler answers "503 Service
 * Unavailable" and iOS drops the RTSP connection after pair-verify).
 */

static void link_thread(void *parameter)
{
    iap2_link_t *link = parameter;

    /* Probe the coprocessor before the Bluetooth controller is enabled so a
     * healthy part is ready by the time the phone connects.  Board logs show
     * the 2.0C can still answer 0x32 to every register this early in boot
     * (even with Bluetooth untouched), so link_mfi() keeps a lazy handle
     * that re-probes at certificate time. */
    const struct rtcp_mfi *mfi = link_mfi();
    if (mfi != NULL && carplay_mfi_provider_install(mfi) != RTCP_OK) {
        IAP2_LOG("AirPlay /auth-setup MFi provider unavailable");
    }

    int result = rtcp_bt_lubanlite.enable("rt-carplay", NULL);
    if (result != RTCP_OK) {
        IAP2_LOG("Bluetooth enable failed rc=%d", result);
        goto done;
    }
    if (!s_serve) goto done;

    result = link_session_create(link, mfi);
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
    link->listening = true;

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
        link->error = RTCP_OK;
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
    if (s_serve && result != RTCP_OK) link->error = result;
    link->listening = false;
    if (link->session != NULL && link->link_up) {
        iap2_session_link_down(link->session);
    }
    /* Also covers stop arriving during bt_enable()/SDP registration: do
     * not leave a freshly initialized controller discoverable after stop. */
    rtcp_bt_lubanlite_disconnect();
    link->fd = -1;
    link->link_up = false;
    link_reset_progress(link);
    if (link->session != NULL) {
        iap2_session_destroy(link->session);
        link->session = NULL;
    }
    carplay_mfi_provider_release(mfi);
    s_serve = false;
    carplay_bt_release(CARPLAY_BT_OWNER_IAP2);
    IAP2_LOG("service stopped");
    /* Publish quiescence last: a mode switch may now replace the receiver
     * strings, MFi provider and this singleton's storage. */
    link->running = false;
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
    if (rt_thread_startup(thread) != RT_EOK) {
        rt_thread_delete(thread);
        s_serve = false; s_link.running = false;
        s_link.error = RTCP_EIO;
        carplay_bt_release(CARPLAY_BT_OWNER_IAP2);
        return RTCP_EIO;
    }
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

void carplay_iap2_get_status(carplay_iap2_status_t *out)
{
    if (!out) return;
    rt_base_t flags = rt_hw_interrupt_disable();
    out->running = s_link.running;
    out->listening = s_link.listening;
    out->connected = s_link.link_up;
    out->identified = s_link.link_up && s_link.identification_accepted;
    out->authenticated = s_link.link_up && s_link.authenticated;
    out->mfi_completed = s_link.mfi_completed;
    out->wifi_configs = s_link.wifi_configs;
    out->availability = s_link.availability;
    out->invites = s_link.invites;
    out->reinvites = s_link.reinvites;
    out->invite_acks = s_link.invite_acks;
    out->error = s_link.error;
    rt_hw_interrupt_enable(flags);
}

/* ------------------------------------------------------------------ */
/* MSH command                                                         */
/* ------------------------------------------------------------------ */

static int cmd_carplay_iap2(int argc, char **argv)
{
    const char *sub = argc >= 2 ? argv[1] : "status";
#ifdef RTCARPLAY_DEMO_LAUNCHER
    if (strcmp(sub, "status") != 0) {
        rt_kprintf("LVGL demo owns Bluetooth iAP2; use the connection selector\n");
        return 0;
    }
#endif

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
        rt_kprintf("  listening=%u error=%d MFi_completed=%u wifi_configs=%u "
                   "availability=%u invites=%u reinvites=%u link_acks=%u\n",
                   s_link.listening, s_link.error, s_link.mfi_completed,
                   s_link.wifi_configs, s_link.availability, s_link.invites,
                   s_link.reinvites, s_link.invite_acks);
        return 0;
    }
    rt_kprintf("usage: carplay_iap2 start|stop|status\n");
    return 0;
}

MSH_CMD_EXPORT_ALIAS(cmd_carplay_iap2, carplay_iap2,
                     Wireless iAP2 over RFCOMM: start|stop|status);

#if defined(RTCARPLAY_MFI)
/*
 * Bring-up diagnostic: dump every transaction shape on the coprocessor bus
 * and scan the bus, so an unexpected probe result can be compared with the
 * raw bus behaviour.  Usage: carplay_mfi [bus] [addr]  (defaults i2c0/0x10).
 *
 *   carplay_mfi cert       read the certificate and print its length/head/tail
 *   carplay_mfi cert raw   read the certificate and print the full DER
 *   carplay_mfi sign       sign a fixed 20-byte challenge and print the result
 */
#define IAP2_LINK_CERT_CAPACITY 1536U

static const struct rtcp_mfi *cmd_mfi_ready(void)
{
    const struct rtcp_mfi *mfi = rtcp_mfi_lubanlite();

    if (mfi != NULL) return mfi;
    int result = rtcp_mfi_lubanlite_start(RTCARPLAY_MFI_I2C_BUS,
                                          RTCARPLAY_MFI_I2C_ADDR);
    if (result != RTCP_OK) {
        rt_kprintf("carplay_mfi: coprocessor not ready rc=%d\n", result);
        return NULL;
    }
    return rtcp_mfi_lubanlite();
}

static int cmd_carplay_mfi_cert(int argc, char **argv)
{
    const struct rtcp_mfi *mfi = cmd_mfi_ready();
    if (mfi == NULL) return 0;

    static uint8_t certificate[IAP2_LINK_CERT_CAPACITY];
    size_t len = sizeof(certificate);
    int result = mfi->read_cert(certificate, sizeof(certificate), &len);
    if (result != RTCP_OK) {
        rt_kprintf("carplay_mfi cert: read failed rc=%d\n", result);
        return 0;
    }

    rt_kprintf("carplay_mfi cert: %u bytes\n", (unsigned)len);
    bool raw = argc >= 3 && strcmp(argv[2], "raw") == 0;
    size_t shown = raw ? len : (len < 16U ? len : 16U);
    for (size_t offset = 0; offset < shown; offset += 16U) {
        rt_kprintf("%04x:", (unsigned)offset);
        for (size_t i = 0; i < 16U && offset + i < shown; ++i) {
            rt_kprintf(" %02x", certificate[offset + i]);
        }
        rt_kprintf("\n");
    }
    if (!raw && len > shown) {
        rt_kprintf("carplay_mfi cert raw  (print the full DER as hex)\n");
    }
    return 0;
}

/*
 * Check a coprocessor signature against the certificate it returned.  The
 * 2.0C signs the 20-byte iAP2 challenge (Apple's platform code calls it the
 * digest) with RSA PKCS#1 v1.5; the exact DigestInfo wrapping differs
 * between parts, so try the raw encoding and the SHA-1 DigestInfo encoding.
 * A match proves that the certificate's public key and the private key
 * inside the coprocessor belong together, so a phone-side rejection after
 * this passes points at iOS trust or the exchange itself, not at the
 * certificate data path.
 */
/*
 * The 2.0C returns its certificate as a PKCS#7 SignedData blob (the first
 * bytes are 06 09 2a 86 48 86 f7 0d 01 07 02, OID 1.2.840.113549.1.7.2),
 * not as a bare X.509 certificate.  iOS expects exactly these bytes and they
 * are sent unchanged; only the local self-test needs the X.509 leaf inside.
 * mbedtls 2.7 has no PKCS#7 parser, so walk the DER containers directly.
 */
struct der_tlv {
    uint8_t tag;
    const uint8_t *start;   /* tag byte */
    const uint8_t *value;   /* first content byte */
    size_t len;             /* content length */
    const uint8_t *next;    /* first byte after this TLV */
};

static int der_next(const uint8_t *p, const uint8_t *end, struct der_tlv *t)
{
    size_t len;

    if (p == NULL || t == NULL || (size_t)(end - p) < 2U) return -1;
    t->start = p;
    t->tag = *p++;
    len = *p++;
    if ((len & 0x80U) != 0U) {
        size_t n = len & 0x7FU;
        if (n == 0U || n > 3U || (size_t)(end - p) < n) return -1;
        len = 0U;
        while (n-- > 0U) len = (len << 8) | *p++;
    }
    if ((size_t)(end - p) < len) return -1;
    t->value = p;
    t->len = len;
    t->next = p + len;
    return 0;
}

/*
 * Locate the first X.509 certificate inside a PKCS#7 SignedData blob and
 * return it as a complete DER TLV.  Returns 0 on success.
 */
static int pkcs7_first_certificate(const uint8_t *der, size_t len,
                                   const uint8_t **cert, size_t *cert_len)
{
    static const uint8_t signed_data_oid[9] = {
        0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x02
    };
    struct der_tlv outer, oid, wrapper, signed_data, field, certs, first;

    /* ContentInfo ::= SEQUENCE { contentType OID, content [0] EXPLICIT } */
    if (der_next(der, der + len, &outer) != 0 || outer.tag != 0x30)
        return -1;
    if (der_next(outer.value, outer.next, &oid) != 0 || oid.tag != 0x06)
        return -1;
    if (oid.len != sizeof(signed_data_oid) ||
        memcmp(oid.value, signed_data_oid, sizeof(signed_data_oid)) != 0)
        return -1;
    if (der_next(oid.next, outer.next, &wrapper) != 0 || wrapper.tag != 0xA0)
        return -1;
    if (der_next(wrapper.value, wrapper.next, &signed_data) != 0 ||
        signed_data.tag != 0x30)
        return -1;

    /* SignedData ::= SEQUENCE { version, digestAlgorithms, contentInfo,
     *                           certificates [0] IMPLICIT ... } */
    if (der_next(signed_data.value, signed_data.next, &field) != 0 ||
        field.tag != 0x02)
        return -1;
    if (der_next(field.next, signed_data.next, &field) != 0 ||
        field.tag != 0x31)
        return -1;
    if (der_next(field.next, signed_data.next, &field) != 0 ||
        field.tag != 0x30)
        return -1;
    if (der_next(field.next, signed_data.next, &certs) != 0 ||
        certs.tag != 0xA0)
        return -1;

    /* The first element of the implicit SET is the leaf certificate. */
    if (der_next(certs.value, certs.next, &first) != 0 || first.tag != 0x30)
        return -1;

    *cert = first.start;
    *cert_len = (size_t)(first.next - first.start);
    return 0;
}

static void cmd_mfi_check_signature(const uint8_t *certificate,
                                    size_t certificate_len,
                                    const uint8_t *challenge,
                                    size_t challenge_len,
                                    const uint8_t *signature,
                                    size_t signature_len)
{
    mbedtls_x509_crt crt;
    const uint8_t *x509 = certificate;
    size_t x509_len = certificate_len;

    mbedtls_x509_crt_init(&crt);

    if (pkcs7_first_certificate(certificate, certificate_len, &x509,
                                &x509_len) == 0) {
        rt_kprintf("carplay_mfi sign: PKCS#7 SignedData, leaf certificate "
                   "%u bytes of %u\n", (unsigned)x509_len,
                   (unsigned)certificate_len);
    } else {
        rt_kprintf("carplay_mfi sign: certificate is not PKCS#7 SignedData; "
                   "parsing as X.509\n");
    }

    int result = mbedtls_x509_crt_parse(&crt, x509, x509_len);
    if (result != 0) {
        rt_kprintf("carplay_mfi sign: certificate parse failed -0x%04x\n",
                   -result);
        mbedtls_x509_crt_free(&crt);
        return;
    }
    if (mbedtls_pk_get_type(&crt.pk) != MBEDTLS_PK_RSA) {
        rt_kprintf("carplay_mfi sign: certificate key is not RSA (type %d)\n",
                   (int)mbedtls_pk_get_type(&crt.pk));
        mbedtls_x509_crt_free(&crt);
        return;
    }

    mbedtls_rsa_context *rsa = mbedtls_pk_rsa(crt.pk);
    size_t rsa_len = (size_t)mbedtls_rsa_get_len(rsa);
    rt_kprintf("carplay_mfi sign: certificate RSA %u bits, signature %u "
               "bytes\n", (unsigned)(rsa_len * 8U), (unsigned)signature_len);
    if (signature_len != rsa_len) {
        rt_kprintf("carplay_mfi sign: signature length does not match the "
                   "certificate key\n");
        mbedtls_x509_crt_free(&crt);
        return;
    }

    int raw = mbedtls_rsa_pkcs1_verify(rsa, NULL, NULL, MBEDTLS_RSA_PUBLIC,
                                       MBEDTLS_MD_NONE, challenge_len,
                                       challenge, signature);
    int sha1 = mbedtls_rsa_pkcs1_verify(rsa, NULL, NULL, MBEDTLS_RSA_PUBLIC,
                                        MBEDTLS_MD_SHA1, challenge_len,
                                        challenge, signature);
    rt_kprintf("carplay_mfi sign: verify raw=%d sha1=%d%s\n", raw, sha1,
               (raw == 0 || sha1 == 0) ? " (key matches certificate)" : "");
    mbedtls_x509_crt_free(&crt);
}

static int cmd_carplay_mfi_sign(void)
{
    const struct rtcp_mfi *mfi = cmd_mfi_ready();
    if (mfi == NULL) return 0;

    static uint8_t certificate[IAP2_LINK_CERT_CAPACITY];
    static uint8_t signature[RTCP_MFI_SIG_MAX];
    uint8_t challenge[RTCP_MFI_CHALLENGE_LEN];
    for (size_t i = 0; i < sizeof(challenge); ++i) {
        challenge[i] = (uint8_t)i;
    }

    size_t signature_len = 0U;
    int result = mfi->sign(challenge, sizeof(challenge), signature,
                           sizeof(signature), &signature_len);
    rt_kprintf("carplay_mfi sign: rc=%d signature %u bytes\n", result,
               (unsigned)signature_len);
    if (result != RTCP_OK) return 0;

    for (size_t offset = 0; offset < signature_len; offset += 16U) {
        rt_kprintf("%04x:", (unsigned)offset);
        for (size_t i = 0; i < 16U && offset + i < signature_len; ++i) {
            rt_kprintf(" %02x", signature[offset + i]);
        }
        rt_kprintf("\n");
    }

    size_t certificate_len = sizeof(certificate);
    if (mfi->read_cert(certificate, sizeof(certificate), &certificate_len) ==
        RTCP_OK) {
        cmd_mfi_check_signature(certificate, certificate_len, challenge,
                                sizeof(challenge), signature, signature_len);
    } else {
        rt_kprintf("carplay_mfi sign: certificate read failed; "
                   "signature not verified\n");
    }
    return 0;
}

static int cmd_carplay_mfi(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "cert") == 0) {
        return cmd_carplay_mfi_cert(argc, argv);
    }
    if (argc >= 2 && strcmp(argv[1], "sign") == 0) {
        return cmd_carplay_mfi_sign();
    }
    if (argc >= 2 && strcmp(argv[1], "dump") == 0) {
        rtcp_mfi_lubanlite_dump(RTCARPLAY_MFI_I2C_BUS, RTCARPLAY_MFI_I2C_ADDR);
        return 0;
    }

    const char *bus_name = argc >= 2 ? argv[1] : RTCARPLAY_MFI_I2C_BUS;
    unsigned addr = RTCARPLAY_MFI_I2C_ADDR;

    if (argc >= 3) {
        const char *p = argv[2];
        unsigned v = 0;

        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        while (*p != '\0') {
            char c = *p++;
            unsigned d;

            if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
            else { v = 0; break; }
            v = (v << 4) | d;
        }
        addr = v;
    }

    rtcp_mfi_lubanlite_dump(bus_name, (uint8_t)addr);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_carplay_mfi, carplay_mfi,
                     raw MFi 2.0C bus dump (bring-up diagnostic));
#endif
