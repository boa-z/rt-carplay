# Luban-Lite port

Adds the ArtInChip SDK specific half of the platform seam on top of the plain
RT-Thread port:

| File | Purpose |
|------|---------|
| `rtcp_wifi_lubanlite.c` | `rtcp_wifi` backed by the Realtek RTL8733BS SoftAP API |
| `rtcp_random_lubanlite.c` | `rtcp_os.random` backed by the ArtInChip CE TRNG |
| `rtcp_bt_lubanlite.c` | `rtcp_bt` backed by the SDK Zephyr Classic/RFCOMM host (iAP2 transport) |
| `rtcp_mfi_lubanlite.c` | `rtcp_mfi` backed by the 2.0C authentication coprocessor on I2C |
| `include/rtcp_lubanlite.h` | `rtcp_wifi_lubanlite`, `rtcp_bt_lubanlite`, `rtcp_mfi_lubanlite`, interface setter and `rtcp_lubanlite_random` |

The first hardware target is an ArtInChip D213 with an RTL8733BS (SDIO Wi-Fi +
HCI H5 Bluetooth).

## Wi-Fi seam

| `rtcp_wifi` call | Luban-Lite call |
|------------------|-----------------|
| `ap_start(ssid, pass, channel)` | `wifi_on(RTW_MODE_AP)` when down, then `wifi_start_ap()` |
| `ap_stop()` | `wifi_off()` |
| `set_vendor_ie(ie, len)` | `wext_del_custom_ie()` then `wext_add_custom_ie()` with `PROBE_RSP \| BEACON` |
| `sta_info()` | not exposed by the driver API, returns `RTCP_ENOTSUP` |

The Apple AirPlay vendor IE is `DD 08 00 A0 40 00 00 02 00 22`
(`core/discovery/airplay_vendor_ie.c`). The driver keeps a single custom-IE
list and rejects a second add, so the seam always deletes first.

`rtcp_wifi_lubanlite_set_interface()` selects the interface that carries the
AP. AP-only mode uses `wlan0`; call it with the actual name when the board runs
STA+AP concurrently.

### Board helpers

The receiver application needs three things the `rtcp_wifi` seam deliberately
does not expose. They live beside it in `rtcp_lubanlite.h`:

| Function | Behavior |
|----------|----------|
| `rtcp_wifi_lubanlite_get_mac()` | Reads the Realtek MAC (after `ap_start`); the device id / Bonjour host name come from it |
| `rtcp_wifi_lubanlite_get_ipv4()` | AP netif address once lwIP configured it; `RTCP_ESTATE` while not ready |
| `rtcp_wifi_lubanlite_start_dhcpd()` | Restarts the lwIP DHCP server on the AP netif (`LWIP_USING_DHCPD`); `RTCP_ENOTSUP` when disabled |

The IP/netif lookup matches the first `w*` interface with an address, as the
WLAN adaptation may register `w0` or `w1` depending on bring-up order.

### Required Kconfig

```
CONFIG_AIC_WLAN_REALTEK=y
CONFIG_REALTEK_FULL_FNC_MODE=y
```

`CONFIG_REALTEK_FULL_FNC_MODE` is what puts the `SetCusIE` handler in the
prebuilt WLAN library. Without it `wext_add_custom_ie()` compiles but the
driver answers "Command not found" at runtime, and the iPhone never shows the
"CarPlay" prompt because the vendor IE is missing from probe responses.

No Realtek firmware blob or library is stored in this repository; they come
from the SDK and stay under their own license.

## Entropy seam

`rtcp_lubanlite_random()` drives the CE TRNG through the public HAL: build an
aligned `struct crypto_task` with `alg.trng.alg_tag = ALG_TRNG`, start it with
`hal_crypto_start_symm()`, wait on `hal_crypto_poll_finish()` and copy the
cache-line buffer out. Install it over the RT-Thread table before starting the
server:

```c
static struct rtcp_os os;

os = rtcp_os_rtthread;
os.random = rtcp_lubanlite_random;
```

Requirements: `CONFIG_AIC_CE_DRV=y` and `CONFIG_DRIVER_HAL_EN=y`. Without
them the function is compiled as a stub returning `false`, and the AirPlay
server refuses to run Pair-Setup (no weak fallback).

