/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * USB host transport seam.
 *
 * Wired CarPlay starts on the D213 USB1 host controller: the iPhone is
 * enumerated, switched to its CarPlay USB configuration, and then driven
 * through a bulk pipe (USBMUX) and a CDC-NCM network interface.  The protocol
 * core must not know which host stack performs those transfers, so this
 * header defines a small hardware-independent description of the attached
 * device and the two transfers wired bring-up needs (control and bulk).
 *
 * The descriptor model below is deliberately a plain copy of what the device
 * reports.  Nothing here assumes an iPhone model, a fixed product id, a fixed
 * configuration number or fixed endpoint addresses; the configuration scan
 * and endpoint lookup helpers in core/usb work from the descriptor bytes.
 */
#ifndef RTCP_USB_H
#define RTCP_USB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* bmAttributes transfer types (USB 2.0, 9.3.4). */
#define RTCP_USB_TRANSFER_CONTROL   0U
#define RTCP_USB_TRANSFER_ISOCHRONOUS 1U
#define RTCP_USB_TRANSFER_BULK      2U
#define RTCP_USB_TRANSFER_INTERRUPT 3U

/* bmRequestType fields.  The low five bits are the recipient. */
#define RTCP_USB_REQ_DIR_IN        0x80U
#define RTCP_USB_REQ_DIR_OUT       0x00U
#define RTCP_USB_REQ_TYPE_STANDARD 0x00U
#define RTCP_USB_REQ_TYPE_CLASS    0x20U
#define RTCP_USB_REQ_TYPE_VENDOR   0x40U
#define RTCP_USB_REQ_RECIPIENT_DEVICE    0x00U
#define RTCP_USB_REQ_RECIPIENT_INTERFACE 0x01U
#define RTCP_USB_REQ_RECIPIENT_ENDPOINT  0x02U

/* bRequest values used by the wired bring-up. */
#define RTCP_USB_REQ_GET_DESCRIPTOR    0x06U
#define RTCP_USB_REQ_SET_CONFIGURATION 0x09U
#define RTCP_USB_REQ_SET_INTERFACE     0x0bU
#define RTCP_USB_DESC_DEVICE           0x01U
#define RTCP_USB_DESC_CONFIGURATION    0x02U

/* Interface class triplets that identify the wired CarPlay functions. */
#define RTCP_USB_CLASS_VENDOR          0xffU
#define RTCP_USB_CLASS_CDC             0x02U
#define RTCP_USB_CLASS_CDC_DATA        0x0aU
#define RTCP_USB_CDC_SUBCLASS_NCM      0x0dU
#define RTCP_USB_APPLE_SUBCLASS_USBMUX 0xfeU
#define RTCP_USB_APPLE_PROTOCOL_USBMUX 0x02U
#define RTCP_USB_APPLE_SUBCLASS_ETH    0xfdU
#define RTCP_USB_APPLE_PROTOCOL_ETH    0x01U

#define RTCP_USB_MAX_INTERFACES  8U
#define RTCP_USB_MAX_ALTSETTINGS 4U
#define RTCP_USB_MAX_ENDPOINTS   4U

typedef enum {
    RTCP_USB_SPEED_UNKNOWN = 0,
    RTCP_USB_SPEED_LOW = 1,
    RTCP_USB_SPEED_FULL = 2,
    RTCP_USB_SPEED_HIGH = 3,
    RTCP_USB_SPEED_SUPER = 4,
} rtcp_usb_speed_t;

typedef struct {
    uint8_t address;        /* bEndpointAddress, bit 7 set for IN */
    uint8_t transfer_type;  /* RTCP_USB_TRANSFER_* */
    uint16_t max_packet_size;
    uint8_t interval;
} rtcp_usb_endpoint_t;

typedef struct {
    uint8_t alternate_setting;
    uint8_t endpoint_count;
    rtcp_usb_endpoint_t endpoints[RTCP_USB_MAX_ENDPOINTS];
} rtcp_usb_altsetting_t;

typedef struct {
    uint8_t number;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t protocol;
    uint8_t altsetting_count;
    rtcp_usb_altsetting_t altsettings[RTCP_USB_MAX_ALTSETTINGS];
} rtcp_usb_interface_t;

typedef struct {
    bool present;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t bcd_device;
    uint16_t configuration_total_length;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t speed;                  /* rtcp_usb_speed_t */
    uint8_t bus_id;
    uint8_t address;                /* USB device address */
    uint8_t configuration_index;    /* index used in GET_DESCRIPTOR */
    uint8_t configuration_value;    /* bConfigurationValue in that descriptor */
    uint8_t configuration_count;    /* bNumConfigurations at enumeration */
    uint8_t interface_count;
    rtcp_usb_interface_t interfaces[RTCP_USB_MAX_INTERFACES];
} rtcp_usb_device_t;

typedef enum {
    /* The device finished enumeration and its configuration was selected. */
    RTCP_USB_EVENT_ATTACHED = 0,
    /* The device left the bus; the snapshot must no longer be used. */
    RTCP_USB_EVENT_DETACHED = 1,
} rtcp_usb_event_t;

struct rtcp_usb;
typedef void (*rtcp_usb_event_cb_t)(struct rtcp_usb *usb,
                                    rtcp_usb_event_t event, void *user);

