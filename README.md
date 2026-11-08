# rt-carplay

An independent, RT-Thread-first CarPlay receiver stack.  The protocol core
builds and runs on a Linux host with no RT-Thread, Luban-Lite, or ArtInChip
headers; platform ports live outside `core/`.

Current state (M0/M1): foundation and host-testable protocol modules.  The
repository is under active development; no hardware has been validated from
this repo yet.

## Contents

- `include/rtcp/` — platform seams (os, net, bt, mfi, wifi, media, input) and
  common error codes.
- `core/mfi/` — Apple MFi 2.0C register state machine, extracted from the
  Luban-Lite PoC but with no RT-Thread dependency.
- `core/iap2/` — portable iAP2 link layer, parameter framing,
  Identify/IdentificationInformation, CarPlayStartSession/WiFiConfig builders,
  the MFi authentication state machine, and the wireless accessory session
  that ties them together behind injected transport/MFi/Wi-Fi/AirPlay
  callbacks.  Ported from `esp32-carplay-web-relay` (GPL-3.0-or-later, itself
  based on DiPlay); copyright notices retained.
- `core/airplay/` — binary plist, RTSP codec, TLV8, HID plists/descriptors,
  screen codec, DataStream AEAD decryption, ControlCipher streams, timing,
  Pair-Setup/Pair-Verify/SRP-6a, the MFiSAP responder, the `/info` binary
  plist writer and the RTSP session (info, SETUP, RECORD, teardown), and
  persistent pairing storage.  On top of those sit the poll-driven
  endpoints — ControlCipher event channel, UDP timing, Screen DataStream —
  and `airplay_rtsp_server`, a single-controller RTSP server that owns the
  listener, the ControlCipher transition after Pair-Verify, the media
  endpoints the session asks for and HID report output over the event
  channel (the touch-input path).
- `core/crypto/` — Ed25519 key/sign/verify wrapper over the vendored
  orlp/ed25519 (zlib), the software identity for Pair-Setup/Pair-Verify.
- `third_party/ed25519/` — unmodified orlp/ed25519 sources and its LICENSE.
- `core/media/`, `core/input/` — timestamp helpers and the input bridge.
- `core/discovery/` — AirPlay Bonjour TXT records, an mDNS responder for
  `_airplay._tcp` (transport-free packet builder plus UDP polling through the
  net seam), and the Apple CarPlay SoftAP vendor IE.
- `port/linux/` — POSIX OS and TCP implementations; the BT and Wi-Fi seams
  are explicit stubs there because a desktop host has no accessory stack.
- `port/rtthread/` — RT-Thread OS and BSD-socket seams; the plain RT-Thread
  BT seam stays an explicit `RTCP_ENOTSUP` stub (the SDK-specific Zephyr
  transport lives in `port/luban-lite/`).
- `port/luban-lite/` — Realtek RTL8733BS SoftAP + Apple vendor-IE seam, the
  CE TRNG entropy provider, and `rtcp_bt_lubanlite`: Zephyr Classic BR/EDR +
  RFCOMM (RTL8733 H5) with SDP records, an iAP2 byte stream and the
  timed-receive/local-BD-address helpers the iAP2 pump uses, for the
  ArtInChip SDK.
- `Kconfig`, `SConscript` — RT-Thread/Luban-Lite build integration.
- `examples/linux_sim/` — the foreground Linux receiver bench (mDNS + RTSP +
  Screen dump) described under “Linux simulator”.
- `examples/rtthread_app/` — the RT-Thread/Luban-Lite receiver application
  (SoftAP + vendor IE + DHCP + mDNS + RTSP server + HID touch), with an
  on-board MPP H.264 -> DE video sink (`video_sink_lubanlite.c`), the
  `carplay_bt` RFCOMM bring-up command, the `carplay_iap2` wireless iAP2
  session pump (`iap2_link_lubanlite.c`: identification, Wi-Fi config,
  CarPlay invite over the same transport) and its MSH commands;
  `examples/sdk_app/` holds the SDK app glue, Kconfig and the build-verified
  d21x defconfig.
- `tests/` — fake-bus MFi test plus host unit tests, all compiled with
  ASan/UBSan.  The end-to-end test runs a real controller implementation
  against `airplay_rtsp_server` on loopback: SRP-6a Pair-Setup, X25519
  Pair-Verify, the ControlCipher transition, SETUP/RECORD and encrypted
  Screen DataStream frames.
- `check-boundary.sh` — fails if platform APIs leak into `core/`.

## Build and test

The AirPlay modules need mbedTLS 2.28 (`MBEDTLS_CHACHAPOLY_C` and
`MBEDTLS_HKDF_C` enabled).  Fetch the pinned source once, or point
`RTCP_MBEDTLS_DIR` at an existing 2.28 checkout such as the one in the
RT-Thread/Luban-Lite SDK:

```sh
scripts/fetch-mbedtls.sh

make check
```

The host build compiles a private `libmbedcrypto.a` under `build/mbedtls`
with `config/mbedtls_rtcp_host.h`; nothing from mbedTLS is vendored in git.

The same tests are wired for CMake/CTest:

```sh
cmake -S . -B build -DRTCP_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Linux simulator

`build/rt-carplay-sim` is an end-to-end bench built by `make`: it advertises
`_airplay._tcp` over mDNS and serves the AirPlay RTSP endpoint with a
software Ed25519 identity.  A controller on the same LAN can pair and send
its Screen DataStream; the H.264 payload is written as an Annex-B dump.
There is no video or audio sink.

```sh
./build/rt-carplay-sim --ip 192.168.1.20 \
    --seed-file ./identity.seed --pairing-store ./pairings.bin \
    --dump-h264 ./screen.h264
```

`--name`, `--model`, `--device-id`, `--pair-code`, `--port`, `--no-dump`
and `--help` are also available.  The identity seed is generated on first
run (mode 0600); delete it to rotate the accessory key.  The pairing PIN is
static (`3939`) because this is a protocol bench, not a product, and the
process stays in the foreground until Ctrl-C.

Verified on the host: mDNS discovery (`PTR`/`SRV`/`TXT`/`A` for
`_airplay._tcp.local`) and `GET /info` returning the binary plist.  Pairing
and Screen streaming still need a real controller.

## License

**GPL-3.0-or-later** (architecture decision A1, confirmed 2026-11-08).  See
`LICENSE` and `NOTICE`.

- Modules ported from `esp32-carplay-web-relay` (GPL-3.0-or-later) must keep
  their copyright and license notices.
- Code translated from LIVI must carry the attribution required by its
  `NOTICE`: "Based on LIVI by Lasse Heitgres".
- Realtek binaries, vendor firmware blobs, and Apple MFi identity material are
  never part of this repository.
