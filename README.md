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
  Pair-Setup/Pair-Verify/SRP-6a, the MFiSAP responder and the RTSP session
  (info, SETUP, RECORD, teardown), and persistent pairing storage.
- `core/media/`, `core/input/` — timestamp helpers and the input bridge.
- `core/discovery/` — AirPlay Bonjour TXT records and the Apple CarPlay
  SoftAP vendor IE.
- `tests/` — fake-bus MFi test plus host unit tests, all compiled with
  ASan/UBSan.
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

## License

**GPL-3.0-or-later** (architecture decision A1, confirmed 2026-11-08).  See
`LICENSE` and `NOTICE`.

- Modules ported from `esp32-carplay-web-relay` (GPL-3.0-or-later) must keep
  their copyright and license notices.
- Code translated from LIVI must carry the attribution required by its
  `NOTICE`: "Based on LIVI by Lasse Heitgres".
- Realtek binaries, vendor firmware blobs, and Apple MFi identity material are
  never part of this repository.