Two caveats, both **[unknown until board-verified]**:

- **Completion unit.** The HAL names symmetric=0, hash=1 and asymmetric=2, and
  `hal_crypto_init()` clears four ISR bits. This port waits on unit 3 for
  `ALG_TRNG`. If that guess is wrong the poll times out and pairing fails
  closed; confirm by checking that `rtcp_lubanlite_random()` returns distinct
  bytes and never times out on hardware.
- **Shared engine.** The CE runs one task at a time. Do not use this provider
  while another CE user (e.g. `drv_ce`, a hardware-backed TLS stack) is
  active. On the reference configuration mbedTLS is software-only, so the
  TRNG is the sole CE user.

## Bluetooth RFCOMM seam

`rtcp_bt_lubanlite` backs the `rtcp_bt` transport with the Zephyr Classic
BR/EDR + RFCOMM host shipped in `packages/third-party/zephyr-bluetooth`.  The
wireless CarPlay path needs it for iAP2 over RFCOMM; the AirPlay mirroring
receiver does not.

| `rtcp_bt` call | Zephyr call |
|----------------|-------------|
| `enable(name, NULL)` | `bt_enable(NULL)`, `bt_br_set_connectable(true, NULL)`, `bt_br_set_discoverable(true, false)`, optional `bt_set_name()` |
| `rfcomm_listen(channel, name, uuid, &chan)` | `bt_rfcomm_server_register()` plus a runtime SDP record registered with `bt_sdp_register_service()` |
| `rfcomm_accept(1, &fd)` | blocks until the DLC `connected` callback; one link is supported |
| `send(fd, buf, len)` | `bt_rfcomm_dlc_send()` chunked at the negotiated DLC MTU |
| `recv(fd, buf, len)` | byte ring fed by the DLC `recv` callback; `0` means the link closed, `RTCP_ETIMEDOUT` means idle |

Besides the seam, the port exports the pieces the iAP2 pump needs:
`rtcp_bt_lubanlite_recv_timeout()` (explicit idle timeout so `advance()` runs
on a 100 ms tick), `rtcp_bt_lubanlite_local_bdaddr()` (the controller BR/EDR
address in display order for iAP2's BluetoothTransportComponent; Zephyr
stores `bt_addr_t` little-endian) and `rtcp_bt_lubanlite_drop_link()` (drop
one accepted link without stopping the listener).  A stop followed by a
start re-arms the listener: `rfcomm_listen()` clears the internal stop flag.

The SDP record advertises one 128-bit service class UUID (pass the iAP2 UUID;
`NULL` advertises the Serial Port profile for bring-up), the dynamically
allocated RFCOMM channel and a service name.  With a 128-bit UUID it also
carries the BluetoothProfileDescriptorList (iAP2 UUID + version 0x0100), which
Apple's iAP2 record specifies beside the service class.  Zephyr's SDP encoder
byte-swaps UUID128 values, so the port stores the reverse of the canonical
form.

### Required Kconfig

```
CONFIG_LPKG_USING_ZEPHYR_BLUETOOTH=y
CONFIG_LPKG_ZEPHYR_BLUETOOTH_CHIPSET_RTL8733=y
CONFIG_LPKG_BT_UART="uart7"
CONFIG_LPKG_BT_H5=y
# CONFIG_LPKG_BT_H4 is not set
CONFIG_LPKG_BT_CLASSIC=y
CONFIG_LPKG_BT_RFCOMM=y
CONFIG_LPKG_BT_BUF_ACL_RX_SIZE=1024
CONFIG_LPKG_BT_RFCOMM_L2CAP_MTU=1011
CONFIG_LPKG_BT_DEVICE_NAME_DYNAMIC=y
CONFIG_LPKG_RT_CARPLAY_LUBANLITE_BT=y
```

- `LPKG_BT_H4` must stay off: H4 and H5 both default to on, both register an
  `"hci"` device, and `bt_uart.c` configures the UART for H5 (even parity)
  when H5 is enabled.  Mixing them leaves an H4 host on an H5-configured
  line.
- `bt_uart_init()` opens `LPKG_BT_UART` at 115200/8E1; the Realtek vendor
  setup then raises the controller and host rate and downloads the firmware
  from `drivers/bluetooth/hci/realtek/`.  The board `uart7` pinmux and the
  `AIC_WIRELESS_BT_PWR_GPIO` power pin must be enabled.
