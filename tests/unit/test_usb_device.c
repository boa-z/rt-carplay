/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rtcp/usb.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rtcp/rtcp.h"

typedef struct {
    uint8_t data[512];
    size_t len;
} builder_t;

static void put(builder_t *builder, const uint8_t *bytes, size_t length)
{
    assert(builder->len + length <= sizeof(builder->data));
    memcpy(builder->data + builder->len, bytes, length);
    builder->len += length;
}

static void put_config(builder_t *builder, uint8_t value)
{
    uint8_t config[9] = {
        9, RTCP_USB_DESC_CONFIGURATION, 0, 0, 0, value, 0, 0x80, 1,
    };
    put(builder, config, sizeof(config));
}

static void put_interface(builder_t *builder, uint8_t number, uint8_t alt,
                          uint8_t endpoints, uint8_t class_code,
                          uint8_t subclass, uint8_t protocol)
{
    uint8_t interface[9] = {
        9, 0x04, number, alt, endpoints, class_code, subclass, protocol, 0,
    };
    put(builder, interface, sizeof(interface));
}

static void put_endpoint(builder_t *builder, uint8_t address,
                         uint8_t attributes, uint16_t max_packet)
{
    uint8_t endpoint[7] = {
        7, 0x05, address, attributes, (uint8_t)max_packet,
        (uint8_t)(max_packet >> 8U), 0,
    };
    put(builder, endpoint, sizeof(endpoint));
}

/* Fill the configuration header once all descriptors are known. */
static size_t finish(builder_t *builder, uint8_t interface_count)
{
    assert(builder->len <= UINT16_MAX);
    builder->data[2] = (uint8_t)builder->len;
    builder->data[3] = (uint8_t)(builder->len >> 8U);
    builder->data[4] = interface_count;
    return builder->len;
}

static void begin(builder_t *builder, uint8_t configuration_value)
{
    memset(builder, 0, sizeof(*builder));
    put_config(builder, configuration_value);
}

static void test_parse_carplay_configuration(void)
{
    builder_t builder;
    begin(&builder, 6);
    /* Interface numbers and addresses intentionally do not match the
     * reference values: everything must come from the descriptor. */
    put_interface(&builder, 3, 0, 2, RTCP_USB_CLASS_VENDOR,
                  RTCP_USB_APPLE_SUBCLASS_USBMUX,
                  RTCP_USB_APPLE_PROTOCOL_USBMUX);
    put_endpoint(&builder, 0x04, RTCP_USB_TRANSFER_BULK, 512);
    put_endpoint(&builder, 0x85, RTCP_USB_TRANSFER_BULK, 512);
    put_interface(&builder, 1, 0, 1, RTCP_USB_CLASS_CDC,
                  RTCP_USB_CDC_SUBCLASS_NCM, 0);
    put_endpoint(&builder, 0x86, RTCP_USB_TRANSFER_INTERRUPT, 16);
    put_interface(&builder, 2, 0, 0, RTCP_USB_CLASS_CDC_DATA, 0, 0);
    put_interface(&builder, 2, 1, 2, RTCP_USB_CLASS_CDC_DATA, 0, 0);
    put_endpoint(&builder, 0x87, RTCP_USB_TRANSFER_BULK, 512);
    put_endpoint(&builder, 0x08, RTCP_USB_TRANSFER_BULK, 512);
    size_t length = finish(&builder, 3);

    rtcp_usb_device_t device;
    memset(&device, 0, sizeof(device));
    assert(rtcp_usb_parse_configuration(&device, builder.data, length) ==
           RTCP_OK);
    assert(device.configuration_value == 6U);
    assert(device.configuration_total_length == length);
    assert(device.interface_count == 3U);

    int mux = rtcp_usb_find_interface(&device, RTCP_USB_CLASS_VENDOR,
                                      RTCP_USB_APPLE_SUBCLASS_USBMUX,
                                      RTCP_USB_APPLE_PROTOCOL_USBMUX);
    assert(mux == 0);
    uint8_t out = 0;
    uint8_t in = 0;
    assert(rtcp_usb_interface_bulk_endpoints(&device.interfaces[mux], &out,
                                             &in));
    assert(out == 0x04U);
    assert(in == 0x85U);

    int data = rtcp_usb_find_interface(&device, RTCP_USB_CLASS_CDC_DATA, 0, 0);
    assert(data == 2);
    assert(device.interfaces[data].altsetting_count == 2U);
    assert(rtcp_usb_interface_bulk_endpoints(&device.interfaces[data], &out,
                                             &in));
    /* The bulk pair lives in alternate setting 1, not in alt 0. */
    assert(out == 0x08U);
    assert(in == 0x87U);

    assert(rtcp_usb_is_carplay_configuration(&device));
    /* The iAP2 interface number comes from the NCM control interface
     * descriptor (interface 1 here), never from a fixed reference value. */
    assert(rtcp_usb_carplay_interface_number(&device) == 1);
}

