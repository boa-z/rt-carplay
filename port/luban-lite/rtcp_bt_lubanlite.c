/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Luban-Lite / Zephyr Classic RFCOMM seam.
 *
 * Backs the rtcp_bt interface with the Zephyr Bluetooth host that ships in
 * the SDK (packages/third-party/zephyr-bluetooth): Classic BR/EDR, SDP and
 * RFCOMM over the RTL8733BS H5 transport.  The iAP2 session uses it as a
 * byte stream:
 *
 *   enable()        -> bt_enable + BR/EDR connectable + discoverable
 *   rfcomm_listen() -> RFCOMM server, dynamic channel, SDP record with the
 *                      caller supplied 128-bit service class UUID
 *   rfcomm_accept() -> blocks until a peer DLC is connected
 *   send()/recv()   -> chunked writes (<= negotiated MTU) and a byte ring
 *
 * Verified against the SDK headers and Zephyr sources at Luban-Lite commit
 * 2a614c6be.  The whole path is build-only until a board runs it; the H5
 * firmware download in the SDK Realtek driver is the first bring-up suspect.
 *
 * Threading contract: one reader thread calls recv() and send(); the Zephyr
 * host calls the DLC callbacks from its own RX thread.  The seam never
 * blocks inside a stack callback, it only copies into the ring and releases
 * a semaphore.
 */
#include "rtcp_lubanlite.h"

#include <string.h>

#if defined(__has_include)
#if __has_include(<rtconfig.h>)
#include <rtconfig.h>
#endif
#endif

#include "rtcp/rtcp.h"

#ifdef LPKG_RT_CARPLAY_LUBANLITE_BT

#include <rtthread.h>

#include <zephyr/autoconf.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/assigned_numbers.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/classic/classic.h>
#include <zephyr/bluetooth/classic/rfcomm.h>
#include <zephyr/bluetooth/classic/sdp.h>
#include <zephyr/net_buf.h>
#if defined(CONFIG_BT_SETTINGS)
#include <zephyr/settings/settings.h>
#endif

#define BT_TAG "bt-luban"

/* One iAP2 link is enough; RFCOMM multiplexes profiles on one ACL. */
#define BT_LINK_MAX 1U

/* Ring depth for received bytes.  The iAP2 pump drains it promptly; a
 * controller that bursts more than this drops bytes and reports it, which is
 * preferable to blocking the Bluetooth host thread. */
#define BT_RX_RING_SIZE 4096U

/* Local DLC MTU.  The negotiated value is min(local, session MTU); the SDK
 * session MTU follows CONFIG_LPKG_BT_RFCOMM_L2CAP_MTU.  1011 matches common
 * iAP2 implementations and fits one L2CAP PDU with the RFCOMM overhead. */
#define BT_DLC_MTU 1011U

/* Outgoing RFCOMM PDUs in flight (allocated from a fixed pool). */
#define BT_TX_POOL_COUNT 8U

/* send() backpressure: wait for a free PDU, bounded. */
#define BT_TX_RETRY_MS 5
#define BT_TX_TIMEOUT_MS 1000

/* recv() idle poll so the caller can observe its stop flag. */
#define BT_RX_WAIT_MS 500

#define BT_SERVICE_NAME_MAX 63U

/* Class of Device: exactly the value the working ZALink stack reports in
 * its CarPlay mode.  bt_gap_init() in libzablue.a loads 0x240000 and adds
 * 1032 (0x408) for BT_MODE=0: Audio + Rendering services, Audio/Video major
 * class, Hands-free minor class.  (Mode 1, the phone-like mode, is
 * 0x7A020C.)  Matching the reference byte-for-byte removes one more
 * classification difference for iOS; the symbolic field macros from
 * assigned_numbers.h are easy to shift differently from the vendor. */
#define RTCP_BT_LUBANLITE_COD 0x00240408U

/* ------------------------------------------------------------------ */
/* Extended Inquiry Response                                           */
/* ------------------------------------------------------------------ */

/* HCI_Write_Extended_Inquiry_Response (OGF 0x03 / OCF 0x0052).  This fork
 * does not define the opcode, so declare it here.  The parameter is
 * FEC_Required (1 byte) followed by the 240-byte EIR; BTstack builds the
 * same 241-byte layout in gap_run_set_eir_data().  A 240-byte-only command
 * is answered with status 0x12 (Invalid HCI Command Parameters). */
#define RTCP_BT_HCI_OP_WRITE_EIR BT_OP(BT_OGF_BASEBAND, 0x0052)
#define RTCP_BT_EIR_SIZE 240U
#define RTCP_BT_EIR_PARAM_SIZE (1U + RTCP_BT_EIR_SIZE)
#define RTCP_BT_EIR_TYPE_UUID128_COMPLETE 0x07
#define RTCP_BT_EIR_TYPE_NAME_SHORTENED 0x08
#define RTCP_BT_EIR_TYPE_NAME_COMPLETE 0x09
/* The reference stack publishes a third name element with this type; iOS
 * ignores unknown EIR types, and the working firmware sends it, so match. */
#define RTCP_BT_EIR_TYPE_NAME_VENDOR 0x30