- `LPKG_BT_ASSERT` must be off with this SDK revision.  `BT_ASSERT` expands
  to `k_oops()`, whose `K_ERR_KERNEL_OOPS` enum is not visible from
  `kernel.h` without `zephyr/fatal_types.h`; the Classic SDP source then
  fails to compile.  Disabling the custom assert falls back to the kernel
  `__ASSERT_NO_MSG` macro.
- The ACL/RFCOMM values set the RFCOMM frame size.  The SDK defaults are
  smaller; 1011 matches common iAP2 implementations.

### Board verification

`carplay_bt start` (examples/rtthread_app) enables the host, registers the
iAP2 UUID and echoes received bytes; `carplay_iap2 start` replaces the echo
with the real iAP2 session (`core/iap2/iap2_session`).  Expected console
output: the Zephyr H5 setup (baudrate, firmware download), `RFCOMM iAP2
service on channel N`, then `[iap2] link up` when a phone connects.  A
Bluetooth serial terminal that browses SDP for the iAP2 UUID exercises the
same path.

**[unknown until board-verified]**

- The H5 handshake, firmware download and baud switch on this RTL8733BS
  module.  The vendor ZABlue library uses its own H5 implementation; this
  Zephyr path has never run on the board.
- The phone's security requirements for the iAP2 link.  The DLC is left at
  the stack default security level and SSP I/O capabilities are not
  configured; application-level authentication is still the iAP2 MFi
  exchange.
- `RTCP_BT_EV_LINK_KEY` is never emitted: the host exposes no public API to
  read the BR/EDR link key.  Only connected/disconnected events are
  reported.
- Only one RFCOMM link and one listener are supported; `rfcomm_listen()` is
  idempotent while the listener is registered, and the SDP record cannot be
  unregistered with this Zephyr version.

## MFi 2.0C coprocessor (I2C)

`rtcp_mfi_lubanlite.c` binds `struct rtcp_mfi` to a 2.0C-compatible
authentication coprocessor on the RT-Thread I2C bus.  Enable it with:

```
CONFIG_LPKG_RT_CARPLAY_LUBANLITE_MFI=y   # package mode
CONFIG_RTCARPLAY_MFI=y                   # app mode (sdk_app Kconfig)
CONFIG_RTCARPLAY_MFI_I2C_BUS="i2c0"
CONFIG_RTCARPLAY_MFI_I2C_ADDR=0x6A
```

The bus/address defaults are the board evidence, not a guess: ZABlue's
`config/rtl8733bs/zablue.properties` sets `HW_SECRUITY_I2C_ID=0` and
`HW_SECRUITY_I2C_ADDR=0x6A`, and its comment records that probing `0x68`
fails and makes the vendor library report "HW Security Init Error".  The
board enables `i2c0` at 100 kHz (PD.6/PD.7 pinmux).

Transaction shape matters and is implemented as the part requires:

- write: one `rt_i2c_master_send()` transfer of `[register, data...]`, STOP.
  The sign payload (`{0x00, len}` + 20-byte challenge = 22 bytes) exceeds
  `rt_i2c_write_reg()`'s fixed command buffer (`AIC_I2C_CMD_BUF_LEN` is 8,
  so at most 7 data bytes), hence the direct send.
- read: `rt_i2c_read_reg()` — register write (STOP), then a separate read
  (STOP), retried with an `aicos_msleep()` delay.  The part does not answer a
  repeated-start combined transfer.
- the register engine is `core/mfi/mfi2c.c` (probe `0x4E`, version
  `0x00..0x03`, ID at `0x04` BE, sign at `0x20`, status `0x10`, signature at
  `0x12`, certificate at `0x31`).  Challenges must be exactly 20 bytes.

`rtcp_mfi_lubanlite_start()` probes once and is idempotent; the iAP2 glue
calls it when the service starts.  On failure the session is created with
`mfi = NULL`, so a board without the part still exercises the transport and
stops after IdentificationInformation.  `rtcp_mfi_lubanlite_ready()` reports
the probe result (the `carplay_iap2 status` command prints it).

