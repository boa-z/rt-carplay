[English](video-stream-session.md) | [简体中文](zh-CN/video-stream-session.md)

# Independent video slots in the RTSP session

The internal RTSP session now supports an explicit paired start_video/stop_video backend for independent Type 110 Main and experimental Type 111 alternate slots. This is the control-plane foundation for the optional second-stream experiment. The production RTSP server and public Receiver do not attach this backend yet; they retain the existing Main-only listener. This milestone adds no /info declaration, altScreen feature, second TCP listener, media sink, codec or D213 rendering behavior. It does not establish L5 or official Ultra.

Reference: LIVI stack/setup.rs at revision 64a05b68358db52ed887169143afd5944fd6f1f7, where setup_screen opens an independent receiver through open_screen(cluster, key). Based on LIVI by Lasse Heitgres (https://github.com/f-io/LIVI); see NOTICE. This implementation retains rt-carplay's existing bounded connection-ID ledger and transaction behavior.

## Backend and lifecycle contract

Both callbacks must be supplied together. They own both types and receive the stream type, connection ID, independently requested port and backend context. Main does not also invoke legacy start_screen/stop_screen. Without the new pair, the legacy Main path is unchanged and Type 111 remains omitted as an unsupported stream. The additional slot is fixed-size session state, adding no frame buffer, software copy, worker or platform dependency.

Type 111 also requires completed initial SETUP. The backend must implement authentication/key derivation and actual endpoint ownership, as the existing Main callback does. The tests use mock callbacks and do not establish encrypted network delivery. No production alternate listener is exposed through the public Receiver at this stage.

A start callback returning false must clean up its own partial resources before returning. If it returns true with port zero, the session refuses it and invokes stop for that type. Successful endpoints are subsequently owned by the session until replacement, teardown, rollback or destruction.

| Trigger | Behavior |
| --- | --- |
| Mixed Main/alternate SETUP | Validate video/audio descriptors before starting endpoints; return distinct type/port entries. |
| Duplicate type or equal IDs in one transaction | Refuse before either callback starts. |
| Same type and active connection ID | Return the existing port without restarting. |
| Reuse across types or after retirement | Refuse through the shared media/Type 130 key ledger. |
| Replacement with a fresh ID | Stop only that slot before starting its replacement. Failure does not restore an already closed endpoint. |
| Callback failure, zero port or response encoding failure | Stop newly started endpoints in that transaction; retain idempotently reused siblings. Reserved IDs stay retired, including an unstarted sibling after an earlier failure. |
| Audio failure after new videos start | Roll back both newly started videos and any newly started audio. |
| Selective TEARDOWN | Stop only the selected type; a supplied connection ID must match, so an old teardown cannot retire its replacement. |
| Full TEARDOWN or destruction | Stop each active slot once. |

IDs are reserved before callbacks because a backend may transmit immediately. Retiring IDs after a partial transaction is conservative reuse prevention, not an inferred phone retransmission policy. The ledger remains bounded; a fresh authenticated session remains the boundary for rebuilding it. Default initial SETUP enabledFeatures bytes remain iAPChannel/viewAreas. Receiving an altScreen offer alone adds no feature.

## Host evidence and next integration

tests/check_video_session_wire.py independently generates binary-plist fixtures and decodes actual session responses through Python plistlib. It checks distinct ports, unchanged initial features and the omitted unsupported alternate stream in the legacy configuration. C tests cover mixed SETUP, duplicate types/IDs, cross-video/audio collisions, live-key collision, idempotency, stale selective teardown, replacement/reuse refusal, either callback failing, a successful callback returning zero, audio failure, small response capacity, full teardown/destruction and incomplete callback-pair refusal.

This evidence concerns session dispatch and ownership only. The next stage still needs explicitly gated public Receiver/server composition, separate listeners, media callbacks or intentional Null Sinks, statistics, socket-loss isolation, authenticated key derivation, encrypted concurrent-stream tests and unchanged Classic regression. Phone-facing second-display metadata and altScreen negotiation need a further matching gate. Real-iPhone Type 111 and physical/official rendering remain NOT_RUN. D213's Vehicle State simulator and independent LVGL fusion demo do not enable this alternate slot.

On 2026-11-15 the complete SDK Make check, all 67 CMake tests with RTCP_SANITIZE enabled, and the complete integration Host suite passed locally. Existing encrypted Vehicle State/UI Sync and Classic media tests passed alongside the new mock video-session tests; they do not turn the new alternate-slot results into encrypted Type 111 reception evidence.
