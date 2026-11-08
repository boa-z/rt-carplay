# RT-Thread receiver application

`carplay_app.c` turns the protocol library into a running AirPlay receiver on
top of the Luban-Lite port.  It is RT-Thread specific; the reusable pieces
stay in `core/` and `port/`.

| Step | What happens |
|------|--------------|
| Entropy | `rtcp_lubanlite_random()` (CE TRNG) replaces the NULL RT-Thread RNG |
| Identity | Ed25519 seed loaded from `/data/rt-carplay.seed`, generated once |
| Persistence | controller table in `/data/rt-carplay-pairings.bin` |
| Wi-Fi | SoftAP from the config, then the Apple vendor IE, then DHCP |
| Discovery | `_airplay._tcp` via `mdns_responder` (TXT from `airplay_txt_build`) |
| AirPlay | `airplay_rtsp_server` on port 7000 with the `/info` plist |
| Input | `carplay_touch` queues events; the poll thread sends HID reports |

The pairing PIN is generated from the CE TRNG at each start and printed on
the console (a product would show it on the display).  The poll thread owns
every socket and cipher, so HID reports are never written concurrently.

## SDK integration

The quickest route is the standalone app mode in `examples/sdk_app/`:

1. Copy `examples/sdk_app/` to `application/rt-thread/rtcarplay/` in the
   Luban-Lite tree (the `Kconfig`, `main.c` and `SConscript` there).
2. Make the repository visible at `packages/third-party/rt-carplay` (a clone
   or a symlink both work), or point `RTCARPLAY_DIR` somewhere else in
   menuconfig.  Do **not** also source the package Kconfig in this mode --
   the app Kconfig defines the `LPKG_RT_CARPLAY_*` switches itself.
3. Copy `examples/sdk_app/d21x_demo128-nand_rt-thread_rtcarplay_defconfig`
   into `target/configs/`, then
   `scons --apply-def=d21x_demo128-nand_rt-thread_rtcarplay_defconfig`
   (build the matching bootloader first if the image step asks for it), or
   select the options in `scons --menuconfig`.
4. Enable `CONFIG_REALTEK_FULL_FNC_MODE=y` so `wext_add_custom_ie()` exists,
   and add the four mbedTLS defines listed in
   `port/luban-lite/README.md` to `tls_config.h`.

Required board options:

```
CONFIG_AIC_WIRELESS_LAN=y
CONFIG_AIC_WLAN_REALTEK=y
CONFIG_REALTEK_FULL_FNC_MODE=y
CONFIG_AIC_USING_CE=y          # CE TRNG for the identity and pairing PIN
CONFIG_LWIP_USING_DHCPD=y
CONFIG_RT_USING_WIFI=y
CONFIG_RT_USING_NETDEV=y
```

Bluetooth (wireless iAP2 transport, optional for the AirPlay receiver):

```
CONFIG_LPKG_USING_ZEPHYR_BLUETOOTH=y
CONFIG_LPKG_ZEPHYR_BLUETOOTH_CHIPSET_RTL8733=y
CONFIG_LPKG_BT_UART="uart7"
CONFIG_LPKG_BT_H5=y            # H4 must stay off
CONFIG_LPKG_BT_CLASSIC=y
CONFIG_LPKG_BT_RFCOMM=y
CONFIG_LPKG_RT_CARPLAY_LUBANLITE_BT=y
CONFIG_RTCARPLAY_IAP2=y         # carplay_iap2 (requires RTCARPLAY_BT)
# CONFIG_LPKG_BT_ASSERT is not set   # see port/luban-lite/README.md
```

Package mode (repo under `packages/third-party`, package Kconfig sourced) is
also supported: the SDK auto-builds the library, and the app SConscript skips
the duplicate include.

## Shell commands

```
carplay_start                 # bring the receiver up, prints the PIN
carplay_status                # running/stopped, display size, PIN
carplay_touch down 640 240    # inject touch in display pixels
carplay_touch move 700 260
carplay_touch up 700 260
carplay_stop

carplay_randtest              # CE TRNG self-test (two distinct 32-byte reads)

carplay_bt start              # Zephyr Classic RFCOMM bring-up (iAP2 UUID, echo)
carplay_bt status             # enable state, SDP/RFCOMM channel, link state
carplay_bt stop               # drop the link and stop accepting

carplay_iap2 start            # real iAP2 session over RFCOMM (needs carplay_start)
carplay_iap2 status           # serving/link/identification/MFi counters
carplay_iap2 stop             # stop the session and disconnect the phone

carplay_video status          # decoder counters, queue depth
carplay_video start 1280 480  # exercise MPP + DE without a controller
carplay_video play /data/sample.h264   # decode an Annex-B file
carplay_video stop            # release the decoder, restore the UI layer
```

`carplay_video play` is the bring-up path when no controller is available:
it starts the sink if needed, feeds complete NAL units with a 64 KB read
window (paced by decoder queue depth), and prints the decoder counters.
`carplay_video start`/`play` also work while the receiver runs, as long as
the controller is not streaming.

`RTCARPLAY_AUTO_START=y` calls `carplay_app_start()` from `main()` instead.

## Screen output

`on_screen` receives Annex-B H.264 (SPS/PPS first, then access units) with the
sender timestamp.  With `CONFIG_RTCARPLAY_VIDEO=y` (default when the VE and DE
drivers are available) the application installs `video_sink_lubanlite.c`:

- the Annex-B data is split into NAL units on a queue (one decoder packet per
  NAL, matching the SDK H.264 decoder);
