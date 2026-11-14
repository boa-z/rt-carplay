[English](zalink-performance-comparison.md) | [简体中文](zh-CN/zalink-performance-comparison.md)

# CarPlay performance comparison: Zalink, open receivers and the v24 experiment

## Historical scope and current integration

This document preserves the investigation, logs, tests and image facts of the historical versions it names. References to this round, current or latest mean that investigation baseline, not physical acceptance of the present revision. During V2.1, the D213 application and concrete platform code moved to the Luban-Lite host; see [host migration](luban-lite-migration.md) for maintained paths and build entrypoints. Retained image hashes, test counts and old branch commands are provenance, not current build artifacts.

The functional baseline was v23, confirmed working by the user. The first target was throughput/latency during continuous interaction at 1024×600, 30 FPS; idle-page averages were excluded. The user subsequently changed the final target to 60 FPS; see [60 FPS feasibility and v24 board analysis](1024x600-60fps-feasibility.md). Older 4.3/6.3 FPS logs lacked matching RX windows and a continuous-motion condition, so they did not identify the bottleneck.

## Reference code and visibility

| Implementation | Revision inspected | Purpose |
| --- | --- | --- |
| SDK `poc/carplay/zalink` | `22fdca3473de01076e35bf424f283ec7d9ff5571` | Same-platform MPP/GE/DE integration, the most relevant platform comparison |
| [UxPlay](https://github.com/FDH2/UxPlay/tree/05b0275cb1b726592700e47a6e0307544b3d3a52) | `05b0275` | Widely used AirPlay mirroring receiver; receive/GStreamer pipeline organization |
| [DiPlay](https://github.com/shihabal3amri/DiPlay/tree/615b32ce0b40cd0b99ce94ba23a512001553684f) | `615b32c` | CarPlay ScreenStream, separated Android decoder input/output, latency diagnostics |
| [LIVI](https://github.com/f-io/LIVI/tree/870d26441e073cdb802c427c5ae8833dba694951) | `870d264` | Asynchronous CarPlay screen proxy, parsing and media-host boundaries |
| [carplayd](https://github.com/lvalen91/carplayd/tree/4eb254499aa00f456aea91d00755cf150c7d18a4) | `4eb2544` | Additional Pi/NCM, IPC and GStreamer presentation comparison |

At investigation time, `/home/boa/Documents/luban-lite-jc` was on `poc/carplay/zalink`. It was inspected read-only, without switching branches or changing files. Tracked integration sources were under `application/rt-thread/helloworld/zalink/`; the separate untracked `application/rt-thread/carplay/` directory was not treated as committed evidence.

The Zalink protocol core is `packages/third-party/zalib/zalink/lib/d21x/libzalink.a`. Visible source covers platform callbacks, queues, threads, presentation and interface configuration. A thin socket-select adapter cannot reveal the closed library's TCP window, decryption efficiency or RTSP/feedback behavior. Other projects' FPS/latency parameters also cannot be extrapolated directly to D213.

## Key properties of same-platform Zalink integration

Main file: `application/rt-thread/helloworld/zalink/src/zalink_video.c`.

1. **Receive callback feeds the MPP packet pool directly.** `zalink_video_play()` acquires a packet, copies once, puts it and wakes decode, without waiting for VE or VSYNC. When full, it releases the main lock and waits in 5 ms steps up to 40 times: about 200 ms of possible blocking, not a completely nonblocking callback.
2. **Decode and render have separate threads.** `decode_once()` drives VE; `render_once()` gets a frame, rotates/copies through GE, then submits to DE and waits for VSYNC. MPP get/put-frame management has internal locks; decode_lock/render_lock additionally serialize lifetime. Never destroy the decoder while another thread can access MPP.
3. **Presentation uses double buffering.** Once GE finishes reading a source, the MPP frame can be returned. DE scans independent `rotated[2]` buffers; a new address crosses VSYNC before reusing the previous one. This separates decode/scan lifetime at the cost of GE scheduling and extra image traffic. Even without rotation this path uses GE and is not zero-copy.
4. **MPP buffering is bounded.** Bitstream 1 MiB, packet_count=10, extra_frame_num=2. Extra slots can overlap reception/VE but also increase backlog latency; larger queues are not universally smoother.
5. **Priorities and instrumentation.** Decode priority 5, render 6, statistics 25; every 5 seconds it reports RX/show FPS, KiB/s, queues, callback/lock wait and decode/rotate/present costs. Statistics printing does not hold the decoder lock. Do not copy these priorities blindly: OSS TCP/IP was 10, IPHETH default 12, decode 14 and Receiver 15, so 5/6 can disturb network/system tasks.
6. **Negotiation conditions differ.** `zalink_carplay.c` registers wireless CarPlay with audio disabled. Static cfg omits fps, while `zalink.h` documents 0 as default 60. This establishes the interface convention, not actual phone output at 60 FPS. OSS explicitly advertised 30 FPS, so direct comparison is invalid.

## Comparison with the OSS path

| Stage | Zalink integration | OSS / v23 | Effect and interpretation |
| --- | --- | --- | --- |
| Reception | Library callback writes directly to MPP pool | Control/Screen share a poll owner; Annex-B frames are copied to FIFO | Extra allocation/copy/scheduling; measure reception/callback cost |
| Decode input | 10 MPP slots, 1 MiB | Software FIFO up to 15 entries, then copy to 2 MPP slots, 1 MiB | Different buffering layers; do not enlarge all queues indiscriminately |
| Decode/presentation | VE and GE/DE on separate threads | One video thread handles decode, get-frame and DE/VSYNC | Avoidable serial waiting, but not proof of the sole low-FPS cause |
| Presentation | GE output double buffer | MPP NV12 directly to DE; scan frame held through the next VSYNC | OSS avoids full-image copying without rotation; preserve that advantage |
| Backlog | One frame per render | Drain up to 8 decoded frames, show the newest | Dropping old decoded pictures is allowed; do not arbitrarily break P-frame references |
| Recovery | Library behavior invisible | Deduplicate config; wait for IDR after input loss/decode errors | Do not regress to arbitrary compressed-frame loss or repeated decoder reconstruction |
| Measurement | Automatic same-window 5-second statistics | Two manual status commands can have different windows | v24 adds automatic same-window reporting |

Observed mean decode≈8 ms plus present≈8 ms gives roughly 16 ms serial cost, below a 30 FPS frame budget of 33.3 ms. These are wall times including waits/preemption, not CPU utilization. max queue≈192 ms shows burst backlog but cannot explain all 4–6 FPS behavior.

## Useful patterns from open implementations

* **UxPlay**: `lib/raop_rtp_mirror.c` has a dedicated mirror receive thread. `renderers/video_renderer.c` separates appsrc → queue → parser → decoder → sink and timestamp synchronization policy. This is an AirPlay-mirroring reference, not a CarPlay MFi/control template. Separate receive/media scheduling is useful; `sync=false` does not justify removing DE's VSYNC lifetime barrier.
* **DiPlay**: `shared/.../airplay/ScreenStream.kt` independently reads/decrypts complete header/body messages. `shared/.../media/AndroidMediaSink.kt` has an output thread and output lock so blocking dequeueOutputBuffer does not hold the global input lock; repeated config preserves the decoder. Low-latency hardware options include capability checks/fallback. Optional smooth video deliberately adds buffering, trading interaction latency for smoothness; do not copy that policy directly into this touch scenario.
* **LIVI**: `livi-cp/src/airplay.rs:screen_proxy` is a separate async task forwarding data to a media host. `livi-screen-stream/src/lib.rs` accumulates chunks and drains all complete messages, including multiple frames per chunk; state/control need not be fully rescheduled for each small Screen fragment.
* **carplayd**: IPC/GStreamer separates reception and display, with Pi-platform receive/display FPS records. Different hardware, resolution, OS and clocks prevent treating those records as D213 performance proof.

## Implemented first step: bounded v24 batch reads

v23 did one recv per Screen-owner dispatch, returning through the full RTSP/event/timing/audio scheduler for the 128-byte header, body and extra fragments. v24 drains already queued data after initial readiness, checking this fd with zero timeout before each additional recv. It returns to the owner after at most 16 recv calls or about 2 ms, reducing full-loop repetition while preserving touch/control opportunities.

The time budget is cooperative: one recv, decryption callback or higher-priority preemption may exceed 2 ms; decryption is not interrupted halfway. Extra single-fd polls remain, so this reduces full-owner dispatches without claiming fewer total syscalls. In the Linux burst test, 20 authenticated frames completed with 40 recv calls and 4 owner dispatches, versus at least 40 dispatches previously. This cannot be converted into board FPS gain.

Resolution, advertised FPS, pairing, touch, MPP frame references and decoder lifetime are unchanged. A dedicated Screen thread is deferred because socket, cipher nonce, exit-callback and statistics ownership must move together; simply passing the current object to another thread is unsafe.

Every 5 seconds, a low-priority demo owner prints two lines, outside decode/network threads:

```
[carplay-perf] dt=... rx=... show=... KiB/s=... reads=... batches=... budget_yields=... queue=... peak_total=... drop=... err=...
[carplay-perf] avg_us rx_gap=... sender_gap=... assembly=... process=... | avg_ms queue=... decode=... present=...
```

Rates/means use counter deltas within that report window. `queue` is the sampled length; `peak_total` is explicitly the cumulative peak since startup. RX uses owner snapshots published every 100 ms, so window edges have sample bias. Short single-core critical sections copy video counters to avoid torn 64-bit accumulators. `sender_gap` is a phone-timestamp difference, not phone-to-screen latency; means with no samples display 0. Low priority/frequency reduces but does not eliminate serial-output disturbance.

## Evidence-driven next decisions

| Continuous-drag result | Next action |
| --- | --- |
| RX≈30, show much lower, growing queue | Separate VE decode and DE render; preserve direct NV12 scanning, references and reset barriers |
| sender_gap≈33 ms, much larger rx_gap/assembly | Inspect single-URB IPHETH resubmission gaps, NCM aggregation, lwIP mailbox and receive preemption; capture packets if needed |
| Low RX with large sender_gap too | Inspect source cadence, negotiation and control feedback; timestamps alone cannot exclude upstream backpressure |
| Large process cost without full queue | Separate decryption/Annex-B/copy cost before choosing a Screen worker or reusable pool |
| Good FPS but slow touch | Measure touch→HID→corresponding frame; FPS is not interaction latency |

Review of SDK lwIP 2.1.2 `src/api/api_msg.c:recv_tcp()` found that TCP explicitly does not constrain already ACKed data by `recv_bufsize`; default 8192 is not a hard TCP limit on this path. TCP_WND was already 65535 and receive mailbox 128. A small default SO_RCVBUF alone does not identify the cause. IPHETH used one 64 KiB RX URB, resubmitted after receipt/parsing: a future observation point, not proof of USB limitation. This version changed no USB driver.

## Validation and board procedure

At that baseline, 50 Host tests/boundary checks, actual video-sink/demo-worker regressions and a full D213 build passed. A new real-TCP encrypted burst test verified nonce order, batching, 16-read/2-ms yielding, no blind blocking on partial packets and closure after authentication failure. Host results do not establish board performance improvement.

After flashing v24, use normal automatic connection, continuously drag a map or scroll a list for 15–20 seconds, and retain 3–4 `[carplay-perf]` groups. No repeat ping/USB enumeration/status-command exercise is needed. Exclude idle pages and startup from performance conclusions. If flashing erased `/data`, first Trust is normal; v23's empty-pairing-directory startup fix is retained. Neither the Zalink branch nor USB driver was modified.
