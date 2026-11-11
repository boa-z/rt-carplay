/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Wired CarPlay USB host seam on top of the Luban-Lite CherryUSB host stack.
 *
 * The D213 has two USB controllers.  USB1 is host-only and is the port the
 * phone is plugged into; USB0 is also configured as a host in this build.
 * CherryUSB enumerates a device by reading configuration index 0 and binding
 * a class driver to each interface.  An iPhone exposes its CarPlay functions
 * (USBMUX bulk pair + CDC-NCM/Apple Ethernet) only after the Apple vendor
 * request 0x52, in a configuration that is not index 0 and whose number
 * differs between models.  Therefore this file does two things:
 *
 *   1. It implements the weak usbh_get_hport_active_config_index() hook.  During
 *      enumeration of an Apple device it scans the reported configuration
 *      descriptors, sends the 0x52 request when none of them is a CarPlay
 *      configuration, and returns the descriptor index that carries the
 *      USBMUX interface.  CherryUSB then selects that configuration by its
 *      bConfigurationValue and binds drivers to the real interfaces.
 *
 *   2. It registers a class driver for the Apple USBMUX interface
 *      (0xff/0xfe/0x02).  connect() publishes the descriptor-derived device
 *      model to the portable core; the protocol layer above only sees
 *      rtcp_usb_* calls.
 *
 * Nothing assumes a product id, a configuration number or an endpoint
 * address: all of them come from the descriptors the phone reports.
 *
 * The hook runs in the CherryUSB hub thread and may issue control transfers.
 * The vendor request and the polling loop can delay enumeration of other
 * devices on the same controller by up to the activation timeout (5 s).
 */
#include "rtcp_lubanlite.h"

#if defined(__has_include)
#if __has_include(<rtconfig.h>)
#include <rtconfig.h>
#endif
#endif

#ifdef LPKG_RT_CARPLAY_LUBANLITE_USB

#include <string.h>

#include <rtthread.h>

#include "usbh_core.h"

#include "rtcp/rtcp.h"

#define APPLE_USB_VENDOR_ID 0x05acU
/* Apple's "publish CarPlay USB configurations" request (LIVI iap2-usbmux). */
#define APPLE_REQUEST_CARPLAY_CONFIGS 0x52U
#define APPLE_CARPLAY_CONFIGS_WINDEX 0x0004U

#define CARPLAY_CONFIG_SCAN_MAX 8U
#define CARPLAY_CONFIG_ACTIVATE_TIMEOUT_MS 5000U
#define CARPLAY_CONFIG_ACTIVATE_POLL_MS 100U
#define CARPLAY_CONFIG_DESC_MAX 1024U
#define CARPLAY_READ_FAILURE_LIMIT 3U

#define MUX_CLASS RTCP_USB_CLASS_VENDOR
#define MUX_SUBCLASS RTCP_USB_APPLE_SUBCLASS_USBMUX

typedef struct {
    volatile bool enabled;
    volatile bool present;
    /* Seqlock: odd while the hub thread updates the snapshot. */
    volatile uint32_t sequence;
    struct usbh_hubport *hport;
    rtcp_usb_device_t device;

    uint8_t mux_out;
    uint8_t mux_in;
    uint16_t mux_out_packet_size;
    uint16_t mux_in_packet_size;
    struct usb_endpoint_descriptor mux_out_desc;
    struct usb_endpoint_descriptor mux_in_desc;

    /* Configuration descriptor index returned by the enumeration hook. */
    uint8_t last_configuration_index;

    rtcp_usb_event_cb_t callback;
    void *user;

    uint32_t attach_count;
    uint32_t detach_count;
} carplay_usb_state_t;

static carplay_usb_state_t s_usb;

/* One configuration descriptor buffer per host controller: the hook is
 * called from each controller's own hub thread. */
static uint8_t
    s_config_desc[CONFIG_USBHOST_MAX_BUS][CARPLAY_CONFIG_DESC_MAX];

static void snapshot_write_begin(void)
{
    ++s_usb.sequence;
    __sync_synchronize();
}

static void snapshot_write_end(void)
{
    __sync_synchronize();
    ++s_usb.sequence;
}

