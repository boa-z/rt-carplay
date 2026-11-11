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

/* CherryUSB carries the next DATA PID in urb.data_toggle, not in the
 * endpoint descriptor. This state outlives each owned DMA transaction. */
struct usb_bulk_state {
    uint8_t data_toggle;
    uint32_t generation;
};

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
    struct usb_bulk_state mux_out_state;
    struct usb_bulk_state mux_in_state;

    /* Configuration descriptor index returned by the enumeration hook. */
    uint8_t last_configuration_index;

    rtcp_usb_event_cb_t callback;
    void *user;

    uint32_t attach_count;
    uint32_t detach_count;
} carplay_usb_state_t;

static carplay_usb_state_t s_usb;

static void reset_bulk_states(void)
{
    size_t flags = usb_osal_enter_critical_section();
    s_usb.mux_out_state.data_toggle = 0U;
    s_usb.mux_in_state.data_toggle = 0U;
    ++s_usb.mux_out_state.generation;
    ++s_usb.mux_in_state.generation;
    usb_osal_leave_critical_section(flags);
}

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

/* Application callers can supply stack or unaligned buffers. Only this owned,
 * cache-line-padded allocation is exposed to DMA, including the Setup stage.
 * Each transfer owns its semaphore/URB so a failed cancel can retain them. */
struct usb_dma_transfer {
    struct usbh_urb urb;
    struct usb_endpoint_descriptor endpoint;
    usb_osal_sem_t done;
    USB_MEM_ALIGNX struct usb_setup_packet setup;
    USB_MEM_ALIGNX uint8_t data[];
};

static void transfer_complete(void *arg, int nbytes)
{
    (void)nbytes;
    usb_osal_sem_give((usb_osal_sem_t)arg);
}

/* Optional SDK read-only HCD diagnostics; no dependency when absent. */
extern void usbh_ehci_transfer_snapshot(struct usbh_urb *, int)
    __attribute__((weak));

static int map_urb_error(int error)
{
    if (error == -USB_ERR_TIMEOUT) return RTCP_ETIMEDOUT;
    if (error == -USB_ERR_NOTCONN || error == -USB_ERR_SHUTDOWN ||
        error == -USB_ERR_NODEV) return RTCP_ESTATE;
    return RTCP_EIO;
}

/* endpoint == NULL selects EP0. Caller buffers are copied only while this
 * function runs; no HCD ever retains a caller's data or stack Setup packet. */
