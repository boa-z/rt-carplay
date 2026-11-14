[English](luban-lite-migration.md) | [简体中文](zh-CN/luban-lite-migration.md)

# Luban-Lite host ownership migration

## Source ownership

The D213 implementation lives in [boa-z/luban-lite-carplay](https://github.com/boa-z/luban-lite-carplay/tree/carplay-oss/application/rt-thread/rtcarplay). Paths below are relative to that repository. The SDK remains a Git submodule at `packages/third-party/rt-carplay`; cherryusb-ipheth remains a separate submodule and driver. No protocol implementation or external driver is copied into the application.

| Host directory | Responsibility |
| --- | --- |
| `application/rt-thread/rtcarplay/app/` | Receiver composition, connection manager, iAP2 pump, USB commands, pairing files and trust cache |
| `application/rt-thread/rtcarplay/platform/` | Realtek Wi-Fi, Zephyr Bluetooth/bonds, CE entropy, MFi, CherryUSB seam, MPP/DE video, sound output/capture and touch reader |
| `application/rt-thread/rtcarplay/ui/` | LVGL launcher and UI visibility policy |
| `application/rt-thread/rtcarplay/tests/` | Actual-source board/composition regressions and host device/scheduler seams |

These implementations previously lived in SDK `runtime/rtthread/`, `port/luban-lite/` and `port/luban-lite/app/`. File names are retained to keep the move reviewable. `port/luban-lite/` now contains integration documentation only. SDK `SConscript` collects generic runtime, protocols, RT-Thread ports and optional codecs; it neither selects hardware nor invokes example applications. Host `SConscript` compiles the composition and hardware backends. Generic Kconfig symbols have one owner in SDK `Kconfig`, sourced once by the host application; board options belong to the host.

## Integration and compatibility

Use public `rtcp/receiver.h` platform/media/iAP2 descriptors and owner-thread lifecycle operations. The host composition uses public diagnostics and input/output operations and holds no internal RTSP server pointer. Existing `carplay_app_*` declarations now live at host `app/carplay_app.h`. Include that directory when using this board-specific API. The old `examples/rtthread_app` header and build entry issue explicit migration errors rather than silently linking an incomplete receiver. The obsolete `examples/sdk_app` board template is retired in favor of the maintained host application and its target defconfigs.

This move preserves Wi-Fi handoff timing, Bluetooth bond files, USB trust records, MPP queues, device operations and audio behavior. It introduces no automatic simultaneous USB/wireless standby or Launcher V2 behavior. Subsequent host changes add copied singleton snapshots, queued owner commands, retained endpoint storage and acknowledged worker retirement. See the host lifecycle contract for the implemented boundaries and Host evidence.

## Reproducible verification

From the Luban-Lite repository, populate submodules and the pinned Host cryptography dependency, then run the actual board/composition suite:

```sh
git submodule update --init --recursive
packages/third-party/rt-carplay/scripts/fetch-mbedtls.sh
make -C application/rt-thread/rtcarplay -j8 check
```

The host Makefile builds portable SDK libraries and runs the five relocated audio/composition/persistence binaries plus video/queue/UI policy, touch, manager/launcher, bond restoration, USB DMA, handoff, reconnect and capability regressions. Device/scheduler seams are modeled; protocol, media and adapter functions are actual sources. The audio sink still reads the SDK's existing AAC fixture, using an explicit path rather than depending on the process working directory. `RTCP_ROOT` can select a separate SDK checkout. The CI host regression job runs this command under ASan/UBSan.

SDK `make -j8 check` and sanitized CMake/CTest remain independent and execute 54 binaries plus the encrypted legacy-configuration variant, yielding 55 CTest cases. The five board binaries were moved, not replaced with empty tests. The generic RT-Thread clock test stays in the SDK; `make check-rtthread-multicast` uses its own SAL header seam.

GUI and Headless firmware jobs cross-compile both maintained D213 defconfigs and package complete images with a matching same-tree baremetal Bootloader. Their exact source revisions, CI conclusions and artifact hashes must be recorded separately. Host regressions and compilation do not prove board display, USB/radio reconnect, audio playback or microphone operation. Physical validation of this migration is NOT_RUN.