static bool snapshot_read(rtcp_usb_device_t *device)
{
    for (unsigned attempt = 0U; attempt < 8U; ++attempt) {
        uint32_t sequence = s_usb.sequence;
        if ((sequence & 1U) != 0U) {
            continue;
        }
        memcpy(device, &s_usb.device, sizeof(*device));
        __sync_synchronize();
        if (s_usb.sequence == sequence) {
            device->present = s_usb.present;
            return device->present;
        }
    }
    memset(device, 0, sizeof(*device));
    return false;
}

static uint8_t map_speed(uint8_t cherryusb_speed)
{
    switch (cherryusb_speed) {
        case USB_SPEED_LOW:
            return RTCP_USB_SPEED_LOW;
        case USB_SPEED_FULL:
            return RTCP_USB_SPEED_FULL;
        case USB_SPEED_HIGH:
            return RTCP_USB_SPEED_HIGH;
        case USB_SPEED_SUPER:
            return RTCP_USB_SPEED_SUPER;
        default:
            return RTCP_USB_SPEED_UNKNOWN;
    }
}

/* CherryUSB's blocking submit uses the URB timeout, and the EHCI driver
 * prints an error for every expired URB.  The wired link polls the bulk IN
 * endpoint while it is idle, so use a completion semaphore instead: the URB
 * is submitted non-blocking, the caller waits on the semaphore with its own
 * timeout and cancels the URB quietly when it expires. */
static usb_osal_sem_t s_control_sem;
static usb_osal_sem_t s_bulk_sem;

static void transfer_complete(void *arg, int nbytes)
{
    (void)nbytes;
    usb_osal_sem_give((usb_osal_sem_t)arg);
}

static int map_urb_error(int error)
{
    if (error == -USB_ERR_TIMEOUT) {
        return RTCP_ETIMEDOUT;
    }
    if (error == -USB_ERR_NOTCONN || error == -USB_ERR_SHUTDOWN) {
        return RTCP_ESTATE;
    }
    return RTCP_EIO;
}

static int wait_urb(usb_osal_sem_t *semaphore, struct usbh_urb *urb,
                    uint32_t timeout_ms)
{
    if (*semaphore == NULL) {
        /* The URB already references the semaphore; never reach here. */
        return RTCP_ESTATE;
    }
    usb_osal_sem_reset(*semaphore);
    int ret = usbh_submit_urb(urb);
    if (ret < 0) {
        return map_urb_error(ret);
    }
    ret = usb_osal_sem_take(*semaphore, timeout_ms);
    if (ret < 0) {
        usbh_kill_urb(urb);
        return RTCP_ETIMEDOUT;
    }
    if (urb->errorcode < 0) {
        return map_urb_error(urb->errorcode);
    }
    return (int)urb->actual_length;
}

static int control_transfer_timeout(struct usbh_hubport *hport,
                                    uint8_t request_type, uint8_t request,
                                    uint16_t value, uint16_t index, void *data,
                                    uint16_t length, uint32_t timeout_ms)
{
    struct usb_setup_packet setup;
    struct usbh_urb urb;

    if (hport == NULL) {
        return RTCP_ESTATE;
    }
    setup.bmRequestType = request_type;
    setup.bRequest = request;
    setup.wValue = value;
    setup.wIndex = index;
    setup.wLength = length;

    memset(&urb, 0, sizeof(urb));
    if (s_control_sem == NULL) {
        s_control_sem = usb_osal_sem_create(0U);
        if (s_control_sem == NULL) {
            return RTCP_ENOMEM;
        }
    }
    usbh_control_urb_fill(&urb, hport, &setup, data, length, 0U,
                          transfer_complete, s_control_sem);
    usb_osal_mutex_take(hport->mutex);
    int ret = wait_urb(&s_control_sem, &urb,
                       timeout_ms != 0U
                           ? timeout_ms
                           : CONFIG_USBHOST_CONTROL_TRANSFER_TIMEOUT);
    usb_osal_mutex_give(hport->mutex);
    /* The upstream HCD includes the SETUP stage in URB actual_length;
     * match usbh_control_transfer(), whose public result is data bytes. */
    if (ret >= 0) {
        if (ret < (int)sizeof(setup)) return RTCP_EIO;
        ret -= (int)sizeof(setup);
    }
    return ret;
}

