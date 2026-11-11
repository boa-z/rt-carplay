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
V18 device FPS, latency and sustained touch behavior were not measured before
the decoder failure below; its performance changes are not yet validated.


## V19: preserve reference chains across repeated configuration

The v18 board run reaches screen SETUP, receives two 33-byte H.264
configuration messages, logs a decoder recreation, then reports missing
references (`ref_pic_list_reordering: i(-1) < 0`) and VE error 80050100.
Subsequent pictures inherit the damaged reference state. The log does not
contain configuration bytes, so equal size alone does not prove identical
SPS/PPS. This is a decoder correctness failure; it prevents a meaningful
performance comparison.

Source inspection confirms a pre-existing sink defect: every configuration
after any displayed frame destroys the decoder, even when SPS/PPS are
unchanged, then feeds following dependent pictures without requiring IDR.
The v18 timing changes can expose that path differently; the log alone does
not isolate which performance change made the defect visible.

V19 compares full configuration bytes on the decoder owner thread. Identical
configuration is skipped with a `repeated configuration ignored` diagnostic,
preserving decoder references. Changed configuration is cached; recreation is
deferred until the first slice of an IDR. The new decoder receives cached
SPS/PPS before IDR, and dependent NALs are withheld meanwhile. Decode/submission
failures similarly quarantine the decoder until an IDR permits recreation.
Queue entries carry a loss generation so entries before a dropped input are
not confused with entries after it; the latter require IDR resynchronization.
A later IDR slice cannot start recovery: first_mb_in_slice must be zero.
The display layer is disabled and vsync awaited before reset releases scanout.

`carplay_video status` adds repeated_configs, waiting_idr, and skipped_nals.
These skipped dependent NALs are separate from FIFO drops and skipped decoded
presentations. V18 polling, priority, backlog handling and timing measurements
remain in place, so this build isolates reference-chain lifetime repair.

The actual sink ASan/UBSan tests now cover repeated config after output with
an inter picture, changed config followed by inter/IDR, a decode error followed
by inter/IDR, and an input gap followed by a later IDR slice before a complete
IDR start. A small reference-dependent MPP model rejects inter pictures after
reference destruction. It does not emulate VE, validate a real SPS or prove
that decoded-backlog handling is correct on silicon. All 49 application tests
and the boundary check pass. D213 image v19 requires board validation.

Recovery currently waits for a sender IDR; it does not request one or promise
a recovery deadline. If waiting_idr remains one and picture freezes, capture
that status and reconnect once to establish a fresh stream. If missing-reference
errors persist without configuration recreation, investigate the original
bitstream/reference marking and independently disable output coalescing.
Do not interpret the recovery guard as proof that the initial fault is fixed.

Board check: repeat the usual start/pair/iap2 sequence, retain startup config,
IDR and reset logs, and confirm picture plus touch. During continuous scrolling,
run carplay_status and carplay_video status twice about ten seconds apart.
Expected: no reference-error cascade, identical second config does not increment
resets, waiting_idr clears after IDR, and no sustained drops/errors. Compare
RX FPS, presentation FPS and queue/decode/present elapsed times in the second
interval before deciding the next performance change.


## V19 board result and V20 interactive TCP experiment

The v19 board run receives and presents 78 pictures (79 MPP packets including
configuration), with zero input drops, decode errors, resets, skipped outputs
and waiting-IDR NALs. HID writes succeed 70/70 with no full touch queue. This
run receives only one configuration; it validates a healthy run but does not
exercise duplicate-configuration recovery on the device. The user still
reports very poor smoothness.

RX reports 2.0 FPS over 38.813 seconds, whereas the first presentation report
covers 88.062 seconds and reports 0.8 FPS. These intervals have different
start times and include startup/static time, so neither is a motion-only rate
and they must not be divided to infer a throughput ratio. At the snapshot the
FIFO is empty (peak nine), queue delay averages 16 ms with max 136 ms, decode
9/15 ms and present 8/16 ms. Screen processing averages 1.429 ms with max 12 ms
(including callback/scheduler preemption). These values do not show a sustained
decoder backlog; they also do not rule out transient stalls or input/network
latency. Sender cadence, TCP delay and motion-only rates remain unmeasured.

V20 makes one behavioral experiment: enable TCP_NODELAY on the RT-Thread TCP
listeners, accepted sockets and outgoing connections, including event/HID and
RTSP. The event channel sends small encrypted HID messages and previously left
Nagle enabled; a subsequent small write can wait for an outstanding segment's
ACK. Disabling Nagle removes that local send delay. It does not disable remote
Nagle, change the receive-side delayed-ACK policy, or prove this delay is the
reported stutter. Socket configuration success/failure is logged once per
socket, never per report. Decode, receive polling, display and USB algorithms
are unchanged in this experiment.

Additional cumulative diagnostics in carplay_status:

* receive gap: intervals between complete, accepted screen frames, excluding
  the first frame; includes static periods and stream/network/scheduler delays.
* sender gap: positive differences in the sender timestamps of consecutive
  frames, in the sender clock domain; zero/backward differences are excluded.
  Timestamp precision/meaning is sender-dependent; this is not measured latency
  and clocks are not subtracted across hosts.
* assembly: first received header bytes through complete body, before screen
  processing; includes packet delivery gaps and scheduling. It excludes any
  wait before the first received header byte.
* receive-gap buckets: <=40 ms, (40,100] ms, >100 ms. These are cumulative counts,
  so compare two status snapshots around continuous motion.

POST /command now parses and logs the bounded binary-plist type for the first
eight commands per session, then every 64th, sanitizing control characters.
Unsupported commands retain 405; no unimplemented UI/audio operation is reported
as successful. DiPlay handles requestUI/duckAudio/unduckAudio, but the v19 log
only contains request lengths. Observing the actual types is required before
assigning these errors a role in the stutter. Feedback behavior is unchanged.

Validation: all 49 host tests and boundary check pass. The real screen-server
socket test uses independent receive and sender clocks, verifies all three gap
buckets and delayed header/body assembly under ASan/UBSan. The actual RT-Thread
TCP seam is also compiled against host BSD sockets with SAL header adapters:
IPv4/IPv6 blocking/bounded/accepted sockets have NODELAY=1 via getsockopt and
transfer data. This checks socket policy, not lwIP timing or a performance gain.
The full D213 build passes. Test logs are under /tmp/ipheth-reference with v20
in their filenames.

Board comparison: flash v20, verify tcp_nodelay=enabled and picture/touch. Run
carplay_status plus carplay_video status, continuously drag/scroll the same
view for ten seconds, then repeat both commands immediately. Retain startup
unsupported_command type logs as well as the complete second status. If HID
feels faster but frame cadence remains low, the local TCP send experiment only
addresses part of the problem. Similar sender/receive gaps point toward source
cadence or upstream flow control; short sender gaps with bursty receive gaps
point toward delivery/scheduling. Large assembly gaps warrant TCP/USB traces.
None of these observations alone establishes a root cause.
