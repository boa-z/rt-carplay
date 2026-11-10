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
 *   carplay_usb stop      disarm the hook
 */
#include <rtthread.h>

#include <string.h>

#include "lockdown.h"
#include "rtcp_lubanlite.h"
#include "rtcp/rtcp.h"
#include "rtcp/usb.h"
#include "usbmux_host.h"

#ifdef LPKG_RT_CARPLAY_LUBANLITE_USB

#define USB_CMD_TAG "carplay_usb"

#define APPLE_REQUEST_CARPLAY_CONFIGS 0x52U
#define APPLE_CARPLAY_CONFIGS_WINDEX 0x0004U

#define MUX_REPLY_MAX 2048U

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
        return cmd_mux();
    }
    rt_kprintf(
        "usage: carplay_usb [start|stop|status|request|mux]\n");
    return 0;
}

MSH_CMD_EXPORT_ALIAS(cmd_carplay_usb, carplay_usb,
                     Wired CarPlay USB host bring-up (Luban-Lite CherryUSB));

#endif /* LPKG_RT_CARPLAY_LUBANLITE_USB */
