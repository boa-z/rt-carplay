[English](audio-routing-reference-review.md) | [简体中文](zh-CN/audio-routing-reference-review.md)

# Wireless music has no audio stream: item-by-item reference review

## Historical scope and current integration

This document preserves all rounds of music-routing, negotiation, playback and diagnostic investigation. "This round", "latest", "current" and test counts refer to each historical baseline. Earlier streams=0 conclusions must not be applied to later type102 sessions, and historical listening/video confirmation is not acceptance of the maintained image. These audio changes predate V2.1; this update only organizes documentation, without adding or restoring audio-repair experiments. See [audio debug handoff](audio-debug-handoff.md) for the V2.1 audio freeze and outstanding work. Actual D213 sinks, offline diagnostics, microphone, Launcher and board tests now live in the Luban-Lite host under `application/rt-thread/rtcarplay/`; portable protocol/audio algorithms remain in the SDK. See [host migration](luban-lite-migration.md). Old SDK build-document sections, images and commands remain provenance, not current firmware identity. From the Luban-Lite root, run maintained host regressions with `make -C application/rt-thread/rtcarplay -j8 check` and independent SDK regressions with `make -C packages/third-party/rt-carplay -j8 check`. Real phone routing, speaker quality, microphone/AEC, timing and hardware headroom still require separate acceptance against a matching image.

## Direct output still sounds wrong: capture real music PCM and replay continuously

The user confirms abnormal sound in direct mode, so mixing/resampling/silence insertion cannot sufficiently explain every symptom. Working offline PCM/AAC still does not prove correct decoded real-music content or real-time playback cadence. No physical capture file or DMA-underrun count is available, so no sole root cause is assigned.

Code comparison shows that offline and live paths use the same AAC ASC/FAAD configuration; differences include actual AAC content, real-time supply and concurrent load. Offline tests default to 20% sample amplitude, whereas unset CarPlay volume defaults to unity. First use carplay_volume 70, approximately 21% amplitude, to equalize gain. SDK audio.c _audio_send_replay_frame inserts zeroes directly when its queue empties without increasing application write_errors, so zero write errors cannot exclude starvation. The sound0 pool at this baseline has two 2048-byte blocks. Real-time scheduling/arrival jitter warrants checking, but does not alone prove the fault.

Add carplay_audio_probe capture/replay/stop/clear/status. The writer allocates at most 384000 bytes on demand, retaining two seconds of type102 stereo PCM after decoding and before gain, without saving a file. Once capture completes, pause phone music while preserving the connection and replay continuously with the same gain. Replay and direct share the output function, bypassing live packet waits. Live media is discarded during replay; completion rebuilds the live decoder, and calls/other prompts can preempt it. STOP drains the final partial sound-card block. Probe state and ignored_live in status explicitly distinguish this diagnostic behavior.

Regression also exposed receive callbacks returning ETIMEDOUT on queue-lock contention. The old try-lock path neither enqueued nor incremented dropped_bytes on failure; a high-priority receiver preempting the writer while it held the lock could cause hidden drops. The wait is now bounded to 2 ms, with the board's 1 ms tick and RT mutex priority inheritance; timeout increments queue_busy and dropped_bytes. There is no decoding/device write under the lock and no unbounded wait. Host modeling now implements positive-timeout timed locks, with timeout-drop accounting and receive-recovery regression. The defect is confirmed, but has not been shown sufficient to explain persistent physical clicking.

Normal replay points first toward real-time supply/playback buffering; abnormal replay calls for checking actual AAC decoding and the shared output/gain path. This was not physically validated and clicking is not marked fixed. Operations are in the application README; the image and validation results were in SDK build-record section 24.

## New logs: normal 1024-frame AAC increments; direct listening comparison pending

New board logs show 1024 decoded frames per packet after the initial zero-frame output and RTP timestamp increments of 1024. init_err/decode_err/negative_ts/mix_drop/device_errors are zero, and peak decode time is 2 ms. The initial six-byte AAC packet also decodes successfully; length alone cannot identify incorrect PCM input. Cumulative silence_frames grows from 6144 to 23552, the latter about 534 ms at 44.1 kHz. This reflects compensation for multiple timestamp gaps, not necessarily one continuous 534 ms gap; missing transport packets, sender jumps and playback-resume boundaries remain unseparated. arrival_max_ms=22323 is the lifecycle maximum and can include stopped playback, not per-packet delay.