static bool read_config_descriptor(struct usbh_hubport *hport, uint8_t index,
                                   uint8_t *buffer, size_t capacity,
                                   uint16_t *total_length)
{
    int ret = control_transfer_timeout(
        hport,
        USB_REQUEST_DIR_IN | USB_REQUEST_STANDARD |
            USB_REQUEST_RECIPIENT_DEVICE,
        USB_REQUEST_GET_DESCRIPTOR,
        (uint16_t)((USB_DESCRIPTOR_TYPE_CONFIGURATION << 8) | index), 0U,
        buffer, 9U, CONFIG_USBHOST_CONTROL_TRANSFER_TIMEOUT);
    if (ret < 9) {
        return false;
    }
    if (buffer[0] < 9U || buffer[1] != USB_DESCRIPTOR_TYPE_CONFIGURATION) {
        return false;
    }
    uint16_t total = (uint16_t)((uint16_t)buffer[2] |
                                ((uint16_t)buffer[3] << 8U));
    if (total < 9U || total > capacity) {
        rt_kprintf("[usb-desc] config %u: wTotalLength=%u exceeds the "
                   "%u-byte diagnostic buffer\n",
                   index, total, (unsigned)capacity);
        return false;
    }
    ret = control_transfer_timeout(
        hport,
        USB_REQUEST_DIR_IN | USB_REQUEST_STANDARD |
            USB_REQUEST_RECIPIENT_DEVICE,
        USB_REQUEST_GET_DESCRIPTOR,
        (uint16_t)((USB_DESCRIPTOR_TYPE_CONFIGURATION << 8) | index), 0U,
        buffer, total, CONFIG_USBHOST_CONTROL_TRANSFER_TIMEOUT);
    if (ret < (int)total) {
        return false;
    }
    *total_length = total;
    return true;
}

static void descriptor_emit(void *user, const char *line)
{
    (void)user;
    rt_kprintf("%s\n", line);
}

/* Scan the configuration descriptors the device currently reports, print a
 * full dump of each, and return the best wired-CarPlay configuration.  A
 * configuration carrying the standard CDC-NCM pair is preferred because the
 * existing CherryUSB usbh_cdc_ncm driver can bind to it; Apple Ethernet
 * (ff/fd/01) is the fallback and needs its own host driver. */
static bool find_carplay_configuration(struct usbh_hubport *hport,
                                       uint8_t configuration_count,
                                       uint8_t *selected_index, bool dump)
{
    uint8_t bus = hport->bus->busid;
    if (bus >= CONFIG_USBHOST_MAX_BUS) {
        return false;
    }
    uint8_t *buffer = s_config_desc[bus];
    uint8_t best_index = 0U;
    rtcp_usb_configuration_kind_t best_kind = RTCP_USB_CONFIG_OTHER;

    for (uint8_t index = 0U;
         index < configuration_count && index < CARPLAY_CONFIG_SCAN_MAX;
         ++index) {
        uint16_t total = 0U;
        if (!read_config_descriptor(hport, index, buffer, CARPLAY_CONFIG_DESC_MAX,
                                    &total)) {
            if (dump) {
                rt_kprintf("[usb-desc] config %u: read failed\n", index);
            }
            continue;
        }
        rtcp_usb_device_t probe;
        memset(&probe, 0, sizeof(probe));
        int parsed = rtcp_usb_parse_configuration(&probe, buffer, total);
        if (parsed != RTCP_OK) {
            if (dump) {
                rt_kprintf("[usb-desc] config %u: parse failed (%d)\n", index,
                           parsed);
            }
            continue;
        }
        if (dump) {
            rtcp_usb_describe_configuration(&probe, buffer, total, index,
                                            descriptor_emit, NULL);
        }
        rtcp_usb_configuration_kind_t kind =
            rtcp_usb_configuration_kind(&probe);
        if (kind > best_kind) {
            best_kind = kind;
            best_index = index;
        }
    }

    if (best_kind != RTCP_USB_CONFIG_OTHER) {
        *selected_index = best_index;
        return true;
    }
    return false;
}

