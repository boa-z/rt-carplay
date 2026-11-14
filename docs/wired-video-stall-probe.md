[English](wired-video-stall-probe.md) | [简体中文](zh-CN/wired-video-stall-probe.md)

# v25: code-level investigation of wired video stalls

## Historical scope and current integration

This document preserves the v25/v26 candidate analysis, historical versions, measurements and experiment design. "This version", "current" and test results refer to each section's historical baseline, not physical acceptance of the maintained firmware. Concrete USB Bulk adaptation, media sinks and diagnostic workers now live in the Luban-Lite host under `application/rt-thread/rtcarplay/`; the SDK retains portable protocol code and IPHETH remains independent. The old path `rt-carplay/application/rt-thread/rtcarplay/platform/rtcp_usb_lubanlite.c` below is a historical migration artifact; the maintained host path is `application/rt-thread/rtcarplay/platform/rtcp_usb_lubanlite.c`. In "Fix ownership", board Bulk-lock and DMA work originally assigned to rt-carplay now belongs to the host, while portable USBMUX error propagation remains in the SDK; see [host migration](luban-lite-migration.md). Historical source line numbers are not current line numbers. Host models and successful builds do not prove that physical video stalls are fixed.

## v25 board results and v26 follow-up validation

User-provided logs with the default Carrier ON narrow the candidates:

| Candidate | Board result | Interpretation at this stage |
| --- | --- | --- |
| USBMUX shared lock → Carrier → RX delay | IN commonly holds the lock for 201ms; EP0 wait is about 86–153ms with hold 0–1ms; Carrier/rearm maxima are about 144–199ms | The blocking mechanism occurs on the board; off/on validation is still needed to measure its FPS contribution. |
| Synchronous TX blocking | tx_urb maximum is usually 1ms; tx_lock maximum is about 1ms | This run does not support it as the main bottleneck. |
| Screen owner not reading | Owner maximum interval about 17ms, recv maximum about 1ms; partial gap reaches 690ms and an incomplete body can remain for over one second | Most waiting precedes socket readiness; this alone cannot distinguish phone/USB/lwIP. |
| USBMUX transmit gap | failed=0, controls=0; Expected110 was not reproduced | This run cannot be attributed to failed automatic ACK transmission. |
| IPHETH receive errors | About 12–32 RX errors per five-second active window, with zero USB/TX errors | Raised in investigation priority; the original logs did not distinguish format validation from network-delivery failures. |

RX error counts failed USB-completion-block processing, not lost Ethernet frames; dividing it by rx_frames does not produce a packet-loss rate. There are two entirely different paths:

1. `worker → ipheth_receive → ncm` rejects format, length, DPE or other fields; the entire block is not delivered.
2. `ncm → frame → usbh_ipheth_net_receive` fails pbuf allocation, copy or netif input. Earlier frames may already have been delivered, but the current and subsequent frames are not. This could lose TCP segments, causing retransmission and Screen-body waits; packet capture has not confirmed that chain in this run.

Review of Linux `ipheth_rcvbulk_callback_ncm` shows that this implementation additionally checks wHeaderLength, wBlockLength, NDP wLength/next, the 1514-frame-size limit, overlap and other constraints; the Linux snapshot does not have identical checks. This is a concrete investigation direction, not proof that the phone sent a legal format rejected incorrectly. Do not remove boundary checks merely to pursue FPS. Cache/HCD corruption could also cause format-validation failure.

v26 retains every parsing constraint and original return value, adding `ipheth_receive_diagnose` categories: size, signature, header, block, table, range, frame, overlap, terminator and callback. The original `ipheth_receive` API remains available. A five-second window prints only nonzero failure reasons and the last failed 20-byte NTH/NDP header fields, DPE index/offset/length, validated count and delivered count. Oversize is counted separately. callback_remaining counts validated frames not successfully delivered from the failing callback onward, excluding frames in format-invalid blocks that cannot be counted reliably. The RT-Thread adapter separately reports alloc/copy/input failures and retains the last input return code. No Ethernet/TLS contents are logged, nothing is printed in the RX worker/ISR, and no DMA/URB is added.