An immediate requested=direct/last_output=mixer after switching is an asynchronous-transition snapshot; subsequent playback 44100 Hz shows source-rate output. At this point the user had not provided direct-mode listening or subsequent status, so the mixer cannot be called the root cause or audio quality restored.

One long diagnostic line exceeded the SDK's 128-byte console buffer, truncating duplicate/overflow and the newline. This round splits lines so each retains its newline even with maximum 32-bit counters. It adds expected/ts/delta/frames for at most eight nonzero gaps per stream generation, allowing the first gap to be observed beyond the first eight packets. Decoding, timeline and mixing behavior are unchanged. Validation and the updated image were in SDK build-record section 23.

## Offline tests work, CarPlay does not: stage counters and direct-output comparison

The user confirms expected offline-audio behavior. The latest CarPlay stack watermark is total=98304, used_peak=47700, free_min=50604, showing no stack-budget shortage in executed paths. Zero write_errors/auth_failures does not prove correct content/timing. Sink packets increase from 124 to 206 while transport changes from streams=0 to streams=1/datagrams=82, showing stream reestablishment. Sink statistics accumulate over the Receiver lifecycle; transport describes current active streams. These two status intervals cannot directly yield a music-packet rate.

Add stage statistics to `carplay_status`: decoded packets/frames, zero-frame outputs, initialization/decode errors, PCM peak, decode time, negative-timestamp drops, inserted silence, mixer-enqueue failure, queue occupancy and maximum arrival gap per lane; output adds actually written frames, peak, device errors and maximum write time. The first eight packets of each stream generation log codec/rate/clock/payload/ts/step/frames, never raw audio or keys. Compare deltas of cumulative counts/peaks within the same Receiver run. mix_queue is the writer's latest observation; jitter_queue is the query-time snapshot.

Fix the AAC-priming zero-frame timeline boundary: zero-frame output is counted without advancing expected, preventing artificial silence on the next packet. This code defect does not directly establish that persistent clicking is resolved.

Add diagnostic `carplay_audio_path [mixer|direct]`, default mixer. Direct applies only to standalone type102 stereo music: retain network, decryption, jitter and the real decoder, bypass timestamp compensation/resampling/mixing, and output to sound0 at source rate while retaining media-focus gain and software volume. Calls, Siri, other tracks or local prompts fall back to mixing. `requested` is the request and `last_output` is the last actual path; stopped playback does not automatically make it inactive. Switching can briefly interrupt sound and should not be repeated rapidly. This is not a permanent product replacement for normal mixing.

On the board, use the same song: play mixer for 5–10 seconds and save status, play direct for 5–10 seconds and save status, then restore mixer. For the current 44.1 kHz AAC baseline, expect `playback 44100 Hz 2 ch 16-bit` and last_output=direct. If direct works and mixer does not, investigate the three bypassed stages and narrow with negative_ts/silence_frames/mix_drop. If both fail, check network/jitter, actual AAC output and device-write cadence rather than relying on zero write_errors. No extra MP3/WAV is required.

Host regressions cover actual sink-worker direct output of raw AAC, source-rate/frame-count preservation, mute, return to mixing, critical local-prompt fallback and negative-timestamp counts. Physical sound quality remains pending user validation. The fixed image and validation results were in SDK build-record section 21.

## PCM works but AAC test overflows the stack: worker stacks and caller workspaces

The user confirms a working direct PCM tone through the board's Speaker connector, but AAC prints `thread:audio-test stack overflow`. The original test stack was only 24 KiB. In the old D213 ELF, `test_worker` used a 4304-byte frame, FAAD `reconstruct_channel_pair` 8304, `ifilter_bank` 8384 and `faad_imdct` 4240. Their nested lower bound is 25232 bytes, exceeding 24576 before intermediate calls and RTOS/libc. The previous synchronous Host model imposed no such stack bound. This clearly explains the test hang without requiring an external music file.

This round sets both offline and CarPlay output-worker stacks to 96 KiB and moves caller PCM, compressed packets, zero frames and output chunks into workspaces owned by their respective single workers. New D213 ELF offline-worker frame size falls to 224 bytes; playback-worker size falls from 18320 to 784. Third-party decoder temporary arrays remain unchanged, as do format, rate, amplifier and Speaker routing.

