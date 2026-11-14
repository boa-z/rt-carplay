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

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "rtcp/rtcp.h"

#define USB_DESC_INTERFACE 0x04U
#define USB_DESC_ENDPOINT  0x05U
#define USB_DESC_CS_INTERFACE 0x24U
#define USB_DESC_INTERFACE_ASSOCIATION 0x0bU
#define USB_CS_SUBTYPE_ETHERNET 0x0fU

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

static bool interface_has_bulk_pair(const rtcp_usb_interface_t *interface)
{
    uint8_t out = 0U;
    uint8_t in = 0U;
    return rtcp_usb_interface_bulk_endpoints(interface, &out, &in) &&
           out != 0U && in != 0U;
}

/* CherryUSB's NCM driver takes the control interface's first endpoint as the
 * interrupt notification pipe; a control interface without one cannot be
 * driven by it. */
static bool interface_has_interrupt_in(const rtcp_usb_interface_t *interface)
{
    if (interface == NULL || interface->altsetting_count == 0U) {
        return false;
    }
    const rtcp_usb_altsetting_t *setting = &interface->altsettings[0];
    for (uint8_t e = 0U; e < setting->endpoint_count; ++e) {
        const rtcp_usb_endpoint_t *endpoint = &setting->endpoints[e];
        if (endpoint->transfer_type == RTCP_USB_TRANSFER_INTERRUPT &&
            (endpoint->address & USB_EP_DIR_MASK) != 0U) {
            return true;
        }
    }
    return false;
}

void rtcp_usb_classify_configuration(const rtcp_usb_device_t *device,
                                     rtcp_usb_functions_t *functions)
{
    if (functions == NULL) {
        return;
    }
    memset(functions, 0, sizeof(*functions));
    if (device == NULL) {
        return;
    }

    int control_number = -1;
    int data_number = -1;
    for (uint8_t i = 0U; i < device->interface_count; ++i) {
        const rtcp_usb_interface_t *interface = &device->interfaces[i];
        if (interface->class_code == RTCP_USB_CLASS_VENDOR &&
            interface->subclass == RTCP_USB_APPLE_SUBCLASS_USBMUX &&
            interface->protocol == RTCP_USB_APPLE_PROTOCOL_USBMUX) {
            functions->usbmux = true;
        }
        if (interface->class_code == RTCP_USB_CLASS_VENDOR &&
            interface->subclass == RTCP_USB_APPLE_SUBCLASS_ETH &&
            interface->protocol == RTCP_USB_APPLE_PROTOCOL_ETH) {
            functions->apple_ethernet = true;
        }
        if (interface->class_code == 0x06U && interface->subclass == 0x01U &&
            interface->protocol == 0x01U) {
            functions->ptp = true;
        }
        if (interface->class_code == RTCP_USB_CLASS_CDC &&
            interface->subclass == RTCP_USB_CDC_SUBCLASS_NCM) {
            if (control_number < 0) {
                control_number = interface->number;
            }
            functions->ncm_control = true;
            if (interface_has_interrupt_in(interface)) {
                functions->ncm_notification = true;
            }
        }
        if (interface->class_code == RTCP_USB_CLASS_CDC_DATA &&
            interface_has_bulk_pair(interface)) {
            functions->ncm_data = true;
            if (control_number >= 0 &&
                interface->number == (uint8_t)(control_number + 1)) {
                data_number = interface->number;
            } else if (data_number < 0) {
                data_number = interface->number;
            }
        }
    }
    functions->ncm_data_adjacent =
        control_number >= 0 && data_number == control_number + 1;
}

rtcp_usb_configuration_kind_t rtcp_usb_configuration_kind(
    const rtcp_usb_device_t *device)
{
    rtcp_usb_functions_t functions;
    rtcp_usb_classify_configuration(device, &functions);
    if (functions.usbmux && functions.ncm_control && functions.ncm_data &&
        functions.ncm_data_adjacent && functions.ncm_notification) {
        return RTCP_USB_CONFIG_NCM;
    }
    if (functions.usbmux && functions.apple_ethernet) {
        return RTCP_USB_CONFIG_APPLE_ETHERNET;
    }
    return RTCP_USB_CONFIG_OTHER;
}

bool rtcp_usb_configuration_has_ncm_link(const rtcp_usb_device_t *device)
{
    return rtcp_usb_configuration_kind(device) == RTCP_USB_CONFIG_NCM;
}

bool rtcp_usb_is_carplay_configuration(const rtcp_usb_device_t *device)
{
    return rtcp_usb_configuration_kind(device) != RTCP_USB_CONFIG_OTHER;
}

int rtcp_usb_carplay_interface_number(const rtcp_usb_device_t *device)
{
    if (device == NULL) {
        return RTCP_ENOENT;
    }
    for (uint8_t i = 0U; i < device->interface_count; ++i) {
        const rtcp_usb_interface_t *interface = &device->interfaces[i];
        if (interface->class_code == RTCP_USB_CLASS_CDC &&
            interface->subclass == RTCP_USB_CDC_SUBCLASS_NCM) {
            return (int)interface->number;
        }
    }
    int index = rtcp_usb_find_interface(device, RTCP_USB_CLASS_VENDOR,
                                        RTCP_USB_APPLE_SUBCLASS_ETH,
                                        RTCP_USB_APPLE_PROTOCOL_ETH);
    if (index < 0) {
        return RTCP_ENOENT;
    }
    return (int)device->interfaces[index].number;
}

static void describe_line(rtcp_usb_text_cb_t emit, void *user,
                          const char *format, ...)
{
    char line[192];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    emit(user, line);
}