/*
 * The wireless CarPlay receiver must announce the iAP2 service class in its
 * EIR.  The working ZALink stack does exactly that: libzablue.a
 * (bt_gap.c:bt_eir_data_init) writes a 0x07 element whose first UUID128 is
 * 00000000-deca-fade-deca-deafdecacafe before it becomes discoverable.
 *
 * The board log of 2026-11-09 (diag6) shows that the iAP2 UUID alone is not
 * enough: the iPhone still ran a generic SDP walk (search 0x0100, PnP 0x1200,
 * SDP server 0x1000) and disconnected with reason 0x13 without ever searching
 * the iAP2 UUID or opening RFCOMM.  The reference stack advertises four
 * UUID128s in this one 0x07 element, and these are those exact bytes.
 * Only the iAP2 UUID is registered in the SDP database, so the other three
 * are announcement-only, again as in the reference.
 *
 * Note that this announced UUID (...cafe) is the iPhone-side iAP2 service:
 * the reference stack's own RFCOMM server, the one a phone connects to, is
 * registered under ...caff (iap2_uuid_srv in libzablue.a).  EIR and SDP use
 * different UUIDs for their different roles; see IAP2_LINK_UUID in
 * iap2_link_lubanlite.c.
 *
 * HCI carries 128-bit UUIDs little-endian (LSO first).
 */
static const uint8_t s_eir_uuid128_list[64] = {
    /* 00000000-deca-fade-deca-deafdecacafe (iAP2) */
    0xfe, 0xca, 0xca, 0xde, 0xaf, 0xde, 0xca, 0xde,
    0xde, 0xfa, 0xca, 0xde, 0x00, 0x00, 0x00, 0x00,
    /* 02030302-1d19-415f-86f2-22a2106a0a77 */
    0x77, 0x0a, 0x6a, 0x10, 0xa2, 0x22, 0xf2, 0x86,
    0x5f, 0x41, 0x19, 0x1d, 0x02, 0x03, 0x03, 0x02,
    /* 2d8d2466-e14d-451c-88bc-7301abea291a */
    0x1a, 0x29, 0xea, 0xab, 0x01, 0x73, 0xbc, 0x88,
    0x1c, 0x45, 0x4d, 0xe1, 0x66, 0x24, 0x8d, 0x2d,
    /* ec884348-cd41-40a2-9727-575d50bf1fd3 */
    0xd3, 0x1f, 0xbf, 0x50, 0x5d, 0x57, 0x27, 0x97,
    0xa2, 0x40, 0x41, 0xcd, 0x48, 0x43, 0x88, 0xec,
};

static size_t eir_put_name(uint8_t *eir, size_t pos, uint8_t type,
                           const char *name, size_t len)
{
    if (len == 0U || pos + 2U + len > RTCP_BT_EIR_PARAM_SIZE) {
        return pos;
    }
    eir[pos++] = (uint8_t)(len + 1U);
    eir[pos++] = type;
    memcpy(&eir[pos], name, len);
    return pos + len;
}

static void write_eir(const char *name)
{
    struct net_buf *buf = bt_hci_cmd_alloc(K_FOREVER);
    if (buf == NULL) {
        RTCP_LOGW(BT_TAG, "EIR buffer allocation failed");
        return;
    }

    /* Byte 0 is FEC_Required (0 = FEC not required); the EIR data follows. */
    uint8_t *eir = net_buf_add(buf, RTCP_BT_EIR_PARAM_SIZE);
    memset(eir, 0, RTCP_BT_EIR_PARAM_SIZE);

    /* Elements: complete name, the 0x07 UUID list (66 bytes), shortened
     * name, vendor-type name.  Keep the three name copies within 240 bytes. */
    size_t name_len = (name != NULL) ? strlen(name) : 0U;
    if (name_len > (RTCP_BT_EIR_SIZE - 72U) / 3U) {
        name_len = (RTCP_BT_EIR_SIZE - 72U) / 3U;
    }

    size_t pos = 1U;
    pos = eir_put_name(eir, pos, RTCP_BT_EIR_TYPE_NAME_COMPLETE, name,
                       name_len);

    eir[pos++] = 1U + sizeof(s_eir_uuid128_list); /* 0x41: type + 4 UUIDs */
    eir[pos++] = RTCP_BT_EIR_TYPE_UUID128_COMPLETE;
    memcpy(&eir[pos], s_eir_uuid128_list, sizeof(s_eir_uuid128_list));
    pos += sizeof(s_eir_uuid128_list);

    pos = eir_put_name(eir, pos, RTCP_BT_EIR_TYPE_NAME_SHORTENED, name,
                       name_len);
    pos = eir_put_name(eir, pos, RTCP_BT_EIR_TYPE_NAME_VENDOR, name,
                       name_len);

    int err = bt_hci_cmd_send_sync(RTCP_BT_HCI_OP_WRITE_EIR, buf, NULL);
    if (err != 0) {
        RTCP_LOGW(BT_TAG, "write EIR failed (%d)", err);
    } else {
        RTCP_LOGI(BT_TAG, "EIR: iAP2 + 3 service UUIDs advertised (%u bytes)",
                  (unsigned)pos);
    }
}

/*
 * Zephyr leaves the controller's default inquiry scan parameters in place:
 * interval 0x1000 (2.56 s) and window 0x0012 (11.25 ms), a 0.44% duty cycle
 * (BT_BR_INQUIRY_SCAN_PARAM_DEFAULT).  An iPhone inquiry then hits the
 * accessory only once in a while, which matches the report that the device
 * is hard to discover.  The working ZALink stack configures page scan as
 * interval 0x50 (50 ms) / window 0x40 (40 ms), interlaced; use the same
 * 80% duty for both scans so discovery and connection setup are reliable.
 */