static uint8_t read_configuration_count(struct usbh_hubport *hport)
{
    uint8_t descriptor[USB_SIZEOF_DEVICE_DESC];
    int ret = control_transfer_timeout(
        hport,
        USB_REQUEST_DIR_IN | USB_REQUEST_STANDARD |
            USB_REQUEST_RECIPIENT_DEVICE,
        USB_REQUEST_GET_DESCRIPTOR,
        (uint16_t)(USB_DESCRIPTOR_TYPE_DEVICE << 8U), 0U, descriptor,
        sizeof(descriptor), CONFIG_USBHOST_CONTROL_TRANSFER_TIMEOUT);
    if (ret < (int)sizeof(descriptor) ||
        descriptor[0] != USB_SIZEOF_DEVICE_DESC ||
        descriptor[1] != USB_DESCRIPTOR_TYPE_DEVICE) {
        return 0U;
    }
    return descriptor[17];
}

static void request_carplay_configurations(struct usbh_hubport *hport)
{
    uint8_t response = 0U;
    int ret = control_transfer_timeout(
        hport,
        USB_REQUEST_DIR_IN | USB_REQUEST_VENDOR | USB_REQUEST_RECIPIENT_DEVICE,
        APPLE_REQUEST_CARPLAY_CONFIGS, 0U, APPLE_CARPLAY_CONFIGS_WINDEX,
        &response, 1U, 1000U);
    if (ret < 0) {
        USB_LOG_WRN("CarPlay config request failed:%d\r\n", ret);
    }
}

/*
 * Weak hook from core/usbh_core.c.  Returning the CarPlay descriptor index
 * makes CherryUSB read that descriptor, send SET_CONFIGURATION with its
 * bConfigurationValue and bind class drivers to its interfaces.
 */
uint8_t usbh_get_hport_active_config_index(struct usbh_hubport *hport)
{
    if (!s_usb.enabled || hport == NULL ||
        hport->device_desc.idVendor != APPLE_USB_VENDOR_ID) {
        return 0U;
    }

    uint8_t selected = 0U;
    uint8_t count = hport->device_desc.bNumConfigurations;
    rt_kprintf("[usb-desc] Apple device %04x:%04x reports %u "
               "configuration(s); dumping all before selection\n",
               hport->device_desc.idVendor, hport->device_desc.idProduct,
               count);
    if (find_carplay_configuration(hport, count, &selected, true)) {
        USB_LOG_INFO("CarPlay configuration already present: index %u\r\n",
                     selected);
        s_usb.last_configuration_index = selected;
        return selected;
    }

    USB_LOG_INFO("Apple device reports %u configurations, requesting CarPlay\r\n",
                 count);
    request_carplay_configurations(hport);

    unsigned failures = 0U;
    for (uint32_t elapsed = 0U;
         elapsed < CARPLAY_CONFIG_ACTIVATE_TIMEOUT_MS;
         elapsed += CARPLAY_CONFIG_ACTIVATE_POLL_MS) {
        usb_osal_msleep(CARPLAY_CONFIG_ACTIVATE_POLL_MS);
        uint8_t refreshed = read_configuration_count(hport);
        if (refreshed == 0U) {
            if (++failures >= CARPLAY_READ_FAILURE_LIMIT) {
                USB_LOG_WRN("Device stopped answering while waiting for "
                            "CarPlay configurations\r\n");
                break;
            }
            continue;
        }
        failures = 0U;
        count = refreshed;
        if (find_carplay_configuration(hport, count, &selected, false)) {
            /* Keep the enumeration record consistent with what the phone now
             * reports; the log and later users see the updated count. */
            hport->device_desc.bNumConfigurations = count;
            USB_LOG_INFO("CarPlay configuration index %u selected\r\n",
                         selected);
            s_usb.last_configuration_index = selected;
            return selected;
        }
    }

    USB_LOG_WRN("CarPlay configuration did not appear; using config 0\r\n");
    s_usb.last_configuration_index = 0U;
    return 0U;
}

/* 'carplay_usb desc': dump every configuration descriptor the attached
 * iPhone reports, including the classification that decides whether the
 * standard CDC-NCM driver can be reused.  Passing request_carplay sends
 * Apple's vendor request 0x52 first, which may make more configurations
 * visible; the phone can re-enumerate, so treat the dump as a snapshot. */
