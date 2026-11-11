/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Wired CarPlay USB bring-up command.
 *
 * This is deliberately a thin shell over the platform seam: all descriptor
 * parsing lives in core/usb, and all transfers are performed by
 * port/luban-lite/rtcp_usb_lubanlite.c.  The command exists so Stage 1 can be
 * verified on the board without any protocol code:
 *
 *   carplay_usb start     arm the enumeration hook (do this before plugging
 *                         the phone; the hook only runs during enumeration)
 *   carplay_usb status    show the attached device, its selected
 *                         configuration, interfaces and endpoints
 *   carplay_usb request   send Apple's vendor request 0x52 again and print
 *                         the descriptor count the phone reports afterwards
 *   carplay_usb mux       open USBMUX to the phone's lockdown port, send a
 *                         GetValue request and print the reply; proves the
 *                         bulk pipe and the mux/TCP layers end to end
 *   carplay_usb pair      run wired lockdown pairing (accept the trust
 *                         dialog on the phone) and open the carkit service;
 *                         blocks until the phone answers or pairing times out
 *   carplay_usb unpair    close the carkit service and its lockdown session
 *   carplay_usb iap2      run the iAP2 session over the open carkit stream
 *                         (identification and MFi; the AirPlay endpoint comes
 *                         from 'carplay_start wired')
 *   carplay_usb iap2stop  stop the wired iAP2 session
 *   carplay_usb net       show the NCM netdev state and its IPv6 link-local
 *                         address (the value CarPlayStartSession advertises)
 *   carplay_usb stop      disarm the hook
 */
#include <rtthread.h>

#include <string.h>

#include "carplay_app.h"
#include "carplay_mfi_provider.h"
#include "iap2_session.h"
#include "iap2_stream.h"
#include "lockdown.h"
#include "lockdown_session.h"
#include "rtcp_lubanlite.h"
#include "rtcp/rtcp.h"
#include "rtcp/stream.h"
#include "rtcp/usb.h"
#include "usbmux_host.h"

#ifdef LPKG_RT_CARPLAY_LUBANLITE_USB

#define USB_CMD_TAG "carplay_usb"

#define APPLE_REQUEST_CARPLAY_CONFIGS 0x52U
#define APPLE_CARPLAY_CONFIGS_WINDEX 0x0004U

#define MUX_REPLY_MAX 2048U

/* Defined with the wired iAP2 state below; the detach event only raises the
 * stop flag so the USB hub thread never blocks on the iAP2 thread. */
static void iap2_usb_request_stop(void);

static const char *speed_name(uint8_t speed)
{
    switch (speed) {
        case RTCP_USB_SPEED_LOW:
            return "low";
        case RTCP_USB_SPEED_FULL:
            return "full";
        case RTCP_USB_SPEED_HIGH:
            return "high";
        case RTCP_USB_SPEED_SUPER:
            return "super";
        default:
            return "unknown";
    }
}

static void usb_event(struct rtcp_usb *usb, rtcp_usb_event_t event, void *user)
{
    (void)usb;
    (void)user;
    if (event == RTCP_USB_EVENT_ATTACHED) {
        rt_kprintf("[carplay_usb] event=attached\n");
    } else {
        rt_kprintf("[carplay_usb] event=detached\n");
        /* The carkit stream is gone with the device.  Ask the iAP2 thread to
         * stop; it releases the stream and MFi provider in its own context. */
        iap2_usb_request_stop();
    }
}

static void print_interface(const rtcp_usb_interface_t *interface)
{
    rt_kprintf("  intf %u class=%02x/%02x/%02x alts=%u\n",
               interface->number, interface->class_code, interface->subclass,
               interface->protocol, interface->altsetting_count);
    for (uint8_t alt = 0U; alt < interface->altsetting_count; ++alt) {
        const rtcp_usb_altsetting_t *setting = &interface->altsettings[alt];
        if (setting->endpoint_count == 0U) {
            continue;
        }
        rt_kprintf("    alt %u endpoints=%u\n", setting->alternate_setting,
                   setting->endpoint_count);
        for (uint8_t e = 0U; e < setting->endpoint_count; ++e) {
            const rtcp_usb_endpoint_t *endpoint = &setting->endpoints[e];
            rt_kprintf("      ep 0x%02x %s mps=%u\n", endpoint->address,
                       (endpoint->address & 0x80U) != 0U ? "in" : "out",
                       endpoint->max_packet_size);
        }
    }
}

