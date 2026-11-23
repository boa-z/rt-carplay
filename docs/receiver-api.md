[English](receiver-api.md) | [简体中文](zh-CN/receiver-api.md)

# Receiver API

## Lifecycle

Include `rtcp/receiver.h`. `rtcp_receiver_create()` copies the public configuration and initial `/info` plist without opening sockets. `rtcp_receiver_start()` opens the RTSP listener. `rtcp_receiver_poll()` services the existing protocol and media endpoints. `rtcp_receiver_stop()` synchronously closes all endpoints. `rtcp_receiver_destroy()` stops and frees the object; destroying NULL succeeds. Start on a running object and stop on a stopped object succeed without duplicating work. Restart retains the copied configuration and increments `generation` after each successful start.

```c
rtcp_receiver_config_t config = {0};
config.platform.os = &host_os;
config.platform.net = &host_net;
config.platform.pairing = host_pairing_providers;
config.platform.info_plist = info_bytes;
config.platform.info_plist_len = info_length;
config.media = host_media_backend;
rtcp_receiver_t *receiver = NULL;
int result = rtcp_receiver_create(&config, &receiver);
if (result == RTCP_OK) result = rtcp_receiver_start(receiver);
/* The host's owner loop calls rtcp_receiver_poll(receiver, timeout_ms). */
/* Stop/join any host worker before freeing its borrowed providers. */
(void)rtcp_receiver_destroy(receiver);
```

The snippet illustrates configuration/ownership, not a complete runnable pairing provider. The working Linux composition is `examples/linux_sim/main.c`. Its identity storage, cryptographic providers, capability document and discovery setup are real implementations. Both it and the RT-Thread composition now populate the public configuration directly.

## Platform and session configuration

`rtcp/platform.h` defines `rtcp_platform_t`: OS/network tables, identity and controller-store callbacks, initial capability bytes and an optional capability refresh callback. `rtcp_pairing_provider_t` preserves separate Pair-Setup and Pair-Verify contexts. Randomness, Ed25519 signing/verification and pairing save/lookup remain injected; the SDK embeds no identity or authentication material. OS/network tables, identity data, provider contexts and callback contexts remain borrowed and immutable until destroy returns. Initial configuration structures and `/info` bytes can be released after create.

The capability document must be nonempty and at most `RTCP_CAPABILITIES_MAX_BYTES` (6144). A dynamic `get_info_plist` callback runs on the poll owner before accepting a new controller session. Its returned bytes must stay valid and immutable through the immediate copy; failure rejects that connection. An existing session keeps its own snapshot.

`port` defaults to 7000; `screen_port` zero requests an ephemeral Screen endpoint. `family` accepts zero/`RTCP_AF_INET` or `RTCP_AF_INET6`; invalid values are rejected at create. `screen_max_body_bytes` zero retains the existing 512 KiB protocol default. Discovery, USB selection, Bluetooth activation, Wi-Fi handover policy and connection arbitration belong to the host. Optional `iap2` callbacks register the authenticated tunnel; they do not start radio hardware. A false tunnel tick retires that tunnel while preserving media.

## Media and input registration

`rtcp/media.h` defines the actual `rtcp_media_backend_t` accepted by Receiver. `on_video` receives configuration or access-unit Annex-B H.264 with writable prefix capacity, sender nanoseconds and local arrival microseconds. The bytes are encoded video, not decoded pixels. The buffer is borrowed until return; asynchronous backends copy it before returning. A NULL video callback selects an intentional discard sink for Headless operation. Display/HMI visibility remains host policy.

`on_audio` receives decrypted LPCM or encoded AAC/Opus, the negotiated `rtcp_audio_format_t`, stream type, RTP timestamp and local arrival microseconds. LPCM is interleaved little-endian S16. Encoded packets retain their codec identity, sample/clock rates and stream-slot metadata. NULL audio retains the existing behavior of refusing Audio SETUP. Selective stop, slot-aware stop, input start/stop, resource modes, ducking and volume callbacks retain the existing protocol contracts; all audio callbacks except volume use `audio_context`, while volume uses `volume_context`.

Microphone start receives `rtcp_audio_input_config_t`, including negotiated sender format, input key, authenticated peer, network table and telephony flag. A capture worker must copy this borrowed configuration before returning and retire its session state before the host frees it. The API migration changes no audio advertisement, SETUP, jitter, codec, mixer, resampling, microphone encryption or AEC behavior.

