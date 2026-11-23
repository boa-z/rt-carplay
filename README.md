[English](README.md) | [简体中文](README.zh-CN.md)

# rt-carplay

[![ci](https://github.com/boa-z/rt-carplay/actions/workflows/ci.yml/badge.svg)](https://github.com/boa-z/rt-carplay/actions/workflows/ci.yml)

rt-carplay is a portable C SDK for implementing a Classic CarPlay receiver. It handles CarPlay protocol sessions and delivers encoded media and metadata to a host application. The host owns device I/O, Bluetooth and Wi-Fi policy, USB enumeration, video decode/display, audio devices and UI.

## Start here

| Goal | Guide |
| --- | --- |
| Evaluate Classic support and physical test status | [Classic capability matrix](docs/classic-capability-audit.md) |
| Embed the SDK in an application | [Receiver integration](docs/receiver-api.md) and [lifecycle and ownership](docs/lifecycle-and-ownership.md) |
| Understand the source layout and how the layers interact | [Architecture](docs/architecture.md) |
| Port to RT-Thread or use the headless sample | [RT-Thread port](port/rtthread/README.md) and [RT-Thread headless example](examples/rtthread_headless/README.md) |
| Run on ArtInChip D213 or use Luban-Lite on another board | [Luban-Lite integration](port/luban-lite/README.md) |

## Run the Linux bench

Use a POSIX shell, a C11 compiler, Python 3, Make, pthreads and the pinned mbedTLS 2.28 source. The bench uses real sockets and a persistent software identity. It writes received H.264 to a file and does not drive a display or speaker.

```sh
./scripts/fetch-mbedtls.sh
make -j8 check
./build/rt-carplay-sim --ip 192.168.1.20 \
    --seed-file ./identity.seed --pairing-store ./pairings.bin \
    --dump-h264 ./screen.h264
```

Replace the address with the host LAN address. `--help` lists the bench options. The default pairing PIN is `3939`. Keep the seed and pairing store between runs to exercise reconnect. The Linux and generic RT-Thread ports require accessory-specific adapters for physical CarPlay.

## Embed the receiver

1. Include `rtcp/receiver.h`. Supply clock/socket providers, pairing/identity callbacks and the capability plist, then register media sinks. See the [Linux composition](examples/linux_sim/main.c) for a complete example.
2. Call `rtcp_receiver_create()`, `rtcp_receiver_start()` and `rtcp_receiver_poll()` on one host-owned thread. Queue UI and input commands to that thread. Callback arguments are borrowed through return, so copy data before dispatching to another worker.
3. Close the session on the owner with `rtcp_receiver_stop()` and destroy it before releasing borrowed providers. Stop and join host-owned device workers before releasing the memory they use.

The [integration guide](docs/receiver-api.md) covers H.264, audio, HID, Classic notifications, and compatibility with low-level AirPlay and iAP2 callers. Use the platform guide for the relevant transport and keep hardware policy with the host.

## Develop and verify

The Make check uses ASan/UBSan by default. CMake 3.16 or later provides the same Host test entry points.

```sh
cmake -S . -B build-cmake -DRTCP_BUILD_TESTS=ON -DRTCP_SANITIZE=ON
cmake --build build-cmake -j8
ctest --test-dir build-cmake --output-on-failure
make check-docs
make docs
```

`make docs` generates the public C API HTML with Doxygen in `build/doxygen/`. Build provenance comes from `include/rtcp/version.h` and the checkout commit. Call `rtcp_version_string()` and `rtcp_build_git_sha()` to report both.

Update the public contract and both languages when changing APIs, options or externally visible behavior. Put status updates in the capability matrix. Internal refactors and test-only changes do not automatically require README updates. See [formatting](docs/code-formatting.md).

## Protocol notes and research

Use [audio negotiation](docs/audio-negotiation.md) and [video sessions](docs/video-stream-session.md) when debugging the protocol. [Wireless bring-up](docs/wireless-bringup.md), [bond reconnect](docs/bluetooth-bond-reconnect.md), [wired trust](docs/wired-pairing-persistence.md) and [audio debug handoff](docs/audio-debug-handoff.md) preserve historical investigations. For work beyond Classic, start at the [experimental Ultra foundation](docs/ultra-foundation.md).

## License and acknowledgments

**GPL-3.0-or-later**. See [LICENSE](LICENSE) and [NOTICE](NOTICE) for component notices and third-party licenses. Based on LIVI by Lasse Heitgres (https://github.com/f-io/LIVI). The iAP2 work derives from `esp32-carplay-web-relay` and DiPlay, with original notices retained.
