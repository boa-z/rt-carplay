# Luban-Lite port

Adds the ArtInChip SDK specific half of the platform seam on top of the plain
RT-Thread port:

| File | Purpose |
|------|---------|
| `rtcp_wifi_lubanlite.c` | `rtcp_wifi` backed by the Realtek RTL8733BS SoftAP API |
| `include/rtcp_lubanlite.h` | `rtcp_wifi_lubanlite` and interface setter |

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
   ```

4. Include the build script from the application (or package) SConscript:

   ```python
   SConscript('<path-to-rt-carplay>/SConscript')
   ```

The group is named `rt-carplay`; it adds `include/`, every `core/*/include`,
`port/rtthread/include` and (with the Wi-Fi option) `port/luban-lite/include`
to the include path.

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
3. **MFi oracle** — start the 2.0C TCP oracle and verify `GET_CERT` returns
   the 908-byte certificate and `SIGN` returns a 128-byte signature. The
   private key never leaves the 2.0C and is never written to flash or logs.
4. **BT H5/HCI** — `hci_h5` must reach `HCI_READY`; Classic inquiry and SDP
   must find and serve `_iap2._tcp` / CarPlay UUIDs. The RFCOMM data path
   remains stubbed until this passes.