At offline-thread exit, every 200 CarPlay packets and CarPlay exit, report RTOS stack total/used_peak/free_min using RT-Thread '#' fill-watermark scanning. This describes executed code only. Host tests now create native pthread stacks at the requested 96 KiB and run the actual AAC fixture. The 56 ASan/UBSan tests, boundary check and D213 build pass. Physical retest remains pending; this does not directly prove the earlier CarPlay clicking was also caused by stack exhaustion. Retest offline AAC first with the corrected image.

No WAV/MP3 is currently required. If built-in AAC works while real music remains abnormal, compare AAC-LC with PCM WAV of the same content later; MP3 does not directly exercise the AAC decoder in use. Image/hash were in SDK build-record section 20.

## Speaker still clicks: add offline LVGL audio tests

The user retests the playback fix and still hears clicking through the board Speaker connector, so that fix did not restore physical sound quality. Configuration remains sound0/SPK1 with PA.7 active-low, matching this board's SDK default. Connector naming is not used to guess a switch to SPK0 or reverse amplifier polarity.

As requested, add Audio tests to the disconnected LVGL launcher. The background owner first stops Receiver and isolates connections throughout testing, checking device refcount before opening the sound card. Direct PCM 1 kHz tone, zero silence and built-in AAC through the real decoder go straight to sound0, bypassing CarPlay network/jitter/mixer. Real microphone level/playback are also available. Controls provide 44.1/48 kHz, left/right/stereo, test volume/mute, stop/back, meters and counts. See Offline audio test page in the application README.

The 56 ASan/UBSan Host tests, boundary check, actual launcher lifecycle regressions and SDK build pass. Modeled-device tests cover left/right volume/mute, real AAC decoding, input level/playback, device busy, startup/write failure and cleanup. Launcher checks stop Receiver first and do not restart it after test-stop timeout. LVGL compiles with real SDK headers; physical touch/rendering/listening are not validated. This is actionable diagnostic firmware, not a fixed-Speaker-clicking result. The image was in SDK build-record section 19.

## Microphone-enabled board run: route/type102 established; investigate playback

The user tests `/info` of 4426 bytes with all four type100 input masks `0x70004554`. The iPhone then requests type102 SETUP, format `0x400000` (AAC-LC, 44.1 kHz stereo); packets arrive, sound0 starts, Control Center shows CarPlay and switches to it by default, and the speaker makes sound. The prior routing blockage is cleared in this mic-on session. This supports a relationship between microphone capability and route establishment, but there is no repeated off/on experiment with equal pairing state, so it is not a proven sole root cause. A pairing completed event also appears; pairing-state equivalence is not established.

The phone's original initial SETUP proposes 14 features including mainBuffered. Receiver still returns only `iAPChannel,viewAreas`, then establishes type102 successfully. This session shows that mainBuffered is not required to establish its music stream.

New symptoms are abnormal sound and inability to adjust volume. `first_packet pcm_bytes=6` actually describes a six-byte decrypted AAC payload; the log is renamed `payload_bytes`. It is not six PCM bytes, nor does its length alone prove decode failure. The existing resampler handles 44.1 kHz input into a fixed 48 kHz mixer; differing rates do not themselves prove error.

Review fixes two playback-worker defects: unconditional network waiting each loop delayed already-decoded data, and fixed 480-frame rendering inserted zeroes when less than a chunk existed. The worker now considers cached output duration and renders media only with a complete 480 frames; short local alerts retain immediate zero-padded playback. With sufficient buffered data, sound0 write backpressure paces output. Also apply volume set before startup before the first mixed chunk, avoiding full-gain initial output under mute/low volume. AAC decoder, format advertisement and sound-card driver are unchanged.

Add accessory-side `carplay_volume [0..100]` using the same software-gain API: zero mutes, 100 is unity and an unset query shows default unity. This uses the existing -45..0 dB mapping, not linear amplitude. Protective local alerts remain unaffected by entertainment volume. Existing SET_PARAMETER callback remains; this phone log contains no volume request, so iPhone buttons/slider cannot be promised to control the accessory. The unified new log is `output volume`.

