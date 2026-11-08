# rt-carplay (local draft)

This directory is a **local, unpublished** skeleton for the independent
`rt-carplay` project.  Nothing here has been pushed to
`github.com/boa-z/rt-carplay`; the repository URL, license, and CI are
deliberately not finalised yet.

Purpose of this skeleton (M0): prove that the protocol core can be built and
tested on a Linux host with no RT-Thread, Luban-Lite, or ArtInChip headers.

Current contents:

- `include/rtcp/` — the six platform seams (os, net, bt, mfi, wifi, media,
  input) plus common error codes.
- `core/mfi/mfi2c.c` — Apple MFi 2.0C register state machine, extracted from
  the Luban-Lite PoC but with no RT-Thread dependency.
- `core/iap2/` — portable iAP2 link layer, parameter/message builders for
  Identify/IdentificationInformation, CarPlayStartSession/WiFiConfig, and the
  MFi authentication state machine.  Ported from `esp32-carplay-web-relay`
  (GPL-3.0-or-later, itself based on DiPlay); copyright notices retained.
- `tests/` — fake-bus MFi test plus the upstream host tests for every iAP2
  module, all compiled with ASan/UBSan.
- `check-boundary.sh` — fails if platform APIs leak into `core/`.

Build and test (no cmake required):

```sh
make check
```

The same test is also wired for CMake/CTest:

```sh
cmake -S . -B build -DRTCP_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

License: **GPL-3.0-or-later** (architecture decision A1, confirmed
2026-11-08).  See `LICENSE`.

- Modules ported from `esp32-carplay-web-relay` (GPL-3.0-or-later) must keep
  their copyright and license notices.
- Code translated from LIVI must carry the attribution required by its
  `NOTICE`: "Based on LIVI by Lasse Heitgres".
- Realtek binaries, vendor firmware blobs, and Apple MFi identity material are
  never part of this repository.