static void test_carplay_configuration_with_apple_ethernet(void)
{
    builder_t builder;
    begin(&builder, 7);
    put_interface(&builder, 0, 0, 2, RTCP_USB_CLASS_VENDOR,
                  RTCP_USB_APPLE_SUBCLASS_USBMUX,
                  RTCP_USB_APPLE_PROTOCOL_USBMUX);
    put_endpoint(&builder, 0x02, RTCP_USB_TRANSFER_BULK, 512);
    put_endpoint(&builder, 0x83, RTCP_USB_TRANSFER_BULK, 512);
    put_interface(&builder, 1, 0, 2, RTCP_USB_CLASS_VENDOR,
                  RTCP_USB_APPLE_SUBCLASS_ETH, RTCP_USB_APPLE_PROTOCOL_ETH);
    put_endpoint(&builder, 0x04, RTCP_USB_TRANSFER_BULK, 512);
    put_endpoint(&builder, 0x85, RTCP_USB_TRANSFER_BULK, 512);
    size_t length = finish(&builder, 2);

    rtcp_usb_device_t device;
    memset(&device, 0, sizeof(device));
    assert(rtcp_usb_parse_configuration(&device, builder.data, length) ==
           RTCP_OK);
    assert(rtcp_usb_is_carplay_configuration(&device));
    /* No NCM function: fall back to Apple Ethernet's interface number. */
    assert(rtcp_usb_carplay_interface_number(&device) == 1);
}

static void test_default_configuration_is_not_carplay(void)
{
    builder_t builder;
    begin(&builder, 4);
    /* The default iPhone configuration carries USBMUX too, but neither the
     * NCM control interface nor Apple Ethernet.  It must not be mistaken for
     * the CarPlay configuration. */
    put_interface(&builder, 0, 0, 2, RTCP_USB_CLASS_VENDOR,
                  RTCP_USB_APPLE_SUBCLASS_USBMUX,
                  RTCP_USB_APPLE_PROTOCOL_USBMUX);
    put_endpoint(&builder, 0x01, RTCP_USB_TRANSFER_BULK, 512);
    put_endpoint(&builder, 0x82, RTCP_USB_TRANSFER_BULK, 512);
    put_interface(&builder, 1, 0, 0, 0xff, 0xfd, 0x00);
    size_t length = finish(&builder, 2);

    rtcp_usb_device_t device;
    memset(&device, 0, sizeof(device));
    assert(rtcp_usb_parse_configuration(&device, builder.data, length) ==
           RTCP_OK);
    assert(!rtcp_usb_is_carplay_configuration(&device));
    assert(rtcp_usb_find_interface(&device, 0x02, 0x0d, 0) == RTCP_ENOENT);
    assert(rtcp_usb_carplay_interface_number(&device) == RTCP_ENOENT);
}

static void test_lookup_rejects_wrong_protocol(void)
{
    builder_t builder;
    begin(&builder, 4);
    put_interface(&builder, 0, 0, 0, RTCP_USB_CLASS_VENDOR,
                  RTCP_USB_APPLE_SUBCLASS_USBMUX, 0x00);
    size_t length = finish(&builder, 1);

    rtcp_usb_device_t device;
    memset(&device, 0, sizeof(device));
    assert(rtcp_usb_parse_configuration(&device, builder.data, length) ==
           RTCP_OK);
    assert(rtcp_usb_find_interface(&device, RTCP_USB_CLASS_VENDOR,
                                   RTCP_USB_APPLE_SUBCLASS_USBMUX,
                                   RTCP_USB_APPLE_PROTOCOL_USBMUX) ==
           RTCP_ENOENT);
}

static void test_invalid_descriptors(void)
{
    rtcp_usb_device_t device;
    uint8_t short_descriptor[8] = {9, RTCP_USB_DESC_CONFIGURATION};
    memset(&device, 0, sizeof(device));
    assert(rtcp_usb_parse_configuration(&device, short_descriptor,
                                        sizeof(short_descriptor)) ==
           RTCP_EINVAL);

    builder_t builder;
    begin(&builder, 4);
    put_interface(&builder, 0, 0, 0, 0xff, 0, 0);
    size_t length = finish(&builder, 1);
    /* A descriptor with bLength 0 would loop forever; the parser rejects it
     * before it can walk off the buffer. */
    builder.data[9] = 0;
    assert(rtcp_usb_parse_configuration(&device, builder.data, length) ==
           RTCP_EINVAL);

    /* Nothing may be parsed from a non-configuration descriptor. */
    begin(&builder, 4);
    builder.data[1] = 0x03;
    assert(rtcp_usb_parse_configuration(&device, builder.data, 9) ==
           RTCP_EINVAL);
}

static void test_rounding_of_max_packet_size(void)
{
    builder_t builder;
    begin(&builder, 4);
    put_interface(&builder, 0, 0, 1, 0xff, 0xfe, 0x02);
    /* Bits 15..11 are the high-bandwidth multiplier and are not part of the
     * packet size. */
    put_endpoint(&builder, 0x81, RTCP_USB_TRANSFER_BULK, 512U | 0x1800U);
    size_t length = finish(&builder, 1);

    rtcp_usb_device_t device;
    memset(&device, 0, sizeof(device));
    assert(rtcp_usb_parse_configuration(&device, builder.data, length) ==
           RTCP_OK);
    assert(device.interfaces[0].altsettings[0].endpoints[0].max_packet_size ==
           512U);
}

int main(void)
{
    test_parse_carplay_configuration();
    test_carplay_configuration_with_apple_ethernet();
    test_default_configuration_is_not_carplay();
    test_lookup_rejects_wrong_protocol();
    test_invalid_descriptors();
    test_rounding_of_max_packet_size();
    puts("usb device host test passed");
    return 0;
}