void rtcp_usb_lubanlite_dump_configurations(bool request_carplay)
{
    struct usbh_hubport *hport = s_usb.hport;
    if (!s_usb.present || hport == NULL) {
        rt_kprintf("[carplay_usb] desc: no iPhone attached; run "
                   "'carplay_usb start' and plug it in\n");
        return;
    }

    uint8_t count = read_configuration_count(hport);
    if (count == 0U) {
        rt_kprintf("[carplay_usb] desc: cannot read the device descriptor\n");
        return;
    }
    rt_kprintf("[carplay_usb] desc: %u configuration(s), selected index %u\n",
               count, s_usb.last_configuration_index);
    if (request_carplay) {
        rt_kprintf("[carplay_usb] desc: sending Apple request 0x52\n");
        request_carplay_configurations(hport);
        usb_osal_msleep(300);
        uint8_t refreshed = read_configuration_count(hport);
        if (refreshed != 0U && refreshed != count) {
            count = refreshed;
            rt_kprintf("[carplay_usb] desc: the device now reports %u "
                       "configuration(s)\n",
                       count);
        }
    }

    uint8_t bus = hport->bus->busid;
    if (bus >= CONFIG_USBHOST_MAX_BUS) {
        return;
    }
    uint8_t *buffer = s_config_desc[bus];
    for (uint8_t index = 0U;
         index < count && index < CARPLAY_CONFIG_SCAN_MAX; ++index) {
        uint16_t total = 0U;
        if (!read_config_descriptor(hport, index, buffer, CARPLAY_CONFIG_DESC_MAX,
                                    &total)) {
            rt_kprintf("[usb-desc] config %u: read failed\n", index);
            continue;
        }
        rtcp_usb_device_t probe;
        memset(&probe, 0, sizeof(probe));
        int parsed = rtcp_usb_parse_configuration(&probe, buffer, total);
        if (parsed != RTCP_OK) {
            rt_kprintf("[usb-desc] config %u: parse failed (%d)\n", index,
                       parsed);
            continue;
        }
        rtcp_usb_describe_configuration(&probe, buffer, total, index,
                                        descriptor_emit, NULL);
    }
    rt_kprintf("[carplay_usb] desc: route=ncm means the existing CherryUSB "
               "CDC-NCM driver can be reused; route=apple-ethernet means the "
               "ff/fd/01 function needs its own host driver\n");
}

static bool endpoint_descriptor_from_info(const rtcp_usb_endpoint_t *info,
                                          struct usb_endpoint_descriptor *desc)
{
    if (info->address == 0U) {
        return false;
    }
    memset(desc, 0, sizeof(*desc));
    desc->bLength = USB_SIZEOF_ENDPOINT_DESC;
    desc->bDescriptorType = USB_DESCRIPTOR_TYPE_ENDPOINT;
    desc->bEndpointAddress = info->address;
    desc->bmAttributes = info->transfer_type;
    desc->wMaxPacketSize = info->max_packet_size;
    desc->bInterval = info->interval;
    return true;
}

