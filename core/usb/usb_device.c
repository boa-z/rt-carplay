/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Descriptor-derived USB device model.
 *
 * This file is deliberately free of any host-stack dependency: it turns the
 * raw configuration descriptor bytes into the flat interface/altsetting/
 * endpoint model used by the wired bring-up, so the identification rules can
 * be exercised on the host without an iPhone.
 */
#include "rtcp/usb.h"

#include <string.h>

#include "rtcp/rtcp.h"

#define USB_DESC_INTERFACE 0x04U
#define USB_DESC_ENDPOINT  0x05U

#define USB_EP_DIR_MASK    0x80U
#define USB_EP_TYPE_MASK   0x03U
#define USB_EP_MPS_MASK    0x07ffU

typedef struct {
    rtcp_usb_interface_t *interface;
    rtcp_usb_altsetting_t *altsetting;
} parse_state_t;

static rtcp_usb_interface_t *find_or_add_interface(
    rtcp_usb_device_t *device, uint8_t number, uint8_t class_code,
    uint8_t subclass, uint8_t protocol)
{
    for (uint8_t i = 0U; i < device->interface_count; ++i) {
        if (device->interfaces[i].number == number) {
            return &device->interfaces[i];
        }
    }
    if (device->interface_count >= RTCP_USB_MAX_INTERFACES) {
        return NULL;
    }
    rtcp_usb_interface_t *interface =
        &device->interfaces[device->interface_count++];
    memset(interface, 0, sizeof(*interface));
    interface->number = number;
    interface->class_code = class_code;
    interface->subclass = subclass;
    interface->protocol = protocol;
    return interface;
}

static rtcp_usb_altsetting_t *find_or_add_altsetting(
    rtcp_usb_interface_t *interface, uint8_t alternate_setting)
{
    for (uint8_t i = 0U; i < interface->altsetting_count; ++i) {
        if (interface->altsettings[i].alternate_setting == alternate_setting) {
            return &interface->altsettings[i];
        }
    }
    if (interface->altsetting_count >= RTCP_USB_MAX_ALTSETTINGS) {
        return NULL;
    }
    rtcp_usb_altsetting_t *setting =
        &interface->altsettings[interface->altsetting_count++];
    memset(setting, 0, sizeof(*setting));
    setting->alternate_setting = alternate_setting;
    return setting;
}

static void add_endpoint(rtcp_usb_altsetting_t *altsetting,
                         const uint8_t *descriptor)
{
    if (altsetting == NULL ||
        altsetting->endpoint_count >= RTCP_USB_MAX_ENDPOINTS) {
        return;
    }
    rtcp_usb_endpoint_t *endpoint =
        &altsetting->endpoints[altsetting->endpoint_count++];
    endpoint->address = descriptor[2];
    endpoint->transfer_type = descriptor[3] & USB_EP_TYPE_MASK;
    endpoint->max_packet_size =
        (uint16_t)(((uint16_t)descriptor[4] | ((uint16_t)descriptor[5] << 8)) &
                   USB_EP_MPS_MASK);
    endpoint->interval = descriptor[6];
}

int rtcp_usb_parse_configuration(rtcp_usb_device_t *device,
                                 const uint8_t *descriptor, size_t length)
{
    if (device == NULL || descriptor == NULL || length < 9U) {
        return RTCP_EINVAL;
    }
    if (descriptor[1] != RTCP_USB_DESC_CONFIGURATION) {
        return RTCP_EINVAL;
    }
    size_t total_length =
        (size_t)descriptor[2] | ((size_t)descriptor[3] << 8);
    if (total_length < 9U) {
        return RTCP_EINVAL;
    }
    if (total_length > length) {
        total_length = length;
    }

    device->interface_count = 0U;
    device->configuration_value = descriptor[5];
    device->configuration_total_length = (uint16_t)total_length;
    memset(device->interfaces, 0, sizeof(device->interfaces));

    parse_state_t state = {0};
    size_t offset = 9U;
    while (offset + 2U <= total_length) {
        uint8_t desc_length = descriptor[offset];
        uint8_t desc_type = descriptor[offset + 1U];
        if (desc_length < 2U || offset + desc_length > total_length) {
            return RTCP_EINVAL;
        }
        if (desc_type == USB_DESC_INTERFACE) {
            if (desc_length < 9U) {
                return RTCP_EINVAL;
            }
            /* bInterfaceNumber, bAlternateSetting, bNumEndpoints, class,
             * subclass, protocol.  The class triplet belongs to the
             * interface, not to a particular alternate setting; alt 0 is
             * used when several descriptors repeat it. */
            rtcp_usb_interface_t *interface = find_or_add_interface(
                device, descriptor[offset + 2U], descriptor[offset + 5U],
                descriptor[offset + 6U], descriptor[offset + 7U]);
            state.interface = interface;
            state.altsetting = interface != NULL
                                   ? find_or_add_altsetting(
                                         interface, descriptor[offset + 3U])
                                   : NULL;
        } else if (desc_type == USB_DESC_ENDPOINT) {
            if (desc_length < 7U) {
                return RTCP_EINVAL;
            }
            if (state.altsetting != NULL) {
                add_endpoint(state.altsetting, descriptor + offset);
            }
        }
        offset += desc_length;
    }
    if (offset != total_length) {
        return RTCP_EINVAL;
    }
    return RTCP_OK;
}