This version also tidies diagnostic output as requested: RT-Thread debug is disabled by default, individual iAP2 and RTSP sends/receives move to debug, and USB automatic enumeration keeps configuration selection without rendering complete descriptors. Define `RTCP_LOG_DEBUG` explicitly or run `carplay_usb desc` for details. Host regressions are not linked into firmware; boundary, cancellation, unplug and DMA coverage remain, and tests providing independent protection are retained.

**The existing v25 is sufficient for Carrier off/on; reflashing is not required for that experiment.** Use v26 to classify RX errors, keeping the same version for on/off/on and continuous motion, and skip the mixed window after each switch:

* If off removes the rearm long tail, reduces RX errors and restores FPS, with on reproducing the problem, this supports Carrier blocking as a main trigger. How format/delivery errors arise still needs explanation; correlation alone is not a conclusion.
* If off improves rearm but errors/stutter remain, the lock is a confirmed problem alongside another receive-loss/supply problem.
* For block/table/header/frame errors, compare printed fields against the phone's actual USB capture, reproduce the same header in a regression, then fix the parser separately in cherryusb-ipheth.
* For callback with nonzero alloc/input failures, investigate pbuf lifetime/memory or the lwIP input-return path before changing the TCP window. Counter snapshots occur at slightly different times and can differ by one across windows; compare several windows.
* For signature/range errors, compare wire data with DMA memory. Normal wire data but abnormal memory provides further support for an HCD/cache problem. The failure category alone does not identify DATA PID or cache as the root cause.
* If off gives fast rearm and no RX errors but the body still waits, inspect TCP sequence/ACK/window and USB completions to distinguish phone pauses from delivery problems before the socket.

A separate issue remains: after the first carkit `rc=-1`, automatic recreation receives 67 bytes without a version frame, while physical reconnection reuses the Trust cache normally. USBMUX service recovery within the same accessory attachment therefore needs a separate audit. Do not delete pairing records on this evidence; neither those 67 bytes nor the cause of stream interruption is known.

v26 passes protocol ASan/UBSan and boundary checks, USB DMA/cancellation regressions, demo-worker regressions, IPHETH parser-reason and mid-callback-failure/remaining-frame tests, actual-worker classification snapshots/window reset and original lifecycle tests, CMake upstream EP0 regressions, and full D213 compilation/packaging. The HCD unchanged in v25 was not modified again. v26 board classification logs and Carrier OFF results remain pending; a successful build or Host-injection test is not a fixed-stutter result.

The original v25 code review and experiment design follow. Candidate confidence must be interpreted using the board results above.

The goal is to locate the first long wait while preserving v24 resolution, advertised 30 FPS, VE/DE threads, TCP parameters, single RX URB, timeouts and cancellation rules. This version provides counters and a single-variable Carrier switch without claiming to fix board stalls.

## Version and actual compilation-path checks

Fetched and checked rt-carplay/main (baseline `f41f035`), cherryusb-ipheth/main (`c4c9a00`), RT-Thread `origin/bsp/artinchip-d213-dev` (`7f3e174361`) and the actual SDK `poc/carplay-oss` (`1a3fd0c`). No RT-Thread workspace branch was switched and no Zalink workspace was modified.

The image actually uses SDK `kernel/rt-thread` and `cherryusb-luban/generated`, not every file from the other RT-Thread branch. The SDK official CherryUSB snapshot is `a0054795`; its actual EHCI already includes the short-IN fix. Reading only the unpatched HCD cannot establish "early short IN does not complete" as a new root cause.

The SDK enables `LWIP_NO_TX_THREAD` and `LWIP_NO_RX_THREAD`; lwipopts also explicitly enables `LWIP_TCPIP_CORE_LOCKING` and `LWIP_TCPIP_CORE_LOCKING_INPUT`. Priorities include TCP/IP 10, IPHETH 12, video 14, Receiver 15 and iAP2 20. Priority alone cannot eliminate lock waits.