static int carplay_usbmux_connect(struct usbh_hubport *hport, uint8_t intf)
{
    (void)intf;
    if (!s_usb.enabled || hport == NULL ||
        hport->device_desc.idVendor != APPLE_USB_VENDOR_ID ||
        hport->raw_config_desc == NULL || hport->config.config_desc.wTotalLength == 0U) {
        return -1;
    }

    rtcp_usb_device_t device;
    memset(&device, 0, sizeof(device));
    device.vendor_id = hport->device_desc.idVendor;
    device.product_id = hport->device_desc.idProduct;
    device.bcd_device = hport->device_desc.bcdDevice;
    device.device_class = hport->device_desc.bDeviceClass;
    device.device_subclass = hport->device_desc.bDeviceSubClass;
    device.device_protocol = hport->device_desc.bDeviceProtocol;
    device.configuration_count = hport->device_desc.bNumConfigurations;
    device.speed = map_speed(hport->speed);
    device.bus_id = hport->bus->busid;
    device.address = hport->dev_addr;
    device.configuration_index = s_usb.last_configuration_index;
    if (rtcp_usb_parse_configuration(&device, hport->raw_config_desc,
                                     hport->config.config_desc.wTotalLength) != RTCP_OK) {
        USB_LOG_ERR("CarPlay: cannot parse the selected configuration\r\n");
        return -1;
    }

    int mux = rtcp_usb_find_interface(&device, MUX_CLASS, MUX_SUBCLASS,
                                      RTCP_USB_APPLE_PROTOCOL_USBMUX);
    if (mux < 0) {
        return -1;
    }
    uint8_t out = 0U;
    uint8_t in = 0U;
    if (!rtcp_usb_interface_bulk_endpoints(&device.interfaces[mux], &out, &in) ||
        out == 0U || in == 0U) {
        USB_LOG_ERR("CarPlay: USBMUX interface has no bulk pair\r\n");
        return -1;
    }

    uint16_t out_packet = 0U;
    uint16_t in_packet = 0U;
    const rtcp_usb_interface_t *interface = &device.interfaces[mux];
    for (uint8_t alt = 0U; alt < interface->altsetting_count; ++alt) {
        for (uint8_t e = 0U; e < interface->altsettings[alt].endpoint_count; ++e) {
            const rtcp_usb_endpoint_t *endpoint =
                &interface->altsettings[alt].endpoints[e];
            if (endpoint->address == out) out_packet = endpoint->max_packet_size;
            if (endpoint->address == in) in_packet = endpoint->max_packet_size;
        }
    }

    struct usb_endpoint_descriptor out_desc;
    struct usb_endpoint_descriptor in_desc;
    if (!endpoint_descriptor_from_info(
            &(rtcp_usb_endpoint_t){.address = out,
                                   .transfer_type = RTCP_USB_TRANSFER_BULK,
                                   .max_packet_size = out_packet},
            &out_desc) ||
        !endpoint_descriptor_from_info(
            &(rtcp_usb_endpoint_t){.address = in,
                                   .transfer_type = RTCP_USB_TRANSFER_BULK,
                                   .max_packet_size = in_packet},
            &in_desc)) {
        return -1;
    }

    snapshot_write_begin();
    s_usb.device = device;
    s_usb.device.present = true;
    s_usb.hport = hport;
    s_usb.mux_out = out;
    s_usb.mux_in = in;
    s_usb.mux_out_packet_size = out_packet;
    s_usb.mux_in_packet_size = in_packet;
    s_usb.mux_out_desc = out_desc;
    s_usb.mux_in_desc = in_desc;
    s_usb.present = true;
    snapshot_write_end();

    ++s_usb.attach_count;
    USB_LOG_INFO("CarPlay USB attached: config %u, USBMUX %02x/%02x\r\n",
                 device.configuration_value, out, in);

    rtcp_usb_event_cb_t callback = s_usb.callback;
    if (callback != NULL) {
        callback((rtcp_usb_t *)&rtcp_usb_lubanlite, RTCP_USB_EVENT_ATTACHED,
                             s_usb.user);
    }
    return 0;
}

static int carplay_usbmux_disconnect(struct usbh_hubport *hport, uint8_t intf)
{
    (void)intf;
    if (s_usb.hport != hport) {
        return 0;
    }

    snapshot_write_begin();
    memset(&s_usb.device, 0, sizeof(s_usb.device));
    s_usb.present = false;
    s_usb.hport = NULL;
    s_usb.mux_out = 0U;
    s_usb.mux_in = 0U;
    s_usb.mux_out_packet_size = 0U;
    s_usb.mux_in_packet_size = 0U;
    snapshot_write_end();

    ++s_usb.detach_count;
    USB_LOG_INFO("CarPlay USB detached\r\n");

    rtcp_usb_event_cb_t callback = s_usb.callback;
    if (callback != NULL) {
        callback((rtcp_usb_t *)&rtcp_usb_lubanlite, RTCP_USB_EVENT_DETACHED,
                             s_usb.user);
    }
    return 0;
}

static const struct usbh_class_driver carplay_usbmux_class_driver = {
    .driver_name = "carplay_usbmux",
    .connect = carplay_usbmux_connect,
    .disconnect = carplay_usbmux_disconnect,
};

CLASS_INFO_DEFINE const struct usbh_class_info carplay_usbmux_class_info = {
    .match_flags = USB_CLASS_MATCH_INTF_CLASS |
                   USB_CLASS_MATCH_INTF_SUBCLASS |
                   USB_CLASS_MATCH_INTF_PROTOCOL,
    .bInterfaceClass = MUX_CLASS,
    .bInterfaceSubClass = MUX_SUBCLASS,
    .bInterfaceProtocol = RTCP_USB_APPLE_PROTOCOL_USBMUX,
    .id_table = NULL,
    .class_driver = &carplay_usbmux_class_driver,
};