static void configure_scan(void)
{
    const struct bt_br_page_scan_param page = {
        .interval = 0x0050U,
        .window = 0x0040U,
        .type = BT_BR_SCAN_TYPE_INTERLACED,
    };
    const struct bt_br_inquiry_scan_param inquiry = {
        .interval = 0x0050U,
        .window = 0x0040U,
        .type = BT_BR_SCAN_TYPE_INTERLACED,
    };

    int err = bt_br_page_scan_update_param(&page);
    if (err != 0) {
        RTCP_LOGW(BT_TAG, "page scan parameters failed (%d)", err);
    }
    err = bt_br_inquiry_scan_update_param(&inquiry);
    if (err != 0) {
        RTCP_LOGW(BT_TAG, "inquiry scan parameters failed (%d)", err);
    }
}

enum bt_link_state {
    BT_LINK_FREE = 0,
    BT_LINK_CONNECTING,
    BT_LINK_CONNECTED,
    BT_LINK_DISCONNECTED,
};

struct bt_link {
    struct bt_rfcomm_dlc dlc;
    volatile int state;
    bool handed_out;

    uint8_t ring[BT_RX_RING_SIZE];
    size_t head; /* producer (Bluetooth RX thread) */
    size_t tail; /* consumer (application thread) */
    struct rt_mutex ring_lock;
    struct rt_semaphore rx_sem;

    struct rt_semaphore accept_sem;
};

static struct bt_link s_links[BT_LINK_MAX];
static struct rt_mutex s_tx_lock;
static struct bt_rfcomm_server s_server;

static bool s_ipc_ready;
static bool s_enabled;
static bool s_listening;
static bool s_stop;
static uint8_t s_rx_dropped;

static void (*s_event_cb)(int ev, void *user);
static void *s_event_user;

/* SDP record storage.  The attribute table is filled at runtime because the
 * service class UUID and the RFCOMM channel are caller/stack supplied; the
 * Zephyr encoder copies the values into the response, so only the pointers
 * need to stay valid. */
static struct bt_sdp_record s_sdp_record;
static struct bt_sdp_attribute s_sdp_attrs[9];
static struct bt_sdp_data_elem s_lang_elems[3];
static struct bt_sdp_data_elem s_browse_elems[1];
static struct bt_sdp_data_elem s_class_elems[1];
static struct bt_sdp_data_elem s_proto_elems[2];
static struct bt_sdp_data_elem s_l2cap_elems[1];
static struct bt_sdp_data_elem s_rfcomm_elems[2];
static struct bt_sdp_data_elem s_profile_elems[2];
static struct bt_sdp_data_elem s_profile_seq[1];
static uint32_t s_zero32;
static uint16_t s_lang_native = 0x656e; /* 'e' 'n' on the wire */
static uint16_t s_lang_code = 106;       /* UTF-8 MIB enum */
static uint16_t s_lang_base = BT_SDP_PRIMARY_LANG_BASE;
static uint16_t s_browse_uuid = BT_SDP_PUBLIC_BROWSE_GROUP;
static uint16_t s_spp_uuid = BT_SDP_SERIAL_PORT_SVCLASS;
static uint16_t s_l2cap_uuid = BT_SDP_PROTO_L2CAP;
static uint16_t s_rfcomm_uuid = BT_SDP_PROTO_RFCOMM;
static uint16_t s_profile_version = 0x0100;
static uint8_t s_service_availability = 0xFF;
static uint8_t s_rfcomm_channel;
static uint8_t s_uuid_le[16];
static char s_service_name[BT_SERVICE_NAME_MAX + 1U];

/* PnP Information (Device ID) record.  The diag4 board log shows the iPhone
 * walking generic SDP (search uuid16 0x1200) and dropping the link when that
 * search matched nothing.  The working ZALink stack does not publish a PnP
 * record and still connects -- its EIR names the iAP2 service class (see
 * write_eir), which is the actual fix.  This record is kept as a defensive
 * answer for the generic enumeration path, in the same shape as Zephyr's DID
 * service (subsys/bluetooth/host/classic/did.c).  The vendor/product values
 * are placeholders: no ArtInChip Bluetooth SIG company identifier is on
 * record here, so VendorID 0xFFFF is deliberately "unassigned". */
#define RTCP_BT_PNP_SPEC_ID       0x0103U  /* Device ID spec 1.3 */
#define RTCP_BT_PNP_VENDOR_SRC    0x0001U  /* VendorID = SIG company ID */
#define RTCP_BT_PNP_VENDOR_ID     0xFFFFU  /* unassigned */
#define RTCP_BT_PNP_PRODUCT_ID    0x0001U
#define RTCP_BT_PNP_VERSION       0x0001U

