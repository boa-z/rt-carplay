[English](ultra-foundation.md) | [简体中文](zh-CN/ultra-foundation.md)

# CarPlay Ultra protocol foundation

The intended product goal is complete official CarPlay Ultra. This milestone provides portable display metadata, OPACK and a bounded Vehicle State processor. It does not establish Ultra negotiation, official qualification or a working Ultra renderer. Classic video with local instruments is a separate integration result. No new Ultra capability is advertised by these APIs automatically.

## Reference and attribution

Based on LIVI by Lasse Heitgres (https://github.com/f-io/LIVI). The protocol review uses LIVI revision 64a05b68358db52ed887169143afd5944fd6f1f7, principally info.rs, stack/setup.rs, stack/data_streams.rs, vehicle_state.rs, vehicle_state/wire.rs and ui_sync.rs. The GPL-3.0-or-later attribution requirements are preserved in NOTICE and relevant source files; an integrating product must also retain visible legal/About attribution. Apple assets, identity material and private specifications are not included.

Apple's [CarPlay developer page](https://developer.apple.com/carplay/), [MFi program process](https://mfi.apple.com/en/how-it-works), [2024 architecture presentation](https://developer.apple.com/videos/play/wwdc2024/10111/) and [Ultra launch announcement](https://www.apple.com/newsroom/2025/05/carplay-ultra-the-next-generation-of-carplay-begins-rolling-out-today/) describe the official product and integration context. They are not a public substitute for all protocol, authorization and product requirements. Source compatibility with LIVI alone does not prove official Ultra support.

## Display and OPACK APIs

The public display profile carries one Main Display UUID, view area, safe area and appearance metadata. A null profile preserves Classic metadata. Both rectangles must fit inside their parent; an omitted rectangle uses the parent area. A profile does not add Type 111, altScreen, HEVC, displayPanels, Vehicle State or UI Sync. The shared integer aspect-fit and inverse touch-map helpers reject touches in letterbox bars.

OPACK uses caller-owned preorder tokens, validates UTF-8, finite numbers, references, nesting, complete containers and trailing bytes, and supports scalar, array and dictionary values. Limits are 512 tokens, depth 16 and 65,536 encoded bytes. Signed transaction identifiers are preserved. Duplicate routing keys are ambiguous and cannot be looked up. Encoding has no object-reference compression. Unknown tags and unsupported references are rejected.

## Vehicle State model

The host explicitly supplies accessory, service and characteristic IID/type values, ownership, mutability, units, optional numeric range/step and initial values. The SDK includes no guessed vehicle catalogue. The current bound is eight characteristics and 256 bytes per string/data value. Boolean, integer, floating, UTF-8 string and binary data values are supported; dictionary/array characteristics and invalidation are not advertised. Uint64 values are limited to INT64_MAX by this OPACK model. Numeric metadata and values must fit the declared format. Values and descriptors are copied; request buffers are never retained.

Phone-owned values reject local setters. Car-owned and fixed values reject phone writes. Shared values may be written by either side, subject to the declared range, format and a pure host validation callback. Writes are requests to the host; neither success nor the post-commit callback proves physical vehicle actuation. Actual actuator permissions, feedback and safety policy belong to the host integration.

## Message handling and notifications

The processor consumes a complete decrypted Type 130 comm body with pluginID and binary OPACK pluginData. It implements configRequest/configResponse, readRequest/readResponse, writeRequest/writeResponse, registerRequest/registerResponse and updateNotify. An explicit signed integer or null transactionID is required and echoed. The plugin identifier must match the configured plugin. Malformed or duplicate identifiers reject the entire request; unknown values and refused writes use the reference's error value -6727. No controls are advertised; controlRequest returns the reference's unknown-command status -6735. An unknown command returns the local unsupported error without fabricating a wire response.

Read and register return known current values and per-IID unavailable errors. Registration can follow a known characteristic that has no current value, so a later valid phone write can generate its first notification. Only subscribed changes become notifications; repeated identical values do not. Changes coalesce to the latest value and are emitted on the high-priority lane. Successful serialization consumes those pending changes, and the caller must retain the resulting bytes until sent or the authenticated session ends. A failed encoding retains them. A session reset removes subscriptions and pending notifications while preserving current vehicle values. Unregister is not invented because its wire form is not established by the reviewed reference.

Requests are staged before serialization. Output exhaustion leaves values, subscriptions and write callbacks unchanged. Write-policy validation must be pure and may run before a serialization failure; the post-commit callback runs only after success. All calls have one serialized owner, and callbacks must not reenter the object. Responses use the request lane. Public status reports request/rejection, write, subscription and notification counters. Local errors distinguish invalid input, unknown plugin/IID, unsupported commands, wrong ownership and bounded workspace/output exhaustion.

## Tests and acceptance

The Host tests exercise the LIVI configRequest byte fixture, negative transaction echo, catalogue structure and permissions, absent values, registration, numeric limits and step checks, host refusal, partial per-IID write errors, copied strings, callback timing, output-failure rollback, notification retry/coalescing/reset, unknown controls, truncations and a deterministic malformed-input corpus under ASan/UBSan. Display tests cover geometry and touch mapping; metadata tests verify that defaults remain Classic and optional features are absent. These tests are not iPhone interoperability evidence. LIVI's own tests have not been run by this milestone.

L1 remains partial until bidirectional Type 130 transport, negotiated capability plumbing and the UI Sync foundation have passed their own tests. L2 fusion, L3 real-iPhone Ultra negotiation, L4 real-iPhone Vehicle State/UI Sync, L5 Type 111 and official product acceptance remain NOT_RUN for this implementation. Complete Classic regression is maintained separately by the integrating host.

## Remaining implementation

The next protocol steps are authenticated bidirectional Type 130 lifecycle/queuing, complete-message assembly, phone-offer gating, binary plist UI Sync state/ACK handling and public receiver composition. The existing iAP tunnel remains unchanged and receive-only. The new Vehicle State API does not yet attach to a live receiver or emit vehicleStateProtocolInfo from its /info endpoint. UI Sync assets, frame composition, HEVC support, official access and qualification remain separate requirements. A D213 build must establish actual graphics/codec capability before advertising it. Optional Type 111 must have an independent sink and must not be implied by a single-screen layout.
