[English](audio-negotiation.md) | [简体中文](zh-CN/audio-negotiation.md)

# Wireless CarPlay without Audio SETUP: negotiation investigation and fixes

## Historical scope and current integration

This document preserves the investigation, logs, tests and image facts of the historical versions it names. References to this round, current or latest mean that investigation baseline, not physical acceptance of the present revision. During V2.1, the D213 application and concrete platform code moved to the Luban-Lite host; see [host migration](luban-lite-migration.md) for maintained paths and build entrypoints. Retained image hashes, test counts and old branch commands are provenance, not current build artifacts.

The later mic-on board run recorded type102 AAC Audio SETUP and a visible CarPlay Control Center route selected by default. Investigation then moved to abnormal sound and local volume control; see the [reference implementation review](audio-routing-reference-review.md). The SHA below identifies historical firmware. Offline PCM/AAC playback subsequently worked while CarPlay remained abnormal; staged diagnostics/direct-output comparison are in section 21 of the historical [SDK integration guide](../port/luban-lite/README.md), with actual music-PCM capture/replay diagnostic images in section 24.

The investigation baseline was main `bffc3be`. Board feedback still showed `entity=1`, `streams=0`, `datagrams=0`, `packets=0`, with only 110/screen and 130/iAP2 in the connection trace. This run had not reached downstream audio transport. That evidence does not implicate AAC decoding, Mixer, jitter queue or sound0; these backends were not functionally changed in this round.

## Confirmed logging error

SDK `.config` did not enable `RT_KPRINTF_USING_LONGLONG`. Its `rt_vsnprintf()` consumed only one `l` in `%llx`, emitted `%lx` literally and did not consume the 64-bit argument. The next `%u` then interpreted that argument's low 32 bits as the format-entry count. The reported `audio_formats=1448324834` is exactly `0x5653aee2`, the old Features low word: **it does not mean the phone received 1.4 billion formats and does not establish plist corruption**.

Protocol logging now uses two explicit 32-bit arguments with `%08x%08x`, without changing global RT-Thread printf configuration. The same correction covers 64-bit format/connection-ID logs in session/server/audio UDP endpoints so following ports/status remain readable after SETUP failures. Protocol encoding, key derivation and audio data are unchanged.

The historical command `make check-lubanlite-capabilities SDK_ROOT=../luban-lite-jc-carplay-oss` extracts the SDK formatter and actual application capability-check/protocol-log functions for a Host ASan/UBSan test with long-long support disabled. Coverage includes high/low words, following format count, Bluetooth identity, encoded TXT/plist agreement and rejection of wrong values, duplicate Features and truncated TXT. This models the failure more closely than glibc printf alone, but is not a D213 run. The maintained host equivalent, from the Luban-Lite root, is `make -C application/rt-thread/rtcarplay check-lubanlite-capabilities`.

## Bluetooth identity and Features checks

- On accepting a control connection, main refreshes `/info`. With Bluetooth enabled, both application and iAP2 identity builders use `rtcp_bt_lubanlite_local_bdaddr()` and the same real address in display byte order, not a hardcoded MAC. After the server copies it, the session document remains immutable.
- Logs read `bluetoothIDs` count/address values from the **actual returned plist**, while iAP2 logs its identity address during construction. Both must agree on the board; a startup address line alone is insufficient. Without access to the board, do not claim capture of the bytes read by that phone.
- The same function derives Features. The application also parses **encoded plist and TXT** and permits startup/refresh only with a unique matching field, reporting `info_match=1`. Host regression rejects malformed, duplicate, truncated and mismatched values.

## Open receiver protocol comparison

The references are public source; no Apple private SDK, authentication material or new codec dependency was introduced.

| Behavior | Reference and finding | Change in this round |
| --- | --- | --- |
| AudioPCM Features bit 18 | catplay sets it; original main advertised PCM formats without it | Set when audio output exists; AAC bit 20 only when AAC-LC is compiled and offered |
| Media sample rates | catplay offers 44.1/48 kHz PCM and AAC-LC; the old application chose only 44.1 | Offer both implemented rates by default, letting the phone choose; explicit 44100/48000 still restricts |
| 102/media latency entry | catplay uses 102/media; DiPlay's old table and original main used 102/default | Match the actual 102/media format entry |
| Initial resource modes | DiPlay uses MainScreen/MainAudio Take, NiceToHave, Anytime; main already matched | Preserve resource modes; do not force an extra MainAudio Take |
| entity=1 | catplay `EntityEnum::Controller=1`, `Accessory=2` | A reported controller entity, not proof of received audio or a request to seize MainAudio for the accessory |
| mainBuffered/type103 | References have a separate TCP/key/anchor protocol requiring complete advertisement/negotiation | This project does not advertise it; do not add a successful stub to lure the phone into choosing 103 |

