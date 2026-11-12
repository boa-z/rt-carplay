# CarPlay audio subsystem development

Branch: `main` (audio changes integrated). Hardware baseline: wireless v30 video and
Bluetooth handoff were confirmed by the user. Audio changes on this branch
have **not** been tested on D213 hardware.

See [audio routing repair](audio-routing.md) for the subsequent discovery-mask
and startup-clock-wrap fixes, the current image checksum and board diagnostics.

## Findings from actual code and open source references

* `/info` offered only LPCM and omitted `102/media` entirely, whereas DiPlay
  offers AAC-LC there. This is a concrete negotiation gap and a plausible
  reason music stays on the phone; it is not a hardware-confirmed root cause.
* RTP L16 sample bytes were sent unchanged to the little-endian device. The
  DiPlay renderer explicitly swaps them. The receiver now converts L16.
* Unknown/composite negotiated formats were silently treated as PCM. They
  now fail negotiation instead of producing noise.
* Three stream types shared one byte ring and format changes flushed other
  streams. There are now independent bounded jitter queues and decoders.
* Failure during format validation could close descriptor zero because the
  partially constructed stream had not initialized fd fields to -1.

Reference files (GPL source already referenced in NOTICE):
DiPlay `AirPlayInfoPlist.kt`, `AudioStream.kt`, `CarPlayMediaEngine.kt`,
`MicrophonePacketizer.kt`, `AndroidMediaSink.kt` in
https://github.com/shihabal3amri/DiPlay.
No Apple private SDK or authentication material is introduced.

## Downlink currently implemented

`RTSP SETUP -> ChaCha20-Poly1305 RTP authentication/replay filter -> bounded
per-role jitter queue -> audio worker -> AAC-LC/Opus/LPCM decode -> resample
and mix -> sound0`.

* Exactly one negotiated format bit. LPCM 8/16/24/32/44.1/48 kHz mono/stereo.
* Optional fixed-point FAAD2 AAC-LC raw access units, stereo 44.1/48 kHz.
* Optional fixed-point Opus mono decoded at 48 kHz; RTP clock kept separately
  (16/24/48 kHz). No unsupported codec is advertised.
* Eight semantic 100/101/102 queues are independent (16 x <=4096 bytes each), sorted by
  authenticated RTP timestamp with wrap handling and a 20 ms reorder wait.
* One audio worker decodes and writes the device; the network uses try-lock
  enqueue and never waits on codec or playback. Contention is a counted drop.
* 48 kHz stereo output, linear interpolation, saturating mix, per-lane gain
  ramps, master volume/mute, navigation/media ducking policy. An optional
  bounded occupancy servo can trim up to 300 ppm; no D213 drift claim.
* Reserved local safety lane bypasses remote entertainment volume/mute/duck.
* Selective TEARDOWN flushes only the corresponding queue/decoder/mixer lane.
* Short device writes are completed, zero/error writes stop the chunk.
* Writer startup flag is set before scheduling, preventing stop-before-entry
  from releasing resources still needed by the thread. A stuck device writer
  leaves resources quarantined rather than freeing live memory.

Codec sources/versions/licenses are in `third_party/audio/README.md`.
FAAD2 is LC-only, not AAC-ELD. No enhanced buffered audio declaration yet.

## Automated validation so far

`make -j8 check`: core tests under ASan/UBSan including real synthetic AAC-LC
44.1/48 kHz decoding, real Opus encode/decode, timestamp wrap, out-of-order,
duplicate/late/overflow queues, stream isolation, mixing, clipping, gain ramp,
resampling and local alert protection. `test_audio_sink` compiles the actual
board sink with a pthread-backed RT-Thread/device model and validates playback,
failed thread startup and repeated cleanup. SDK `scons -j8` also passes with
`CONFIG_LPKG_RT_CARPLAY_AUDIO_CODECS=y` (FAAD/Opus built from source).

These results establish Host behavior and SDK buildability, not iPhone route
selection, loudspeaker output, microphone wiring or D213 real-time headroom.

