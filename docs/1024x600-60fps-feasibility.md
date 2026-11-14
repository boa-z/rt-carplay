[English](1024x600-60fps-feasibility.md) | [简体中文](zh-CN/1024x600-60fps-feasibility.md)

# 1024×600 at 60 FPS: feasibility and v24 board-log analysis

## Historical scope and current integration

This document preserves the investigation, logs, tests and image facts of the historical versions it names. References to this round, current or latest mean that investigation baseline, not physical acceptance of the present revision. During V2.1, the D213 application and concrete platform code moved to the Luban-Lite host; see [host migration](luban-lite-migration.md) for maintained paths and build entrypoints. Retained image hashes, test counts and old branch commands are provenance, not current build artifacts.

Evaluation baseline: application v24 (`1415cce`), D213 SDK `1a3fd0c`. The final target was updated to approximately 60 distinct displayed pictures per second during continuous motion at the current resolution. The earlier v24 30 FPS experiment is no longer the final target.

Conclusion: the configured display timing permits 60 FPS and observed decode costs justify further work, but sustained full rate is not proven. The application still advertised 30 FPS, and these logs did not even show stable 30 FPS reception. Investigate input cadence first, then use a 60 FPS source to validate sustained parallel decoding/presentation. This investigation changed no firmware behavior and produced no new test image.

## Hardware and configuration evidence

| Item | Source/configuration evidence | Meaning for 60 FPS |
| --- | --- | --- |
| CPU | SDK `.config`, CPU 600 MHz | Decryption, TCP/NCM, scheduling and stream processing share the CPU; full-load measurement is still required |
| VE | `VE_CLOCK=200000000` in `bsp/artinchip/hal/ve/aic_hal_ve.c` | A configured clock alone cannot establish decode FPS |
| Panel | LVDS, simple panel, match ID=0 | Selects `timing_1024_600` in `panel_simple.c` |
| Timing | pclk=52 MHz; horizontal 1024+160+160+20; vertical 600+12+20+2 | Theoretical refresh is 52e6/(1364×634)=60.131 Hz; measure actual clock/VSYNC on the board |
| Negotiation | `info.max_fps=30U` in `carplay_app.c:build_announcement_data()` | Advertise 60 and verify actual phone output; changing the value alone does not establish acceptance |
| Presentation | MPP NV12 directly to DE, without rotation | Avoids a full-frame GE copy; preserve this advantage |

SDK VE documentation lists H.264 BP/MP/HP, Level 4.2, maximum 1920×1088 and 80 Mbps. These are general SDK capabilities, not a sustained 60 FPS guarantee for the D213/200 MHz/DDR configuration. Documented 1080p60 display support cannot replace H.264 decode-throughput measurement.

1024×600@60 requires 36.864 Mpixel/s of active pixels. Aligned to 16×16 macroblocks, 1024×608 has 2432 macroblocks per frame and 145920 macroblocks per second at 60 FPS. Inspect the actual SPS for profile/level, reference frames and coded dimensions rather than relying only on the Receiver's advertised resolution.

An active NV12 frame is 921600 bytes. At 60 FPS, decode output writes about 55.3 MB/s; adding one DE read gives about 110.6 MB/s. Stride/height alignment, reference-frame traffic, UI scanning, stream copies and other bus activity add further cost, so this is not a complete DDR budget. USB carries compressed H.264: for example, 20 KiB per average frame at 60 FPS is about 1.23 MB/s or 9.83 Mbit/s. Measure motion-scene bitrate and bursts; neither raw NV12 bandwidth nor USB2's theoretical 480 Mbit/s establishes USB requirements or usable throughput.

## What the new v24 log proves

| Observation | Supported interpretation |
| --- | --- |
| First window RX/show both 9.3; later about 0.1–1.5 FPS | Presentation largely kept up with received video in this run; investigate the upstream path first |
| queue=0/1, peak_total=2, drop=0, err=0 | No evidence of sustained decode backlog or lost compressed input; larger queues are not an evidenced fix |
| decode 7–15 ms, present 6–13 ms | Work cost at the observed low frame rate; not full-rate 60 FPS tail latency or contention |
| process 0.6–14 ms, assembly 55 ms–6.343 s | Most elapsed time lay between the first header fragment and complete message, outside the processing callback |
| sender_gap about 53 ms–1.475 s | Received timestamps were also far apart; limited content change or upstream backpressure remain possible, not just deliberate phone rate limiting |
| KiB/s=15–61 | Observed completed-read rate, not maximum USB-link throughput |