- the sink's own thread decodes with `mpp_decoder` and presents NV12 frames
  through the `aicfb` video layer, letterboxed into the configured display
  size;
- a configuration message (SPS/PPS) that arrives after frames were shown
  marks a new stream (reconnect or resolution change) and recreates the
  decoder before feeding it;
- the UI layer alpha is saved, dimmed to 0 while running, and restored on
  stop.

Passing `config.on_screen` yourself bypasses the on-board sink.  Without
`CONFIG_RTCARPLAY_VIDEO` (or when `aicfb` is missing) the receiver stays
headless and only counts frames.

Known bring-up notes: video memory comes from the VE/DE pools, so the frame
count in use depends on the controller's H.264 profile; the first hardware
run should watch `[carplay-vid]` messages for decoder resets and errors.

## Bluetooth transport and wireless iAP2

`RTCARPLAY_BT` builds `carplay_bt`, a transport-only check: enable the Zephyr
Classic host, advertise the iAP2 service class UUID over SDP and echo
received RFCOMM bytes.

`RTCARPLAY_IAP2` (default on, needs `RTCARPLAY_BT`) builds `carplay_iap2`,
which runs the real session from `core/iap2/` on the same RFCOMM transport:

| Step | What happens |
|------|--------------|
| 1 | `carplay_iap2 start` requires the receiver to be running (`carplay_start`) and claims the single Bluetooth link |
| 2 | Enables the host, reads the local BR/EDR address for IdentificationInformation, registers the iAP2 SDP record |
| 3 | Accepts one phone, calls `iap2_session_link_up()` and pumps bytes both ways on a 100 ms tick (`iap2_session_feed`, `iap2_session_advance`) |
| 4 | Logs identification, MFi, subscription, Wi-Fi-configuration and CarPlay-invite events as `[iap2] ...` |
| 5 | Watches the AirPlay event channel state and reports transitions to the session (re-invite pacing) |

`carplay_bt` and `carplay_iap2` are mutually exclusive; the link owner is
tracked in the application.  Stopping the receiver (`carplay_stop`) also
retires the iAP2 service.

The iAP2 session builds 5 GHz channels (the SoftAP default is 36), not just
the 2.4 GHz 1-14 range the original relay accepted.

**MFi is the current wall.**  No MFi coprocessor provider is built in, so
`iap2` is started with `mfi = NULL`: the phone's MFi certificate request and
challenge are ignored and the session stops after IdentificationInformation.
`core/mfi/mfi2c.c` already implements the 2.0C register state machine over
an injected I2C bus, but the board port and the SDP/identification result on
real hardware need the board.  Without a 2.0C signature the session cannot
reach Wi-Fi configuration or `CarPlayStartSession`, so hands-free wireless
CarPlay needs the coprocessor; the AirPlay receiver path is independent of
it.  Security level, link-key reporting and the H5 firmware/baud path remain
board-verification items (see `port/luban-lite/README.md`).

## Verification status

- Host protocol tests (including the socket-level Pair-Setup/Pair-Verify and
  Screen DataStream test) pass under ASan/UBSan.
- **Build-verified (2026-11-08)**: with this defconfig, the Luban-Lite d21x
  project compiles and links into
  `output/d21x_demo128-nand_rt-thread_rtcarplay/images/d21x.elf` and the
  full image `d21x_demo128_nand_carplay_page_2k_block_128k_v1.0.0.img`
  (4.0 MB), built in an isolated worktree of the SDK.  Verified in the ELF:
  `carplay_app_start`, the `carplay_*` MSH commands,
  `airplay_rtsp_server_start`, `rtcp_lubanlite_random`, `wifi_start_ap`,
  `wext_add_custom_ie`, `dhcpd_start`, plus mbedTLS ChaCha20-Poly1305 and
  HKDF.  Build-only, not executed.
- **Video sink**: the MPP H.264 + DE path is build-verified with the API of
  the pinned SDK (`mpp_decoder`, `AICFB_UPDATE_LAYER_CONFIG`).  It has not
  decoded or displayed anything yet; the first board run with a real
  controller is the validation.
- **Bluetooth iAP2 transport (2026-11-08)**: `rtcp_bt_lubanlite` is
  build-verified with the SDK Zephyr Classic + RFCOMM host and the RTL8733
  H5 driver.  The ELF contains `bt_rfcomm_server_register`,
  `bt_sdp_register_service`, `h5_hci_init`, `bt_h5_vnd_update_controller_firmware`
  and the registered `carplay_bt` command.  Build-only: `carplay_bt start` on
  the board is the first validation of the H5 firmware download, SDP record
  and data path.
- **Wireless iAP2 session (2026-11-08)**: `iap2_link_lubanlite.c` wires
  `iap2_session_t` to the RFCOMM transport.  The d21x ELF contains
  `carplay_iap2`, `carplay_iap2_start`, `iap2_session_create/feed/advance`,
  `iap2_identity_build_wireless_omitting`,
  `iap2_carplay_build_start_session`, `rtcp_bt_lubanlite_recv_timeout` and
  `rtcp_bt_lubanlite_local_bdaddr`; the image is 4,008,448 bytes.  The host
  suite (including a new 5 GHz channel case) passes under ASan/UBSan.
  Build-only: on the board, watch for `[iap2] link up` and
  `identification accepted`; MFi will then stop the flow until a 2.0C
  provider exists.
- **Not board-verified yet**: Wi-Fi bring-up, vendor IE, pairing with a real
  controller, touch input, the CE TRNG unit assignment, the Bluetooth
  H5/RFCOMM path and the iAP2 session all need the hardware run.  Treat the
  first boot as a bring-up session and watch the console log.
