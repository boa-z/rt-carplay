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
```

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

## Verification status

- Host protocol tests (including the socket-level Pair-Setup/Pair-Verify and
  Screen DataStream test) pass under ASan/UBSan.
- **Build-verified (2026-11-08)**: with this defconfig, the Luban-Lite d21x
  project compiles and links into
  `output/d21x_demo128-nand_rt-thread_rtcarplay/images/d21x.elf` and the
  full image `d21x_demo128_nand_carplay_page_2k_block_128k_v1.0.0.img`
  (3.6 MB), built in an isolated worktree of the SDK.  Verified in the ELF:
  `carplay_app_start`, all four `carplay_*` MSH commands,
  `airplay_rtsp_server_start`, `rtcp_lubanlite_random`, `wifi_start_ap`,
  `wext_add_custom_ie`, `dhcpd_start`, plus mbedTLS ChaCha20-Poly1305 and
  HKDF.  Build-only, not executed.
- **Video sink**: the MPP H.264 + DE path is build-verified with the API of
  the pinned SDK (`mpp_decoder`, `AICFB_UPDATE_LAYER_CONFIG`).  It has not
  decoded or displayed anything yet; the first board run with a real
  controller is the validation.
- **Not board-verified yet**: Wi-Fi bring-up, vendor IE, pairing with a real
  controller, touch input and the CE TRNG unit assignment all need the
  hardware run.  Treat the first boot as a bring-up session and watch the
  console log.