**Verification status**: the 2.0C register engine and the transaction shape
were verified on this board during the PoC (ID `0x00000200`, 31-byte probe,
certificate read, 20-byte challenge signature).  The peripheral wrapper is
build-only until `carplay_iap2 start` runs on the board; watch for
`I/mfi: 2.0C at i2c0 0x6a: id=0x...`.  The RJGT204 clone's 2.0C
compatibility and the full MFi exchange with a real phone are unverified.
The private key never leaves the coprocessor: this port reads the
certificate and requests signatures only.

## mbedTLS configuration

Luban-Lite copies `packages/third-party/mbedtls/ports/inc/tls_config.h` over
`mbedtls/include/mbedtls/config.h` at build time. That file predates ChaCha20
and HKDF, so add these four lines to `tls_config.h` (inside the
`MBEDTLS_CONFIG_H` guard):

```c
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_HKDF_C
```

`MBEDTLS_CHACHA20_C` and `MBEDTLS_POLY1305_C` are hard requirements of
`MBEDTLS_CHACHAPOLY_C` (enforced by `check_config.h`). The rest of the needed
set — ECP/Curve25519, ECDH, AES-CTR, SHA-1/SHA-256/SHA-512, MPI — is already
enabled.

The library sources are compiled from `mbedtls/library/*.c`, so only the
config file needs to change; no source patching is involved.

## Build integration

1. Place (or clone) this repository inside the SDK, for example
   `packages/third-party/rt-carplay`.
2. Make Kconfig aware of it, either by adding
   `source "packages/third-party/rt-carplay/Kconfig"` to
   `packages/third-party/Kconfig`, or by copying `Kconfig` into an existing
   package menu.
3. Select it:

   ```
   CONFIG_LPKG_USING_RT_CARPLAY=y
   CONFIG_LPKG_RT_CARPLAY_LOG=y
   CONFIG_LPKG_RT_CARPLAY_LUBANLITE_WIFI=y
   CONFIG_LPKG_RT_CARPLAY_LUBANLITE_MFI=y
   ```

4. Include the build script from the application (or package) SConscript:

   ```python
   SConscript('<path-to-rt-carplay>/SConscript')
   ```

The group is named `rt-carplay`; it adds `include/`, every `core/*/include`,
`port/rtthread/include` and (with any Luban-Lite option)
`port/luban-lite/include` to the include path.

## Board verification (pending hardware)

These are the checks to run once a D213 + RTL8733BS board is free. Each one is
independent.

1. **SoftAP + vendor IE**
   ```
   wifi_ap_start
   ```
   Wait >10 s, then scan from a phone/PC: the SSID must advertise the Apple IE
   (`DD 08 00 A0 40 00 00 02 00 22`). The SDK `wifi_ie_probe` shell command
   prints the current custom-IE list.
2. **mDNS** — start the SDK responder with `_airplay._tcp` and
   `_raop._tcp`, then `avahi-browse -rt _airplay._tcp` on the host. The TXT
   record is produced by `airplay_txt_build()` (`deviceid`, `features`,
   `flags`, `model`, `srcvers`, `protovers`, `pi`, `pk`).
3. **MFi 2.0C** — run `carplay_iap2 start`; the probe must log
   `I/mfi: 2.0C at i2c0 0x6a: id=0x...` (otherwise `carplay_iap2 status`
   reports `coprocessor absent` and the session stops after identification).
   With a phone connected, `[iap2] MFi certificate sent` followed by
   `[iap2] MFi authenticated` confirms the exchange. The certificate is
   public; the private key never leaves the 2.0C and is never written to
   flash or logs.
4. **BT H5/RFCOMM** — with the Zephyr BT options enabled, run
   `carplay_bt start`: the H5 vendor setup must download the firmware and
   reach ready, `RFCOMM listening on channel N` must appear, and a phone
   (or the Zephyr `bt` shell) must find the iAP2 UUID in SDP, connect and
   see echoed bytes. This is the first validation of the
   `rtcp_bt_lubanlite` seam; the earlier "stub" status no longer applies.
5. **CE TRNG** — with `CONFIG_AIC_CE_DRV=y`, run `carplay_randtest` (it calls
   `rtcp_lubanlite_random()` twice): both calls must succeed, the two hex
   dumps must differ, and no poll timeout may appear in the log. This
   confirms the unit-3 inference above.