static int cmd_status(void)
{
    static rtcp_usb_device_t device;
    int ret = rtcp_usb_lubanlite.ops->get_device(
        rtcp_usb_lubanlite.context, &device);
    if (ret != RTCP_OK) {
        rt_kprintf("[carplay_usb] not attached (ret=%d); run "
                   "'carplay_usb start' before plugging the phone\n", ret);
        return 0;
    }

    rt_kprintf("[carplay_usb] device %04x:%04x bcd=%04x bus=%u addr=%u "
               "speed=%s configs=%u\n",
               device.vendor_id, device.product_id, device.bcd_device,
               device.bus_id, device.address, speed_name(device.speed),
               device.configuration_count);
    rt_kprintf("[carplay_usb] config index=%u value=%u interfaces=%u "
               "total=%u carplay=%s\n",
               device.configuration_index, device.configuration_value,
               device.interface_count, device.configuration_total_length,
               rtcp_usb_is_carplay_configuration(&device) ? "yes" : "no");

    for (uint8_t i = 0U; i < device.interface_count; ++i) {
        print_interface(&device.interfaces[i]);
    }

    int mux = rtcp_usb_find_interface(&device, RTCP_USB_CLASS_VENDOR,
                                      RTCP_USB_APPLE_SUBCLASS_USBMUX,
                                      RTCP_USB_APPLE_PROTOCOL_USBMUX);
    if (mux >= 0) {
        uint8_t out = 0U;
        uint8_t in = 0U;
        if (rtcp_usb_interface_bulk_endpoints(&device.interfaces[mux], &out,
                                              &in)) {
            rt_kprintf("[carplay_usb] usbmux out=0x%02x in=0x%02x\n", out, in);
        }
    }
    return 0;
}