The bounded slot mapping is: 100/default, 101, 102, 100/telephony,
100/speechRecognition, 100/alert, 100/compatibility, 100/media. A ninth lane
is reserved for local audio. Multiple roles of type 100 coexist; 101 roles
share one slot, as do 102 roles. This is a single-controller receiver, not
arbitrary multi-controller mixing. Extended start/stop callbacks carry the
role; legacy callbacks remain usable with one stream per type. Selective
TEARDOWN matches connection ID/role, so stale teardown does not close a new
stream. A type-only teardown closes every role of that type.

## Uplink and control implementation progress

* Independent DataStream Input Encryption Key HKDF, not the output key.
* Mono L16 big-endian or fixed-point Opus encoder; RTP/AEAD header, nonce tail,
  MTU-safe packets, separate 16/24/48 kHz RTP clock and 48 kHz Opus capture.
* SETUP validates input port, audioType, framesPerPacket; input targets only
  the authenticated control peer. Same-id renegotiation with different
  options is rejected. Key ids are reserved before callbacks can transmit;
  duplicate keys in a multi-stream transaction are rejected.
* Opt-in `RTCARPLAY_MICROPHONE`: dedicated source/encode/send worker, SDK
  exclusive dmic0 capture at 48 kHz stereo, mono conversion/resampling.
  Nonblocking pipe reads, async stop request on protocol owner, bounded
  shutdown wait and quarantine if a worker remains busy. `carplay_mic
  mute|unmute` preserves RTP continuity while muting the transmitted samples.
* SpeexDSP fixed-point NS; bounded RMS AGC. Experimental AEC uses actual
  submitted mixed playback frames as a reference; delay is configurable
  (`RTCARPLAY_MIC_AEC_DELAY_MS`). AEC is opt-in and needs physical calibration.
* Normalized modesChanged call/Siri/resource state and validated duckAudio/
  unduckAudio volume/duration drive worker-owned gain policy and ramps.
* `/feedback` now returns active stream metadata. Real DAC sampleTime and
  NTP-correlated presentation timestamps remain absent; no received-packet or
  wall-time counter is passed off as a hardware presentation clock.
* `carplay_siri` queues event-channel requestSiri actions 2/3 on the RTSP owner.

New host tests run the actual microphone worker against virtual PCM capture
and real loopback UDP, decrypt its packets and test PCM/Opus clocking plus
stop/restart. Synthetic aligned echo attenuation is tested independently;
this is not measured echo performance on D213. The SDK microphone/AEC build
is also checked separately from the default configuration.


## Feature and verification matrix

| Feature | Implementation | Evidence / boundary |
| --- | --- | --- |
| 100/101 LPCM output | Enabled | Host encrypted UDP, endian conversion, actual sink worker model |
| 102/media AAC-LC output | Enabled with codec dependency | Raw AAC 44.1/48 kHz tests; real iPhone routing awaits board |
| Mono Opus output/input | Fixed-point decode/encode | 16/24/48 kHz RTP clocks, 48 kHz codec, authenticated loopback input test |
| Main/alternate/media mixing | Independent queues/decoders, resampling, gain ramps | Numerical and worker tests; nine bounded lanes |
| Local HMI / critical alerts | Public local PCM enqueue, safety gain priority | Local lane bypasses remote mute/volume; HMI must use this API instead of concurrently writing sound0 |
| Call/Siri/navigation focus | modesChanged, active-role policy, duck/unduck | State/command tests; no invented remote ownership grant |
| Microphone | Opt-in dmic0 capture, mono conversion, RTP uplink | Actual worker tested with virtual source; board capture path compile-checked |
| Siri trigger | carplay_siri -> requestSiri actions 2/3 | Encrypted command, CSeq and binary plist tests |
| NS/AGC | Speex fixed-point NS, bounded RMS AGC | Host signal tests; acoustic tuning pending |
| AEC | Optional Speex echo cancellation + mixed output reference | Synthetic aligned echo attenuation test; actual device delay/double-talk not validated |
| Input mute | Silence encoded at continuous RTP clock | Host sender and worker tests |
| Reconnect / cleanup | Per-slot generations, consumed key IDs, bounded worker shutdown | Reconnect, startup failure, selective teardown tests; blocked drivers quarantine live resources |
| Occupancy clock servo | Core API, slew-limited +/-300 ppm | Simulated +/-100 ppm source drift over 120 s; not enabled automatically on sound0 |
| AAC-ELD | Unavailable, not advertised | Selected FAAD LC-only decoder has no ELD support; LPCM/Opus voice fallback |
| Main Buffered / Auxiliary / Enhanced Siri | Not advertised | Different buffering/feedback/capability semantics need verified wire evidence |
| Full take/borrow resource protocol | Not implemented | Incoming state tracked; available references disagree on routing effect of accessory Take |
| RTCP resend / DAC presentation feedback | Not implemented | Control datagrams drained; no proven resend or hardware presentation-clock contract |