static struct bt_sdp_attribute s_pnp_attrs[] = {
    BT_SDP_NEW_SERVICE,
    BT_SDP_LIST(
        BT_SDP_ATTR_SVCLASS_ID_LIST,
        BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 3),
        BT_SDP_DATA_ELEM_LIST(
            { BT_SDP_TYPE_SIZE(BT_SDP_UUID16),
              BT_SDP_ARRAY_16(BT_SDP_PNP_INFO_SVCLASS) },
        )),
    BT_SDP_LIST(BT_SDP_ATTR_SPECIFICATION_ID,
                BT_SDP_TYPE_SIZE(BT_SDP_UINT16),
                BT_SDP_ARRAY_16(RTCP_BT_PNP_SPEC_ID)),
    BT_SDP_LIST(BT_SDP_ATTR_VENDOR_ID, BT_SDP_TYPE_SIZE(BT_SDP_UINT16),
                BT_SDP_ARRAY_16(RTCP_BT_PNP_VENDOR_ID)),
    BT_SDP_LIST(BT_SDP_ATTR_PRODUCT_ID, BT_SDP_TYPE_SIZE(BT_SDP_UINT16),
                BT_SDP_ARRAY_16(RTCP_BT_PNP_PRODUCT_ID)),
    BT_SDP_LIST(BT_SDP_ATTR_VERSION, BT_SDP_TYPE_SIZE(BT_SDP_UINT16),
                BT_SDP_ARRAY_16(RTCP_BT_PNP_VERSION)),
    BT_SDP_LIST(BT_SDP_ATTR_PRIMARY_RECORD, BT_SDP_TYPE_SIZE(BT_SDP_BOOL),
                BT_SDP_ARRAY_8(1)),
    BT_SDP_LIST(BT_SDP_ATTR_VENDOR_ID_SOURCE,
                BT_SDP_TYPE_SIZE(BT_SDP_UINT16),
                BT_SDP_ARRAY_16(RTCP_BT_PNP_VENDOR_SRC)),
};
static struct bt_sdp_record s_pnp_record = BT_SDP_RECORD(s_pnp_attrs);

NET_BUF_POOL_FIXED_DEFINE(bt_tx_pool, BT_TX_POOL_COUNT,
                          BT_RFCOMM_BUF_SIZE(BT_DLC_MTU),
                          LPKG_BT_CONN_TX_USER_DATA_SIZE, NULL);

static void emit_event(int ev)
{
    if (s_event_cb != NULL) s_event_cb(ev, s_event_user);
}

static void sdp_elem(struct bt_sdp_data_elem *elem, uint8_t type,
                     uint32_t size, uint32_t total, const void *data)
{
    elem->type = type;
    elem->data_size = size;
    elem->total_size = total;
    elem->data = data;
}

/* Parses "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (or 32 bare hex digits) into
 * canonical big-endian bytes. */
