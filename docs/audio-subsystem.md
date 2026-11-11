# CarPlay audio subsystem development

Branch: `feat/carplay-audio`. Hardware baseline: wireless v30 video and
Bluetooth handoff were confirmed by the user. Audio changes on this branch
have **not** been tested on D213 hardware.

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
per-type jitter queue -> audio worker -> AAC-LC/Opus/LPCM decode -> resample
and mix -> sound0`.

* Exactly one negotiated format bit. LPCM 8/16/24/32/44.1/48 kHz mono/stereo.
* Optional fixed-point FAAD2 AAC-LC raw access units, stereo 44.1/48 kHz.
* Optional fixed-point Opus mono decoded at 48 kHz; RTP clock kept separately
  (16/24/48 kHz). No unsupported codec is advertised.
* 100/101/102 queues are independent (16 x <=4096 bytes each), sorted by
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

Further protocol/resource/uplink work is ongoing; this document is updated
alongside implementation. The current slot model remains one instance per
stream type and must not be described as arbitrary same-type multi-session
support.
