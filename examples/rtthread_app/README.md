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
| Audio | decrypted PCM -> ring buffer -> `sound0` writer thread (RTCARPLAY_AUDIO) |
| Input | panel reader (`RTCARPLAY_TOUCH_PANEL`) or `carplay_touch` queues events; the poll thread sends HID reports |

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
CONFIG_AIC_USING_AUDIO=y       # selects RT_USING_AUDIO; registers "sound0"
CONFIG_RTCARPLAY_AUDIO=y       # default on when RT_USING_AUDIO is set
```

`CONFIG_AIC_AUDIO_EN_PIN_LOW`/`_HIGH` and the SPK choice belong to the board
target; demo128-nand-carplay uses `AIC_AUDIO_EN_PIN_LOW` with `AIC_AUDIO_SPK_1`.

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
carplay_status                # running/stopped, display size, PIN, audio counters
carplay_touch down 640 240    # inject touch in display pixels (panel runs too)
carplay_touch move 700 260
carplay_touch up 700 260
carplay_stop

carplay_randtest              # CE TRNG self-test (two distinct 32-byte reads)

carplay_bt start              # Zephyr Classic RFCOMM bring-up (iAP2 UUID, echo)
carplay_bt status             # enable state, SDP/RFCOMM channel, link state
carplay_bt stop               # drop the link and stop accepting

carplay_iap2 start            # real iAP2 session over RFCOMM (needs carplay_start)
carplay_iap2 status           # serving/link/coprocessor/identification/MFi counters
carplay_iap2 stop             # stop the session and disconnect the phone

carplay_video status          # decoder counters, queue depth
carplay_video start 1024 600  # exercise MPP + DE without a controller
carplay_video play /data/sample.h264   # decode an Annex-B file
carplay_video stop            # release the decoder, restore the UI layer
```

`carplay_video play` is the bring-up path when no controller is available:
it starts the sink if needed, feeds complete NAL units with a 64 KB read
window (paced by decoder queue depth), and prints the decoder counters.

## Touch input

With `CONFIG_RTCARPLAY_TOUCH_PANEL=y` (default when `RT_USING_TOUCH` is on)
`touch_panel_lubanlite.c` opens the RT-Thread CTP device named by
`CONFIG_AIC_TOUCH_PANEL_NAME` (`gt911` on demo128-nand-carplay).  The panel
interrupt disables the IRQ, a dedicated thread reads the point buffer,
resolves DOWN/MOVE/UP with the panel track id, scales the driver's
`range_x`/`range_y` to the display size and calls `carplay_app_touch()`; the
poll thread turns the queue into HID reports, so the panel thread never
touches the event channel.  A missing device logs
`[carplay-touch] touch device ... not found` and leaves `carplay_touch`
available for synthetic input.

The CarPlay HID report carries two contacts
(`INPUT_BRIDGE_TOUCH_CONTACTS`), so a third simultaneous finger is not
forwarded.  Rotate the panel and the display with the same board settings:
the driver applies flip/rotate/crop and `input_bridge` applies the CarPlay
rotation on top.
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
  through the `aicfb` video layer, letterboxed into the panel geometry
  reported by `AICFB_GET_SCREENINFO` (the DE layer works in panel pixels);
- a configuration message (SPS/PPS) that arrives after frames were shown
  marks a new stream (reconnect or resolution change) and recreates the
  decoder before feeding it;
- the UI layer alpha is saved, dimmed to 0 while running, and restored on
  stop.

Passing `config.on_screen` yourself bypasses the on-board sink.  Without
`CONFIG_RTCARPLAY_VIDEO` (or when `aicfb` is missing) the receiver stays
headless and only counts frames.

The AirPlay display mode (`config.width_pixels`/`height_pixels`) is
advertised in `/info`, so it must match the panel.  On demo128-nand-carplay
that is 1024x600; the app-mode Kconfig (`RTCARPLAY_SCREEN_WIDTH/HEIGHT`)
and `carplay_start` use the same values, and the panel reader scales the
GT911 range to them.  A different mode still displays (the sink letterboxes
into the panel), but the UI aspect and the touch mapping no longer agree.