## Candidate ranking and causal chains

### 1. USBMUX shared lock and Carrier control block RX progress: likely mechanism, board causality pending

Locations: `rt-carplay/application/rt-thread/rtcarplay/platform/rtcp_usb_lubanlite.c:dma_transfer()` around line 251; `cherryusb-ipheth/port/cherryusb/usbh_ipheth.c:worker()` around line 110; official SDK `packages/third-party/cherryusb/core/usbh_core.c:usbh_control_transfer()` line 727.

Call chain: iAP2 thread → TLS read → USBMUX pump → Bulk IN → wait for completion/200ms timeout while holding `hport->mutex`. The IPHETH worker performs Carrier GET every second → `usbh_control_transfer` waits for the same lock → synchronous EP0 operation. That worker also consumes RX completions, parses NCM and rearms the sole RX URB.

The directly confirmed structural problems are that even idle Bulk IN holds the EP0 shared lock and that Carrier control and RX progress share a worker. This is not "the HCD waits for this lock before executing RX": in-flight RX can complete, but the worker may be unable to consume/rearm it. Even if Carrier starts while RX is in flight, one completion can accumulate during the blocked control request.

An actual-worker Host test injects 120ms Carrier time and independently completes RX during it, reproducing wake/rearm over 50ms. Off/on and original readiness, unplug and cancellation-isolation regressions pass. This proves the mechanism, not slow Carrier on the board. A loss of 200ms per second alone also cannot explain every 0.1FPS window; total observed waiting must be measured.

### 2. Synchronous TX, lwIP core lock and RX parsing feedback: likely mechanism, duration unmeasured

Locations: `cherryusb-ipheth/port/rtthread/ipheth_rtthread.c:transmit()` line 33; `port/cherryusb/usbh_ipheth.c:usbh_ipheth_send()` line 217; the direct `eth_tx` branch around line 490 of SDK `kernel/rt-thread/components/net/lwip/port/ethernetif.c`.

lwIP transmit → adapter `n->lock` → driver `tx_lock` → synchronous Bulk OUT with timeout=1000ms → EHCI completion wait. 1000ms is an upper bound, not a fixed duration per call; normal submission can be fast.

Another path is RX worker → `ipheth_receive` → `usbh_ipheth_net_receive` → `tcpip_input`, which directly processes input under the core lock with `LWIP_TCPIP_CORE_LOCKING_INPUT=1`. If this generates an ACK and enters synchronous TX, RX parsing also waits, delaying rearm; other socket/core operations may wait too, delaying ACK and Window Update. New metrics can measure correlated TX and parse/rearm degradation, but did not yet prove that this was slow on the board.

### 3. EHCI predecessor-QH cache writeback overwrites hardware progress: reproduced model defect, board relevance unknown

Location: official SDK `port/ehci/usb_hc_ehci.c:ehci_qh_remove()` line 158, retained in the actual generated version. The latest D213 RT-Thread branch has the same predecessor-QH cache-line clean. SDK descriptor-cache maintenance is enabled, and `USB_NOCACHE_RAM_SECTION` is empty with cache enabled; descriptors cannot be assumed uncached.

Unlinking QH N modifies predecessor P's `hw.hlp` and cleans the entire line while P may still be executing. The link pointer, current qTD and overlay token share a cache line. If hardware advances P after the CPU's last refresh, cleaning the whole line can restore stale current/token/DATA toggle values; disabling CPU interrupts does not stop DMA.

The new test invokes actual `ehci_qh_remove` with independent CPU/DMA images and reproduces predecessor-overlay overwrite. Both the official and current short-IN-patched versions have this risk. The model proves that this interleaving corrupts state, not that every board unlink triggers it. USB may report success while the device discards duplicate DATA PID data; USB capture is needed to establish that connection.

v25 leaves the HCD unchanged to preserve the single-variable Carrier experiment. A final fix should use guaranteed coherent/uncached descriptor access, or update the shared line only after the relevant schedule is confirmed quiescent, while reviewing IAA, cancellation and toggle reads. Adding one invalidate still leaves a DMA race between invalidate and clean and is not a complete fix.