static int dma_transfer(struct usbh_hubport *hport,
                        const struct usb_endpoint_descriptor *endpoint,
                        const struct usb_setup_packet *setup, void *data,
                        size_t length, uint32_t timeout_ms,
                        struct usb_bulk_state *pipe)
{
    if (!hport || (!endpoint && !setup) || (length && !data))
        return RTCP_EINVAL;
    if (length > INT32_MAX ||
        length > SIZE_MAX - sizeof(struct usb_dma_transfer) -
                     (CONFIG_USB_ALIGN_SIZE - 1U)) return RTCP_EINVAL;
    uint32_t generation = pipe ? pipe->generation : 0U;
    size_t padded = (length + CONFIG_USB_ALIGN_SIZE - 1U) &
                    ~(size_t)(CONFIG_USB_ALIGN_SIZE - 1U);
    struct usb_dma_transfer *t = rt_malloc_align(sizeof(*t) + padded,
                                                CONFIG_USB_ALIGN_SIZE);
    if (!t) return RTCP_ENOMEM;
    memset(t, 0, sizeof(*t) + padded);
    t->done = usb_osal_sem_create(0U);
    if (!t->done) { rt_free_align(t); return RTCP_ENOMEM; }
    bool input = endpoint ? (endpoint->bEndpointAddress & USB_REQUEST_DIR_IN) != 0U
                          : (setup->bmRequestType & USB_REQUEST_DIR_IN) != 0U;
    if (length && !input) memcpy(t->data, data, length);
    if (endpoint) {
        t->endpoint = *endpoint;
        usbh_bulk_urb_fill(&t->urb, hport, &t->endpoint,
                           length ? t->data : NULL, (uint32_t)length, 0U,
                           transfer_complete, t->done);
    } else {
        t->setup = *setup;
        usbh_control_urb_fill(&t->urb, hport, &t->setup,
                              length ? t->data : NULL, (uint32_t)length, 0U,
                              transfer_complete, t->done);
        t->endpoint = *t->urb.ep;
        t->urb.ep = &t->endpoint;
    }
    usb_osal_mutex_take(hport->mutex);
    /* Serialize read/submit/write of the endpoint toggle. Reject detected
     * generation changes and suppress stale toggle updates after reconnect. */
    size_t flags = usb_osal_enter_critical_section();
    bool current = !pipe || pipe->generation == generation;
    if (pipe) t->urb.data_toggle = pipe->data_toggle;
    usb_osal_leave_critical_section(flags);
    int ret = current ? usbh_submit_urb(&t->urb) : -USB_ERR_NOTCONN;
    bool reclaim = true;
    if (ret < 0) {
        USB_LOG_WRN("USB submit error ep=%02x raw=%d length=%u\r\n",
                     t->urb.ep->bEndpointAddress, ret, (unsigned)length);
        ret = map_urb_error(ret);
    } else if (usb_osal_sem_take(t->done, timeout_ms) < 0) {
        /* On cancellation failure retain URB, callback argument and DMA
         * storage: a late HCD access must not touch freed memory. */
        USB_LOG_WRN("USB wait timeout ep=%02x length=%u actual=%u pid=%u urb=%d\r\n",
                     t->urb.ep->bEndpointAddress, (unsigned)length,
                     (unsigned)t->urb.actual_length, t->urb.data_toggle,
                     t->urb.errorcode);
        if (usbh_ehci_transfer_snapshot)
            usbh_ehci_transfer_snapshot(&t->urb, -USB_ERR_TIMEOUT);
        /* UART snapshot output can outlast an in-flight transfer. If its
         * callback has now completed, there is nothing left to cancel. */
        reclaim = usb_osal_sem_take(t->done, 1U) == 0;
        if (!reclaim) reclaim = usbh_kill_urb(&t->urb) == 0;
        ret = RTCP_ETIMEDOUT;
    } else if (t->urb.errorcode < 0) {
        USB_LOG_WRN("USB completion error ep=%02x urb=%d actual=%u pid=%u\r\n",
                     t->urb.ep->bEndpointAddress, t->urb.errorcode,
                     (unsigned)t->urb.actual_length, t->urb.data_toggle);
        ret = map_urb_error(t->urb.errorcode);
    } else {
        flags = usb_osal_enter_critical_section();
        if (pipe && pipe->generation == generation)
            pipe->data_toggle = t->urb.data_toggle;
        usb_osal_leave_critical_section(flags);
        size_t actual = t->urb.actual_length;
        size_t prefix = endpoint ? 0U : sizeof(t->setup);
        if (actual < prefix || actual - prefix > length) {
            ret = RTCP_EIO;
        } else {
            ret = (int)(actual - prefix);
            if (input && ret > 0) memcpy(data, t->data, (size_t)ret);
        }
    }
    if (pipe && pipe->generation != generation) ret = RTCP_ESTATE;
    usb_osal_mutex_give(hport->mutex);
    if (reclaim) {
        usb_osal_sem_delete(t->done);
        rt_free_align(t);
    } else {
        USB_LOG_ERR("USB transfer cancel failed; DMA storage quarantined\r\n");
    }
    return ret;
}

static int control_transfer_timeout(struct usbh_hubport *hport,
                                    uint8_t request_type, uint8_t request,
                                    uint16_t value, uint16_t index, void *data,
                                    uint16_t length, uint32_t timeout_ms)
{
    struct usb_setup_packet setup = {
        .bmRequestType = request_type, .bRequest = request,
        .wValue = value, .wIndex = index, .wLength = length,
    };
    return dma_transfer(hport, NULL, &setup, data, length,
                         timeout_ms ? timeout_ms :
                         CONFIG_USBHOST_CONTROL_TRANSFER_TIMEOUT, NULL);
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
    reset_bulk_states();
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
    reset_bulk_states();
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
                       size_t length, uint32_t timeout_ms,
                       struct usb_bulk_state *pipe)
{
    return dma_transfer(hport, endpoint, NULL, data, length,
                         timeout_ms ? timeout_ms : 1000U, pipe);
}

static int lubanlite_bulk_transfer(void *context, uint8_t endpoint, void *data,
                                   size_t length, uint32_t timeout_ms)
{
    (void)context;
    if (!s_usb.present || s_usb.hport == NULL || data == NULL) {
        return RTCP_ESTATE;
    }

    if (endpoint == s_usb.mux_in) {
        /* usbmuxd USB_MRU=16384: use packet-aligned bounded USB reads and
         * let the mux reassembler join frames across transfers. */
        if (length > 16384U) length = 16384U;
        return submit_bulk(s_usb.hport, &s_usb.mux_in_desc, data, length,
                           timeout_ms, &s_usb.mux_in_state);
    }
    if (endpoint != s_usb.mux_out) {
        return RTCP_ENOTSUP;
    }

    int ret = submit_bulk(s_usb.hport, &s_usb.mux_out_desc, data, length,
                          timeout_ms, &s_usb.mux_out_state);
    if (ret < 0) {
        return ret;
    }
    /* A bulk OUT transfer that ends exactly on a packet boundary needs a
     * zero-length packet, otherwise the device keeps waiting for the rest of
     * the transfer (LIVI's UsbWriter does the same). */
    if (length != 0U && s_usb.mux_out_packet_size != 0U &&
        (length % s_usb.mux_out_packet_size) == 0U) {
        int zlp = submit_bulk(s_usb.hport, &s_usb.mux_out_desc, NULL, 0U,
                              timeout_ms, &s_usb.mux_out_state);
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