The uplink follows the authenticated AirPlay SETUP peer/dataPort and independent
Input Encryption Key. It is not Bluetooth HFP. Only one capture worker owns
dmic0. A competing input SETUP returns 503 without stopping the live input;
TEARDOWN of the owning stream requests asynchronous stop. A replacement while
the old worker is still exiting can return 503 and requires a fresh connection
ID on retry (IDs that could have transmitted are never reused). This is a
bounded safe fallback, not seamless Siri/call microphone arbitration.

## Reproducible Host checks

Run from the rt-carplay checkout:

```sh
make -j8 check
make BUILD=build-pcm AUDIO_CODECS=0 -j8 check
cmake -S . -B build-cmake -DRTCP_SANITIZE=ON
cmake --build build-cmake -j8
ctest --test-dir build-cmake --output-on-failure
make check-lubanlite-demo check-lubanlite-handoff SDK_ROOT=../luban-lite-jc-carplay-oss
./check-boundary.sh
```

CMake and Make each cover 55 test programs. CMake sanitizer mode instruments
core, tests and the audio dependency libraries with AddressSanitizer; core and
tests also use UndefinedBehaviorSanitizer. Vendored fixed-point codec/Ed25519
arithmetic retains intentional signed wrapping/shifts and is excluded from UB
instrumentation, not from address checking. mbedTLS uses its separate Host
library build. These tests are not a protocol conformance certification.

Codec-disabled builds advertise only PCM and retain the RMS AGC fallback;
NS/AEC and AAC/Opus requests do not silently succeed without their dependency.

## Measured Host cost and bounded memory

```sh
cmake -S . -B build-bench -DRTCP_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-bench -j8 --target audio_bench
./build-bench/audio_bench
```

The benchmark retains assertions even in Release and wraps malloc/calloc/
realloc/free from linked project/codec objects. Reported heap excludes libc
internals, thread stacks, RTOS allocations and DMA. The sample run is recorded
in [audio-host-benchmark.txt](audio-host-benchmark.txt): x86_64 AMD EPYC 7402,
GCC, optimized generic fixed-point implementations, 300 iterations. AAC-LC
stereo decode p95 169 us per 1024 frames, Opus mono encode p95 761 us per
20 ms, NS+AGC+AEC p95 290 us per 10 ms. These are observed Host timings,
not D213 CPU estimates. Benchmarks have no timing pass/fail threshold.

Measured allocations: AAC decoder peak 27,992 bytes; simultaneous Opus encoder
and decoder 42,512; NS+AGC 51,720; NS+AGC+AEC 181,342; nine-lane mixer plus
all jitter queues 840,640. Codec states are additional to mixer/queues.
Board output and input workers each reserve a 64 KiB stack. Experimental
AEC reference history reserves about 31 KiB. Network buffers, application,
video, RTOS device buffers and DMA are additional. Allocation failures reject synchronous startup/negotiation or are logged by
the asynchronous input worker. A successful SETUP is not proof that physical
capture subsequently opened; inspect the microphone worker status/logs.

