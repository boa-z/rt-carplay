[English](audio-routing.md) | [简体中文](zh-CN/audio-routing.md)

# Music still plays on the iPhone: discovery capability and clock fixes

## Historical scope and current integration

This document preserves the investigation, logs, tests and image facts of the historical versions it names. References to this round, current or latest mean that investigation baseline, not physical acceptance of the present revision. During V2.1, the D213 application and concrete platform code moved to the Luban-Lite host; see [host migration](luban-lite-migration.md) for maintained paths and build entrypoints. Retained image hashes, test counts and old branch commands are provenance, not current build artifacts.

This page records the first two investigations. See [audio-negotiation.md](audio-negotiation.md) for the later printf correction, AudioPCM Features, dual-rate advertisement, firmware SHA and acceptance logs. The old Features, 44.1 kHz-only bitmaps and image SHA below describe their historical builds, not present expected values.

## What the logs establish

The user's board trace `tunnel=1 identified=1 authenticated=1 handoff_complete=1` establishes authenticated wireless iAP2 tunneling and completed handoff. A bootstrap Bluetooth `link down` at that stage follows the existing flow. `sound0 ref count=1` and a `carplay-aud` thread establish only that the output worker started, not receipt of Audio SETUP/RTP. Static-screen video `rx=0` also says nothing about the audio path.

## Code findings and fixes

1. **Wireless mDNS and RTSP capabilities disagreed.** With audio enabled, `/info` Features were `0x615653aee2`, but DNS-SD TXT construction omitted features and fell back to `0x5203a4e2,0x61`, the no-audio mask `0x615203a4e2`. Discovery Audio bit 9 was therefore zero. Both now use `airplay_info_plist_features()`, with explicit low/high 32-bit DNS-SD formatting `0x5653aee2,0x61`. Audio-device startup failure disables audio in both; custom Features also remain consistent. Both RT-Thread application and Linux simulator were corrected. This is a proven code defect consistent with the symptom, but whether it suffices to restore this iPhone's route requires a new board run.
2. **First clock wrap after about 65 seconds.** SDK `kernel/rt-thread/src/clock.c` initializes ticks to `0xffff0000`, wrapping after about 65.536 seconds at the default 1000 Hz. Casting 32-bit milliseconds to 64 bits cannot extend them. Backward time previously saturated screen interval subtraction to `4294967295` and affected shared AirPlay timing/audio jitter. A short interrupt-disabled section now extends modulo-32-bit deltas onto one 64-bit monotonic ms/us timeline. A running Receiver must sample at least once per 32-bit millisecond wrap period; normal polling is much faster. Playback jitter and microphone AEC reference use that clock too. This critical section targets the current single-core D213 and does not claim arbitrary SMP synchronization.
3. **Audio-stage status was added.** The poll owner periodically copies active stream count, received UDP datagrams, authentication failures and last audio modes. Shell reads snapshots instead of stream objects that the protocol thread may destroy. `carplay_status` distinguishes no stream from received packets without output; `entity` reports phone state and does not reinterpret an arbitrary entity value as successful audio-resource acquisition.

This round did not force MainAudio Take to change phone resource ownership or alter the existing audio-format set.

## Second board response: still no Audio SETUP

After flashing the discovery/clock fixes, the user still observed `streams=0 datagrams=0` and zero sink packets. The complete connection trace contained only 130/iAP2 and 110/screen, without 100/101/102 candidates. `modesChanged` received 200 and wireless handoff succeeded. The earlier fixes therefore had not restored this phone's audio route. Decode speed, speakers, UDP loss or larger buffers are not supported explanations; the evidence places this symptom before audio-stream establishment.

Further review found the application never supplied the actual Bluetooth address to `/info`: `bluetoothIDs` was always empty, while iAP2 Identification already used the real BR/EDR address. The associated identities differed because AirPlay started before Bluetooth and the server copied capabilities at startup.

The server now supports an optional accept-time capability provider. For each accepted control connection, the RT-Thread application reads the same `rtcp_bt_lubanlite_local_bdaddr()` as iAP2 and rebuilds `/info`; the server validates/copies it before constructing the session. The borrowed document is not modified during that session; reconnect refreshes it. Provider failure closes that connection while keeping the listener for retry. Bluetooth-disabled/wired mode may still have an empty Bluetooth ID. No MAC from user logs is hardcoded and the audio-format set is unchanged. This corrects a proven omission without proving it the sole reason for missing routing.