Reference revisions:

- [catplay audio_defaults.rs](https://github.com/catplay-labs/catplay/blob/66340ad4a0ec8257dafb8ed2d693d1adfc81740b/carplay/catplay_carplay/src/carplay_rx/sink/audio_defaults.rs).
- DiPlay commit `615b32ce0b40cd0b99ce94ba23a512001553684f`, [AirPlayInfoPlist.kt](https://github.com/shihabal3amri/DiPlay/blob/615b32ce0b40cd0b99ce94ba23a512001553684f/shared/src/main/java/com/shilapi/xcertplay/airplay/AirPlayInfoPlist.kt).

These are receiver protocol choices, not proof that this D213/iPhone route was fixed. Existing board evidence does not uniquely identify iOS's internal reason for rejecting the route. This round corrected verifiable omissions/differences; the next acceptance run must observe phone-initiated 100/101/102 SETUP.

## Advertised defaults and board acceptance

At this historical baseline, D213 with audio/compressed codecs enabled and microphone disabled reports:

```text
[carplay] capabilities features=0x5657aee2,0x61 audio_output=1 compressed=1
[carplay] capabilities_check txt_features=0x5657aee2,0x61 info_match=1
[iap2] identity bluetoothID=<板端真实BR/EDR地址>
I/airplay_rtsp_session: info_bluetooth count=1
I/airplay_rtsp_session: info_bluetooth index=0 id=<与iAP2一致的地址>
I/airplay_rtsp_session: info=served bytes=... features=0x000000615657aee2 audio_formats=9
I/airplay_rtsp_session: info_audio type=100 role=media output=0x000000000000cffc input=0x0000000000000000
I/airplay_rtsp_session: info_audio type=102 role=media output=0x0000000000c00000 input=0x0000000000000000
```

Relative to the old Features, this adds the supported PCM bit 18. PCM-only builds no longer claim AAC bit 20: their mask is `0x615647aee2`, with eight formats. Without an audio device, audio remains unadvertised. The new `audio_sample_rate=0` means offer 44.1 and 48 kHz. Integrations that previously relied on zero to mean 44.1 kHz only can set 44100 explicitly or `CARPLAY_APP_AUDIO_SAMPLE_RATE`. `info=served` appears only after an actual `/info` request, not an assumption that the phone read it.

Flash/reboot the board, reconnect wireless CarPlay and press Play in its music UI. Retain the entire connection trace and run `carplay_status` and `carplay_iap2 status`. Entry to audio transport requires observations such as:

```text
setup=stream_candidate index=0 type=0x0000000000000066
audio_setup=ready type=102 ...
carplay audio transport: streams=1 datagrams=<持续增长> auth_failures=0
```

100/101 are also valid: judge the phone's actual selection rather than requiring 102. If all three SETUP types remain absent, retain identity/Features/format-table/mode logs and whether CarPlay appears in the iPhone output selector. If a candidate exists but is rejected, investigate that SETUP error instead of interpreting every `streams=0` as no attempt. Investigate playback backends after UDP arrives.

## Automation and image

The historical checks passed 55 Host tests, PCM-only configuration, sanitized CTest, boundary checks and the default SDK build. Additional SDK formatter/capability, real Linux DNS-SD/RTSP agreement, LVGL and wireless-handoff regressions are documented in the integration guide. Run network suites sequentially to avoid port 7000 contention. Host success, SDK build success and actual D213/iPhone success are separate results.

Historical image directory: SDK `output/d21x_demo128-nand_rt-thread_rtcarplay/images/`; file `d21x_demo128_nand_carplay_page_2k_block_128k_v1.0.0.img`. SHA-256: `9af6c4ad1121c1372276b70ab655f8c4396a9aa996d2c12496922d6487c016b0`.