static bool parse_uuid128(const char *text, uint8_t out[16])
{
    if (text == NULL) return false;
    size_t digits = 0U;
    bool high = true;
    uint8_t value = 0U;
    for (const char *p = text; *p != '\0'; ++p) {
        char c = *p;
        if (c == '-') continue;
        uint8_t nibble;
        if (c >= '0' && c <= '9') {
            nibble = (uint8_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            nibble = (uint8_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            nibble = (uint8_t)(c - 'A' + 10);
        } else {
            return false;
        }
        if (high) {
            value = (uint8_t)(nibble << 4);
        } else {
            if (digits >= 16U) return false;
            out[digits++] = (uint8_t)(value | nibble);
        }
        high = !high;
    }
    return digits == 16U && high;
}

static void sdp_build_record(const char *name, const uint8_t uuid_le[16],
                             uint8_t channel)
{
    size_t name_len = (name != NULL) ? strlen(name) : 0U;
    if (name_len > BT_SERVICE_NAME_MAX) name_len = BT_SERVICE_NAME_MAX;
    if (name_len > 0U) memcpy(s_service_name, name, name_len);
    s_service_name[name_len] = '\0';

    s_rfcomm_channel = channel;

    sdp_elem(&s_lang_elems[0], BT_SDP_UINT16, 2, 3, &s_lang_native);
    sdp_elem(&s_lang_elems[1], BT_SDP_UINT16, 2, 3, &s_lang_code);
    sdp_elem(&s_lang_elems[2], BT_SDP_UINT16, 2, 3, &s_lang_base);
    sdp_elem(&s_browse_elems[0], BT_SDP_UUID16, 2, 3, &s_browse_uuid);
    sdp_elem(&s_l2cap_elems[0], BT_SDP_UUID16, 2, 3, &s_l2cap_uuid);
    sdp_elem(&s_rfcomm_elems[0], BT_SDP_UUID16, 2, 3, &s_rfcomm_uuid);
    sdp_elem(&s_rfcomm_elems[1], BT_SDP_UINT8, 1, 2, &s_rfcomm_channel);
    sdp_elem(&s_proto_elems[0], BT_SDP_SEQ8, 3, 5, s_l2cap_elems);
    sdp_elem(&s_proto_elems[1], BT_SDP_SEQ8, 5, 7, s_rfcomm_elems);

    uint32_t class_size;
    uint32_t class_total;
    if (uuid_le != NULL) {
        sdp_elem(&s_class_elems[0], BT_SDP_UUID128, 16, 17, uuid_le);
        class_size = 17U;
        class_total = 19U;
    } else {
        sdp_elem(&s_class_elems[0], BT_SDP_UUID16, 2, 3, &s_spp_uuid);
        class_size = 3U;
        class_total = 5U;
    }

    sdp_elem(&s_sdp_attrs[0].val, BT_SDP_UINT32, 4, 5, &s_zero32);
    s_sdp_attrs[0].id = BT_SDP_ATTR_RECORD_HANDLE;
    sdp_elem(&s_sdp_attrs[1].val, BT_SDP_UINT32, 4, 5, &s_zero32);
    s_sdp_attrs[1].id = BT_SDP_ATTR_RECORD_STATE;
    sdp_elem(&s_sdp_attrs[2].val, BT_SDP_SEQ8, 9, 11, s_lang_elems);
    s_sdp_attrs[2].id = BT_SDP_ATTR_LANG_BASE_ATTR_ID_LIST;
    sdp_elem(&s_sdp_attrs[3].val, BT_SDP_SEQ8, 3, 5, s_browse_elems);
    s_sdp_attrs[3].id = BT_SDP_ATTR_BROWSE_GRP_LIST;
    sdp_elem(&s_sdp_attrs[4].val, BT_SDP_SEQ8, class_size, class_total,
             s_class_elems);
    s_sdp_attrs[4].id = BT_SDP_ATTR_SVCLASS_ID_LIST;
    sdp_elem(&s_sdp_attrs[5].val, BT_SDP_SEQ8, 12, 14, s_proto_elems);
    s_sdp_attrs[5].id = BT_SDP_ATTR_PROTO_DESC_LIST;
    sdp_elem(&s_sdp_attrs[6].val, BT_SDP_TEXT_STR8, (uint32_t)name_len,
             (uint32_t)name_len + 2U, s_service_name);
    s_sdp_attrs[6].id = BT_SDP_ATTR_SVCNAME_PRIMARY;

    size_t attr_count = 7U;
    /* BluetoothProfileDescriptorList: the Serial Port profile (0x1101)
     * version 0x0100.  This matches the reference iAP record in BlueZ
     * (profiles/iap/main.c): iOS searches for 0x1101 after browsing our
     * record, and a zero-match search makes it drop the link.  The iAP2
     * UUID128 stays in the service class list above, where BlueZ keeps it
     * too.  s_spp_uuid is BT_SDP_SERIAL_PORT_SVCLASS. */
    sdp_elem(&s_profile_elems[0], BT_SDP_UUID16, 2, 3, &s_spp_uuid);
    sdp_elem(&s_profile_elems[1], BT_SDP_UINT16, 2, 3, &s_profile_version);
    sdp_elem(&s_profile_seq[0], BT_SDP_SEQ8, 6, 8, s_profile_elems);

    /* ServiceAvailability 0xFF: the record answers while the accessory is in
     * range, which is the value the BlueZ iAP record publishes. */
    sdp_elem(&s_sdp_attrs[7].val, BT_SDP_UINT8, 1, 2,
             &s_service_availability);
    s_sdp_attrs[7].id = BT_SDP_ATTR_SERVICE_AVAILABILITY;
    sdp_elem(&s_sdp_attrs[8].val, BT_SDP_SEQ8, 8, 10, s_profile_seq);
    s_sdp_attrs[8].id = BT_SDP_ATTR_PROFILE_DESC_LIST;
    attr_count = 9U;

    s_sdp_record.attrs = s_sdp_attrs;
    s_sdp_record.attr_count = attr_count;
    s_sdp_record.handle = 0U;
    s_sdp_record.index = 0U;
}

static void ring_push(struct bt_link *link, const uint8_t *data, size_t len)
{
    bool pushed = false;
    rt_mutex_take(&link->ring_lock, RT_WAITING_FOREVER);
    while (len-- > 0U) {
        size_t next = (link->head + 1U) % BT_RX_RING_SIZE;
        if (next == link->tail) {
            if (s_rx_dropped < 255U) s_rx_dropped++;
            break;
        }
        link->ring[link->head] = *data++;
        link->head = next;
        pushed = true;
    }
    rt_mutex_release(&link->ring_lock);
    if (pushed) rt_sem_release(&link->rx_sem);
}

static void link_connected_cb(struct bt_rfcomm_dlc *dlc)
{
    for (size_t i = 0U; i < BT_LINK_MAX; ++i) {
        if (&s_links[i].dlc == dlc) {
            s_links[i].state = BT_LINK_CONNECTED;
            rt_sem_release(&s_links[i].accept_sem);
            break;
        }
    }
    RTCP_LOGI(BT_TAG, "RFCOMM DLC connected");
    emit_event(RTCP_BT_EV_CONNECTED);
}

static void link_disconnected_cb(struct bt_rfcomm_dlc *dlc)
{
    for (size_t i = 0U; i < BT_LINK_MAX; ++i) {
        if (&s_links[i].dlc == dlc) {
            s_links[i].state = BT_LINK_DISCONNECTED;
            /* Wake a blocked accept() and recv() so the application can
             * recycle the slot. */
            rt_sem_release(&s_links[i].accept_sem);
            rt_sem_release(&s_links[i].rx_sem);
            break;
        }
    }
    RTCP_LOGI(BT_TAG, "RFCOMM DLC disconnected");
    emit_event(RTCP_BT_EV_DISCONNECTED);
}

static void link_recv_cb(struct bt_rfcomm_dlc *dlc, struct net_buf *buf)
{
    for (size_t i = 0U; i < BT_LINK_MAX; ++i) {
        if (&s_links[i].dlc == dlc) {
            /* The L2CAP layer unrefs the buffer after this callback, so the
             * bytes must be copied here. */
            ring_push(&s_links[i], buf->data, buf->len);
            break;
        }
    }
}

static void link_sent_cb(struct bt_rfcomm_dlc *dlc, int err)
{
    (void)dlc;
    if (err != 0) RTCP_LOGW(BT_TAG, "RFCOMM send completion error %d", err);
}

static struct bt_rfcomm_dlc_ops s_link_ops = {
    .connected = link_connected_cb,
    .disconnected = link_disconnected_cb,
    .recv = link_recv_cb,
    .sent = link_sent_cb,
};

static int server_accept_cb(struct bt_conn *conn, struct bt_rfcomm_server *server,
                            struct bt_rfcomm_dlc **dlc)
{
    (void)conn;
    (void)server;
    for (size_t i = 0U; i < BT_LINK_MAX; ++i) {
        if (s_links[i].state == BT_LINK_FREE) {
            s_links[i].state = BT_LINK_CONNECTING;
            s_links[i].handed_out = false;
            s_links[i].head = 0U;
            s_links[i].tail = 0U;
            *dlc = &s_links[i].dlc;
            return 0;
        }
    }
    RTCP_LOGW(BT_TAG, "no free RFCOMM link slot");
    return -1;
}

int rtcp_bt_lubanlite_enable(const char *name, const uint8_t bdaddr[6])
{
    /* The controller keeps its own BR/EDR address; Zephyr exposes no public
     * API to override it at runtime, so bdaddr is informational only. */
    (void)bdaddr;
    if (!s_ipc_ready) {
        rt_mutex_init(&s_tx_lock, "bt-tx", RT_IPC_FLAG_FIFO);
        for (size_t i = 0U; i < BT_LINK_MAX; ++i) {
            rt_mutex_init(&s_links[i].ring_lock, "bt-rx", RT_IPC_FLAG_FIFO);
            rt_sem_init(&s_links[i].rx_sem, "bt-rx", 0, RT_IPC_FLAG_FIFO);
            rt_sem_init(&s_links[i].accept_sem, "bt-acc", 0, RT_IPC_FLAG_FIFO);
            s_links[i].dlc.ops = &s_link_ops;
            s_links[i].dlc.mtu = BT_DLC_MTU;
            s_links[i].state = BT_LINK_FREE;
        }
        s_ipc_ready = true;
    }

    if (s_enabled) return RTCP_OK;

    RTCP_LOGI(BT_TAG, "enabling Bluetooth (H5 setup + firmware download)");
    int err = bt_enable(NULL);
    if (err != 0 && err != -EALREADY) {
        RTCP_LOGE(BT_TAG, "bt_enable failed (%d)", err);
        return RTCP_EIO;
    }

#if defined(CONFIG_BT_SETTINGS)
    /* The iPhone keeps its half of a bond across our reboots.  Restore the
     * link keys before the device becomes connectable; otherwise iOS finds
     * the device but every reconnect fails until it is forgotten and paired
     * again.  Nothing in this SDK runs settings_subsys_init(), so do it here
     * (it loads /data/bt_settings.bin). */
    err = settings_subsys_init();
    if (err != 0) {
        RTCP_LOGW(BT_TAG, "settings_subsys_init failed (%d)", err);
    }
    err = settings_load();
    if (err != 0) {
        RTCP_LOGW(BT_TAG, "settings_load failed (%d)", err);
    } else {
        RTCP_LOGI(BT_TAG, "Bluetooth settings loaded");
    }
#endif

    configure_scan();

    err = bt_br_set_connectable(true, NULL);
    if (err != 0 && err != -EALREADY) {
        RTCP_LOGE(BT_TAG, "set connectable failed (%d)", err);
        return RTCP_EIO;
    }

    err = bt_br_set_class_of_device(RTCP_BT_LUBANLITE_COD);
    if (err != 0) {
        RTCP_LOGW(BT_TAG, "set class of device failed (%d)", err);
    }

    if (name != NULL && name[0] != '\0') {
        /* With LPKG_BT_DEVICE_NAME_DYNAMIC, bt_set_name() issues the HCI
         * Write_Local_Name for Classic once the host is ready, so this is
         * the name the phone sees during discovery. */
        err = bt_set_name(name);
        if (err != 0) {
            RTCP_LOGW(BT_TAG, "set local name failed (%d)", err);
        }
    }

    /* The EIR must exist before the controller answers inquiries; include
     * the iAP2 service class here or iOS never starts the iAP2 flow (see
     * the comment on write_eir). */
    write_eir(name);

    err = bt_br_set_discoverable(true, false);
    if (err != 0 && err != -EALREADY) {
        RTCP_LOGW(BT_TAG, "set discoverable failed (%d)", err);
    }

    s_enabled = true;
    RTCP_LOGI(BT_TAG, "Bluetooth enabled (connectable + discoverable)");
    return RTCP_OK;
}

int rtcp_bt_lubanlite_rfcomm_listen(uint8_t channel, const char *name,
                                    const char *uuid128, uint8_t *out_channel)
{
    if (out_channel == NULL) return RTCP_EINVAL;
    if (!s_enabled) return RTCP_ESTATE;

    if (s_listening) {
        /* Re-arm: a previous disconnect() set s_stop, and the application
         * restarting the service expects accept() to block again. */
        s_stop = false;
        *out_channel = s_server.channel;
        return RTCP_OK;
    }

    uint8_t uuid[16];
    bool have_uuid = false;
    if (uuid128 != NULL && uuid128[0] != '\0') {
        if (!parse_uuid128(uuid128, uuid)) return RTCP_EINVAL;
        /* Zephyr's SDP encoder byte-swaps UUID128 values, so store the
         * reverse of the canonical form. */
        for (size_t i = 0U; i < 16U; ++i) s_uuid_le[i] = uuid[15U - i];
        have_uuid = true;
    }

    s_server.channel = channel;
    s_server.accept = server_accept_cb;
    int err = bt_rfcomm_server_register(&s_server);
    if (err != 0) {
        RTCP_LOGE(BT_TAG, "RFCOMM server register failed (%d)", err);
        s_server.channel = 0U;
        return RTCP_EIO;
    }

    sdp_build_record(name, have_uuid ? s_uuid_le : NULL, s_server.channel);
    err = bt_sdp_register_service(&s_sdp_record);
    if (err != 0) {
        RTCP_LOGE(BT_TAG, "SDP register failed (%d)", err);
        (void)bt_rfcomm_server_unregister(&s_server);
        s_server.channel = 0U;
        return RTCP_EIO;
    }

    /* The iPhone searches 0x1200 (PnP Information) right after the iAP2
     * record; see the record comment above.  A failure here only costs the
     * iOS path, so keep the iAP2 service registered and log it. */
    err = bt_sdp_register_service(&s_pnp_record);
    if (err != 0) {
        RTCP_LOGE(BT_TAG, "PnP SDP register failed (%d)", err);
    }

    s_listening = true;
    s_stop = false;
    *out_channel = s_server.channel;
    RTCP_LOGI(BT_TAG, "RFCOMM listening on channel %u (uuid %s)",
              s_server.channel, have_uuid ? uuid128 : "SPP");
    return RTCP_OK;
}

int rtcp_bt_lubanlite_rfcomm_accept(int lfd, int *fd)
{
    if (lfd != 1 || fd == NULL) return RTCP_EINVAL;
    if (!s_listening) return RTCP_ESTATE;

    for (;;) {
        rt_err_t err = rt_sem_take(&s_links[0].accept_sem,
                                   rt_tick_from_millisecond(500));
        if (s_stop) return RTCP_EIO;
        if (err != RT_EOK) continue;

        for (size_t i = 0U; i < BT_LINK_MAX; ++i) {
            struct bt_link *link = &s_links[i];
            if (link->state == BT_LINK_CONNECTED && !link->handed_out) {
                link->handed_out = true;
                *fd = (int)i + 1;
                return RTCP_OK;
            }
        }
        /* A disconnected slot is reusable once the reader has drained it. */
        for (size_t i = 0U; i < BT_LINK_MAX; ++i) {
            struct bt_link *link = &s_links[i];
            if (link->state == BT_LINK_DISCONNECTED) {
                link->state = BT_LINK_FREE;
                link->head = 0U;
                link->tail = 0U;
            }
        }
    }
}

static int link_send_chunk(struct bt_link *link, const uint8_t *buf, size_t len)
{
    uint32_t waited = 0U;
    for (;;) {
        struct net_buf *pdu = bt_rfcomm_create_pdu(&bt_tx_pool);
        if (pdu != NULL) {
            net_buf_add_mem(pdu, buf, len);
            int err = bt_rfcomm_dlc_send(&link->dlc, pdu);
            if (err >= 0) return 0;
            net_buf_unref(pdu);
            if (err != -ENOMEM && err != -EAGAIN) return err;
        }
        if (waited >= BT_TX_TIMEOUT_MS) return -ETIMEDOUT;
        rt_thread_mdelay(BT_TX_RETRY_MS);
        waited += BT_TX_RETRY_MS;
    }
}

int rtcp_bt_lubanlite_send(int fd, const void *buf, size_t len)
{
    if (fd < 1 || fd > (int)BT_LINK_MAX || buf == NULL) return RTCP_EINVAL;
    if (len == 0U) return 0;

    struct bt_link *link = &s_links[fd - 1];
    if (link->state != BT_LINK_CONNECTED) return RTCP_ESTATE;

    const uint8_t *p = buf;
    size_t sent = 0U;
    rt_mutex_take(&s_tx_lock, RT_WAITING_FOREVER);
    while (sent < len) {
        size_t chunk = len - sent;
        if (chunk > link->dlc.mtu) chunk = link->dlc.mtu;
        int err = link_send_chunk(link, p + sent, chunk);
        if (err != 0) {
            rt_mutex_release(&s_tx_lock);
            RTCP_LOGW(BT_TAG, "RFCOMM send failed (%d) after %u bytes",
                      err, (unsigned)sent);
            return sent > 0U ? (int)sent : RTCP_EIO;
        }
        sent += chunk;
    }
    rt_mutex_release(&s_tx_lock);
    return (int)sent;
}

static size_t link_drain(struct bt_link *link, void *buf, size_t len,
                         bool *closed)
{
    size_t copied = 0U;
    uint8_t *out = buf;
    rt_mutex_take(&link->ring_lock, RT_WAITING_FOREVER);
    while (copied < len && link->tail != link->head) {
        out[copied++] = link->ring[link->tail];
        link->tail = (link->tail + 1U) % BT_RX_RING_SIZE;
    }
    *closed = (link->state != BT_LINK_CONNECTED) &&
              (link->tail == link->head);
    rt_mutex_release(&link->ring_lock);
    return copied;
}

int rtcp_bt_lubanlite_recv_timeout(int fd, void *buf, size_t len,
                                   int timeout_ms)
{
    if (fd < 1 || fd > (int)BT_LINK_MAX || buf == NULL) return RTCP_EINVAL;
    if (len == 0U) return 0;

    struct bt_link *link = &s_links[fd - 1];

    /* Drain first: a bulk read often leaves bytes in the ring, and the
     * semaphore count does not track how much is still queued. */
    bool closed = false;
    size_t copied = link_drain(link, buf, len, &closed);
    if (copied > 0U) return (int)copied;
    if (closed) return 0;

    rt_tick_t wait = timeout_ms > 0
                         ? rt_tick_from_millisecond((rt_int32_t)timeout_ms)
                         : 0;
    if (rt_sem_take(&link->rx_sem, wait) != RT_EOK) {
        /* No data yet; let the caller run its periodic work. */
        return RTCP_ETIMEDOUT;
    }

    copied = link_drain(link, buf, len, &closed);
    if (copied > 0U) return (int)copied;
    return closed ? 0 : RTCP_ETIMEDOUT;
}

int rtcp_bt_lubanlite_recv(int fd, void *buf, size_t len)
{
    return rtcp_bt_lubanlite_recv_timeout(fd, buf, len, BT_RX_WAIT_MS);
}

void rtcp_bt_lubanlite_set_event_cb(void (*cb)(int ev, void *user), void *user)
{
    s_event_cb = cb;
    s_event_user = user;
}

void rtcp_bt_lubanlite_drop_link(int fd)
{
    if (fd < 1 || fd > (int)BT_LINK_MAX) return;
    struct bt_link *link = &s_links[fd - 1];
    if (link->state == BT_LINK_CONNECTED) {
        (void)bt_rfcomm_dlc_disconnect(&link->dlc);
    }
}

void rtcp_bt_lubanlite_disconnect(void)
{
    s_stop = true;
    if (!s_ipc_ready) return;
    for (size_t i = 0U; i < BT_LINK_MAX; ++i) {
        struct bt_link *link = &s_links[i];
        rt_sem_release(&link->accept_sem);
        rt_sem_release(&link->rx_sem);
        if (link->state == BT_LINK_CONNECTED) {
            (void)bt_rfcomm_dlc_disconnect(&link->dlc);
        }
    }
}

bool rtcp_bt_lubanlite_is_up(void)
{
    return s_enabled;
}

bool rtcp_bt_lubanlite_link_up(void)
{
    if (!s_ipc_ready) return false;
    for (size_t i = 0U; i < BT_LINK_MAX; ++i) {
        if (s_links[i].state == BT_LINK_CONNECTED) return true;
    }
    return false;
}

uint8_t rtcp_bt_lubanlite_channel(void)
{
    return s_listening ? s_server.channel : 0U;
}

unsigned rtcp_bt_lubanlite_rx_dropped(void)
{
    return s_rx_dropped;
}

int rtcp_bt_lubanlite_local_bdaddr(uint8_t address[6])
{
    if (address == NULL) return RTCP_EINVAL;
    if (!s_enabled) return RTCP_ESTATE;

    struct bt_br_oob oob;
    int err = bt_br_oob_get_local(&oob);
    if (err != 0) {
        RTCP_LOGW(BT_TAG, "cannot read the local BR/EDR address (%d)", err);
        return RTCP_EIO;
    }
    /* bt_addr_t stores the address little-endian (val[0] is the last
     * printed octet); iAP2 wants the printed order. */
    for (size_t i = 0U; i < 6U; ++i) {
        address[i] = oob.addr.val[5U - i];
    }
    return RTCP_OK;
}

#else /* LPKG_RT_CARPLAY_LUBANLITE_BT */

int rtcp_bt_lubanlite_enable(const char *name, const uint8_t bdaddr[6])
{
    (void)name;
    (void)bdaddr;
    return RTCP_ENOTSUP;
}

int rtcp_bt_lubanlite_rfcomm_listen(uint8_t channel, const char *name,
                                    const char *uuid128, uint8_t *out_channel)
{
    (void)channel;
    (void)name;
    (void)uuid128;
    (void)out_channel;
    return RTCP_ENOTSUP;
}

int rtcp_bt_lubanlite_rfcomm_accept(int lfd, int *fd)
{
    (void)lfd;
    (void)fd;
    return RTCP_ENOTSUP;
}

int rtcp_bt_lubanlite_send(int fd, const void *buf, size_t len)
{
    (void)fd;
    (void)buf;
    (void)len;
    return RTCP_ENOTSUP;
}

int rtcp_bt_lubanlite_recv(int fd, void *buf, size_t len)
{
    (void)fd;
    (void)buf;
    (void)len;
    return RTCP_ENOTSUP;
}

int rtcp_bt_lubanlite_recv_timeout(int fd, void *buf, size_t len,
                                   int timeout_ms)
{
    (void)fd;
    (void)buf;
    (void)len;
    (void)timeout_ms;
    return RTCP_ENOTSUP;
}

void rtcp_bt_lubanlite_set_event_cb(void (*cb)(int ev, void *user), void *user)
{
    (void)cb;
    (void)user;
}

void rtcp_bt_lubanlite_drop_link(int fd)
{
    (void)fd;
}

void rtcp_bt_lubanlite_disconnect(void)
{
}

bool rtcp_bt_lubanlite_is_up(void)
{
    return false;
}

bool rtcp_bt_lubanlite_link_up(void)
{
    return false;
}

uint8_t rtcp_bt_lubanlite_channel(void)
{
    return 0U;
}

unsigned rtcp_bt_lubanlite_rx_dropped(void)
{
    return 0U;
}

int rtcp_bt_lubanlite_local_bdaddr(uint8_t address[6])
{
    (void)address;
    return RTCP_ENOTSUP;
}

#endif /* LPKG_RT_CARPLAY_LUBANLITE_BT */

const struct rtcp_bt rtcp_bt_lubanlite = {
    .enable = rtcp_bt_lubanlite_enable,
    .rfcomm_listen = rtcp_bt_lubanlite_rfcomm_listen,
    .rfcomm_accept = rtcp_bt_lubanlite_rfcomm_accept,
    .send = rtcp_bt_lubanlite_send,
    .recv = rtcp_bt_lubanlite_recv,
    .set_event_cb = rtcp_bt_lubanlite_set_event_cb,
};
