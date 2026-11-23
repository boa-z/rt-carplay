[English](classic-capability-audit.md) | [简体中文](zh-CN/classic-capability-audit.md)

# Classic CarPlay capability matrix

**Implemented** means production code exposes the behavior. **Host Tested** means automated tests exercise the listed behavior with fixtures, loopback connections or platform stubs. **Hardware Pending** identifies D213 and iPhone checks still needed for this revision. **Experimental** marks optional foundations with incomplete interoperability evidence.

| Capability | Status and evidence |
| --- | --- |
| Connection and session ownership | **Implemented, Host Tested, Hardware Pending.** Receiver owns the RTSP session, setup and teardown. Luban-Lite owns transport selection, retry, USB and wireless recovery and pairing persistence. Host tests cover listener rollback, reconnect, resource release and transport fallback. |
| H.264 and Classic audio | **Implemented, Host Tested.** The Classic event channel supports a keyframe request after H.264 loss. D213 previously exercised music, calls and Siri. **Hardware Pending** for keyframe recovery after entering Siri and for playback, microphone, Direct/Mixer and resource-switching regression. |
| HID, appearance, OEM tile and projection | **Implemented, Host Tested, Hardware Pending.** SDK provides two-contact touch, raw HID, knob, directional and media controls, Siri, live appearance and projection commands. Advertisement must match the actual input device. OEM focus and projection requests are confirmed through phone events. |
| Geometry | **Implemented, Host Tested** for initial geometry, viewport fitting and coordinate mapping. **Hardware Pending** for D213 edge alignment. Live view-area selection and coded resolution renegotiation are separate protocol questions. Runtime coded-resolution renegotiation remains open. |
| Location and vehicle speed | **Implemented, Host Tested, Hardware Pending.** Opt-in GGA/RMC and `$PASCD` follow phone selectors and stop on unsubscribe or reset. Luban-Lite can inject wireless synthetic GNSS and speed. Wired inputs require a host data source. Interval fields and additional vehicle sensors remain open. |
| Phone status and device time | **Implemented, Host Tested, Hardware Pending.** iAP2 delivers deltas for battery/power, cellular service, call state and `DeviceTimeUpdate`. The RTC adapter invalidates its pending time sample on link loss. The optional D213 diagnostic reports status categories. |
| Now Playing | **Implemented, Host Tested, Hardware Pending.** Legacy text and playback state remain available. Extended callbacks expose duration, elapsed position, inline AirPlay artwork and iAP2 artwork transfer ID. The iAP2 artwork file-session assembler remains open. |
| Route guidance | **Implemented, Host Tested, Hardware Pending.** `0x5201`, `0x5202` and `0x5204` deliver route deltas, ordered indices, maneuver descriptions and bounded lane angles. Cache invalidation covers route end and link loss. The optional D213 diagnostic reports presence, indices and counts. |
| Vehicle State and UI Sync beyond Classic | **Experimental, Host Tested, Hardware Pending.** Bounded channels and data models have Host coverage. Full Ultra rendering and acceptance are outside this scope. |

## Protocol evidence and open questions

The `$PASCD` sentence layout and selector follow DiPlay's `VehicleSpeedNmea.kt` and `Iap2LocationClient.kt`. The observed fixed fields retain their reference values. Their broader semantics and location interval handling still need evidence. LIVI's iAP2 message definitions and event dispatch establish the layouts for power, communications, Now Playing, clock and route guidance. OCBM separates declared view-area capabilities from the coded Screen stream in its configuration and AV documentation.

Reference revisions are [DiPlay](https://github.com/shihabal3amri/DiPlay/tree/615b32ce0b40cd0b99ce94ba23a512001553684f), [LIVI](https://github.com/f-io/LIVI/tree/64a05b68358db52ed887169143afd5944fd6f1f7) and [OCBM](https://github.com/lvalen91/ocbm/tree/45f5a637d2c59c7b779d47c9ccefa8138764dbf4). See the [Receiver API](receiver-api.md) for contracts, [lifecycle and ownership](lifecycle-and-ownership.md) for shutdown order, and [audio debugging](audio-debug-handoff.md) for media checkpoints.

## D213 acceptance

1. Establish wired and wireless sessions. Play music, place and end a call, and invoke Siri with microphone input. Expect stable video, audible playback and capture, and restoration of the prior audio route. Repeat with the existing Direct and Mixer selections.
2. Run the opt-in synthetic GNSS source on wireless. Start navigation, then park, stop the location request and reconnect. Check `carplay_gnss` for active subscription and PASCD counts. Expect subscribed samples only, a zero-speed Park sample when requested, and no old subscription after reconnect. Repeat on wired with a host data source.
3. Repeat over wired USB and the wireless tunnel. Seek, change tracks, change the phone's power and cellular state, and start and end a call. Drive a route with several maneuvers and lanes, then cancel it. Expect updated elapsed position, maneuver and lane records, and invalidations on route end or disconnect. Use `carplay_classic_diag on` for presence and count diagnostics, and `off` to stop logging. Compare `DeviceTimeUpdate` with the phone time zone and check that a retired link cannot apply a queued time sample.
4. Unplug during active projection and replug. Drop Wi-Fi and let a remembered phone reconnect. Switch between wired and wireless with a session active. Expect a single active owner, release of old media and input resources, and successful retry after failed setup.
5. Exercise touch at the corners and with two fingers, knob and media controls where advertised, day/night, the OEM tile and projection focus. Expect correct coordinate mapping and focus changes with independent audio and iAP2 lifecycles.
