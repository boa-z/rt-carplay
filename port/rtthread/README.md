# RT-Thread port

This port binds the rt-carplay core to a plain RT-Thread (or Luban-Lite)
tree. It contains no CarPlay logic, only the three platform seams declared in
`include/rtcp/{os,net,bt}.h`.

| File | Seam | Notes |
|------|------|-------|
| `rtcp_os_rtthread.c` | `rtcp_os` | tick-based `now_ms`, `rt_thread_mdelay`, `rt_malloc/free`, `rt_thread_create`, mutexes with delete |
| `rtcp_net_rtthread.c` | `rtcp_net` | BSD sockets from SAL (`RT_USING_SAL`) or lwIP directly, including IPv4 UDP for mDNS |
| `rtcp_bt_rtthread.c` | `rtcp_bt` | **stub**: every call returns `RTCP_ENOTSUP` |

`include/rtcp_rtthread.h` exports the three constant tables plus
`rtcp_net_rtthread_local_port()`.

## Wiring

Pass the seams down from the application layer; the core never looks them up
by name:

```c
#include "rtcp_rtthread.h"

airplay_rtsp_pairing_config_t pairing = {
    .os = &rtcp_os_rtthread,
    .net = &rtcp_net_rtthread,
    /* ... */
};
```

`rtcp_os_rtthread.thread_create` starts the thread before returning and uses a
stack size in bytes / priority in RT-Thread units. Threads created this way are
detached: returning from the entry function must not free the handle.

The socket seam uses a fixed `poll()` array of 8 descriptors. Enlarge
`RTCP_NET_POLL_MAX` in `rtcp_net_rtthread.c` if a session needs more.

## Requirements

* `RT_USING_HEAP`
* `RT_USING_SAL` (recommended) or lwIP with `SAL`-compatible `closesocket()`
* For mDNS: UDP sockets plus IGMP multicast membership (`LWIP_IGMP` in lwIP);
  `udp_join` returns `RTCP_ENOTSUP` when `IP_ADD_MEMBERSHIP` is unavailable
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

  Luban-Lite ships an older `tls_config.h`; see `port/luban-lite/README.md`
  for the four lines to add.

## Bluetooth (Classic BR/EDR + RFCOMM)

The wireless-car PlayStream path is TCP, so iAP2 and AirPlay can be brought up
without Bluetooth. Bluetooth is still required for:

* wireless accessory discovery (Classic inquiry + SDP),
* the iAP2 link when the phone selects the BT transport,
* HID input forwarding over RFCOMM.

`rtcp_bt_rtthread.c` intentionally returns `RTCP_ENOTSUP` rather than
pretending a link exists. Implementing it requires board validation of the
RTL8733BS H5 transport in the Zephyr host that ships with Luban-Lite:

1. Bring up H5 (`hci_h5`) and confirm `bt_hci` reports `HCI_READY`.
2. Register an SDP record for the iAP2 UUID and the CarPlay accessory UUID.
3. Accept `bt_rfcomm_dlc` connections and expose the DLC as an `int` handle
   that the `send`/`recv` callbacks forward to `bt_rfcomm_dlc_send()` and a
   ring buffer.
4. Deliver `RTCP_BT_EV_CONNECTED` / `RTCP_BT_EV_DISCONNECTED` through
   `set_event_cb`.

Until then, leave `rtcp_bt` unset or use a test double; the core checks for
`RTCP_ENOTSUP` and skips BT-only features.