### 4. USBMUX automatic ACK failure is not propagated: confirmed error-propagation gap

Locations: `rt-carplay/core/usb/usbmux_host.c:send_tcp()` line 16 and `handle_tcp()` line 65.

An actual unit test makes automatic ACK transmission at index 110 fail; pump still returns success and the next packet uses 111. Incrementing before send is not by itself sufficient evidence of error: usbmuxd also does so. Continuing the session after send failure is the problem. With partial-send success unknown, do not blindly roll the sequence back and resend TLS bytes.

v25 only aggregates attempt/failure counts and failed index/port/flags/length/return value, without changing retry policy. Matching device Expected110 with Host failed110 would establish a direct link; with zero Host failures, inspect short transfers, DATA PID and HCD cache instead. USBMUX carries iAP2 control, while Screen uses IPHETH TCP. They are not the same logical channel, and a sequence gap cannot directly be declared the video root cause.

### 5. Screen scheduling/fragmentation and control protocol: insufficient evidence

Locations: `airplay_screen_server.c:handle_client()` line 151, `screen_recv()` line 129, `airplay_rtsp_server.c:airplay_rtsp_server_poll()` and recv/poll in `rtcp_net_rtthread.c`.

The code assembles a 128-byte header and its body length; v24 checks readiness with zero timeout before additional recv calls. No deliberate multi-second sleep was found here. Assembly is fragment-wait/scheduling wall time, not decryption CPU time. A new real-TCP test delays the body for 1.25 seconds with the owner running every 50ms, distinguishing partial gaps from owner gaps.

DiPlay `AirPlaySession.kt:handleCommand()` invokes its listener before responding. LIVI `stack/commands.rs` updates modes for modesChanged and sends a host event for disableBluetooth; `stack/feedback.rs` returns 200 without audio, or reports played samples/timestamps with audio. This establishes a functional gap behind 405, not a cause of multi-second TCP fragmentation stalls. v25 retains protocol behavior to avoid a second experimental variable.

## Metrics and measurement boundaries

All new aggregate output is triggered approximately every five seconds by the low-priority demo worker. USB/driver windows are copied and cleared in short critical sections before printing; no network, USB, core or decoder lock is held during output. Windows are close but not one globally atomic sample, and cross-window operations are charged when measurement completes. There are no new URB/DMA buffers and no changes to timeout, cancellation or DATA PID rules.

| Log field | Definition |
| --- | --- |
| usb-lock in_wait/in_hold/out_wait/out_hold | USBMUX wait/hold time for the shared hport lock, ms. |
| usb-lock ep0_wait/ep0_hold | Carrier GET lock wait/hold inside the actual EP0 function; hold includes existing retries. |
| ipheth-time carrier | Complete Carrier call, including EP0-lock wait. |
| wake | RX ISR callback to worker completion consumption. |
| rearm | RX callback to the start of the next successful submission, including error recovery and excluding idle waiting for new data. |
| parse | NCM parsing and frame delivery, including synchronous lwIP/ACK handling required by the configuration. |
| tx_lock / tx_urb | Driver TX-lock wait / full synchronous HCD submission; excludes adapter outer-lock wait. |
| ipheth-perf submit/complete/frame/bytes | Window counts; errors are window differences of cumulative counters. |
| screen-wait owner_calls/ge100ms | Every owner loop, even with an idle Screen fd; count of intervals over 100ms in this window. |
| partial_gaps/ge100ms | Fragment gaps within an incomplete message and count over 100ms in this window; excludes idle between complete frames. |
| recv_ge10ms | Count of ready recv calls taking over 10ms, with wall time including preemption/core waits. |
| pending_header/body_left/age_ms | Incomplete-message state at the latest owner snapshot. |
| mux-perf | Session-cumulative failures and control count; compare deltas for A/B. |