On each session's first `/info` request, logs read Features, format count and per-type/role input/output bitmaps from the actual response plist. This distinguishes application configuration, a phone actually reading AAC advertisement and subsequent SETUP. Logs contain no pairing keys, certificates or arbitrary request bodies.

## Validation

Historical commands:

```sh
make -j8 check
make BUILD=build-pcm AUDIO_CODECS=0 -j8 check
make check-audio-discovery
cmake -S . -B build-cmake -DRTCP_SANITIZE=ON
cmake --build build-cmake -j8
ctest --test-dir build-cmake --output-on-failure
make check-lubanlite-demo check-lubanlite-handoff SDK_ROOT=../luban-lite-jc-carplay-oss
```

At that baseline, 55 C test binaries passed. New tests compiled the actual RT-Thread OS seam, modeled first/repeated wrap and concurrent callers, and verified the shared ms/us timeline. Encrypted audio socket tests covered the new statistics. `check-audio-discovery` starts the actual Linux application, reads network DNS-SD TXT and RTSP `/info`, and checks bit-for-bit agreement with audio on/off. It uses standard mDNS multicast/unicast and requires local multicast support; it does not stop an existing desktop mDNS service. Default D213 `scons -j8` also built without compiler warnings. None establishes phone routing, physical sound output or acoustics.

After identity correction, the 55 tests, PCM-only suite, sanitized CTest, discovery, LVGL/handoff and SDK build passed again. Real RTSP socket tests additionally covered late Bluetooth identity in `/info`, provider-buffer reuse without mutating a live session, no refresh on repeated GET, refresh after reconnect, rejection of failed/oversized/corrupt/NULL provider data, and listener recovery afterward. The two Host socket suites share default ports: run `make check`, CTest and PCM suites sequentially to avoid port 7000 contention.

The maintained host equivalents, from the Luban-Lite root, are `make -C application/rt-thread/rtcarplay check-lubanlite-demo check-lubanlite-handoff`. The old command above is retained for historical reproducibility.

## Board procedure and interpretation

The historical instructions used application `main` and SDK `poc/carplay-oss`, with `scons -j8` at the SDK root. After flashing/rebooting, reconnect the iPhone to this Wireless CarPlay receiver so discovery information is read again. Deleting trust/bond files first is unnecessary.

Startup with default audio codecs should report:

```text
[carplay] capabilities features=0x5653aee2,0x61 audio_output=1 compressed=1
```

Connection should additionally report the actual board Bluetooth address:

```text
[carplay] session capabilities bluetoothID=66:55:44:33:22:11 bytes=...
I/airplay_rtsp_session: info=served bytes=... features=0x615653aee2 audio_formats=9
I/airplay_rtsp_session: info_audio type=102 role=media output=0x400000 input=0x0
```

That historical default offered 44.1 kHz AAC-LC, giving the type102 bitmap `0x400000`; explicit 48 kHz gives `0x800000`. `info=served` is emitted only for an actual `/info` request, not an assumption at boot. If absent, retain logs from connection start.

Retain the full trace through pressing Play on the phone, then run:

```text
carplay_status
carplay_iap2 status
```

Example new `carplay_status` fields, with traffic-dependent values:

```text
carplay audio transport: streams=1 datagrams=123 auth_failures=0
carplay audio modes: entity=1 call=0 speech=-1
```

- `streams=0`: no active Audio SETUP. Inspect capabilities from boot, whether SETUP was attempted/rejected/subsequently torn down, and the displayed phone output route.
- `streams>0, datagrams=0`: a stream exists but audio UDP has not arrived.
- `datagrams>0, auth_failures>0`: inspect audio keys/packet format rather than blaming speakers.
- UDP and sink packet counters increase without authentication errors but remain silent: investigate decoder, volume, sound0 write errors and physical output. Device reference count cannot replace that distinction.

Keep the connection beyond 70 seconds: interval statistics should not jump to `4294967295` at startup-tick wrap. Also check sustained music and establishment of any 100/101/102 stream. The fix does not require type102; follow actual phone negotiation.

Historical image SHA-256: `1af7ab53f843e58e8fd6d35a851760e36338fd0995020334561906f0e95dbdd7`. SDK directory: `output/d21x_demo128-nand_rt-thread_rtcarplay/images/`; file `d21x_demo128_nand_carplay_page_2k_block_128k_v1.0.0.img`.
