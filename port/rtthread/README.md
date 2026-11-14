[English](README.md) | [简体中文](README.zh-CN.md)

# RT-Thread port

This port binds the rt-carplay core to a plain RT-Thread (or Luban-Lite) tree. It contains no CarPlay logic, only the three platform seams declared in `include/rtcp/{os,net,bt}.h`.

| File | Seam | Notes |
|------|------|-------|
| `rtcp_os_rtthread.c` | `rtcp_os` | tick-based `now_ms`, `rt_thread_mdelay`, `rt_malloc/free`, `rt_thread_create`, mutexes with delete |
| `rtcp_net_rtthread.c` | `rtcp_net` | BSD sockets from SAL (`RT_USING_SAL`) or lwIP directly, including IPv4 UDP for mDNS |
| `rtcp_bt_rtthread.c` | `rtcp_bt` | **stub**: every call returns `RTCP_ENOTSUP` |

`include/rtcp_rtthread.h` relative to this port exports the three constant tables, `rtcp_net_rtthread_local_port()` and `rtcp_net_rtthread_multicast_interface()`. Configure the multicast interface before opening discovery sockets; its owner may change it only while those sockets are closed. `NULL` restores routing-based interface selection.

## Wiring

New applications should use the public [Receiver API](../../docs/receiver-api.md) through the SDK root `SConscript`, with one owner for create/start/poll/stop/destroy. The [standalone RT-Thread Headless example](../../examples/rtthread_headless/README.md) supplies a complete composition with actual identity, persistence, entropy checks and cleanup. The lower-level RTSP excerpt below is retained as seam wiring background; it is not a complete Receiver application and deliberately omits startup checks, pairing implementation, stop and resource destruction. Do not use that endless loop as a lifecycle template.

Pass the seams down from the application layer; the core never looks them up by name:

```c
#include "airplay_rtsp_server.h"
#include "rtcp_rtthread.h"

airplay_rtsp_server_config_t config = {
    .os = &rtcp_os_rtthread,
    .net = &rtcp_net_rtthread,
    .pairing = { /* Pair-Setup / Pair-Verify identity providers */ },
    .info_plist = info_plist,
    .info_plist_len = info_plist_len,
    .on_screen_message = on_screen_message,
};
airplay_rtsp_server_t *server = NULL;
airplay_rtsp_server_start(&config, &server);

for (;;) {
    airplay_rtsp_server_poll(server, 100); /* or interleave with your loop */
}
```

The RTSP server drives its own endpoints (listener, event channel, timing, Screen DataStream) from that single poll call.

`rtcp_os_rtthread.thread_create` starts the thread before returning and uses a stack size in bytes / priority in RT-Thread units. Threads created this way are detached: returning from the entry function must not free the handle.

The socket seam uses a fixed `poll()` array of 12 descriptors by default, covering RTSP, event, timing, screen and three audio data/control pairs with some headroom. Enlarge `RTCP_NET_POLL_MAX` in `rtcp_net_rtthread.c` if a session needs more. TCP listeners, accepted sockets and outgoing connections request TCP_NODELAY. IPv6 is available only when `NETDEV_IPV6` or `LWIP_IPV6` is enabled; otherwise IPv6 operations return `RTCP_ENOTSUP`. These socket policies do not establish board latency or throughput.

## Randomness

`rtcp_os_rtthread.random` is `NULL`: RT-Thread has no portable CSPRNG, and Pair-Setup must not fall back to `rand()`. Copy the constant OS table and install a real platform entropy callback before enabling pairing. The maintained Luban-Lite host does so with the ArtInChip CE TRNG (`ALG_TRNG` in `hal_ce.h`) through its `rtcp_lubanlite_random`; this is a host implementation, not a generic-port dependency. An application must check `rtcp_os.random` and decline to advertise the AirPlay service when entropy is missing or fails. The Headless example rejects missing entropy with `RTCP_EINVAL` and provider failure with `RTCP_EIO`, even when reusing an existing identity.

## Requirements

* `RT_USING_HEAP`
* `RT_USING_SAL` (recommended) or lwIP with `SAL`-compatible `closesocket()`
* For mDNS: UDP sockets plus IGMP multicast membership (`LWIP_IGMP` in lwIP); `udp_join` returns `RTCP_ENOTSUP` when `IP_ADD_MEMBERSHIP` is unavailable
* mbedTLS 2.28.x built with:

  | Symbol | Why |
  |--------|-----|
  | `MBEDTLS_ECP_DP_CURVE25519_ENABLED` | Pair-Setup / Pair-Verify key agreement |
  | `MBEDTLS_ECDH_C` | shared secret |
  | `MBEDTLS_AES_C` + `MBEDTLS_CIPHER_MODE_CTR` | MFiSAP signature encryption |
  | `MBEDTLS_SHA512_C` / `MBEDTLS_SHA1_C` | SRP, HKDF |
  | `MBEDTLS_MPI_C` | SRP-6a |
  | `MBEDTLS_CHACHA20_C` | DataStream and ControlCipher |
  | `MBEDTLS_POLY1305_C` | DataStream AEAD |
  | `MBEDTLS_CHACHAPOLY_C` | DataStream AEAD |
  | `MBEDTLS_HKDF_C` | AirPlay key derivation |

Luban-Lite bring-up historically required additions to its older `tls_config.h`; see the [Luban-Lite integration notes](../luban-lite/README.md) (`port/luban-lite/README.md` from the SDK root) for the historical configuration and verify the maintained host's checked-in configuration before applying it. Do not assume that every current SDK still needs those additions.

## Bluetooth (Classic BR/EDR + RFCOMM)

The AirPlay media path uses TCP/IP, and the Headless LAN bench can run without Bluetooth. This does not provide complete wireless CarPlay discovery and activation. A complete wireless accessory still needs Bluetooth for:

* wireless accessory discovery (Classic inquiry + SDP),
* the iAP2 link when the phone selects the BT transport,
* any host-specific HID input forwarding over RFCOMM. The maintained D213 composition uses the AirPlay event channel for CarPlay touch reports; Bluetooth is not required for every HID report.

`rtcp_bt_rtthread.c` intentionally returns `RTCP_ENOTSUP` rather than pretending a link exists. The concrete RTL8733BS H5/Zephyr Classic/RFCOMM adapter already lives in the Luban-Lite host as `application/rt-thread/rtcarplay/platform/rtcp_bt_lubanlite.c`; see [host migration](../../docs/luban-lite-migration.md). It is not a missing implementation to duplicate inside this generic port. A new board adapter must implement and physically validate the following sequence:

1. Bring up H5 (`hci_h5`) and confirm `bt_hci` reports `HCI_READY`.
2. Register an SDP record for the iAP2 UUID and the CarPlay accessory UUID.
3. Accept `bt_rfcomm_dlc` connections and expose the DLC as an `int` handle that the `send`/`recv` callbacks forward to `bt_rfcomm_dlc_send()` and a ring buffer.
4. Deliver `RTCP_BT_EV_CONNECTED` / `RTCP_BT_EV_DISCONNECTED` through `set_event_cb`.

On a board without that adapter, leave `rtcp_bt` unset or use a test double in Host tests; the core checks for `RTCP_ENOTSUP` and skips BT-only features. A test double cannot establish hardware support. The Headless example's Host regressions and the D213 cross-build are separate from physical acceptance on another RT-Thread board.