IPHETH timing reports window n/avg/p95_upper/max. The p95 upper bound is estimated from 0/1/4/16/64/256/1000/>1000ms buckets; max is observed within the window. Screen `max_total_us` is cumulative since startup and cannot be expected to decrease during B. Idle IN waiting is not counted as worker blocking. The SDK console buffer is only 128 bytes, so this version splits performance lines to avoid truncating key fields. Serial output still introduces some disturbance; low frequency is not zero overhead.

## Board validation: on/off/on within the same connection

Flash v25, unlock the phone, complete initial Trust and enter CarPlay automatically, then wait for startup activity to settle. Continuously drag a map or scroll a list, retain full serial logs and mark action start/end. Do not unplug or re-pair during A/B.

1. Keep the default Carrier **on for 30 seconds**, collecting 4–6 stable reports.
2. Run `ipheth_probe carrier off` and repeat the same action for **30 seconds**.
3. Run `ipheth_probe carrier on`, repeat for **30 seconds** and check whether the symptom returns.

The first report after switching can mix fragments/in-flight control from the previous phase; skip it. Off skips periodic GET only after the initial query; it does not force carrier up and temporarily retains the last link state for diagnosis. USB unplug still cleans up normally, and reconnect restores default on. The command can be used alongside the automatic LVGL demo.

| Observation | Interpretation / next step |
| --- | --- |
| Long ep0_wait/Carrier and wake/rearm with on; rearm/RX improve with off and degrade after on returns | Strong support for shared lock → Carrier → RX rearm; first separate the incorrect shared lock and control from RX progress. |
| Carrier samples become zero with off while rearm/RX remain similar | Falsifies periodic Carrier as the main bottleneck; other USB problems remain possible. |
| Long tx_urb p95/max with long parse/rearm | Prioritize the synchronous-TX feedback path, then capture TCP ACK/window; priority changes are no substitute. |
| owner ge100 grows in the window while RX wake/rearm are short | Inspect RTSP/event/HID sends or core waits in the owner, together with recv time. |
| Timely owner, fast recv, long partial gaps, low RX completion/bytes but fast rearm | Supply is insufficient before the socket; USB capture distinguishes no source transmission from missing HCD delivery. |
| Host failed index matches the 110 continually expected by the device | Correlates the send error; a later version should close/recreate safely, without resending TLS in place. |
| Zero Host failures but device still reports gaps | Successful send return does not prove delivery; inspect HCD/DATA PID cache and USB capture. |

This patch does not read private lwIP PCB state, so it does not claim measured TCP windows/retransmissions. Further separation of source behavior from network backpressure requires TCP sequence/ACK/window/retransmission and USB-transaction captures aligned with completion times. Existing logs and Host tests alone do not identify the sole board root cause or first blocking point.

## Fix ownership and validation results

* rt-carplay, as historically assigned: serialize Bulk by independent endpoint/direction rather than holding the EP0 shared lock for long periods; retain generation, toggle and cancellation isolation. Propagate automatic-ACK failures and recreate the session safely, without blind TLS retries. The historical-scope note above identifies the maintained host/SDK split.
* cherryusb-ipheth: if Carrier A/B confirms the chain, move control queries off the RX critical path; if the TX feedback path is confirmed, use a bounded asynchronous TX queue/independent owner and wait outside the core lock, preserving DMA ownership, unplug drain and failure quarantine. Never submit an asynchronous URB and immediately reuse the sole TX buffer.
* CherryUSB/SDK: fix shared-QH-cache-line coherence and audit schedule quiescence, IAA reclamation and toggle state; synchronize fixes when the RT-Thread branch contains the same code. This round does not change the scheduler or core-lock configuration.

Passed: 50 core ASan/UBSan tests and boundary checks; actual USB DMA/cancellation/reconnect regressions; actual demo-worker regressions; IPHETH slow-Carrier/slow-TX/off-on and original lifecycle tests; actual EP0 lock wait/hold/retry/clock-wrap tests; actual HCD cache-overwrite model and original short-IN regressions; and the complete D213 build. Board causality and FPS gains still require the single-connection experiment above. This version does not claim that stutter is fixed.