Configured packet reorder delay is 20 ms; output chunks are 10 ms; decoder
ahead limit is about 60 ms at 48 kHz (source frame count at other rates).
The mixer capacity is 8192 source frames/lane. sound0 initially waits 200 ms
for the amplifier, following the SDK aplay behavior. These are configured
software buffering values, not measured end-to-end or DAC latency.
`audioLatencies` values remain negotiation hints and must be calibrated.

## Board validation checklist and commands

Use SDK [build instructions](../../luban-lite-jc-carplay-oss/doc/rt-carplay-build.md)
with app `main` and SDK `poc/carplay-oss` checkouts. Default enables downlink
AAC/Opus and leaves microphone/AEC disabled. Enable RTCARPLAY_MICROPHONE
(and optionally RTCARPLAY_MIC_AEC) in menuconfig only for capture tests.

```text
list_device
carplay_status
carplay_iap2 status
carplay_bt status
carplay_mic
carplay_mic mute
carplay_mic unmute
carplay_siri
list_thread
free
```

Start Wireless from the existing LVGL demo, pair normally, play music and
capture from startup. Expect `audio_setup=ready type=102` for AAC media (or
100/101 for a negotiated PCM/voice path), `writer running: independent
streams, 48kHz stereo mixer`, increasing `carplay audio` packet counts and no
write errors. Check actual sound rather than treating SETUP as audible output.
The microphone command exists only in a microphone-enabled build; expect
`[carplay-mic] started dmic0=48000/stereo ...` after an input-capable SETUP.
Do not infer microphone availability from registration of dmic0 alone.

Verify these on D213: music route and sound0 channel ordering/gain; actual
dmic pins/channel/gain; simultaneous 48 kHz capture/replay; Siri and telephone
round trips; input mute; navigation duck/recovery; local critical alert while
remote muted; rapid USB/Wireless switching and reconnect; CPU/thread stack
high-water and sustained heap; acoustic echo delay/double-talk/noise quality;
video smoothness during AAC plus capture/AEC. Use at least 30 minutes of
playback to assess drift before selecting a sound0 occupancy servo target.
Do not replace real DAC feedback with received RTP sample counts.

## Remaining external dependencies

Hardware is required for the board-only checks above, not for Host development.
Full resource borrowing/return semantics, enhanced stream framing and accurate
presentation feedback need corroborated protocol traces or public contracts.
AAC-ELD requires the negotiated profile/ASC and a complete decoder test
vector; FAAD LC-only cannot be extended by declaring an ELD bit. FFmpeg
7.1.5 is a possible LGPL route and was inspected: it parses ER AAC-ELD, but
`libavcodec/aac/aacdec.c:decode_eld_specific_config` explicitly rejects
Low Delay SBR and data-resilience flags. Without an actual CarPlay ELD
profile/vector this partial support cannot justify advertising full ELD.
Source: https://github.com/FFmpeg/FFmpeg/blob/n7.1.5/libavcodec/aac/aacdec.c .
FDK-AAC is not bundled: its
additional license terms require compatibility/distribution review for this
GPL project. There is no private Apple SDK, key, or authentication material in
this branch. LPCM/Opus voice and AAC-LC media are the implemented paths.

## Initial SDK build results (2026-11-11, before routing repair)

Both configurations completed `scons -j8` without compiler warnings. GNU size
below describes the **entire linked firmware**, not audio-only allocation:

| Configuration | text | data | bss | total bytes |
| --- | ---: | ---: | ---: | ---: |
| Default codecs, microphone off | 2,591,024 | 1,050,404 | 3,663,968 | 7,305,396 |
| Codecs + microphone + experimental AEC | 2,735,184 | 1,054,340 | 3,695,072 | 7,484,596 |

The workspace was returned to the default defconfig after voice validation.
The generated default image is
`../luban-lite-jc-carplay-oss/output/d21x_demo128-nand_rt-thread_rtcarplay/images/d21x_demo128_nand_carplay_page_2k_block_128k_v1.0.0.img`.
Its SHA-256 for this build is
`3623f14f82f263dd0e144b2a26017c83162d03a299bfcc6ac7eae2e8022a43b2`.
Image bytes may change with build timestamps/toolchain. No image was flashed
and no D213 audio test is claimed.