/*
 * Platform operations.  A port supplies these; the protocol core only sees
 * this table.  All calls are made from one owning thread (the shell command
 * or the wired control task), except the event callback, which the host
 * stack may invoke from its own thread.  The callback must therefore do no
 * more than record the event and wake the owner.
 *
 *   control_transfer  returns the number of bytes transferred (>= 0) or a
 *                     negative RTCP_* error.  `data` is IN or OUT according
 *                     to the direction bit of `request_type`.
 *   bulk_transfer     returns the number of bytes transferred or a negative
 *                     RTCP_* error.  `endpoint` is the bEndpointAddress from
 *                     the interface descriptor.  A bulk OUT transfer whose
 *                     length is a multiple of wMaxPacketSize must be followed
 *                     by a zero-length packet for the device to see the end
 *                     of the transfer.
 *   get_device        copies the current snapshot; returns RTCP_ESTATE when
 *                     no device is attached.
 */
typedef struct {
    int (*open)(void *context, rtcp_usb_event_cb_t callback, void *user);
    void (*close)(void *context);
    int (*get_device)(void *context, rtcp_usb_device_t *device);
    int (*control_transfer)(void *context, uint8_t request_type,
                            uint8_t request, uint16_t value, uint16_t index,
                            void *data, uint16_t length, uint32_t timeout_ms);
    int (*bulk_transfer)(void *context, uint8_t endpoint, void *data,
                         size_t length, uint32_t timeout_ms);
} rtcp_usb_ops_t;

typedef struct rtcp_usb {
    const rtcp_usb_ops_t *ops;
    void *context;
} rtcp_usb_t;

/* Parse a raw configuration descriptor into `device`.  Device-level fields
 * are left untouched.  Returns RTCP_OK or RTCP_EINVAL. */
int rtcp_usb_parse_configuration(rtcp_usb_device_t *device,
                                 const uint8_t *descriptor, size_t length);

/* Return the index into device->interfaces for the first interface whose
 * class/subclass/protocol match, or RTCP_ENOENT. */
int rtcp_usb_find_interface(const rtcp_usb_device_t *device, uint8_t class_code,
                            uint8_t subclass, uint8_t protocol);

/* Find one bulk OUT and one bulk IN endpoint in the best alternate setting of
 * `interface` (the highest alternate setting that has both).  Either output
 * pointer may be NULL.  Returns true when at least one was found. */
bool rtcp_usb_interface_bulk_endpoints(const rtcp_usb_interface_t *interface,
                                       uint8_t *out_endpoint,
                                       uint8_t *in_endpoint);

/*
 * Functions present in one configuration, derived from its interface
 * descriptors.  `ncm_*` describe the *standard* CDC-NCM function
 * (control 02/0d plus a CDC data interface 0a with a bulk pair); that is
 * what CherryUSB's usbh_cdc_ncm driver can bind to.  Apple Ethernet is the
 * vendor-specific ff/fd/01 function and needs its own host driver.
 */
typedef struct {
    bool usbmux;
    bool ncm_control;       /* interface class 02 / subclass 0d */
    bool ncm_data;          /* interface class 0a with a bulk pair */
    bool ncm_data_adjacent; /* data interface number == control + 1 */
    bool ncm_notification;  /* control alt 0 has an interrupt IN endpoint */
    bool apple_ethernet;    /* class ff / subclass fd / protocol 01 */
    bool ptp;               /* class 06 / subclass 01 / protocol 01 */
} rtcp_usb_functions_t;

void rtcp_usb_classify_configuration(const rtcp_usb_device_t *device,
                                     rtcp_usb_functions_t *functions);

/* Selection preference for the wired CarPlay configuration.  The standard
 * CDC-NCM pair is preferred because the existing host driver can drive it;
 * Apple Ethernet is the fallback; OTHER means the configuration carries no
 * usable wired IP link. */
typedef enum {
    RTCP_USB_CONFIG_OTHER = 0,
    RTCP_USB_CONFIG_APPLE_ETHERNET = 1,
    RTCP_USB_CONFIG_NCM = 2,
} rtcp_usb_configuration_kind_t;

rtcp_usb_configuration_kind_t rtcp_usb_configuration_kind(
    const rtcp_usb_device_t *device);

/* True when the configuration carries USBMUX plus a complete, adjacent
 * standard CDC-NCM function.  This is the descriptor answer to "can the
 * existing CherryUSB CDC-NCM driver be reused?". */
bool rtcp_usb_configuration_has_ncm_link(const rtcp_usb_device_t *device);

/* True when the selected configuration carries the wired CarPlay function
 * set: a vendor-specific USBMUX interface plus either the standard CDC-NCM
 * pair or Apple's Ethernet interface.  This is the descriptor-derived
 * replacement for "configuration number 6". */
bool rtcp_usb_is_carplay_configuration(const rtcp_usb_device_t *device);

/*
 * Human-readable dump of one raw configuration descriptor.  `emit` is called
 * once per line (no newline needed).  The walk uses the raw bytes, so it also
 * prints interfaces, alternate settings and endpoints beyond the
 * RTCP_USB_MAX_* model limits.  When `device` is not NULL, its parsed
 * classification is appended.  Returns RTCP_OK or RTCP_EINVAL.
 */
typedef void (*rtcp_usb_text_cb_t)(void *user, const char *line);
int rtcp_usb_describe_configuration(const rtcp_usb_device_t *device,
                                    const uint8_t *descriptor, size_t length,
                                    uint8_t index, rtcp_usb_text_cb_t emit,
                                    void *user);

/* CarPlay NCM control interface number in the selected configuration, taken
 * from the descriptor (CDC-NCM control interface, or Apple Ethernet when the
 * device exposes no NCM function).  iAP2 USBHostTransportComponent parameter
 * 3 carries this number.  Returns RTCP_ENOENT when neither interface exists. */
int rtcp_usb_carplay_interface_number(const rtcp_usb_device_t *device);

#endif /* RTCP_USB_H */