Known bring-up notes: video memory comes from the VE/DE pools, so the frame
count in use depends on the controller's H.264 profile; the first hardware
run should watch `[carplay-vid]` messages for decoder resets and errors.

## Audio output

With `CONFIG_RTCARPLAY_AUDIO=y` (default when `RT_USING_AUDIO` is on) the
application wires `airplay_rtsp_server_config.on_audio_pcm` to
`audio_sink_lubanlite.c` and advertises PCM audio in `/info` (feature bits
plus the `audioFormats`/`audioLatencies` arrays).  The RTSP session accepts
audio stream types 100/101/102 in SETUP, derives the per-stream key with
HKDF-SHA512 and decrypts the ChaCha20-Poly1305 RTP datagrams.

The sink has two threads by design:

- the AirPlay poll thread only copies each decrypted packet into a 64 KB ring
  and wakes the writer -- it never blocks on the codec;
- the writer thread owns `sound0`: on the first packet it issues
  `AUDIO_CTL_CONFIGURE` (output, negotiated rate/channels, 16-bit),
  `AUDIO_CTL_START` with `AUDIO_STREAM_REPLAY`, waits 200 ms for the power
  amplifier (aplay's value) and then feeds `rt_device_write()`.  A later SETUP
  that negotiates another rate/channels reconfigures the codec in place.

If the ring overflows (codec stall), whole packets are dropped and counted;
`carplay_status` prints packets, replay starts, dropped bytes and write
errors.  When `sound0` is missing the application logs
`audio sink unavailable` and continues video-only -- `/info` then does not
advertise audio, so a controller starts no audio stream.

The sender also controls the speaker with SET_PARAMETER `volume: <dB>` (0 dB
is full scale; AirPlay sends down to -144 dB).  The RTSP session parses the
parameter, the server forwards it to `on_volume`, and the sink applies it
through the codec mixer (`AUDIO_TYPE_MIXER` / `AUDIO_MIXER_VOLUME`).  The
scale is mapped linearly over [-45, 0] dB onto the codec's 0..100 range
(below -45 dB the output is inaudible); the writer thread applies it on its
next wake, and a value that arrives before playback starts is applied when
the stream starts.  Each change logs `[carplay-aud] sender volume -<x>.<y>
dB -> <pct>%`, and `carplay_status` prints the last requested percent.  The
mapping and the codec fade curve are to be tuned on hardware.

`airplay_audio_pcm_cb_t` delivers signed 16-bit little-endian interleaved
PCM.  Opus and AAC are not advertised and are rejected at SETUP, so the
sender always negotiates LPCM.

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

`RTCARPLAY_MFI` (default on, needs `AIC_I2C_DRV`) adds the 2.0C
authentication coprocessor from `port/luban-lite/rtcp_mfi_lubanlite.c`.
The board wires it to `i2c0` at `0x6A` (ZABlue's
`HW_SECRUITY_I2C_ID`/`HW_SECRUITY_I2C_ADDR`); both are Kconfig values
(`RTCARPLAY_MFI_I2C_BUS`, `RTCARPLAY_MFI_I2C_ADDR`).  The session is created
with the provider (`config.mfi = rtcp_mfi_lubanlite()`), so the phone's
certificate/challenge exchange runs and the flow can reach Wi-Fi
configuration and `CarPlayStartSession`.  The probe happens when the iAP2
service starts; if the coprocessor does not answer, the log says so and the
session falls back to identify-only (`mfi = NULL`) instead of failing the
transport path.  `carplay_iap2 status` reports the coprocessor as
`ready`/`absent`.  The private key never leaves the coprocessor; only its
certificate and signatures are used.

The register engine and transaction shape were verified on the board in the
PoC (`core/mfi/mfi2c.c`: probe at 0x4E, sign at 0x20, certificate at 0x31,
ID `0x00000200`).  The RJGT204's compatibility with the Apple 2.0C register
set and the full MFi exchange with a real phone remain board-verification
items, as do the security level, link-key reporting and the H5
firmware/baud path (see `port/luban-lite/README.md`).  The AirPlay receiver
path is independent of MFi.

## Verification status

- Host protocol tests (including the socket-level Pair-Setup/Pair-Verify,
  Screen DataStream, encrypted-audio and SET_PARAMETER volume test, and a
  full second session after a tampered frame) pass under ASan/UBSan.
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
  `rtcp_bt_lubanlite_local_bdaddr`.  The host suite (including a new 5 GHz
  channel case) passes under ASan/UBSan.
  Build-only: on the board, watch for `[iap2] link up` and
  `identification accepted`.
- **MFi 2.0C provider (2026-11-08)**: `rtcp_mfi_lubanlite` adapts
  `core/mfi/mfi2c` to the RT-Thread I2C API (`rt_i2c_master_send` for the
  one-transfer register write, `rt_i2c_read_reg` for write-register-then-read,
  `aicos_msleep` for the poll delay).  The ELF contains
  `rtcp_mfi_lubanlite_start`, `rtcp_mfi_lubanlite_ready`,
  `rtcp_mfi2c_probe/sign/read_cert` and `rt_i2c_read_reg`; the image is
  4,012,544 bytes with zero build warnings.  The transaction shape, the
  register addresses and the 2.0C ID were verified on the board in the PoC;
  the I2C bus/address defaults come from ZABlue's properties (i2c0/0x6A).
  Build-only for the provider itself: the first `carplay_iap2 start` on the
  board should log `I/mfi: 2.0C at i2c0 0x6a: id=0x00000200 ...` and
  `MFi certificate sent`, followed by `MFi authenticated` when a phone
  connects.
- **Audio (2026-11-08)**: the audio DataStream is host-verified end to end.
  `test_airplay_audio_stream` covers the format table, start validation,
  decrypting a real ChaCha20-Poly1305 datagram, rejecting tampered/short
  datagrams and draining the control port; `test_airplay_rtsp_session` covers
  multi-stream SETUP (screen+audio in one request, alt stream, key reuse 503,
  audio-only TEARDOWN); the socket-level pairing test now sends an encrypted
  audio RTP packet through the real server and checks the decrypted PCM and
  format.  The session test also covers SET_PARAMETER volume parsing
  (`volume: <dB>` value and clamping, non-volume parameters ignored, no
  callback for GET_PARAMETER).  The SDK build links
  `carplay_audio_sink_start/pcm/stop/stats/set_volume_db/volume_percent`,
  `airplay_audio_stream_start/poll/stop`, `sound0` and `RT_USING_AUDIO`
  (image 4,030,976 bytes with the touch milestone, zero warnings).
  Build-only: the codec has not played a sample; the first board run with a
  controller should log `[carplay-aud] playback <rate> Hz <ch> ch 16-bit`,
  `carplay_status` should show growing packet counters, and moving the
  phone's volume slider should log `[carplay-aud] sender volume -<x>.<y> dB
  -> <pct>%`.
- **Touch panel (2026-11-08)**: `touch_panel_lubanlite.c` opens the
  RT-Thread CTP device named by `CONFIG_AIC_TOUCH_PANEL_NAME` (`gt911` on
  demo128-nand-carplay), takes points on the panel interrupt (the rx
  indication disables the IRQ, the reader thread drains the panel and
  re-enables it) and forwards DOWN/MOVE/UP with the driver's track id into
  the same queue as `carplay_touch`.  Coordinates are scaled from
  `range_x`/`range_y` to the display size.  The ELF contains
  `carplay_touch_panel_start/stop/points`, `panel_thread`, `panel_rx`,
  `rt_touch_read/control`, `touch_irq_callback` and `gt911_read_point`.
  Build-only: the first board run must log `[carplay-touch] gt911: <n>
  point(s), 1024x600 -> <w>x<h>` and the phone UI must follow the panel.
- **Not board-verified yet**: Wi-Fi bring-up, vendor IE, pairing with a real
  controller, touch input, the CE TRNG unit assignment, the Bluetooth
  H5/RFCOMM path, the MFi exchange with a real phone, the iAP2 session and
  audio playback all need the hardware run.  Treat the first boot as a
  bring-up session and watch the console log.