/* ------------------------------------------------------------------ */
/* struct rtcp_usb operations                                          */
/* ------------------------------------------------------------------ */

static int lubanlite_open(void *context, rtcp_usb_event_cb_t callback,
                          void *user)
{
    (void)context;
    s_usb.callback = callback;
    s_usb.user = user;
    s_usb.enabled = true;
    return RTCP_OK;
}

static void lubanlite_close(void *context)
{
    (void)context;
    s_usb.enabled = false;
    s_usb.callback = NULL;
    s_usb.user = NULL;
}

static int lubanlite_get_device(void *context, rtcp_usb_device_t *device)
{
    (void)context;
    if (device == NULL) {
        return RTCP_EINVAL;
    }
    return snapshot_read(device) ? RTCP_OK : RTCP_ESTATE;
}

static int lubanlite_control_transfer(void *context, uint8_t request_type,
                                      uint8_t request, uint16_t value,
                                      uint16_t index, void *data,
                                      uint16_t length, uint32_t timeout_ms)
{
    (void)context;
    if (!s_usb.present || s_usb.hport == NULL) {
        return RTCP_ESTATE;
    }
    return control_transfer_timeout(s_usb.hport, request_type, request, value,
                                    index, data, length, timeout_ms);
}

static int submit_bulk(struct usbh_hubport *hport,
                       struct usb_endpoint_descriptor *endpoint, void *data,
                       size_t length, uint32_t timeout_ms)
{
    struct usbh_urb urb;

    if (s_bulk_sem == NULL) {
        s_bulk_sem = usb_osal_sem_create(0U);
        if (s_bulk_sem == NULL) {
            return RTCP_ENOMEM;
        }
    }
    memset(&urb, 0, sizeof(urb));
    usbh_bulk_urb_fill(&urb, hport, endpoint, data, (uint32_t)length, 0U,
                       transfer_complete, s_bulk_sem);
    usb_osal_mutex_take(hport->mutex);
    int ret = wait_urb(&s_bulk_sem, &urb,
                       timeout_ms != 0U ? timeout_ms : 1000U);
    usb_osal_mutex_give(hport->mutex);
    return ret;
}

static int lubanlite_bulk_transfer(void *context, uint8_t endpoint, void *data,
                                   size_t length, uint32_t timeout_ms)
{
    (void)context;
    if (!s_usb.present || s_usb.hport == NULL || data == NULL) {
        return RTCP_ESTATE;
    }

    if (endpoint == s_usb.mux_in) {
        return submit_bulk(s_usb.hport, &s_usb.mux_in_desc, data, length,
                           timeout_ms);
    }
    if (endpoint != s_usb.mux_out) {
        return RTCP_ENOTSUP;
    }

    int ret = submit_bulk(s_usb.hport, &s_usb.mux_out_desc, data, length,
                          timeout_ms);
    if (ret < 0) {
        return ret;
    }
    /* A bulk OUT transfer that ends exactly on a packet boundary needs a
     * zero-length packet, otherwise the device keeps waiting for the rest of
     * the transfer (LIVI's UsbWriter does the same). */
    if (length != 0U && s_usb.mux_out_packet_size != 0U &&
        (length % s_usb.mux_out_packet_size) == 0U) {
        int zlp = submit_bulk(s_usb.hport, &s_usb.mux_out_desc, NULL, 0U,
                              timeout_ms);
        if (zlp < 0) {
            return zlp;
        }
    }
    return ret;
}

static const rtcp_usb_ops_t s_lubanlite_ops = {
    .open = lubanlite_open,
    .close = lubanlite_close,
    .get_device = lubanlite_get_device,
    .control_transfer = lubanlite_control_transfer,
    .bulk_transfer = lubanlite_bulk_transfer,
};

const rtcp_usb_t rtcp_usb_lubanlite = {
    .ops = &s_lubanlite_ops,
    .context = NULL,
};

#endif /* LPKG_RT_CARPLAY_LUBANLITE_USB */
