[English](README.md) | [简体中文](README.zh-CN.md)

# rt-carplay

[![ci](https://github.com/boa-z/rt-carplay/actions/workflows/ci.yml/badge.svg)](https://github.com/boa-z/rt-carplay/actions/workflows/ci.yml)

## Project overview

An independent CarPlay Receiver SDK with Linux Host and RT-Thread ports. The protocol core and portable Receiver runtime build without RT-Thread, Luban-Lite, ArtInChip or GUI headers. Hosts supply platform services, identity/pairing providers, media backends and input ownership through public C interfaces.

Current state: the portable Receiver API and public media registration are implemented; D213 composition, board adapters and board tests now belong to the Luban-Lite host. Lifecycle auditing and the complete SDK bilingual-document audit are still in progress. Historical D213 observations do not validate the current refactor on hardware. The application Connection Manager has moved to Luban-Lite; simultaneous USB/wireless policy and Launcher V2 are subsequent work.

## Features

- AirPlay/CarPlay RTSP, Pair-Setup/Pair-Verify, encrypted control/event/media, H.264 and HID output.
- USBMUX/Lockdown/TLS protocol support and wireless iAP2/MFi/Wi-Fi handover behind injected transport services.
- Independent transport, authentication, session, video, audio and input readiness; explicit owner-thread lifecycle and failure retry.
- Registered encoded-video, LPCM/AAC/Opus audio, microphone, modes/ducking/volume and iAP2 callbacks, preserving the existing audio behavior.
- Real-socket, cryptographic and lifecycle Host tests with ASan/UBSan; optional board adapters remain outside the protocol core.

## Architecture

- `examples/rtthread_headless/` — standalone filesystem-backed ordinary-thread Receiver bench with real identity/controller persistence, optional mDNS and a counted Null video sink; see its [integration and limitations](examples/rtthread_headless/README.md).

- `include/rtcp/` — public Receiver/platform/media/input/event contracts, OS/network/BT/MFi/Wi-Fi seams and common error codes.
- `core/mfi/` — Apple MFi 2.0C register state machine, extracted from the Luban-Lite PoC but with no RT-Thread dependency.
- `core/iap2/` — portable iAP2 link layer, parameter framing, Identify/IdentificationInformation, CarPlayStartSession/WiFiConfig builders, the MFi authentication state machine, and the wireless accessory session that ties them together behind injected transport/MFi/Wi-Fi/AirPlay callbacks.  Ported from `esp32-carplay-web-relay` (GPL-3.0-or-later, itself based on DiPlay); copyright notices retained.
- `core/airplay/` — binary plist, RTSP codec, TLV8, HID plists/descriptors, screen codec, DataStream AEAD decryption, ControlCipher streams, timing, Pair-Setup/Pair-Verify/SRP-6a, the MFiSAP responder, the `/info` binary plist writer and the RTSP session (info, SETUP, RECORD, teardown), and persistent pairing storage.  On top of those sit the poll-driven endpoints — ControlCipher event channel, UDP timing, Screen DataStream and the audio DataStreams (types 100/101/102: HKDF-SHA512 output key, ChaCha20-Poly1305 RTP datagrams, 12-byte header + 16-byte tag + 8-byte nonce tail) — and `airplay_rtsp_server`, a single-controller RTSP server that owns the listener, the ControlCipher transition after Pair-Verify, the media endpoints the session asks for and HID report output over the event channel (the touch-input path).
- `core/crypto/` — Ed25519 key/sign/verify wrapper over the vendored orlp/ed25519 (zlib), the software identity for Pair-Setup/Pair-Verify.
- `third_party/ed25519/` — unmodified orlp/ed25519 sources and its LICENSE.
- `core/media/`, `core/input/` — timestamp helpers and the input bridge.
- `core/discovery/` — AirPlay Bonjour TXT records, an mDNS responder for `_airplay._tcp` (transport-free packet builder plus UDP polling through the net seam), and the Apple CarPlay SoftAP vendor IE.
- `port/linux/` — POSIX OS and TCP implementations; the BT and Wi-Fi seams are explicit stubs there because a desktop host has no accessory stack.
- `port/rtthread/` — RT-Thread OS and BSD-socket seams; the plain RT-Thread BT seam stays an explicit `RTCP_ENOTSUP` stub (the concrete Zephyr transport lives in the Luban-Lite host).
- `port/luban-lite/` — integration documentation only; D213 implementation and actual-source regressions belong to the Luban-Lite repository.
- `Kconfig`, `SConscript` — RT-Thread/Luban-Lite build integration.
- `examples/linux_sim/` — the foreground Linux receiver bench (mDNS + RTSP + Screen dump) described under “Linux simulator”.
- The Luban-Lite host owns `app/` receiver composition, pairing files, USB commands and iAP2 workers, plus `platform/` Realtek, Zephyr, MFi, CherryUSB, MPP/DE, sound and touch implementations under `application/rt-thread/rtcarplay/`. Existing protocol timing and audio behavior are preserved; the SDK no longer compiles board implementation files.
- `tests/` — fake-bus MFi test plus host unit tests, all compiled with ASan/UBSan.  The end-to-end test runs a real controller implementation against `airplay_rtsp_server` on loopback: SRP-6a Pair-Setup, X25519 Pair-Verify, the ControlCipher transition, SETUP/RECORD, encrypted Screen DataStream frames and concurrent encrypted audio DataStream packets (HKDF output key, ChaCha20-Poly1305 RTP, PCM verified by the sink callback).
- `check-boundary.sh` — fails if platform APIs leak into `core/`, public headers or the portable Receiver runtime.
- The LVGL launcher and application Connection Manager belong to Luban-Lite at `application/rt-thread/rtcarplay/ui/lvgl_launcher.c` and `app/connection_manager.c`. The production SDK build never invokes an example SConscript.

## Supported platforms

| Platform | Implementation and evidence |
| --- | --- |
| Linux Host | POSIX OS/network ports and foreground simulator; Host Tested with real loopback pairing, encrypted media and reconnect. BT/Wi-Fi accessory services are explicit stubs. |
| Generic RT-Thread | OS/BSD-socket seams are available; the BT seam returns `RTCP_ENOTSUP`. The standalone `examples/rtthread_headless` composition runs without GUI/board services and requires real host entropy, sockets and filesystem providers. |
| D213 Luban-Lite | Optional Realtek/Zephyr/MFi/CherryUSB and MPP/sound/touch adapters. Published integration has GUI/Headless cross-build evidence; current-revision build results must be checked separately. |
| Physical controller and board | Historical observations exist in subsystem documents. Current refactor video, reconnect, device lifecycle and acoustic behavior are Pending Validation, not Hardware Validated. |

## Requirements

Use a C11 compiler, Make or CMake 3.16+, a POSIX shell, pthreads and mbedTLS 2.28. Public host configuration headers also compile as C99 and C++11. The Host build enables software AAC/Opus/SpeexDSP by default. Board builds additionally require the matching BSP, RT-Thread networking, hardware services and toolchain documented by the host integration.

The AirPlay modules need `MBEDTLS_CHACHAPOLY_C` and `MBEDTLS_HKDF_C`. Fetch the pinned mbedTLS source or set `RTCP_MBEDTLS_DIR` to an existing 2.28 checkout. A physical iPhone and suitable accessory transport/hardware are needed for board acceptance; Host tests do not establish physical pairing or playback.

## Build and test

```sh
./scripts/fetch-mbedtls.sh
make -j8 check

cmake -S . -B build-cmake -DRTCP_BUILD_TESTS=ON -DRTCP_SANITIZE=ON
cmake --build build-cmake -j8
ctest --test-dir build-cmake --output-on-failure
```

Make builds a private `libmbedcrypto.a` under `build/mbedtls` using `config/mbedtls_rtcp_host.h`; mbedTLS is fetched rather than committed. CMake uses its selected binary directory. Make enables ASan/UBSan by default; the CMake command above enables them explicitly. The suite executes 54 test binaries plus the legacy-configuration encrypted end-to-end variant (55 CTest cases), boundary checks and a public-header compilation without private include paths. Adapter-specific regressions are documented with the corresponding host integration.

## Quick start

After `make`, run the foreground protocol bench on your LAN address:

```sh
./build/rt-carplay-sim --ip 192.168.1.20 \
    --seed-file ./identity.seed --pairing-store ./pairings.bin \
    --dump-h264 ./screen.h264
```

The simulator advertises `_airplay._tcp` and serves RTSP using a software Ed25519 identity. The seed is generated on first run with mode 0600; deleting it rotates the accessory identity. The protocol-bench PIN is static (`3939`). Keep pairing state between runs to exercise Pair-Verify reconnect. The process stays in the foreground until Ctrl-C.

## SDK integration

Include `rtcp/receiver.h` and populate `rtcp_receiver_config_t` with public `platform`, `media` and `iap2` descriptors. A single host-selected owner creates, starts, polls, stops and destroys the receiver. Queue cross-thread requests on the host, retire host workers before freeing their contexts, and keep borrowed OS/network/provider tables valid through destruction. No GUI or internal RTSP configuration header is needed for this contract. See [Receiver API](docs/receiver-api.md) and [lifecycle/ownership](docs/lifecycle-and-ownership.md).

The optional `rtcp/receiver_compat.h` converter migrates existing low-level `airplay_rtsp_server_config_t` configuration. The legacy `carplay_app_*` wrapper now belongs to the Luban-Lite host; it is not the portable Receiver threading contract.

## Linux Host usage

`--name`, `--model`, `--device-id`, `--pair-code`, `--port`, `--no-dump`, `--no-audio` and `--help` are also available. Audio is advertised by default; `--no-audio` selects video-only operation. H.264 is written as an Annex-B dump; decrypted audio packets are counted/discarded, with no Host playback device.

Host verification covers mDNS `PTR`/`SRV`/`TXT`/`A` records for `_airplay._tcp.local`, binary-plist `GET /info`, role-specific `audioFormats`/`audioLatencies`, encrypted RTP and payload verification, SET_PARAMETER volume callbacks, tampered-frame rejection and stored-pairing reconnect. Optional codec/capture capabilities change capability size and format masks; unsupported codecs are not advertised.

## RT-Thread integration

Call the SDK root `SConscript` from the host application and source its `Kconfig` once. `port/rtthread/` supplies generic OS/network seams. The Luban-Lite host builds its own composition and hardware backends; its board Kconfig symbols are separate from the SDK switches. The obsolete example application entry reports an explicit migration error. See the [RT-Thread port](port/rtthread/README.md), [Luban-Lite integration](port/luban-lite/README.md) and [host migration](docs/luban-lite-migration.md).

## Video, audio and input interfaces

`rtcp/media.h` registers borrowed Annex-B configuration/access units with sender-nanosecond and arrival-microsecond timestamps. The host owns H.264 decoding, queues, DMA/cache handling, display and HMI policy. A NULL video callback is an intentional discard sink.

Audio callbacks preserve LPCM little-endian S16 or encoded AAC/Opus plus codec, rates, RTP timestamp and stream-slot metadata. NULL audio refuses Audio SETUP. Microphone start/stop, selective teardown, resource modes, ducking and volume retain their current contracts. See [audio subsystem](docs/audio-subsystem.md) for the format/stream matrix, microphone/AEC configuration, measured Host costs, sanitizer checks and board commands; see [audio debug handoff](docs/audio-debug-handoff.md) for frozen behavior and unresolved routing/zero-stream symptoms.

`rtcp/input.h` exposes owner-thread HID/Siri output; Receiver also exposes tunneled iAP2 writes. The host owns touch reading, focus, coordinate conversion and cross-thread queues. No video backend may force a persistent host HMI to disappear.

## Development status and known limitations

The portable API, native Linux/RT-Thread configuration, public media metadata and compatibility conversion are Host Tested. The wrapper's scheduling model covers rollback, immediate stop, input exclusion and Bluetooth claims; it does not prove every legacy diagnostic or borrowed string is thread-safe. Board-adapter ownership and Kconfig ownership are separated. The standalone Headless example has real-socket/discovery/persistence Host regression coverage. Remaining device-worker lifecycle audits and physical acceptance are still open. All 26 maintained primary documents now have complete Chinese counterparts and a local/CI documentation gate; mechanical checks do not prove semantic translation accuracy.

Audio negotiation/output policy is frozen during this refactor. Existing iPhone routing, zero Audio SETUP streams, Direct/Mixer and acoustic issues remain separate investigations. No Android Auto, CarLife or CarLink support is included. cherryusb-ipheth stays independent; the host owns USB configuration selection and CarPlay sessions. Do not equate Host Tested or Cross-Compiled with Hardware Validated.

## Documentation

- [Architecture](docs/architecture.md), [Receiver API](docs/receiver-api.md) and [lifecycle/ownership](docs/lifecycle-and-ownership.md) have full English/Chinese pairs.
- [Audio debug handoff](docs/audio-debug-handoff.md) has a full English/Chinese pair.
- Historical [60 FPS feasibility](docs/1024x600-60fps-feasibility.md), [Zalink comparison](docs/zalink-performance-comparison.md), [audio routing](docs/audio-routing.md), [audio negotiation](docs/audio-negotiation.md) and [wired pairing persistence](docs/wired-pairing-persistence.md) have full English/Chinese pairs; their versioned observations are preserved as historical evidence.
- [Bond reconnect](docs/bluetooth-bond-reconnect.md), [wired video performance](docs/wired-video-performance.md) and [wired stall investigation](docs/wired-video-stall-probe.md) also have full pairs with historical measurements and current ownership notes. [RT-Thread](port/rtthread/README.md) and [Luban-Lite](port/luban-lite/README.md) port notes, [synthetic audio fixtures](tests/fixtures/audio/README.md) and [audio dependency provenance](third_party/audio/README.md) have full pairs. Current build/entropy/lifecycle guidance is distinguished from preserved bring-up commands.
- [Audio subsystem](docs/audio-subsystem.md), [audio reference review](docs/audio-routing-reference-review.md) and [wireless bring-up](docs/wireless-bringup.md) have full pairs, preserving implementation matrices, measured Host costs, historical phone/board feedback and unresolved limits. Audio behavior remains frozen during V2.1.
- [USBMUX negotiation/validation](docs/usbmux-validation.md), [wired authentication investigation](docs/wired-auth-investigation.md) and the [retired RT-Thread board-template record](examples/rtthread_app/README.md) also have full counterparts. Historical commands and results remain version-scoped; current host ownership and command equivalents are identified.
- Run `make check-docs` for all 26 pairs: discovery, naming, reciprocal navigation, local files/heading fragments, paragraph layout, language, heading hierarchy, exact fenced blocks, inline literals and external-reference parity. The gate reuses the Apache-2.0 checker from cherryusb-ipheth and runs its 17 isolated mutation regressions. External sites are not fetched. `docs/documentation-exceptions.json` exempts only preserved upstream Ed25519 attribution. Human review remains necessary for technical facts, numeric prose and translation meaning.

## License

**GPL-3.0-or-later** (architecture decision A1, confirmed 2026-11-08). See `LICENSE` and `NOTICE`.

- Modules ported from `esp32-carplay-web-relay` retain their GPL-3.0-or-later copyright/license notices.
- Code translated from LIVI carries its required attribution: "Based on LIVI by Lasse Heitgres".
- Realtek binaries, vendor firmware blobs and Apple MFi identity material are not distributed in this SDK repository.

## Acknowledgments

The iAP2 work derives from `esp32-carplay-web-relay` and DiPlay, with original notices retained. The screen implementation records DiPlay's protocol behavior; relevant audio transport references and LIVI attribution are preserved in source and notices. orlp/ed25519, mbedTLS, FAAD2, Opus and SpeexDSP retain their respective third-party licenses. RT-Thread, Luban-Lite/ArtInChip and CherryUSB supply the host integration foundations; cherryusb-ipheth remains a separately maintained driver.

## Repository history

The owner requested repository recreation to remove closed pull-request records after the portable SDK refactor. The current main branch starts from the verified SDK source baseline. Earlier commit and CI references in research documents remain historical evidence and may no longer resolve in this repository; the previous complete history, pull-request metadata and final CI logs were preserved in verified offline backups.