int rtcp_usb_find_interface(const rtcp_usb_device_t *device, uint8_t class_code,
                            uint8_t subclass, uint8_t protocol)
{
    if (device == NULL) {
        return RTCP_EINVAL;
    }
    for (uint8_t i = 0U; i < device->interface_count; ++i) {
        const rtcp_usb_interface_t *interface = &device->interfaces[i];
        if (interface->class_code == class_code &&
            interface->subclass == subclass &&
            interface->protocol == protocol) {
            return (int)i;
        }
    }
    return RTCP_ENOENT;
}

bool rtcp_usb_interface_bulk_endpoints(const rtcp_usb_interface_t *interface,
                                       uint8_t *out_endpoint,
                                       uint8_t *in_endpoint)
{
    if (out_endpoint != NULL) *out_endpoint = 0U;
    if (in_endpoint != NULL) *in_endpoint = 0U;
    if (interface == NULL) {
        return false;
    }

    uint8_t found_out = 0U;
    uint8_t found_in = 0U;
    /* NCM data interfaces keep the bulk pair in their highest alternate
     * setting; USBMUX uses alt 0.  Walking from the highest setting down
     * keeps both cases descriptor-driven. */
    for (int alt = (int)interface->altsetting_count - 1; alt >= 0; --alt) {
        const rtcp_usb_altsetting_t *setting = &interface->altsettings[alt];
        for (uint8_t e = 0U; e < setting->endpoint_count; ++e) {
            const rtcp_usb_endpoint_t *endpoint = &setting->endpoints[e];
            if (endpoint->transfer_type != RTCP_USB_TRANSFER_BULK) continue;
            if ((endpoint->address & USB_EP_DIR_MASK) != 0U) {
                if (found_in == 0U) found_in = endpoint->address;
            } else if (found_out == 0U) {
                found_out = endpoint->address;
            }
        }
    }
    if (out_endpoint != NULL) *out_endpoint = found_out;
    if (in_endpoint != NULL) *in_endpoint = found_in;
    return found_out != 0U || found_in != 0U;
}

bool rtcp_usb_is_carplay_configuration(const rtcp_usb_device_t *device)
{
    if (device == NULL) {
        return false;
    }
    bool has_mux =
        rtcp_usb_find_interface(device, RTCP_USB_CLASS_VENDOR,
                                RTCP_USB_APPLE_SUBCLASS_USBMUX,
                                RTCP_USB_APPLE_PROTOCOL_USBMUX) >= 0;
    bool has_ncm = rtcp_usb_find_interface(device, RTCP_USB_CLASS_CDC,
                                           RTCP_USB_CDC_SUBCLASS_NCM, 0U) >= 0;
    bool has_apple_eth =
        rtcp_usb_find_interface(device, RTCP_USB_CLASS_VENDOR,
                                RTCP_USB_APPLE_SUBCLASS_ETH,
                                RTCP_USB_APPLE_PROTOCOL_ETH) >= 0;
    return has_mux && (has_ncm || has_apple_eth);
}

int rtcp_usb_carplay_interface_number(const rtcp_usb_device_t *device)
{
    if (device == NULL) {
        return RTCP_ENOENT;
    }
    int index = rtcp_usb_find_interface(device, RTCP_USB_CLASS_CDC,
                                        RTCP_USB_CDC_SUBCLASS_NCM, 0U);
    if (index < 0) {
        index = rtcp_usb_find_interface(device, RTCP_USB_CLASS_VENDOR,
                                        RTCP_USB_APPLE_SUBCLASS_ETH,
                                        RTCP_USB_APPLE_PROTOCOL_ETH);
    }
    if (index < 0) {
        return RTCP_ENOENT;
    }
    return (int)device->interfaces[index].number;
}
