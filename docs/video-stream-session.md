[English](video-stream-session.md) | [简体中文](zh-CN/video-stream-session.md)

# Independent video sessions and experimental Receiver transport

The RTSP session supports independent Type 110 Main and experimental Type 111 alternate slots. The public Receiver can explicitly attach separate authenticated H.264 TCP receivers through experimental_alt_video; default configuration retains the existing Main-only path. This transport bench adds no /info declaration, altScreen negotiation, HEVC or D213 rendering behavior. It does not establish L5 or official Ultra.

Reference: LIVI stack/setup.rs at revision 64a05b68358db52ed887169143afd5944fd6f1f7, where setup_screen opens an independent receiver through open_screen(cluster, key). Based on LIVI by Lasse Heitgres (https://github.com/f-io/LIVI); see NOTICE. This implementation retains rt-carplay's existing bounded connection-ID ledger and transaction behavior.

## Backend and lifecycle contract

Both callbacks must be supplied together. They own both types and receive the stream type, connection ID, independently requested port and backend context. Main does not also invoke legacy start_screen/stop_screen. Without the new pair, the legacy Main path is unchanged and Type 111 remains omitted as an unsupported stream. The session slots are fixed-size state. Enabling the Receiver transport allocates a separate screen receiver, key/nonce epoch and bounded assembly buffer on demand, without a worker or platform dependency. Each listener closes after accepting one peer; a new endpoint needs a fresh connection ID. The shared Linux/RT-Thread poll capacity grows from 24 to 25 endpoints to accommodate alternate video alongside the existing maximum endpoint set.

Type 111 also requires completed initial SETUP. The backend must implement authentication/key derivation and actual endpoint ownership, as the existing Main callback does. Session tests use mock callbacks; the separate public Receiver tests described below use real encrypted sockets. The alternate backend additionally requires active ControlCipher before listener allocation.

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

## Session Host evidence

tests/check_video_session_wire.py independently generates binary-plist fixtures and decodes actual session responses through Python plistlib. It checks distinct ports, unchanged initial features and the omitted unsupported alternate stream in the legacy configuration. C tests cover mixed SETUP, duplicate types/IDs, cross-video/audio collisions, live-key collision, idempotency, stale selective teardown, replacement/reuse refusal, either callback failing, a successful callback returning zero, audio failure, small response capacity, full teardown/destruction and incomplete callback-pair refusal.

## Public Receiver transport contract

Set receiver_config.experimental_alt_video=true and optionally alt_screen_port (zero chooses an independent ephemeral port). media.on_alt_video/media.alt_video_context receive only alternate Annex-B H.264; a NULL callback explicitly selects an intentional Null Sink when the flag is true. Main media.on_video and video_ready/diagnostics retain their meanings. Merely setting an alternate callback while the flag is false enables nothing. Legacy configuration migration copies the new explicit transport fields without enabling them by default.

rtcp_receiver_get_alt_video_status is an owner-thread copied snapshot, also available while stopped. It reports configured/ready, bound port, opened/closed counts, last_error and alternate screen statistics. Screen statistics survive closure until the next successful open; open/close counts survive controller reconnects and reset on Receiver stop/start. Unexpected socket/crypto/assembly loss reports generic RTCP_EIO, without claiming a more precise category. A failed listener reports its start error. Normal selective/full teardown clears readiness and port.

Alternate unexpected exit retires only its endpoint and session slot, retaining the consumed ID ledger. Main loss retains the existing full controller teardown and closes alternate too. Failure is retired before RTSP dispatch can replace the endpoint, preventing a stale exit from closing a new key. A separate listener and key do not imply simultaneous hardware decode or a second display.

## Encrypted Host evidence and remaining work

The --alt-video and --alt-video-null end-to-end scenarios use the public Receiver, genuine TCP, Pair-Setup/Pair-Verify, ControlCipher and independently encrypted H.264 frames. They verify authentication and initial-SETUP gates, concurrent interleaved delivery with distinct markers/keys, independent ports/sinks/counters, idempotency, stale selective teardown, ID retirement, failed listener allocation, error-only listener readiness without blocking accept, wrong-key and bad-tag isolation, EOF, recovery with fresh IDs, Main-loss cleanup and stop/start reset. Both continue the existing Classic audio/video/reconnect regression. These scenarios are part of Make check and CTest. Existing Python plistlib checks independently verify unchanged initial features and session response shape.

Phone-facing second-display metadata and altScreen negotiation still need a matching capability gate and real phone evidence. Real-iPhone Type 111 and physical/official rendering remain NOT_RUN. D213's Vehicle State simulator and independent LVGL fusion demo do not enable this experimental alternate transport. The current bench supports the existing H.264 wire format only; it does not demonstrate official Cluster codec, assets, presentation timing or official Ultra.