The 55 ASan/UBSan Host tests, boundary check and D213 build pass without compiler warnings/errors. Regressions cover exactly 48000 output frames for one second of 44.1 kHz input without zero-fill boundaries, and actual-worker output length/samples plus first-chunk 100/50/0 volume for 1024-frame packets. Restored physical sound is not yet confirmed. Image hash/listening procedure were in SDK build-record section 18.

## Microphone-enabled comparison: does input capability affect music routing?

The user asks to enable real microphone input to test whether its absence prevents iOS 27.3 music from switching to CarPlay. D213 defconfig enables `RTCARPLAY_MICROPHONE`; dependency `AIC_AUDIO_DMIC` was already on, and AEC stays off. Retain existing `dmic0` capture, 48 kHz stereo-to-mono conversion, voice processing and encrypted PCM/Opus UDP uplink. Recording need not be forced during music; capture opens only after phone input negotiation.

Independent plistlib parsing of the actual writer confirms that enabling microphone adds only four `audioInputFormats=0x70004554` entries: type100 compatibility, default, telephony and speechRecognition. Features remains `0x6144540280`, with the same nine output formats and all other document fields. This checks capability generation when microphone is available; the board still requires `carplay_audio_input_available()` to confirm the device exists and is not occupied. If actual `/info` input masks remain zero, this round has not offered input capability and cannot confirm or reject the hypothesis.

D213 `scons -j8` passes without compiler warnings/errors; ELF contains input startup, DMIC capture open/read and `carplay_mic`. The 55 codec-enabled Host tests and boundary check pass, including actual microphone-worker DSP, PCM/Opus encoding, encrypted UDP transmit/receive and stop/restart with virtual capture. SDK formatter/capability-document checks pass. These do not validate the physical microphone or establish an iPhone music route.

The board was not connected at this stage and no physical result was available. Fixed on/off images, hashes and full acceptance steps were in SDK `doc/rt-carplay-build.md` section 17. Preserve phone pairing; use the on image to confirm nonzero input advertisement, play music and record Audio SETUP, route selector and counters. If SETUP appears, retest the off image identically for repeatability. Nonzero input advertisement without Audio SETUP does not support "missing microphone capability alone blocks routing". `carplay_mic` running=0 is normal before microphone is requested.

## Wireless explicit profile confirmed: capture the actual initial SETUP offer

The user adds session logs after wireless WPA2 handshake: `capabilities_profile=explicit`, `features=0x44540280,0x61`, `info_match=1`, correct session bluetoothID and a 4278-byte document. Together with earlier zero audio state, this shows the narrowed-mask attempt did not clear audio-stream establishment. An earlier `wired mode` line was from a different startup segment; mode or an unapplied old mask no longer explains this wireless result. iOS is known to be 27.3 and need not be requested again.

Further review of MFi-SAP public-key concatenation, AES derivation/response encoding and initial SETUP/RECORD responses finds no new causal difference. catplay remote HEAD advances to `471b0114ad2344cca9cb1d775e318a6ab175547a`, differing from earlier `66340ad` only in sender-gadget address selection; relevant Receiver response paths are unchanged. There is no new confirmed fix.

The RTSP trace records only initial SETUP body length and describes Receiver response as `enabled_features=iAPChannel,viewAreas`. This is not the phone's original offer and cannot establish that iOS proposed no other features or requires type103. This round adds only bounded structured initial-SETUP logs:

```text
setup_offer feature_count=... limit=16
setup_offer index=0 name=...
setup_peer osVersion=...
setup_peer sourceVersion=...
setup_peer timingProtocol=...
setup=initial ... enabled_features=iAPChannel,viewAreas
```

Print at most 16 features, each up to 63 bytes, replacing nontoken characters with `?`; nonstrings/overlong items show invalid_or_overlong. Version/timingProtocol appear only for readable string fields in the original request, with control characters replaced by `?`. Missing/wrong-type features show missing_or_invalid without changing acceptance/rejection. Logs omit complete SETUP, device identifiers and encrypted fields. They run only while initial SETUP is incomplete; later audio/video SETUP retains separate candidate logs.

This only captures the offer, enabling neither mainBuffered nor changes to enabledFeatures, capabilities, mode ownership or audio backends. Seeing mainBuffered proves a proposal, not a requirement. Compare offer/response/SETUP for the same iPhone on a reference Receiver before drawing that conclusion; never respond with a fake type103 handler.

