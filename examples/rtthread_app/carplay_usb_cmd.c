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
 *   carplay_usb stop      disarm the hook
 */
#include <rtthread.h>

#include <string.h>

#include "rtcp_lubanlite.h"
#include "rtcp/rtcp.h"
#include "rtcp/usb.h"

#ifdef LPKG_RT_CARPLAY_LUBANLITE_USB

#define USB_CMD_TAG "carplay_usb"

#define APPLE_REQUEST_CARPLAY_CONFIGS 0x52U
#define APPLE_CARPLAY_CONFIGS_WINDEX 0x0004U

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
    rt_kprintf("usage: carplay_usb [start|stop|status|request]\n");
    return 0;
}

MSH_CMD_EXPORT_ALIAS(cmd_carplay_usb, carplay_usb,
                     Wired CarPlay USB host bring-up (Luban-Lite CherryUSB));

#endif /* LPKG_RT_CARPLAY_LUBANLITE_USB */