`assembly` begins with the first received message header and includes waiting for subsequent bytes, owner scheduling and preemption. It excludes pure idle before the message starts and is not isolated CPU/decryption cost. Receive and sender-gap windows do not contain exactly the same samples; subtracting them does not yield end-to-end latency. Without a continuous-motion condition, these windows cannot prove an inherently slow source.

### USBMUX anomalies have high priority

The phone repeatedly reports `detected duplicate packet. Expected 110 received 111...118`: its expected sequence remains at 110. The word duplicate does not prove that the application repeatedly sent the same sequence; displayed received sequence numbers increase.

Comparison with usbmuxd `3ded00c`, `src/device.c`, found the current mux TX increment and peer `rx_seq` echo rules consistent with the reference. Do not change the echoed field or force the counter to 110 solely because of this warning.

`core/usb/usbmux_host.c:send_tcp()` increments `tx_index` before `transport.send`, while several automatic ACK paths in `handle_tcp()` ignore send errors. A failed send could therefore be followed by later sequence numbers. This log lacks the underlying return value for the first failure, and transport/device reception remains another possibility. Capture mux sequence, TCP flags, port, frame length, actual USB length and return value at the first failure/anomaly before deciding to close and rebuild the session. Do not blindly retransmit TLS bytes when partial-send success is unknown.

USBMUX carries carkit/iAP2; Screen uses IPv6 TCP over IPHETH. They share a USB controller but are different logical streams. Repair control failures, but the evidence does not establish them as the sole Screen bottleneck.

### Control commands now have concrete types

Previously unidentified `/command` requests include `modesChanged` and `disableBluetooth`, still returning 405 in this trace; `/feedback` also returns 405. Implement state/capability handling according to command semantics and compare open implementations. The log does not prove that these 405 responses necessarily cause the low rate, and unknown commands must not all receive an unconditional 200.

## Sequence toward sustained 60 FPS

1. **Locate input stalls and USBMUX sequence gaps first.** Add first-error diagnostics/propagation; measure Screen body-fragment intervals, maximum owner service gap and IPHETH completion-to-resubmit gap, with TCP-window/retransmission capture if necessary. v24 batching did not itself prove these stalls eliminated; repeating basic ping/enumeration tests adds no new evidence.
2. **Establish a 60 FPS input experiment.** Advertise 60, inspect SPS/timestamps and verify sender/RX near 60 during continuous motion. Repeated Receiver scans cannot create 60 distinct pictures when the source supplies fewer.
3. **Overlap decoding and presentation.** The per-frame budget is 16.667 ms. Historical serial decode≈8 ms plus present≈8 ms leaves little margin; decode statistics count nonempty calls and may include more than one call per frame. VSYNC waiting is wall time, not CPU cost, and adding separate maxima does not establish the limit. Separate VE and DE workers while preserving direct NV12, scan-frame references and stop/reset barriers; validate MPP buffer/lock concurrency contracts. This is the most useful Zalink integration pattern to study.
4. **Optimize reception/copying according to measured occupancy.** If decoding/presentation have margin but owner/CPU processing does not, consider a dedicated Screen owner, reusable input storage and fewer copies. Raising every priority or unbounded queue expansion is not a sound first step; lost reference chains still require IDR recovery.

Zalink's fps=0 comment means default 60, and its separate decode/render threads support feasibility exploration. Its closed protocol library, wireless/audio-disabled conditions and lack of a 60 FPS run on this board limit its evidentiary value. UxPlay/DiPlay asynchronous receive/output organization is likewise a structural reference, not proof of D213 full-rate performance.

## Suggested acceptance criteria

* Use a source that actually updates continuously for at least 60 seconds. Sender/RX/successfully presented distinct-picture rates should approach 59–60 FPS; retain 5-second windows and per-frame intervals so a long average cannot hide stalls.
* Require no compressed-input loss, no decode errors and no sustained queue growth. Count successful DE updates and VSYNC; the historical `frame_count` also increments after failed layer ioctl and is not a strict display-success counter.
* Record p95/p99 frame interval and queue age, explicitly count long frames, retain touch/control responsiveness and retest the target audio scenario. A 60 FPS average does not establish acceptable touch latency.
* Test a local matching H.264 60 FPS stream through VE→DE separately from the complete wired network/decryption→VE→DE path. The former separates hardware-decode limits from input problems but cannot replace complete-path acceptance.

The conclusion is conditionally feasible and worth optimizing in this order, not hardware-proven sustained 60 FPS.
