[English](README.md) | [简体中文](README.zh-CN.md)

# Luban-Lite integration

Build the maintained D213 firmware from [luban-lite-carplay](https://github.com/boa-z/luban-lite-carplay/tree/carplay-oss/application/rt-thread/rtcarplay). Its application owns the connection manager, CherryUSB and Bluetooth adapters, MFi provider, hardware media, and LVGL. It pins this SDK as a submodule.

## Integrate on another board

1. Call the SDK root `SConscript` and source its `Kconfig` once. Set board features in the host application.
2. Reuse the generic [RT-Thread port](../rtthread/README.md) for clocks and sockets. Bring up the physical transports, real entropy and identity providers on the host.
3. Build a capability plist for the available media and input devices. Register backends and run the [Receiver lifecycle](../../docs/receiver-api.md) on one thread.
4. Run Host tests and cross-build, then validate plug/unplug, Bluetooth pairing, VSync, audio and microphone on the target.

## Migration and historical detail

The [migration guide](../../docs/luban-lite-migration.md) maps former board files to their active owners. The earlier bring-up record with register experiments and obsolete build recipes is available through `git log -- port/luban-lite/README.md`.
