[English](audio-negotiation.md) | [简体中文](zh-CN/audio-negotiation.md)

# Classic audio negotiation

## Capability advertisement

The D213 host refreshes `/info` before each control session. With Bluetooth enabled, `/info` and iAP2 use the same local Bluetooth address. The host checks that the encoded discovery TXT and plist Features values agree and reports `info_match=1`. On builds without `RT_KPRINTF_USING_LONGLONG`, protocol logs print 64-bit values as two 32-bit words so the following arguments stay aligned.

AudioPCM is advertised when output is available. AAC is advertised only when AAC-LC is built and offered. The usual media offer includes 44.1 and 48 kHz; an explicit rate limits that offer. Type 102 uses its media latency entry. The Receiver leaves Type 103 buffered audio unadvertised.

The protocol comparison used [catplay audio defaults](https://github.com/catplay-labs/catplay/blob/66340ad4a0ec8257dafb8ed2d693d1adfc81740b/carplay/catplay_carplay/src/carplay_rx/sink/audio_defaults.rs) and [DiPlay AirPlayInfoPlist](https://github.com/shihabal3amri/DiPlay/blob/615b32ce0b40cd0b99ce94ba23a512001553684f/shared/src/main/java/com/shilapi/xcertplay/airplay/AirPlayInfoPlist.kt) as reference implementations.

## Diagnosis

Check Audio SETUP, datagram arrival, authentication and audible output independently. A session that only sets up Type 110 video and Type 130 iAP2 may display video while the phone keeps music on its local output. A Type 102 AAC SETUP establishes negotiation, while PCM/AAC bench playback exercises the downstream host path. Follow the [audio debugging guide](audio-debug-handoff.md) to capture the point of failure.

From the Luban-Lite root, run `make -C application/rt-thread/rtcarplay check-lubanlite-capabilities` to check capability encoding, Bluetooth identity and the log formatter against the actual host code.