The SDK small-printf regression compiles actual logging functions and checks 20 items truncated to 16, newline sanitization, nonstring/overlong items, malformed features, absent version fields and exclusion of keys/device IDs from output. The 55 Host tests, boundary, SDK integration and D213 build pass. No physical board was connected in this round. Diagnostic image/SHA were in SDK build-record section 16.

## iOS 27.3 board test: timing works; single-variable narrowed-Features comparison

The user validates the timing-fix firmware: `request_tx/response_rx` grows from 34/34 to 41/41, `rejected/send_errors/timeouts` all stay zero, and audio streams/datagrams/packets remain zero. The iPhone Control Center audio selector omits the Receiver. MainAudio initially has current/permanent=2, then both become 1; video remains 1. This records resource changes rather than being stuck with accessory ownership. Timing round trips are established, and the prior patch has not fixed music activation. Missing timing replies no longer explain this observation.

The absent selector entry matters for routing investigation, but is not direct evidence of failed internal iOS route registration. CarPlay resource notifications, Audio SETUP and the general AirPlay selector are different state layers. This observation does not justify adding unimplemented RAOP, microphone or type103.

This round switches the RT-Thread application's default Features from an inherited broad mask to an explicit set for controlled comparison on the same phone. It corrects advertisement scope; **it is not a confirmed root-cause fix for iOS 27.3 audio routing**. catplay `AirPlayReceiver::features(CarPlay)` and LIVI `livi-core-proto/src/features.rs` explicitly combine Screen, Rotate, Audio, PCM, AAC, Unencrypted, MFi, UnifiedBonjour, Car, CarPlayControl and HKPairingAndEncrypt. This application takes only the intersection with already-provided bits, adds no previously absent Rotate, and changes neither Core public defaults nor other integrations.

| Item | Old RT-Thread advertisement | This round's RT-Thread advertisement |
| --- | --- | --- |
| Compressed audio enabled | `0x5657aee2,0x61` | `0x44540280,0x61` |
| PCM-only | `0x5647aee2,0x61` | `0x44440280,0x61` |
| No audio output | `0x5203a4e2,0x61` | `0x40000080,0x61` |

Retain actually provided Screen, Audio, PCM, AAC-LC, unencrypted/MFi audio, UnifiedBonjour, Car, CarPlayControl and encrypted-pairing bits. Remove Photo/Slideshow/PhotoCaching (1/5/13), metadata (15/16/17), iTunes encryption (25), unexplained bits (6/10/28) and RedundantAudio (11). This is explicit set alignment with those references, not proof that any one bit causes the fault. Removing RedundantAudio does not disable audio-transport fault tolerance.

Features advertises capabilities; the format table retains the same nine entries with compression enabled. PCM/AAC/Opus masks, latencies, Bluetooth identity, pairing files, modes, SETUP responses and AAC/Mixer/sound0 protocol behavior remain unchanged. `/info` and TXT share the narrowed mask and retain encoded-consistency checks.

The build logs `capabilities_profile=explicit`. Define `CARPLAY_APP_LEGACY_FEATURES=1` to build the old mask; the previously user-tested image was retained, so rollback needs no rebuild. Both images/hashes were in SDK build-record section 15.

New tests generate both documents with the real plist writer and parse them independently with Python plistlib, asserting entire dictionaries equal except Features for no-audio, PCM-only and compressed cases. They also verify each encoded TXT/plist pair, avoiding audio in no-audio builds or AAC in PCM-only. Decoder-availability seams are controllable and are not counted as actual codec execution or iOS validation.

First verify actual new-image `/info` shows `features=0x0000006144540280` with nine audio formats, then observe output selector and 100/101/102 SETUP. If SETUP appears, retest the original image once for repeatable difference before narrowing individual bits; one success cannot identify a bit. If absent, the feature-set difference has not fixed the symptom, so repeatedly treating Features consistency or timing port as the primary cause is unwarranted.

## Subsequent board result: normal resources, still no audio negotiation

The user then tests `6d0b2a5`: MainAudio current/permanent are both 1, MainScreen current/permanent are both 1, HID_sent=2395 and HID_failed=0. Video continues, iAP2 tunnel identified/authenticated/handoff_complete are all 1, and audio streams/datagrams/packets are all zero. This does not support accessory MainAudio seizure or missing touch delivery. Owner=1 is a resource entity, not proof that iPhone selected CarPlay output. Shown main-control requests return 200, but logging the first 32 requests and a bounded later error set is not capture of every request or event-channel request. SETUP candidates have independent logs.