`rtcp/input.h` exposes `rtcp_receiver_send_hid_report()` and `rtcp_receiver_request_siri()`. `rtcp_receiver_send_iap()` supplies tunneled iAP2 output. These operations run on the receiver owner, including callbacks on that owner, and synchronously consume their buffers. They return false when the encrypted event channel is absent or a write fails. The host owns touch-device reading, focus, coordinate conversion, HID descriptors and cross-thread queues. The existing portable input bridge can generate the negotiated touch report; the RT-Thread wrapper now routes its output through Receiver rather than the internal RTSP server.

## Classic runtime controls

For an OEM tile, populate `oem_icons`, `oem_icon_count` and `oem_icon_label` in `airplay_info_plist_config_t`. Supply compact square PNG variants at 120, 180 and 256 px as `rtcp_oem_icon_t` entries. The builder copies the PNGs and UTF-8 label into the plist, checks each declared size against its PNG header and retains the 6144-byte budget for the entire document. With no branding configured, it omits the OEM keys. Hosts supplying their own `/info` continue using the capability provider. The wire fields follow [DiPlay's AirPlayInfoPlist](https://github.com/shihabal3amri/DiPlay/blob/main/shared/src/main/java/com/shilapi/xcertplay/airplay/AirPlayInfoPlist.kt) and [OCBM's info builder](https://github.com/lvalen91/ocbm/blob/main/crates/vendor/receiver/src/info.rs).

Register `rtcp_receiver_set_host_ui_callback()` to accept the phone's plain `requestUI`, such as when the user taps the OEM tile. The callback runs on the owner and schedules the host's local UI. Requests with a specific URL are outside this callback.

Call `rtcp_receiver_set_projection_enabled(receiver, false)` to request MainScreen for the host and pass `true` to ask the phone to foreground CarPlay. The accessory sends MainScreen-only `changeModes` (Take) and `requestUI` over the existing encrypted event channel. `RTCP_OK` means a socket write. Use `rtcp_receiver_get_projection_owner()` to observe the phone's `modesChanged`, and wait for video before displaying resumed frames. Audio, iAP2 and Type 111 have independent lifecycles. The phone controls the Type 110 lifecycle and may keep the transport open. See [OCBM's event implementation](https://github.com/lvalen91/ocbm/blob/main/crates/vendor/receiver/src/events.rs).

After local H.264 input loss, call `rtcp_receiver_request_screen_keyframe()` on the owner thread. When the Classic MainScreen and event channels are active, the SDK sends `forceKeyFrame` over the encrypted event channel and coalesces requests within 250 ms. The host should keep waiting for a decodable IDR and stop retrying when it can resume decoding.

Call `rtcp_receiver_set_appearance()` on the owner with `RTCP_APPEARANCE_AUTOMATIC`, `RTCP_APPEARANCE_LIGHT` or `RTCP_APPEARANCE_DARK`. Automatic consumes the host's current UI and map night decisions, for example from ambient light and vehicle lights. Light and Dark override both decisions. The update uses the negotiated main display UUID and the encrypted event channel. `RTCP_OK` means both messages were written. Apply the preference again after reconnect and verify the rendered UI and map on iPhone. The host owns its own HMI theme.

Use `rtcp_receiver_set_volume_db()` after the host applies a gain change, including one requested through `on_volume`. It stores the effective gain for the phone's next `GET_PARAMETER volume` query. Use `-144` dB for mute. Duck, resources and active streams retain their separate contracts. Initial `streams=0`, missing Audio SETUP, and bad playback after setup are separate checkpoints.

The owner may send a complete contact snapshot with `rtcp_receiver_send_touch()`, or send rotary, directional, Home, Back and confirmation state with `rtcp_receiver_send_knob()`. `rtcp_receiver_send_media_button()` supports playback and track controls. Publish matching optional HID devices before session setup and send release after a button press. The host owns physical keys, touch mapping, focus and local volume policy.

Register `rtcp_receiver_set_now_playing_callback()` for incremental `rtcp_now_playing_t` updates. Copy any non-null title, artist and album strings before the callback returns, and update playback state only when `has_playing` is set. The optional `rtcp_receiver_set_now_playing_details_callback()` adds duration and elapsed position in milliseconds plus borrowed inline artwork. On full session retirement, its `invalidated` flag clears the host's accumulated snapshot. Hosts using only the legacy callback clear that snapshot from their session state. See the [capability matrix](classic-capability-audit.md).

A host that owns an iAP2 session can register `iap2_session_config_t.on_now_playing` and `on_now_playing_details` for `0x5001`. Identification and MFi authentication precede callbacks. An empty string clears a text field. Wired iAP2 and the wireless AirPlay iAP2 tunnel assemble one artwork file transfer per session up to 192 KiB. The extended callback delivers the completed encoded bytes when the transfer ID matches Now Playing, clears the old cover when the ID changes and invalidates metadata on link retirement. Copy bytes before returning if the host needs to retain them. The host chooses which encoded formats to display. An abandoned transfer expires after 10 seconds.

Set `iap2_session_config_t.on_device_time` to receive `DeviceTimeUpdate` (`0x4E0B`) on wired USB or the wireless AirPlay DataStream tunnel. The session declares it in `MessagesReceivedFromDevice` and the iPhone sends updates without a subscription command. After identification and MFi authentication, the callback receives a borrowed `rtcp_device_time_t` delta with UTC Unix seconds, a base time-zone offset and a daylight-saving offset in minutes. Presence flags distinguish omitted fields from zero values. The host owns system clock and RTC policy. AirPlay media timing uses its existing clock. See [LIVI's message layout](https://github.com/f-io/LIVI/blob/main/native/crates/iap2-csm/src/messages/device_notifications.rs) and [OCBM's decoder](https://github.com/lvalen91/ocbm/blob/main/crates/vendor/iap2-core/src/metadata.rs).

For Classic navigation, the iAP2 owner can register `on_route_guidance` (`0x5201`) and `on_route_maneuver` (`0x5202`). The delta types in `rtcp/navigation.h` carry bounded UTF-8 text and the original route state or maneuver type. Text is borrowed until the callback returns. Preserve omitted fields, clear explicit empty strings, clear the route on state `0` or disconnect, and correlate maneuvers by their index if the host maintains a maneuver list. The callback filters updates addressed to a different display component.

A host with a GNSS source can set `identity.provide_location` before creating the iAP2 session. On the same owner, call `iap2_session_location_requested()` to read the phone's GGA/RMC subscription and `iap2_session_send_location()` with one checksummed NMEA sentence, excluding CR/LF. The host owns GNSS acquisition, fix quality, pacing and retries. The SDK clears the subscription on phone stop, link loss or reauthentication. These transport APIs are in `iap2_session.h`.

Register `on_phone_status` for authenticated power, cellular and call deltas. Copy borrowed data on the iAP2 owner before return and clear cached status on `RTCP_PHONE_STATUS_INVALIDATED`. Enable `notify_device_time_invalidation` only if the clock callback accepts a NULL update. It signals retirement of a link that delivered clock data and lets the host discard pending samples.

Register `on_route_details` to receive numeric distances, wire units and ordered maneuver indices alongside the legacy text. An omitted list preserves the host list and a present empty list clears it. Register `on_lane_guidance` with `on_route_details` for `0x5204`. Use the returned lane index to associate updates, and clear route, maneuver and lane caches when `invalidated` is set.

For optional speed assistance, set `identity.provide_vehicle_speed` together with `identity.provide_location`. Call `iap2_session_send_vehicle_speed()` on the owner when `RTCP_LOCATION_PASCD` is requested. Supply a monotonic millisecond sample timestamp, speed in millimetres per second and a supported `rtcp_vehicle_gear_t`. False means the sample could not be queued. The host owns source selection and sample pacing.

## Diagnostics and peer address

`rtcp_receiver_get_diagnostics()` copies screen, audio and timing counters plus the ControlCipher framing flag into `rtcp_receiver_diagnostics_t` from `rtcp/diagnostics.h`. Counters retain their existing meaning, field types and ordering; low-level AirPlay diagnostic names remain aliases. Screen elapsed times include callbacks and preemption. Audio datagrams precede authentication, while active streams count Audio SETUP endpoints. Timing exchange counters do not establish clock synchronization. A stopped Receiver returns zero counters. `rtcp_receiver_get_peer()` copies the current IPv4/IPv6 RTSP peer, or clears the output and returns `RTCP_ENOENT` when no peer is attached. NULL arguments return `RTCP_EINVAL`.

`rtcp_receiver_set_single_cipher_frame()` is an owner-thread A/B diagnostic for control replies. Chunked framing remains the default; the option resets after stop/start and setting it on a stopped Receiver returns `RTCP_ESTATE`. All these APIs belong to the Receiver owner, including callbacks on that owner. They are not cross-thread accessors. The RT-Thread `carplay_ab` command queues a requested value under its lifecycle gate and queue mutex; the poll worker applies it before its next protocol poll. Output reports the applied value, requested value and pending flag separately. Shell execution never dereferences a protocol object or frees resources.

## Compatibility

Existing Receiver configuration and metadata callback layouts are unchanged. The low-level `iap2_session_config_t` and its identity descriptor have new opt-in fields. Rebuild the SDK and any direct iAP2 consumers together, initialize the full descriptor to zero, and set new callbacks or declarations explicitly.

The early `.airplay` configuration bridge has been replaced by public `platform`, `media` and `iap2` descriptors. Include `rtcp/receiver_compat.h` and call `rtcp_receiver_config_from_airplay(&legacy, &config)` when migrating an existing `airplay_rtsp_server_config_t`. This copies every descriptor/provider reference, clears the destination first and returns `RTCP_EINVAL` for NULL arguments. Set `on_state` and its context after conversion. Existing provider lifetimes still apply; the helper does not allocate or open resources. New integrations should populate the public configuration directly.

The old unused `rtcp_video_frame`, `rtcp_pcm` and `rtcp_media` draft descriptors were not wired to Receiver and could not express compressed audio metadata. They are replaced by the executable backend contract above. Internal protocol type names remain aliases to the public media/provider types so existing low-level callers retain their function signatures. The legacy `carplay_app_*` API remains in `application/rt-thread/rtcarplay/app/carplay_app.h`, and is included through the host app include directory; the retired SDK example header reports a migration error. The temporary `rtcp_receiver_server()` escape hatch and `receiver_internal.h` have been removed; the host cannot borrow the internal RTSP object through Receiver.

## State, errors and threading

`rtcp_receiver_get_state()` copies a snapshot defined in `rtcp/events.h`. `running` describes listener ownership; `transport_connected`, `authenticated`, `session_established`, `video_ready`, `audio_ready` and `input_ready` are independent observations. RECORD acceptance does not imply media delivery. Video/audio readiness describes protocol endpoints, not decoder output or successful hardware playback. `generation` counts successful starts. `last_error` is the latest lifecycle/poll result, cleared by a successful operation. The optional `on_event_state` reports encrypted event-channel attachment, not overall CarPlay connectivity.

`on_state` runs synchronously on the calling lifecycle/poll thread after a change. Its argument is borrowed until return. It may query state but may not start, stop, poll or destroy Receiver: those operations return `RTCP_ESTATE` during an active operation/callback. Media and protocol callbacks retain their original buffers and owner-thread contracts. No asynchronous dispatch queue is created by Receiver.

The API returns existing `RTCP_OK`, `RTCP_EINVAL`, `RTCP_ENOMEM`, `RTCP_ESTATE` and protocol/port error codes. Failed start leaves the instance stopped and retryable. Poll before start returns `RTCP_ESTATE`; negative timeout returns `RTCP_EINVAL`. A poll error is reported without implicitly destroying the listener, allowing the host to choose retry or stop. All operations, including snapshots, belong to one owner thread. Hosts serialize external requests; the reentrancy guard is not a cross-thread lock.

## Regression coverage and limits

`test_receiver` uses native public configuration and real loopback sockets to verify copied descriptors/capabilities, invalid-family rejection, failed-bind retry, reentrancy rejection, live-connection teardown, peer address copying, diagnostic snapshots, framing reset and 32 start/stop cycles. `test_airplay_pairing_e2e` drives native Receiver configuration through real Pair-Setup, Pair-Verify, encrypted control, audio, video, HID and reconnect, checking distinct readiness states, separate callback contexts and public audio diagnostics. Its client drives Receiver without an internal server pointer. Its `--legacy-config` run executes the same exchanges through the compatibility conversion. Public header compilation uses only `-Iinclude`, without private protocol/platform include directories.

The RT-Thread wrapper has a separate cross-thread lifecycle regression covering deferred framing application, requests pending at stop and reentry during mutex teardown. The application Connection Manager and board adapters now live in the Luban-Lite host; its actual-source regressions cover copied diagnostics, queued owner commands and worker retirement. Device playback, DMA/cache behavior and physical reconnect are outside these Host tests.
