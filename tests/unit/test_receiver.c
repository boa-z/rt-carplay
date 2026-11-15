/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "rtcp/receiver.h"
#include "rtcp_linux.h"
#include "airplay_ed25519.h"
#include "airplay_info_plist.h"
#include "rtcp/receiver_compat.h"
#include "airplay_rtsp_server.h"

static airplay_ed25519_keypair_t key;
static rtcp_receiver_t *receiver;
static unsigned events;
static bool random_bytes(uint8_t *b, size_t n, void *c)
{ (void)c; return rtcp_os_linux.random(b, n); }
static bool sign_bytes(const uint8_t *b, size_t n, uint8_t s[64], void *c)
{ (void)c; return airplay_ed25519_sign(&key, b, n, s); }
static bool verify_bytes(const uint8_t k[32], const uint8_t *b, size_t n,
                         const uint8_t s[64], void *c)
{ (void)c; return airplay_ed25519_verify(k, b, n, s); }
static bool lookup(const uint8_t *id, size_t n, uint8_t k[32], void *c)
{ (void)id; (void)n; (void)k; (void)c; return false; }
static bool save(const uint8_t *id, size_t n, const uint8_t k[32], void *c)
{ (void)id; (void)n; (void)k; (void)c; return false; }
static void on_state(const rtcp_receiver_state_t *s, void *c)
{
    (void)c;
    rtcp_receiver_state_t copy;
    ++events;
    assert(rtcp_receiver_get_state(receiver, &copy) == RTCP_OK);
    assert(copy.running == s->running);
    assert(rtcp_receiver_start(receiver) == RTCP_ESTATE);
    assert(rtcp_receiver_stop(receiver) == RTCP_ESTATE);
    assert(rtcp_receiver_destroy(receiver) == RTCP_ESTATE);
    assert(rtcp_receiver_poll(receiver, 0) == RTCP_ESTATE);
}
static void expect_empty_diagnostics(rtcp_receiver_t *receiver)
{
    rtcp_receiver_diagnostics_t diagnostics;
    memset(&diagnostics, 0xa5, sizeof(diagnostics));
    assert(rtcp_receiver_get_diagnostics(receiver, &diagnostics) == RTCP_OK);
    assert(diagnostics.screen.frames == 0 && diagnostics.screen.wire_bytes == 0);
    assert(diagnostics.audio.active_streams == 0 && diagnostics.audio.datagrams == 0);
    assert(!diagnostics.timing.active && diagnostics.timing.requests_sent == 0);
    assert(!diagnostics.single_cipher_frame);
    rtcp_addr_t peer, zero = {0};
    memset(&peer, 0xa5, sizeof(peer));
    assert(rtcp_receiver_get_peer(receiver, &peer) == RTCP_ENOENT);
    assert(memcmp(&peer, &zero, sizeof(peer)) == 0);
}
static int failed_listen(uint8_t family, uint16_t port)
{ (void)family; (void)port; return RTCP_EIO; }
static bool raw_body(rtcp_data_stream_kind_t kind, const uint8_t *b, size_t n, void *ctx)
{ (void)kind; (void)b; (void)n; (void)ctx; return true; }