Further checks:

* DiPlay `AirPlaySession.kt::runEventRead` returns empty 200 to event-channel inbound requests, as does this project. Missing semantic dispatch alone does not prove a required routing instruction is swallowed here.
* Actual Linux application DNS-SD and RTSP `/info` are independently parsed again with Python `plistlib`; audio-on/off Features match encoded TXT. The local simulator is PCM-only at this baseline, showing `0x615647aee2` when enabled. It is not D213 codec-enabled `0x615657aee2` physical wire capture, which the user's prior logs confirm.
* Reference PCM/AAC/Opus bit positions match the format table. Differences in 44.1/48 kHz and broad formats do not sufficiently explain total absence of Audio SETUP; no Codec or forced resource Take is added.
* Narrow the earlier no-microphone comparison: DiPlay config defaults false, but actual `CarPlayHostActivity.kt` passes `microphoneAvailable`. Source allowing no microphone does not prove that composition was physically tested on the same iOS. It also does not justify false input capabilities.
* DiPlay `docs/ISSUE-AUDIT-2026-10-03.md` #29 itself records an unresolved report of all sound remaining on the phone without Audio SETUP evidence. Success on some devices does not mean the reference resolves every similar routing issue.

### Reproducible timing defect and fix scope

DiPlay `NtpClock.kt` and catplay `clock/rtcp_timesync.rs` use bidirectional UDP 210/211 timing after session establishment. This project also does, but `timing_setup=ready` previously proved only port creation; successful samples were DEBUG-only, leaving ordinary serial logs unable to show valid replies. Working video or empty RTSP `/feedback` 200 cannot replace UDP timing evidence.

A defect is independently reproducible: when readability and the one-second probe deadline coincide, the old poll sends a new probe and replaces `pending_timestamp` before reading the old reply, causing an otherwise valid originate timestamp to mismatch. Consume readable replies before due sends instead. Time base, clock-adjustment algorithm, audio advertisement, AAC, Mixer and sound0 remain unchanged.

The RTSP owner collects a timing snapshot and hands it to Shell through the application's existing mutex:

```text
carplay timing: active=1 request_tx=... response_rx=... request_rx=... response_tx=... rejected=... send_errors=... timeouts=...
```

`response_rx` counts only replies passing source-address, packet-shape, originate-timestamp and RTT checks, not arbitrary received UDP. `timeouts` means the previous probe remained incomplete when the next was due. `rejected` includes wrong source/shape, duplicate/expired replies and unknown type; the count alone cannot separate these. Counters belong to the current timing endpoint and reset on session closure. `response_rx>0` proves a round trip, not synchronized full playback clocks. This project samples offsets without applying them to a shared media clock as DiPlay does. With Audio SETUP entirely absent, later playback-clock work must not be presented as a routing fix.

Host tests use fixed clocks and real loopback UDP for the one-second reply boundary, duplicate rejection and incomplete-probe timeout. Restoring the old send/receive order makes the new test fail at `requests_sent == 2 && responses_received == 1`; the new order passes. The 55 Host tests, boundary, SDK formatter/demo/handoff and D213 build pass. **No physical board was connected; causality between this defect and this iPhone's absent Audio SETUP is unproven.**

### Key evidence needed from the next board run

Preserve pairing, connect/play using the new image and run `carplay_status` twice five seconds apart to check timing growth. Record iOS version and whether the iPhone output selector contains/selects this CarPlay Receiver. Repeated static-video performance output is unnecessary.

* Growing `response_rx` without Audio SETUP excludes missing timing round trips as the explanation; investigate whether the same iPhone registered/selected the route and compare a reference Receiver under equal conditions.
* Audio SETUP immediately after selecting CarPlay establishes route selection as a trigger, not the timing fix as its cause. Record automatic and manual selection separately.
* Only growing `request_tx` with `response_rx=0` supports further timing-UDP capture or rejected/send_errors/timeouts inspection; it still does not directly prove timing prevents iOS SETUP.

Latest firmware/SHA for that round were in SDK `doc/rt-carplay-build.md` section 14. This is not a physically verified music-activation fix; the target remains actual iPhone 100/101/102 SETUP.