static int cmd_request(void)
{
    static rtcp_usb_device_t device;
    int ret = rtcp_usb_lubanlite.ops->get_device(
        rtcp_usb_lubanlite.context, &device);
    if (ret != RTCP_OK) {
        rt_kprintf("[carplay_usb] not attached\n");
        return 0;
    }

    uint8_t response = 0U;
    ret = rtcp_usb_lubanlite.ops->control_transfer(
        rtcp_usb_lubanlite.context,
        RTCP_USB_REQ_DIR_IN | RTCP_USB_REQ_TYPE_VENDOR |
            RTCP_USB_REQ_RECIPIENT_DEVICE,
        APPLE_REQUEST_CARPLAY_CONFIGS, 0U, APPLE_CARPLAY_CONFIGS_WINDEX,
        &response, 1U, 1000U);
    rt_kprintf("[carplay_usb] request 0x52 ret=%d response=%02x\n", ret,
               response);

    uint8_t descriptor[18];
    ret = rtcp_usb_lubanlite.ops->control_transfer(
        rtcp_usb_lubanlite.context,
        RTCP_USB_REQ_DIR_IN | RTCP_USB_REQ_TYPE_STANDARD |
            RTCP_USB_REQ_RECIPIENT_DEVICE,
        RTCP_USB_REQ_GET_DESCRIPTOR,
        (uint16_t)(RTCP_USB_DESC_DEVICE << 8), 0U, descriptor,
        sizeof(descriptor), 1000U);
    if (ret >= (int)sizeof(descriptor) && descriptor[0] == 18U &&
        descriptor[1] == RTCP_USB_DESC_DEVICE) {
        rt_kprintf("[carplay_usb] bNumConfigurations now %u (was %u); "
                   "replug or reboot to enumerate the new set\n",
                   descriptor[17], device.configuration_count);
    } else {
        rt_kprintf("[carplay_usb] device descriptor read ret=%d\n", ret);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* USBMUX end-to-end smoke test                                        */
/* ------------------------------------------------------------------ */

static struct {
    uint8_t out_endpoint;
    uint8_t in_endpoint;
} s_mux_transport;

static rtcp_usbmux_host_t s_mux;
static uint8_t s_mux_reply[MUX_REPLY_MAX];

static int mux_transport_send(void *user, const uint8_t *data, size_t length)
{
    (void)user;
    int ret = rtcp_usb_lubanlite.ops->bulk_transfer(
        rtcp_usb_lubanlite.context, s_mux_transport.out_endpoint,
        (void *)data, length, 1000U);
    if (ret < 0) {
        return ret;
    }
    return (size_t)ret == length ? RTCP_OK : RTCP_EIO;
}

static int mux_transport_recv(void *user, uint8_t *data, size_t capacity,
                              uint32_t timeout_ms)
{
    (void)user;
    return rtcp_usb_lubanlite.ops->bulk_transfer(
        rtcp_usb_lubanlite.context, s_mux_transport.in_endpoint, data,
        capacity, timeout_ms);
}

static int cmd_mux(void)
{
    static rtcp_usb_device_t device;
    int ret = rtcp_usb_lubanlite.ops->get_device(
        rtcp_usb_lubanlite.context, &device);
    if (ret != RTCP_OK) {
        rt_kprintf("[carplay_usb] mux: not attached\n");
        return 0;
    }

    int mux_interface = rtcp_usb_find_interface(
        &device, RTCP_USB_CLASS_VENDOR, RTCP_USB_APPLE_SUBCLASS_USBMUX,
        RTCP_USB_APPLE_PROTOCOL_USBMUX);
    uint8_t out = 0U;
    uint8_t in = 0U;
    if (mux_interface < 0 ||
        !rtcp_usb_interface_bulk_endpoints(&device.interfaces[mux_interface],
                                           &out, &in) ||
        out == 0U || in == 0U) {
        rt_kprintf("[carplay_usb] mux: no USBMUX bulk pair\n");
        return 0;
    }
    if (!rtcp_usb_is_carplay_configuration(&device)) {
        rt_kprintf("[carplay_usb] mux: current configuration is not a "
                   "CarPlay one; replug the phone after 'carplay_usb "
                   "start'\n");
        return 0;
    }
    s_mux_transport.out_endpoint = out;
    s_mux_transport.in_endpoint = in;

    rtcp_usbmux_transport_t transport = {
        .send = mux_transport_send,
        .recv = mux_transport_recv,
        .user = NULL,
    };
    rtcp_usbmux_init(&s_mux, &transport);
    ret = rtcp_usbmux_start(&s_mux, 0U);
    rt_kprintf("[carplay_usb] mux: version/setup ret=%d\n", ret);
    if (ret < 0) {
        return 0;
    }

    rtcp_usbmux_connection_t *connection = NULL;
    ret = rtcp_usbmux_connect(&s_mux, RTCP_USBMUX_LOCKDOWN_PORT, 0U, 5000U,
                              &connection);
    rt_kprintf("[carplay_usb] mux: lockdown connect ret=%d\n", ret);
    if (ret < 0) {
        return 0;
    }

    uint8_t request[RTCP_LOCKDOWN_REQUEST_MAX];
    size_t request_length = rtcp_lockdown_build_get_value(
        request, sizeof(request), "rt-carplay", "ProductVersion");
    if (request_length == 0U) {
        rt_kprintf("[carplay_usb] mux: cannot build lockdown request\n");
        rtcp_usbmux_connection_close(&s_mux, connection);
        return 0;
    }
    ret = rtcp_usbmux_connection_send(&s_mux, connection, request,
                                      request_length);
    rt_kprintf("[carplay_usb] mux: GetValue sent ret=%d bytes=%u\n", ret,
               (unsigned)request_length);
    if (ret < 0) {
        rtcp_usbmux_connection_close(&s_mux, connection);
        return 0;
    }

    size_t reply_length = 0U;
    bool complete = false;
    for (unsigned elapsed = 0U; elapsed < 5000U; elapsed += 200U) {
        ret = rtcp_usbmux_pump(&s_mux, 200U, elapsed);
        if (ret < 0) {
            rt_kprintf("[carplay_usb] mux: pump ret=%d\n", ret);
            break;
        }
        int received;
        do {
            size_t space = sizeof(s_mux_reply) - 1U - reply_length;
            if (space == 0U) {
                break;
            }
            received = rtcp_usbmux_connection_recv(
                &s_mux, connection, s_mux_reply + reply_length, space);
            if (received > 0) {
                reply_length += (size_t)received;
            }
        } while (received > 0 && reply_length < sizeof(s_mux_reply) - 1U);
        s_mux_reply[reply_length] = '\0';
        if (reply_length != 0U &&
            strstr((const char *)s_mux_reply, "</plist>") != NULL) {
            complete = true;
            break;
        }
    }

    rt_kprintf("[carplay_usb] mux: reply bytes=%u complete=%u\n",
               (unsigned)reply_length, complete ? 1U : 0U);
    if (reply_length != 0U) {
        char version[32];
        if (rtcp_lockdown_extract_string(s_mux_reply, reply_length, "Value",
                                         version, sizeof(version)) == RTCP_OK) {
            rt_kprintf("[carplay_usb] mux: ProductVersion=%s\n", version);
        }
        rt_kprintf("%s\n", (const char *)s_mux_reply);
    }
    rtcp_usbmux_connection_close(&s_mux, connection);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Wired lockdown pairing and carkit service                           */
/* ------------------------------------------------------------------ */

#define LOCKDOWN_CONNECTIONS 2U

typedef struct {
    bool used;
    rtcp_usbmux_connection_t *connection;
} lockdown_connection_t;

static lockdown_connection_t s_lockdown_connections[LOCKDOWN_CONNECTIONS];
static rtcp_lockdown_session_t s_lockdown_session;
static rtcp_pair_record_t s_pair_record;

static uint32_t lockdown_now_ms(void)
{
    return (uint32_t)rt_tick_get_millisecond();
}

static int lockdown_connection_read(void *context, void *data, size_t length,
                                    uint32_t timeout_ms)
{
    lockdown_connection_t *slot = context;
    int received = rtcp_usbmux_connection_recv(&s_mux, slot->connection, data,
                                               length);

    if (received != 0) {
        return received;
    }
    int ret = rtcp_usbmux_pump(&s_mux, timeout_ms, lockdown_now_ms());
    if (ret < 0) {
        return ret;
    }
    received = rtcp_usbmux_connection_recv(&s_mux, slot->connection, data,
                                           length);
    if (received != 0) {
        return received;
    }
    return RTCP_ETIMEDOUT;
}

static int lockdown_connection_write(void *context, const void *data,
                                     size_t length)
{
    lockdown_connection_t *slot = context;

    return rtcp_usbmux_connection_send(&s_mux, slot->connection, data, length);
}

static const rtcp_stream_ops_t lockdown_connection_ops = {
    .read = lockdown_connection_read,
    .write = lockdown_connection_write,
};

static int lockdown_open_connection(void *user, uint16_t port,
                                    rtcp_stream_t *stream)
{
    (void)user;
    for (unsigned i = 0U; i < LOCKDOWN_CONNECTIONS; ++i) {
        lockdown_connection_t *slot = &s_lockdown_connections[i];
        if (slot->used) {
            continue;
        }
        rtcp_usbmux_connection_t *connection = NULL;
        int ret = rtcp_usbmux_connect(&s_mux, port, lockdown_now_ms(),
                                      5000U, &connection);
        if (ret != RTCP_OK) {
            return ret;
        }
        slot->connection = connection;
        slot->used = true;
        stream->ops = &lockdown_connection_ops;
        stream->context = slot;
        return RTCP_OK;
    }
    return RTCP_ENOMEM;
}

static void lockdown_close_connection(void *user, const rtcp_stream_t *stream)
{
    lockdown_connection_t *slot = stream->context;

    (void)user;
    if (slot->connection != NULL) {
        rtcp_usbmux_connection_close(&s_mux, slot->connection);
    }
    slot->connection = NULL;
    slot->used = false;
}

static void lockdown_sleep(void *user, uint32_t ms)
{
    (void)user;
    rt_thread_mdelay(ms);
}

static int lockdown_rng(void *context, uint8_t *output, size_t length)
{
    (void)context;
    return rtcp_lubanlite_random(output, length) ? 0 : -1;
}

static bool lockdown_session_active(void)
{
    return s_lockdown_session.control_tls != NULL ||
           s_lockdown_session.service.ops != NULL;
}

static int cmd_pair(void)
{
    static rtcp_usb_device_t device;
    int ret;

    if (lockdown_session_active()) {
        rt_kprintf("[carplay_usb] pair: a session is already open; run "
                   "'carplay_usb unpair' first\n");
        return 0;
    }
    ret = rtcp_usb_lubanlite.ops->get_device(rtcp_usb_lubanlite.context,
                                             &device);
    if (ret != RTCP_OK) {
        rt_kprintf("[carplay_usb] pair: not attached; run 'carplay_usb "
                   "start' before plugging the phone\n");
        return 0;
    }
    int mux_interface = rtcp_usb_find_interface(
        &device, RTCP_USB_CLASS_VENDOR, RTCP_USB_APPLE_SUBCLASS_USBMUX,
        RTCP_USB_APPLE_PROTOCOL_USBMUX);
    uint8_t out = 0U;
    uint8_t in = 0U;
    if (mux_interface < 0 ||
        !rtcp_usb_interface_bulk_endpoints(&device.interfaces[mux_interface],
                                           &out, &in) ||
        out == 0U || in == 0U ||
        !rtcp_usb_is_carplay_configuration(&device)) {
        rt_kprintf("[carplay_usb] pair: the current configuration is not a "
                   "CarPlay one; replug the phone after 'carplay_usb "
                   "start'\n");
        return 0;
    }
    s_mux_transport.out_endpoint = out;
    s_mux_transport.in_endpoint = in;

    rtcp_usbmux_transport_t transport = {
        .send = mux_transport_send,
        .recv = mux_transport_recv,
        .user = NULL,
    };
    rtcp_usbmux_init(&s_mux, &transport);
    ret = rtcp_usbmux_start(&s_mux, lockdown_now_ms());
    rt_kprintf("[carplay_usb] pair: mux version/setup ret=%d\n", ret);
    if (ret < 0) {
        return 0;
    }

    rtcp_lockdown_session_config_t config;
    memset(&config, 0, sizeof(config));
    config.open_connection = lockdown_open_connection;
    config.close_connection = lockdown_close_connection;
    config.sleep_ms = lockdown_sleep;
    config.rng = lockdown_rng;
    config.label = "rt-carplay";
    config.record = &s_pair_record;
    config.step_timeout_ms = 5000U;
    config.pair_retry_ms = 1000U;
    config.pair_attempts = 180U; /* three minutes for the trust dialog */
    memset(&s_lockdown_connections, 0, sizeof(s_lockdown_connections));
    memset(&s_pair_record, 0, sizeof(s_pair_record));
    memset(&s_lockdown_session, 0, sizeof(s_lockdown_session));

    rt_kprintf("[carplay_usb] pair: watch the phone and accept the trust "
               "dialog; generating keys takes a while\n");
    ret = rtcp_lockdown_session_open(&s_lockdown_session, &config);
    if (ret != RTCP_OK) {
        rt_kprintf("[carplay_usb] pair: failed ret=%d stage=%s error='%s'\n",
                   ret, rtcp_lockdown_stage_name(s_lockdown_session.stage),
                   s_lockdown_session.remote_error);
        return 0;
    }
    rt_kprintf("[carplay_usb] pair: host=%s system=%s wifi=%s\n",
               s_lockdown_session.config.host_id,
               s_lockdown_session.config.system_buid,
               s_pair_record.wifi_address);
    rt_kprintf("[carplay_usb] pair: carkit port=%u service_tls=%u "
               "session_tls=%u\n",
               s_lockdown_session.service_port,
               s_lockdown_session.service_ssl ? 1U : 0U,
               s_lockdown_session.control_tls != NULL ? 1U : 0U);
    rt_kprintf("[carplay_usb] pair: service stream open; run "
               "'carplay_usb unpair' to close it\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Wired iAP2 over the carkit service stream                           */
/* ------------------------------------------------------------------ */

#define IAP2_USB_RX_TIMEOUT_MS 200U
#define IAP2_USB_STACK_BYTES (12U * 1024U)
#define IAP2_USB_PRIORITY 20U
#define IAP2_USB_TICK 20U

typedef struct {
    iap2_session_t *session;
    iap2_stream_rx_t rx;
    rt_thread_t thread;
    volatile bool run;
    volatile bool running;
    volatile bool identified;
    volatile bool authenticated;
    unsigned events;
    bool airplay_unavailable_logged;
    char ip_address[64];
    const struct rtcp_mfi *mfi; /* installed AirPlay /auth-setup provider */
} iap2_usb_link_t;

static iap2_usb_link_t s_iap2_usb;

static uint32_t iap2_usb_now_ms(void)
{
    return (uint32_t)rt_tick_get_millisecond();
}

static bool iap2_usb_tx(const uint8_t *bytes, size_t length, void *context)
{
    iap2_usb_link_t *link = context;

    if (link->rx.stream.ops == NULL || link->rx.stream.ops->write == NULL) {
        return false;
    }
    return link->rx.stream.ops->write(link->rx.stream.context, bytes, length) ==
           RTCP_OK;
}

/*
 * The wired AirPlay endpoint is served by the receiver started with
 * 'carplay_start wired': its IPv6 listener is up, and the address advertised
 * here is the NCM netif's link-local.  The receiver owns the address query,
 * so the iAP2 session never talks to lwIP.  When the receiver is not running
 * (or DAD has not completed) the invite is withheld once rather than claiming
 * an unreachable endpoint; identification and MFi still complete.
 */
static bool iap2_usb_get_airplay(iap2_session_airplay_t *out, void *context)
{
    iap2_usb_link_t *link = context;
    carplay_app_wired_info_t info;

    if (carplay_app_wired_info(&info) != RTCP_OK) {
        if (!link->airplay_unavailable_logged) {
            link->airplay_unavailable_logged = true;
            rt_kprintf("[carplay_usb] iap2: no wired AirPlay endpoint yet; "
                       "run 'carplay_start wired' and check 'carplay_usb "
                       "net'. 0x4301 is not sent\n");
        }
        return false;
    }

    int written = rt_snprintf(link->ip_address, sizeof(link->ip_address),
                              "%s", info.ipv6);
    if (written <= 0 || (size_t)written >= sizeof(link->ip_address)) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->airplay_port = info.airplay_port;
    out->ip_address = link->ip_address;
    out->device_identifier = info.device_id;
    out->public_key = info.public_key_hex;
    out->source_version = info.source_version;
    /* Wired sessions ignore mdns_ready; keep the field meaningful anyway. */
    out->mdns_ready = true;
    out->event_connected = info.airplay_connected;
    rt_kprintf("[carplay_usb] iap2: wired AirPlay endpoint [%s]:%u\n",
               link->ip_address, (unsigned)info.airplay_port);
    return true;
}

static void iap2_usb_event(iap2_session_event_t event, void *context)
{
    iap2_usb_link_t *link = context;

    ++link->events;
    switch (event) {
        case IAP2_SESSION_EVENT_IDENTIFICATION_ACCEPTED:
            link->identified = true;
            rt_kprintf("[carplay_usb] iap2: identification accepted\n");
            break;
        case IAP2_SESSION_EVENT_IDENTIFICATION_REJECTED:
            rt_kprintf("[carplay_usb] iap2: identification rejected\n");
            break;
        case IAP2_SESSION_EVENT_MFI_CERTIFICATE_SENT:
            rt_kprintf("[carplay_usb] iap2: MFi certificate sent\n");
            break;
        case IAP2_SESSION_EVENT_MFI_AUTHENTICATED:
            link->authenticated = true;
            rt_kprintf("[carplay_usb] iap2: MFi authenticated\n");
            break;
        case IAP2_SESSION_EVENT_MFI_FAILED:
            rt_kprintf("[carplay_usb] iap2: MFi failed\n");
            break;
        case IAP2_SESSION_EVENT_SUBSCRIPTIONS_SENT:
            rt_kprintf("[carplay_usb] iap2: subscriptions sent\n");
            break;
        case IAP2_SESSION_EVENT_CARPLAY_INVITE_SENT:
            rt_kprintf("[carplay_usb] iap2: CarPlayStartSession sent\n");
            break;
        case IAP2_SESSION_EVENT_LINK_READY:
            rt_kprintf("[carplay_usb] iap2: link ready\n");
            break;
        case IAP2_SESSION_EVENT_LINK_DEAD:
            rt_kprintf("[carplay_usb] iap2: link failed\n");
            break;
        default:
            break;
    }
}

static iap2_identity_config_t iap2_usb_identity(void)
{
    iap2_identity_config_t identity;

    memset(&identity, 0, sizeof(identity));
    identity.name = "rt-carplay";
    identity.model_identifier = "D213";
    identity.manufacturer = "ArtInChip";
    identity.serial_number = "rt-carplay-wired";
    identity.firmware_version = "0.1";
    identity.hardware_version = "ArtInChip demo128";
    identity.language = "en";
    /* Wired identification carries no Wi-Fi transport component. */
    identity.wifi_ssid = NULL;
    return identity;
}

static const struct rtcp_mfi *iap2_usb_mfi(void)
{
#if defined(RTCARPLAY_MFI)
    int ret = rtcp_mfi_lubanlite_start(RTCARPLAY_MFI_I2C_BUS,
                                       RTCARPLAY_MFI_I2C_ADDR);
    if (ret == RTCP_OK) {
        return rtcp_mfi_lubanlite();
    }
    rt_kprintf("[carplay_usb] iap2: MFi coprocessor not ready rc=%d; the "
               "phone's authentication request will fail\n", ret);
#else
    rt_kprintf("[carplay_usb] iap2: MFi support not built in; only "
               "identification can complete\n");
#endif
    return NULL;
}

static void iap2_usb_thread(void *parameter)
{
    iap2_usb_link_t *link = parameter;

    iap2_session_link_up(link->session, iap2_usb_now_ms());
    rt_kprintf("[carplay_usb] iap2: link up\n");
    while (link->run) {
        int ret = iap2_stream_rx_step(&link->rx, link->session,
                                      IAP2_USB_RX_TIMEOUT_MS,
                                      iap2_usb_now_ms());
        if (ret == RTCP_OK || ret == RTCP_ETIMEDOUT) {
            continue;
        }
        if (ret == 0) {
            rt_kprintf("[carplay_usb] iap2: carkit stream closed by the "
                       "phone\n");
        } else {
            rt_kprintf("[carplay_usb] iap2: carkit stream error rc=%d\n", ret);
        }
        break;
    }
    iap2_session_link_down(link->session);
    iap2_session_destroy(link->session);
    link->session = NULL;
    carplay_mfi_provider_release(link->mfi);
    link->mfi = NULL;
    link->running = false;
    rt_kprintf("[carplay_usb] iap2: stopped (events=%u)\n", link->events);
}

static int cmd_iap2(void)
{
    if (!lockdown_session_active()) {
        rt_kprintf("[carplay_usb] iap2: no open carkit session; run "
                   "'carplay_usb pair' first\n");
        return 0;
    }
    if (s_iap2_usb.running) {
        rt_kprintf("[carplay_usb] iap2: already running\n");
        return 0;
    }

    rtcp_stream_t service = rtcp_lockdown_session_service(&s_lockdown_session);
    if (service.ops == NULL) {
        rt_kprintf("[carplay_usb] iap2: carkit service stream is not open\n");
        return 0;
    }

    rtcp_usb_device_t device;
    int ret = rtcp_usb_lubanlite.ops->get_device(rtcp_usb_lubanlite.context,
                                                 &device);
    if (ret != RTCP_OK) {
        rt_kprintf("[carplay_usb] iap2: device is no longer attached\n");
        return 0;
    }
    int interface_number = rtcp_usb_carplay_interface_number(&device);
    if (interface_number < 0) {
        rt_kprintf("[carplay_usb] iap2: the active configuration has no "
                   "NCM control or Apple Ethernet interface; replug the "
                   "phone\n");
        return 0;
    }

    memset(&s_iap2_usb, 0, sizeof(s_iap2_usb));
    iap2_stream_rx_init(&s_iap2_usb.rx, &service);

    iap2_identity_config_t identity = iap2_usb_identity();
    iap2_session_config_t config;
    memset(&config, 0, sizeof(config));
    config.identity = identity;
    s_iap2_usb.mfi = iap2_usb_mfi();
    config.mfi = s_iap2_usb.mfi;
    if (s_iap2_usb.mfi != NULL &&
        carplay_mfi_provider_install(s_iap2_usb.mfi) != RTCP_OK) {
        rt_kprintf("[carplay_usb] iap2: AirPlay /auth-setup MFi provider "
                   "unavailable; auth-setup would return 503\n");
    }
    config.tx = iap2_usb_tx;
    config.get_airplay = iap2_usb_get_airplay;
    config.on_event = iap2_usb_event;
    config.context = &s_iap2_usb;
    config.transport = IAP2_SESSION_TRANSPORT_WIRED_USB;
    config.car_play_interface_number = (uint8_t)interface_number;

    s_iap2_usb.session = iap2_session_create(&config);
    if (s_iap2_usb.session == NULL) {
        carplay_mfi_provider_release(s_iap2_usb.mfi);
        s_iap2_usb.mfi = NULL;
        rt_kprintf("[carplay_usb] iap2: cannot allocate the session\n");
        return 0;
    }

    s_iap2_usb.run = true;
    s_iap2_usb.running = true;
    s_iap2_usb.thread = rt_thread_create("carplay-iap2u",
                                         iap2_usb_thread, &s_iap2_usb,
                                         IAP2_USB_STACK_BYTES,
                                         IAP2_USB_PRIORITY, IAP2_USB_TICK);
    if (s_iap2_usb.thread == NULL) {
        iap2_session_destroy(s_iap2_usb.session);
        s_iap2_usb.session = NULL;
        carplay_mfi_provider_release(s_iap2_usb.mfi);
        s_iap2_usb.mfi = NULL;
        s_iap2_usb.running = false;
        rt_kprintf("[carplay_usb] iap2: cannot create the link thread\n");
        return 0;
    }
    rt_thread_startup(s_iap2_usb.thread);
    rt_kprintf("[carplay_usb] iap2: running over the %s carkit stream, "
               "usb interface=%d\n",
               s_lockdown_session.service_ssl ? "TLS" : "raw",
               interface_number);
    return 0;
}

static void iap2_usb_request_stop(void)
{
    if (s_iap2_usb.running) {
        s_iap2_usb.run = false;
        rt_kprintf("[carplay_usb] iap2: stop requested\n");
    }
}

/* Returns true only when the iAP2 thread has finished and is no longer
 * touching the carkit stream.  Callers that free the stream must check it:
 * closing underneath a live thread would free its TLS/mux state while the
 * thread can still read from it. */
static bool cmd_iap2_stop(void)
{
    if (!s_iap2_usb.running) {
        rt_kprintf("[carplay_usb] iap2: not running\n");
        return true;
    }
    s_iap2_usb.run = false;
    for (unsigned waited = 0U; s_iap2_usb.running && waited < 3000U;
         waited += 10U) {
        rt_thread_mdelay(10);
    }
    if (s_iap2_usb.running) {
        rt_kprintf("[carplay_usb] iap2: stop timed out; the thread still "
                   "owns the carkit stream\n");
        return false;
    }
    rt_kprintf("[carplay_usb] iap2: stopped\n");
    return true;
}

static int cmd_unpair(void)
{
    /* Never close the lockdown session while the iAP2 thread can still use
     * its stream.  A timed-out stop leaves the thread alive; the user can
     * retry after it notices the link is gone. */
    if (!cmd_iap2_stop()) {
        rt_kprintf("[carplay_usb] unpair: refused while the iAP2 thread is "
                   "alive; retry 'carplay_usb iap2stop'\n");
        return 0;
    }
    rtcp_lockdown_session_close(&s_lockdown_session);
    memset(&s_lockdown_session, 0, sizeof(s_lockdown_session));
    rt_kprintf("[carplay_usb] unpair: session closed\n");
    return 0;
}

static int cmd_net(void)
{
    static rtcp_usb_device_t device;
    char address[64];
    bool valid = false;
    bool up = rtcp_lubanlite_wired_net_up();
    bool found = rtcp_lubanlite_wired_ipv6(address, sizeof(address), &valid);
    bool apple_ethernet = false;
    bool ncm_link = false;
    const char *route = "none";

    if (rtcp_usb_lubanlite.ops->get_device(rtcp_usb_lubanlite.context,
                                           &device) == RTCP_OK) {
        apple_ethernet = rtcp_usb_find_interface(
                             &device, RTCP_USB_CLASS_VENDOR,
                             RTCP_USB_APPLE_SUBCLASS_ETH,
                             RTCP_USB_APPLE_PROTOCOL_ETH) >= 0;
        ncm_link = rtcp_usb_configuration_has_ncm_link(&device);
        switch (rtcp_usb_configuration_kind(&device)) {
            case RTCP_USB_CONFIG_NCM:
                route = "ncm";
                break;
            case RTCP_USB_CONFIG_APPLE_ETHERNET:
                route = "apple-ethernet";
                break;
            default:
                route = "none";
                break;
        }
    }

    rt_kprintf("[carplay_usb] net: netif=%s route=%s ncm_link=%s "
               "apple_ethernet=%s\n",
               up ? "up" : "down", route, ncm_link ? "yes" : "no",
               apple_ethernet ? "present" : "absent");
    if (found) {
        rt_kprintf("[carplay_usb] net: ipv6=%s (%s)\n", address,
                   valid ? "valid" : "tentative");
    } else {
        rt_kprintf("[carplay_usb] net: ipv6=unavailable\n");
        if (apple_ethernet && !ncm_link && !up) {
            rt_kprintf("[carplay_usb] net: the selected configuration has "
                       "Apple Ethernet (ff/fd/01) and no standard CDC-NCM; "
                       "the existing NCM netif cannot come up until that "
                       "function is driven\n");
        } else if (ncm_link && !up) {
            rt_kprintf("[carplay_usb] net: the selected configuration has "
                       "CDC-NCM but the NCM netif is down; check "
                       "LPKG_CHERRYUSB_HOST_NCM and the enumeration log\n");
        }
    }
    return 0;
}

static int cmd_carplay_usb(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "status";

    if (strcmp(what, "start") == 0) {
        int ret = rtcp_usb_lubanlite.ops->open(
            rtcp_usb_lubanlite.context, usb_event, NULL);
        rt_kprintf("[carplay_usb] start ret=%d; plug the iPhone now\n", ret);
        return 0;
    }
    if (strcmp(what, "stop") == 0) {
        rtcp_usb_lubanlite.ops->close(rtcp_usb_lubanlite.context);
        rt_kprintf("[carplay_usb] stopped; existing device is left "
                   "configured\n");
        return 0;
    }
    if (strcmp(what, "status") == 0) {
        return cmd_status();
    }
    if (strcmp(what, "request") == 0) {
        return cmd_request();
    }
    if (strcmp(what, "mux") == 0) {
        if (lockdown_session_active()) {
            rt_kprintf("[carplay_usb] mux: a pairing session is open; run "
                       "'carplay_usb unpair' first\n");
            return 0;
        }
        return cmd_mux();
    }
    if (strcmp(what, "pair") == 0) {
        return cmd_pair();
    }
    if (strcmp(what, "unpair") == 0) {
        return cmd_unpair();
    }
    if (strcmp(what, "iap2") == 0) {
        return cmd_iap2();
    }
    if (strcmp(what, "iap2stop") == 0) {
        return cmd_iap2_stop() ? 0 : 1;
    }
    if (strcmp(what, "net") == 0) {
        return cmd_net();
    }
    if (strcmp(what, "desc") == 0) {
        bool request = argc > 2 && strcmp(argv[2], "request") == 0;
        rtcp_usb_lubanlite_dump_configurations(request);
        return 0;
    }
    rt_kprintf("usage: carplay_usb "
               "[start|stop|status|request|mux|pair|unpair|iap2|iap2stop|net|"
               "desc [request]]\n");
    return 0;
}

MSH_CMD_EXPORT_ALIAS(cmd_carplay_usb, carplay_usb,
                     Wired CarPlay USB host bring-up (Luban-Lite CherryUSB));

#endif /* LPKG_RT_CARPLAY_LUBANLITE_USB */
