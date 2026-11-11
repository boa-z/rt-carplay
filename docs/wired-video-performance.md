# Wired video performance: v17 baseline and v18

V17 is the first user-confirmed baseline with both visible picture and physical
touch. The user reports severe stutter. Its logs do not supply a measured frame
rate; `screen frames=120` alone does not establish FPS. The current /info advertises
30 FPS at 1024x600, so the first target is smooth 30 FPS during continuous motion.
Do not increase the advertised rate or reduce resolution before locating the
bottleneck. V17 remains archived separately for comparison.

## Confirmed scheduling limit

The application poll loop previously slept 10 ms unconditionally in wired mode,
then called airplay_rtsp_server_poll(..., 0). The screen server performs at most
one blocking-socket recv after each readiness notification: a header and its
body need at least two polls, with more polls for fragmented bodies. Thus the
sleep imposed at least 20 ms of software pacing per ordinary frame and 10 ms
per additional fragment, regardless of available data. This is a concrete
throughput/latency limitation, not a measurement of the complete board pipeline.

V18 calls the server poll with a 10 ms readiness timeout and no preceding sleep.
Readable data returns immediately. When idle, the timeout bounds touch queue
latency without a busy spin. The wireless/mDNS path retains its existing policy.
The socket reader still performs one recv per ready poll; it never issues an
unconditional second blocking recv.

The decoder thread moves from priority 20 to 14, ahead of the priority-15
receiver, so a continuously readable socket cannot starve the consumer after
removing the sleeps. TCP/IP retains priority 10 and system USB workers are
unchanged. The decoder blocks for VE/vsync so reception can overlap these waits.
This scheduling choice needs board validation under sustained motion and touch.

## Decoded backlog

The sink previously presented every available decoded frame and waited for
vsync after each one. Catch-up therefore replayed old pictures at panel cadence.
V18 drains up to eight already decoded pictures, returns superseded display
references and presents the newest once. It keeps the old scanout reference
until after the new layer's vsync wait. MPP's frame manager separately retains
decoder references; returning a display reference does not invalidate H.264
reference pictures still in use.

Compressed NALs are still fed in order. The input FIFO remains bounded; its
existing full-queue drops are reported and still need attention if they grow.
This change does not claim to repair an already damaged compressed reference
chain. Decoded-frame skipping is reported separately as `skipped`.

## Measurement

Run the following once after the picture starts, then again after approximately
10 seconds of continuous dragging/scrolling on the same view:

```
carplay_status
carplay_video status
```

Use the second interval for the comparison. The first interval includes boot,
pairing/startup time. Static content can legitimately produce less than 30 FPS.

* `carplay screen RX`: accepted frame count, cumulative wire KiB and successful
  recv calls, plus frame rate since the previous status command. The receiver
  samples counters on its owner thread every 100 ms and publishes a mutex
  protected snapshot; the shell never dereferences the live server object.
* `screen process_us avg/max`: screen message processing wall time, including
  authentication/decryption, Annex-B conversion, callback and scheduler
  preemption. It is not isolated crypto CPU time.
* Video `fps`: layer presentations since the previous video status command.
  `packets` counts submitted MPP packets in that interval. The legacy NALs
  total includes whole SPS/PPS configuration packets.
* `queue_peak`, pending, dropped, errors and resets: FIFO pressure and failures.
* `elapsed_ms avg/max`: enqueue-to-dequeue wait, non-empty MPP decode calls
  and presentation (layer ioctl plus vsync). These timing totals/maxima are
  cumulative since start, at RT-Thread millisecond resolution. Video statistics
  are diagnostic snapshots, not a transactional cross-thread sample.
* `skipped`: decoded pictures superseded before presentation, distinct from
  dropped compressed input packets.

The timing endpoint's large peer offset formerly overflowed a signed int when
logged in milliseconds. V18 prints seconds/fractions safely. This offset was
only logged and was not used to pace the video sink, so its old negative value
is not evidence of a video scheduling failure.

## Next decisions from board measurements

| Observation during continuous motion | Next investigation |
| --- | --- |
| RX near 30 FPS, output low, queue wait growing | Separate MPP decode wall time from display/vsync time; check VE completion waits and panel refresh. |
| RX low, queue short, process wall time high | Profile ChaCha20-Poly1305, Annex-B scanning and copies with CPU time; distinguish higher-priority decoder preemption from crypto cost. |
| RX low, queue short, process time low | Inspect sender cadence, TCP receive behavior/window and IPHETH throughput/USB completion cadence. |
| Output rate healthy but interaction remains delayed | Measure touch-to-HID-to-received-frame and queue age; FPS alone does not measure interactive latency. |
| Input drops grow | Remove the producer/consumer bottleneck or implement explicit keyframe resynchronization; do not randomly discard compressed P frames. |

After measuring, candidates include pooling NAL storage to replace per-NAL
malloc/free, transferring buffer ownership to reduce copies, separating costly
screen processing from control/HID polling, and tuning TCP/USB queueing. DMA
lifetime/cache ownership and authentication/nonce ordering must remain correct.
No performance gain from these candidates is claimed before implementation and
measurement. One stage should change at a time against this baseline.

Inbound /command and /feedback still return 405. Rechecked references:
DiPlay AirPlaySession.kt and LIVI stack/control.rs, stack/feedback.rs handle
those routes. LIVI feedback contains audio stream playhead/timestamp information;
this does not establish that our missing route causes video stutter. Incoming
command types need decoding before implementing their semantics. V18 does not
replace these errors with unconditional success responses.

## Validation

All 49 host tests and the platform boundary check pass, including screen/RTSP
and timing tests. The actual sink ASan/UBSan regression passes for configuration
atomicity, packet-slot progress and decoded backlog handling. The backlog model
verifies three outputs produce one layer update/vsync, two superseded returns,
and release of the old scanout only after vsync. Full D213 build passes.
V18 device FPS, latency and sustained touch behavior remain to be measured.