## Board evidence and conclusion boundaries for the initial review

The user log comes from firmware carrying the H5-fix marker. Saved-link-key authentication succeeds; RFCOMM, MFi, Wi-Fi, AirPlay Pair-Verify, event channel, authenticated iAP2 tunnel and first video frame all establish. Only 130/iAP2 and 110/screen appear, with no 100/101/102 audio candidates. `/info` is 4278 bytes with nine output-format entries. AAC media mask 0xc00000 and PCM mask 0xcffc are nonempty. Bluetooth ID matches iAP2 and TXT/plist Features match.

This locates the "Audio SETUP not yet entered" stage, without uniquely identifying why iOS did not select CarPlay output. H5 retransmissions remain but the prior Host NCP 0x12 does not. Successful control/Wi-Fi handoff means Bluetooth retries cannot directly be declared the routing cause, nor H5 fully stable. This round changes no AAC, Mixer, sound0 or capability bits to claim a fix.

## Public references and verifiable locations

* [DiPlay](https://github.com/shihabal3amri/DiPlay/tree/615b32ce0b40cd0b99ce94ba23a512001553684f): `shared/src/main/java/com/shilapi/xcertplay/airplay/AirPlayInfoPlist.kt`, `AirPlaySession.kt`, `AirPlayConfig.kt` and `network/CarPlayBonjour.kt`. Fetching origin HEAD again in that round retained this commit.
* [catplay](https://github.com/catplay-labs/catplay/tree/66340ad4a0ec8257dafb8ed2d693d1adfc81740b): `carplay/catplay_carplay/src/carplay_rx/sink/audio_defaults.rs`, `receiver.rs`, `modes/change_modes.rs`, `modes/modes_changed.rs` and `modes/transaction.rs`.
* [LIVI](https://github.com/f-io/LIVI), local reference commit `870d26441e073cdb802c427c5ae8833dba694951`: `native/crates/livi-cp/src/info.rs` and `stack/commands.rs` as a third comparison. Third-party test/hardware results are not this project's validation.

Only public protocol implementations are compared; no SDK, authentication material or new dependency is imported. Third-party experimental comments and attempts also lacking Audio SETUP are not verified solutions.

## Comparison results

| Item | Reference behavior | This project and interpretation |
| --- | --- | --- |
| Bonjour TXT | DiPlay deviceid/features/flags/model/srcvers/protovers/pi/pk | Same field set; actual encoded Features are checked bitwise. Old TXT mismatch no longer explains the current baseline. |
| Bluetooth identity | `/info.bluetoothIDs` associates BR/EDR identity | Physically matched; private Wi-Fi MAC must not replace BR address. |
| No microphone | DiPlay permits microphone=false and omits audioInputFormats | This project also omits it; input=0 in logs is the missing-field display default, not explicit wire zero. |
| Media | DiPlay offers 100/media PCM and 102/media AAC-LC; catplay also offers both | Both media paths are advertised, selectable at 44.1/48 kHz; media-output masks are not empty. |
| Role formats | catplay narrows default/alert/telephony PCM/Opus; DiPlay is broader | This project is closer to DiPlay, constrained by compiled codecs. No evidence shows the broader set disables the entire media route. |
| Latency | catplay 102/media; this DiPlay version 102/default | Current 102/media matches its format role; the difference is not a proven root cause. |
| Initial resources | DiPlay `/info.modes` offers MainScreen/MainAudio Take=1, NiceToHave=100, Anytime=100 | Same here; initial advertisement and accessory-originated runtime changeModes operate in different directions. |
| Permanent resource ownership | catplay supplies initialPermanentEntity; DiPlay does not | Public Receivers differ, so this optional field cannot be declared mandatory on all iOS; advertisement remains unchanged. |
| modesChanged response | catplay empty-body 200 and resource update; LIVI acknowledges and updates | Empty 200 matches, but this project previously kept only MainAudio current entity, losing screen/permanentEntity. This round completes diagnostics. |
| Runtime Take MainAudio | catplay distinguishes Controller=1 and Accessory=2; resources can be borrowed temporarily | entity=1 is not the phone's local speaker or a reason to send accessory Take. Forced Take may mean local accessory-source seizure, suppressing iPhone media. |
| mainBuffered/type103 | DiPlay mainBufferedAudio defaults false and requires separate feature/SETUP/clock/TCP implementation | Not advertised here. Never add fake 103/mainBufferedInfo just to trigger a stream; absence is not proven to prevent 102. |
| Unknown requests | DiPlay returns 200 to many remaining routes; catplay explicitly handles /audio-mode; this project returns strict 405 | A difference requiring physical comparison, but logs do not prove those requests occurred. Record path/status/unknown command instead of hiding unsupported behavior with unconditional 200. |

Observe MainAudio **current entity** and **permanent entity** separately. `current=1/permanent=2` differs from `current=1/permanent=1`. Old `carplay_status entity=1` distinguishes neither and does not establish audio transport.

## Actual code changes in the initial review

* Parse `modesChanged.resources` screen/current, screen/permanent, mainAudio/current and mainAudio/permanent with separate presence flags. Notifications are deltas; missing fields do not reset to None. Invalid entities do not invoke callbacks; known enums are 0/1/2.
* Log structured fields for the first eight modesChanged notifications, distinguishing values from unchanged without dumping request bodies. `carplay_status` shows accumulated resource snapshots with 0=None, 1=iPhone, 2=Accessory.
* On first `/info`, log resource ID, transfer, priority and take constraint from the actual transmitted document.
* At INFO, log method/path/body bytes/status/encrypted for the first 32 control requests and up to eight later error responses. Previously DEBUG-only paths/status could not reveal overlooked 400/405/503 in ordinary serial logs.
* Log bounded sanitized unknown `/command` names, never arguments, keys or arbitrary bodies.

These improve diagnostics and mode parsing; **they are not a confirmed iPhone music-output fix**.

## Automated validation

```sh
make -j8 check
make check-lubanlite-capabilities check-lubanlite-demo check-lubanlite-handoff \
  SDK_ROOT=../luban-lite-jc-carplay-oss
# 在 SDK 根目录
scons -j8
```

The 55 Host C tests and boundary check pass. New mode cases cover differing current/permanent owners, permanent-only delta, screen-only delta and invalid-entity non-delivery. Capability tests assert actual input-field omission without microphone and semantically check encoded initial resources. SDK small-printf tests cover info_resource fields and existing 64-bit Features logs. LVGL/handoff and D213 builds pass with no final warnings/errors. D213 was not connected to validate these new logs.

## Next physical procedure and falsifiable decisions

Preserve pairing/trust files. Cold boot, connect Wireless and capture serial logs through playback. Select a song and play within CarPlay's own music UI, then check whether the iPhone output selector contains/selects CarPlay. Separately record playback from the phone's local app rather than merging the two origins into one step.

Run before and during playback:

```text
carplay_status
carplay_iap2 status
```

New-log examples, with illustrative values only:

```text
info_resource id=2 transfer=1 priority=100 take_constraint=100
negotiation id=1 request=... method=POST path=/command body=... status=200 encrypted=1
modes_changed count=... main=1 permanent=1 screen=1 screen_permanent=2 ...
carplay audio resource: permanent=1 screen=1 screen_permanent=2 (0=none 1=iPhone 2=accessory)
```

| New evidence | Next step |
| --- | --- |
| /audio-mode or other path 405, or known resource command 400/503 before playback | Implement actual handling for the observed request shape and retest SETUP; do not change codecs first. |
| main/permanent points to accessory, or borrow/return occurs | Investigate resource ownership/local-source policy. Identify permanent ownership before choosing Untake/Unborrow; do not Take blindly. |
| Both owners are iPhone, every request succeeds, selector lacks CarPlay | Investigate iOS route registration, actual capability requests and the same phone on a reference Receiver. Public code does not uniquely explain internal iOS selection. |
| Selector contains CarPlay but it is not selected | Select it and observe immediate 100/101/102, distinguishing routing choice from protocol rejection. |
| SETUP appears with non-200 status | Locate Receiver rejection from candidate type and parse/port errors, not merely final streams=0. |
| SETUP 200, streams>0, datagrams=0 | Only then investigate UDP address, port, peer and network transport. |
| datagrams>0 and authentication succeeds | Then inspect Codec, Mixer, volume and sound0. |

The negotiation blockage is cleared only when the real phone initiates Audio SETUP. A simulated Host phone establishing a stream proves acceptance of that simulated request, not that real iOS chooses the route.