static const char *transfer_name(uint8_t attributes)
{
    switch (attributes & USB_EP_TYPE_MASK) {
        case RTCP_USB_TRANSFER_CONTROL:
            return "control";
        case RTCP_USB_TRANSFER_ISOCHRONOUS:
            return "iso";
        case RTCP_USB_TRANSFER_BULK:
            return "bulk";
        case RTCP_USB_TRANSFER_INTERRUPT:
            return "interrupt";
        default:
            return "?";
    }
}

static const char *yes_no(bool value)
{
    return value ? "yes" : "no";
}

static const char *configuration_kind_name(rtcp_usb_configuration_kind_t kind)
{
    switch (kind) {
        case RTCP_USB_CONFIG_NCM:
            return "ncm";
        case RTCP_USB_CONFIG_APPLE_ETHERNET:
            return "apple-ethernet";
        default:
            return "none";
    }
}

int rtcp_usb_describe_configuration(const rtcp_usb_device_t *device,
                                    const uint8_t *descriptor, size_t length,
                                    uint8_t index, rtcp_usb_text_cb_t emit,
                                    void *user)
{
    if (descriptor == NULL || emit == NULL || length < 9U ||
        descriptor[0] < 9U || descriptor[1] != RTCP_USB_DESC_CONFIGURATION) {
        return RTCP_EINVAL;
    }
    size_t total_length =
        (size_t)descriptor[2] | ((size_t)descriptor[3] << 8U);
    if (total_length < 9U) {
        return RTCP_EINVAL;
    }
    if (total_length > length) {
        total_length = length;
    }

    describe_line(emit, user,
                  "[usb-desc] config %u: value=%u interfaces=%u total=%u "
                  "attr=0x%02x power=%umA",
                  index, descriptor[5], descriptor[4],
                  (unsigned)total_length, descriptor[7],
                  (unsigned)descriptor[8] * 2U);

    size_t offset = 9U;
    while (offset + 2U <= total_length) {
        uint8_t desc_length = descriptor[offset];
        uint8_t desc_type = descriptor[offset + 1U];
        if (desc_length < 2U || offset + desc_length > total_length) {
            describe_line(emit, user,
                          "[usb-desc]   invalid descriptor at offset %u "
                          "(type=0x%02x len=%u)",
                          (unsigned)offset, desc_type, desc_length);
            return RTCP_EINVAL;
        }
        if (desc_type == USB_DESC_INTERFACE && desc_length >= 9U) {
            describe_line(
                emit, user,
                "[usb-desc]   interface %u alt %u class=%02x/%02x/%02x "
                "endpoints=%u",
                descriptor[offset + 2U], descriptor[offset + 3U],
                descriptor[offset + 5U], descriptor[offset + 6U],
                descriptor[offset + 7U], descriptor[offset + 4U]);
        } else if (desc_type == USB_DESC_ENDPOINT && desc_length >= 7U) {
            uint16_t max_packet =
                (uint16_t)((uint16_t)descriptor[offset + 4U] |
                           ((uint16_t)descriptor[offset + 5U] << 8U)) &
                USB_EP_MPS_MASK;
            describe_line(emit, user,
                          "[usb-desc]     endpoint 0x%02x %s %s mps=%u "
                          "interval=%u",
                          descriptor[offset + 2U],
                          (descriptor[offset + 2U] & USB_EP_DIR_MASK) != 0U
                              ? "in"
                              : "out",
                          transfer_name(descriptor[offset + 3U]), max_packet,
                          descriptor[offset + 6U]);
        } else if (desc_type == USB_DESC_CS_INTERFACE && desc_length >= 3U) {
            uint8_t subtype = descriptor[offset + 2U];
            if (subtype == USB_CS_SUBTYPE_ETHERNET && desc_length >= 13U) {
                uint16_t segment =
                    (uint16_t)((uint16_t)descriptor[offset + 8U] |
                               ((uint16_t)descriptor[offset + 9U] << 8U));
                describe_line(emit, user,
                              "[usb-desc]     cdc-ethernet subtype=0x0f "
                              "mac_string=%u max_segment=%u",
                              descriptor[offset + 3U], segment);
            } else {
                describe_line(emit, user,
                              "[usb-desc]     class-specific type=0x24 "
                              "subtype=0x%02x len=%u",
                              subtype, desc_length);
            }
        } else if (desc_type == USB_DESC_INTERFACE_ASSOCIATION &&
                   desc_length >= 8U) {
            describe_line(emit, user,
                          "[usb-desc]   interface-association first=%u "
                          "count=%u class=%02x/%02x/%02x",
                          descriptor[offset + 2U], descriptor[offset + 3U],
                          descriptor[offset + 4U], descriptor[offset + 5U],
                          descriptor[offset + 6U]);
        } else {
            describe_line(emit, user,
                          "[usb-desc]   descriptor type=0x%02x len=%u",
                          desc_type, desc_length);
        }
        offset += desc_length;
    }

    if (device != NULL) {
        rtcp_usb_functions_t functions;
        rtcp_usb_classify_configuration(device, &functions);
        describe_line(
            emit, user,
            "[usb-desc]   functions: usbmux=%s ncm_control=%s ncm_data=%s "
            "ncm_adjacent=%s apple_eth=%s ptp=%s route=%s",
            yes_no(functions.usbmux), yes_no(functions.ncm_control),
            yes_no(functions.ncm_data), yes_no(functions.ncm_data_adjacent),
            yes_no(functions.apple_ethernet), yes_no(functions.ptp),
            configuration_kind_name(rtcp_usb_configuration_kind(device)));
    }
    return RTCP_OK;
}