int main(void)
{
    uint8_t seed[32] = {1}; /* Public test fixture, never a device identity. */
    assert(airplay_ed25519_keypair_from_seed(seed, &key));
    uint8_t plist[AIRPLAY_INFO_PLIST_MAX_BYTES];
    size_t length;
    airplay_info_plist_config_t info = {.device_id = "02:00:00:00:00:01",
        .name = "receiver-test", .width_pixels = 800, .height_pixels = 480};
    assert(airplay_info_plist_build(&info, plist, sizeof(plist), &length));
    rtcp_receiver_config_t config = {.on_state = on_state};
    struct rtcp_net net = rtcp_net_linux;
    config.platform.os = &rtcp_os_linux; config.platform.net = &net;
    config.platform.info_plist = plist; config.platform.info_plist_len = length;
    config.port = 17081;
    static const uint8_t id[] = "receiver-test";
    config.platform.pairing.pair_setup = (rtcp_pair_setup_provider_t){
        .pairing_code = "3939", .accessory_identifier = id,
        .accessory_identifier_len = sizeof(id)-1, .accessory_public_key = key.public_key,
        .random = random_bytes, .sign_accessory = sign_bytes,
        .verify_controller = verify_bytes, .save_pairing = save};
    config.platform.pairing.pair_verify = (rtcp_pair_verify_provider_t){
        .accessory_identifier = id, .accessory_identifier_len = sizeof(id)-1,
        .random = random_bytes, .sign_accessory = sign_bytes,
        .verify_controller = verify_bytes, .lookup_controller = lookup};
    assert(rtcp_receiver_create(NULL, &receiver) == RTCP_EINVAL);
    assert(receiver == NULL);
    config.family = 99;
    assert(rtcp_receiver_create(&config, &receiver) == RTCP_EINVAL && receiver == NULL);
    config.family = RTCP_AF_INET;
    config.data_stream.channel_mask = RTCP_DATA_STREAM_ALL_MASK;
    assert(rtcp_receiver_create(&config, &receiver) == RTCP_EINVAL && receiver == NULL);
    config.data_stream.channel_mask = 1U << 12;
    assert(rtcp_receiver_create(&config, &receiver) == RTCP_EINVAL && receiver == NULL);
    config.data_stream.channel_mask = 0;
    rtcp_vehicle_characteristic_t characteristic = {
        .accessory_iid = 1, .accessory_type = 100, .service_iid = 2, .service_type = 200,
        .iid = 3, .type = 1003, .format = RTCP_VEHICLE_UINT32, .owner = RTCP_VEHICLE_CAR};
    rtcp_vehicle_config_t vehicle_config = {.struct_size = sizeof(vehicle_config), .plugin_name = "test",
        .characteristics = &characteristic, .count = 1};
    rtcp_vehicle_t *vehicle;
    assert(rtcp_vehicle_create(&vehicle_config, &vehicle) == RTCP_OK);
    config.vehicle = vehicle;
    config.data_stream.channel_mask = RTCP_DATA_STREAM_MASK(RTCP_DATA_STREAM_VEHICLE);
    config.data_stream.on_body = raw_body;
    assert(rtcp_receiver_create(&config, &receiver) == RTCP_EINVAL && receiver == NULL);
    config.data_stream.channel_mask = 0;
    assert(rtcp_receiver_create(&config, &receiver) == RTCP_OK);
    rtcp_vehicle_delivery_status_t delivery;
    assert(rtcp_receiver_get_vehicle_delivery_status(receiver, &delivery) == RTCP_OK && delivery.attached);
    rtcp_data_stream_status_t attached_stream;
    assert(rtcp_receiver_get_data_stream_status(receiver, RTCP_DATA_STREAM_VEHICLE_HIGH, &attached_stream) == RTCP_OK);
    assert(attached_stream.registered && !attached_stream.offered && attached_stream.state == RTCP_DATA_STREAM_IDLE);
    assert(rtcp_receiver_destroy(receiver) == RTCP_OK); receiver = NULL;
    config.vehicle = NULL; rtcp_vehicle_destroy(vehicle);
    rtcp_ui_sync_config_t ui_config={.struct_size=sizeof(ui_config),.display_uuid="display"};
    rtcp_ui_sync_t *ui;
    assert(rtcp_ui_sync_create(&ui_config,&ui) == RTCP_OK);
    config.ui_sync=ui; config.data_stream.channel_mask=RTCP_DATA_STREAM_MASK(RTCP_DATA_STREAM_UI_SYNC);
    assert(rtcp_receiver_create(&config,&receiver) == RTCP_EINVAL && receiver == NULL);
    config.data_stream.channel_mask=RTCP_DATA_STREAM_MASK(RTCP_DATA_STREAM_VEHICLE);
    assert(rtcp_receiver_create(&config,&receiver) == RTCP_OK);
    rtcp_ui_sync_delivery_status_t ui_delivery;
    assert(rtcp_receiver_get_ui_sync_delivery_status(receiver,&ui_delivery) == RTCP_OK && ui_delivery.attached);
    assert(rtcp_receiver_get_data_stream_status(receiver,RTCP_DATA_STREAM_UI_SYNC,&attached_stream) == RTCP_OK && attached_stream.registered);
    assert(rtcp_receiver_destroy(receiver) == RTCP_OK); receiver=NULL;
    rtcp_ui_sync_destroy(ui); config.ui_sync=NULL; config.data_stream.channel_mask=0; config.data_stream.on_body=NULL;
    assert(rtcp_receiver_create(&config, &receiver) == RTCP_OK);
    assert(rtcp_receiver_get_ui_sync_delivery_status(receiver,&ui_delivery) == RTCP_OK && !ui_delivery.attached);
    assert(rtcp_receiver_get_ui_sync_delivery_status(NULL,&ui_delivery) == RTCP_EINVAL);
    assert(rtcp_receiver_get_ui_sync_delivery_status(receiver,NULL) == RTCP_EINVAL);
    assert(rtcp_receiver_get_vehicle_delivery_status(receiver, &delivery) == RTCP_OK && !delivery.attached);
    assert(rtcp_receiver_get_vehicle_delivery_status(NULL, &delivery) == RTCP_EINVAL);
    assert(rtcp_receiver_get_vehicle_delivery_status(receiver, NULL) == RTCP_EINVAL);
    rtcp_receiver_diagnostics_t diagnostics;
    rtcp_addr_t peer;
    assert(rtcp_receiver_get_diagnostics(NULL, &diagnostics) == RTCP_EINVAL);
    assert(rtcp_receiver_get_diagnostics(receiver, NULL) == RTCP_EINVAL);
    assert(rtcp_receiver_get_peer(NULL, &peer) == RTCP_EINVAL);
    assert(rtcp_receiver_get_peer(receiver, NULL) == RTCP_EINVAL);
    assert(rtcp_receiver_set_single_cipher_frame(NULL, true) == RTCP_EINVAL);
    assert(rtcp_receiver_set_single_cipher_frame(receiver, true) == RTCP_ESTATE);
    expect_empty_diagnostics(receiver);
    rtcp_data_stream_status_t stream_status;
    assert(rtcp_receiver_get_data_stream_status(receiver, RTCP_DATA_STREAM_VEHICLE, &stream_status) == RTCP_OK);
    assert(!stream_status.registered && !stream_status.offered && stream_status.state == RTCP_DATA_STREAM_IDLE);
    assert(rtcp_receiver_get_data_stream_status(NULL, RTCP_DATA_STREAM_VEHICLE, &stream_status) == RTCP_EINVAL);
    assert(rtcp_receiver_get_data_stream_status(receiver, RTCP_DATA_STREAM_COUNT, &stream_status) == RTCP_EINVAL);
    assert(rtcp_receiver_send_data_stream(receiver, RTCP_DATA_STREAM_VEHICLE, NULL, 0) == RTCP_ESTATE);
    assert(rtcp_receiver_send_data_stream(receiver, RTCP_DATA_STREAM_COUNT, NULL, 0) == RTCP_EINVAL);
    assert(!rtcp_receiver_request_siri(receiver));
    assert(!rtcp_receiver_send_iap(receiver, id, sizeof(id)));
    assert(!rtcp_receiver_send_hid_report(receiver, 1, id, sizeof(id)));
    memset(plist, 0, length); /* create must own its /info copy. */
    config.port = 17082; /* configuration structure was copied too. */
    assert(rtcp_receiver_poll(receiver, 0) == RTCP_ESTATE);
    net.listen = failed_listen;
    assert(rtcp_receiver_start(receiver) == RTCP_EIO);
    rtcp_receiver_state_t state;
    assert(rtcp_receiver_get_state(receiver, &state) == RTCP_OK);
    assert(!state.running && state.generation == 0 && state.last_error == RTCP_EIO);
    net = rtcp_net_linux;
    for (unsigned cycle = 0; cycle < 32; ++cycle) {
        assert(rtcp_receiver_start(receiver) == RTCP_OK);
        assert(rtcp_receiver_start(receiver) == RTCP_OK);
        assert(rtcp_receiver_port(receiver) == 17081);
        expect_empty_diagnostics(receiver);
        assert(rtcp_receiver_set_single_cipher_frame(receiver, true) == RTCP_OK);
        assert(rtcp_receiver_get_diagnostics(receiver, &diagnostics) == RTCP_OK);
        assert(diagnostics.single_cipher_frame);
        assert(rtcp_receiver_set_single_cipher_frame(receiver, false) == RTCP_OK);
        assert(rtcp_receiver_get_diagnostics(receiver, &diagnostics) == RTCP_OK);
        assert(!diagnostics.single_cipher_frame);
        rtcp_addr_t addr = {.family = RTCP_AF_INET, .port = rtcp_receiver_port(receiver)};
        addr.addr[0] = 127; addr.addr[3] = 1;
        int fd = rtcp_net_linux.connect(&addr);
        assert(fd >= 0);
        assert(rtcp_receiver_poll(receiver, 10) == RTCP_OK);
        assert(rtcp_receiver_get_state(receiver, &state) == RTCP_OK);
        assert(state.running && state.transport_connected && !state.authenticated);
        assert(rtcp_receiver_get_peer(receiver, &peer) == RTCP_OK);
        assert(peer.family == RTCP_AF_INET && peer.addr[0] == 127 && peer.addr[3] == 1);
        assert(peer.port != 0);
        assert(!state.session_established && !state.video_ready && !state.audio_ready);
        const char request[] = "GET /info RTSP/1.0\r\nCSeq: 1\r\n\r\n";
        assert(rtcp_net_linux.send(fd, request, sizeof(request)-1) == sizeof(request)-1);
        assert(rtcp_receiver_poll(receiver, 10) == RTCP_OK);
        char reply[8192];
        int got = rtcp_net_linux.recv(fd, reply, sizeof(reply)-1);
        assert(got > 0); reply[got] = 0;
        assert(strstr(reply, "200 OK") != NULL);
        assert(strstr(reply, "application/x-apple-binary-plist") != NULL);
        assert(rtcp_receiver_stop(receiver) == RTCP_OK); /* live socket teardown */
        expect_empty_diagnostics(receiver);
        assert(rtcp_receiver_set_single_cipher_frame(receiver, true) == RTCP_ESTATE);
        rtcp_net_linux.close(fd);
        unsigned before = events;
        assert(rtcp_receiver_stop(receiver) == RTCP_OK && before == events);
        assert(rtcp_receiver_get_state(receiver, &state) == RTCP_OK);
        assert(!state.running && !state.transport_connected && !state.authenticated);
        assert(!state.session_established && !state.video_ready && !state.audio_ready);
        assert(state.generation == cycle + 1);
    }
    assert(rtcp_receiver_destroy(receiver) == RTCP_OK);
    receiver = NULL;
    assert(rtcp_receiver_destroy(NULL) == RTCP_OK);
    airplay_ed25519_keypair_clear(&key);
    puts("receiver: real sockets, config ownership, failure retry, reentrancy, 32 start/stop cycles passed");
    return 0;
}
