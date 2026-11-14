[English](audio-debug-handoff.md) | [简体中文](zh-CN/audio-debug-handoff.md)

# Audio debugging handoff

## Scope and frozen behavior

The architecture migration does not attempt to fix the existing CarPlay audio routing failure. It preserves `/info` capabilities, format masks, RTSP Audio SETUP, resource ownership, jitter scheduling, Mixer/Direct selection, codecs/resampling, encrypted microphone uplink and AEC behavior. Moving files or wrapping server ownership is not evidence that audio negotiation or playback now works. Protocol changes require a separate debugging task and regression evidence.

## Existing symptom and reproduction

The existing board reports describe an authenticated wireless iAP2 tunnel with completed handoff and working video, while music remains on the iPhone. `carplay_status` reports `streams=0 datagrams=0`, and the audio sink has zero packets. Recorded SETUP candidates include 130/iAP2 and 110/Screen without 100/101/102 audio streams. A running `carplay-aud` worker or an open `sound0` device does not establish Audio SETUP or media receipt. Earlier capability/clock/Bluetooth-identity fixes are recorded in `audio-routing.md` and `audio-negotiation.md`; their presence did not establish a complete audio fix.

To reproduce on the reference board, use the normal D213 GUI profile, complete phone consent and the established USB/Wireless connection flow, play changing video content and start music on the phone, then collect connection logs and the following read-only snapshots. Record the exact firmware/submodule revisions, iOS/device model, selected mode, actual phone audio route and whether a call/Siri/navigation stream was involved. Do not mix manual start/stop commands with the automatic launcher owner.

```text
carplay_status
carplay_audio_path
carplay_volume
carplay_mic
```

Compare snapshots before playback, during playback and after stopping it. `carplay_perf on` enables periodic diagnostics and `carplay_perf off` disables them; account for UART overhead. No new board reproduction was performed during this architecture migration.

## Data flow and source map

| Stage | Current source |
| --- | --- |
| Capability document, format masks and Bluetooth identity | `core/airplay/airplay_info_plist.c`, `application/rt-thread/rtcarplay/app/carplay_app.c` |
| RTSP routing, Audio SETUP, RECORD, modes, duck/volume | `core/airplay/airplay_rtsp_session.c` |
| Socket/endpoints, audio keys, input configuration and counters | `core/airplay/airplay_rtsp_server.c` |
| Authenticated UDP, RTP metadata and decrypted payload callback | `core/airplay/airplay_audio_stream.c` |
| Codec, jitter, focus, mixer and voice DSP | `core/audio/` |
| Board output worker, Direct/Mixer, volume and PCM probe | `application/rt-thread/rtcarplay/platform/audio_sink_lubanlite.c` |
| Microphone worker and encrypted sending | `application/rt-thread/rtcarplay/platform/audio_input_lubanlite.c`, `core/airplay/airplay_audio_sender.c` |
| RT-Thread capture device seam | `application/rt-thread/rtcarplay/platform/rtcp_audio_capture_lubanlite.c` |
| Standalone local tone/AAC/microphone diagnostics | `application/rt-thread/rtcarplay/platform/audio_test_lubanlite.c` |
| Clock extension shared by receive/jitter/microphone | `port/rtthread/rtcp_os_rtthread.c` |

Receive flow is negotiated endpoint → UDP datagram → authentication/decryption → callback with stream type/slot, codec format, RTP timestamp and monotonic arrival time → bounded sink queue → jitter/decoder → Direct or Mixer → `sound0`. LPCM callbacks carry little-endian interleaved int16 samples; AAC/Opus callbacks retain encoded access units. Do not connect that callback directly to a PCM-only backend without decoding and preserving metadata.

Microphone flow is `dmic0` capture → voice processing → negotiated mono LPCM/Opus → independent input encryption keys → authenticated peer UDP. `RTCARPLAY_MICROPHONE` is opt-in; the normal reference profile currently enables it. `RTCARPLAY_MIC_AEC` remains disabled unless explicitly configured and acoustically calibrated. Host synthetic echo tests do not establish installed-vehicle AEC quality.

## Thread model and resource ownership

The RTSP poll owner receives packets and invokes protocol callbacks. `on_audio_pcm` enqueues data into the board sink and returns. The `carplay-aud` worker owns decoder/jitter/Mixer/Direct processing and output configuration/writes. Microphone capture/sending has a separate worker. Local audio tests are owned by the launcher connection worker and run after the CarPlay session is quiesced; they must not compete for `sound0`.

The server stops audio endpoints before the sink is stopped; no new PCM callback should arrive after server teardown. Worker shutdown must complete before freeing devices, queues or contexts. Receiver's owner-thread contract does not automatically repair every legacy cross-thread wrapper; see [lifecycle and ownership](lifecycle-and-ownership.md).

## Direct, Mixer and diagnostic probes

`carplay_audio_path` queries the current selection; `carplay_audio_path direct` and `carplay_audio_path mixer` are existing comparison controls. Direct preserves supported native-rate media output and yields when critical local audio requires mixing. Mixer uses the existing 48 kHz path with resampling, gain and ducking. Keep their negotiation/selection policy unchanged during architecture work.

`carplay_audio_probe capture` records decoded pre-gain media into a bounded RAM probe, `replay` plays it without new network data, `stop` stops a probe action, `clear` releases it and `status` reports it. A successful local tone or replay narrows the output-device path; it does not prove that the phone established an audio stream. These operations change playback and belong in a controlled audio-debugging run, not the initial read-only reproduction above.

## Interpreting evidence

`streams=0` means no active negotiated audio endpoint; it is not evidence of decoder slowness. Streams with zero datagrams move the investigation to sender/network/endpoint delivery. Datagrams with authentication failures require separate key/nonce/format evidence. Accepted payloads without output require sink/queue/decoder/device evidence. Resource/modes events, iAP2/MFi authentication, AirPlay authentication, endpoint readiness and audible playback are separate facts. Do not infer arbitrary resource entity values as successful ownership or force MainAudio Take without a supported requirement.

Preserve stage, error code, counters, timing, role/type/format and teardown reason. Do not publish pairing records, identity seeds, private keys, authentication material, raw certificates or arbitrary captured request bodies. The current diagnostics are intended to describe state without dumping those materials.

## Regression and remaining work

`make -j8 check` includes protocol, authenticated UDP, actual board sink worker under a host device seam, microphone uplink, audio engine and local diagnostic tests. CMake/CTest now also includes `test_audio_diagnostics`; `test_audio_engine` and `test_audio_sink` run from the source directory so their checked-in audio fixtures resolve. The end-to-end protocol test exercises encrypted audio independently of D213 hardware.

The normal D213 firmware and the Headless profile must be reported separately. The Headless profile disables audio/video/touch and cannot validate speaker or microphone operation. Pending physical checks include phone Audio SETUP, actual output route, audible playback, device open/close across reconnect, microphone capture, resource transitions and calibrated AEC. No stream is fabricated to mask the existing zero-stream symptom.
