# USBMUX negotiation audit and D213 validation

## Source reference

Reviewed usbmuxd `src/device.c` at
[3ded00c9985a5108cfc7591a309f9a23d57a8cba](https://github.com/libimobiledevice/usbmuxd/blob/3ded00c9985a5108cfc7591a309f9a23d57a8cba/src/device.c):
`device_add`, `send_packet`, `device_version_input` and `device_data_input`.
The existing GPL-3.0-or-later implementation is updated from these protocol
behaviors; no usbmuxd source is vendored.

The initial VERSION packet uses an eight-byte mux header followed by three
big-endian u32 values (major, minor, padding). Its total length is 20 bytes.
The outgoing vector was already byte-correct, but our parser incorrectly
returned the body at offset 16 and our startup ignored the receive result.
A successful `version/setup ret=0` therefore did not prove negotiation.

usbmuxd resets v2 SETUP's mux sequence fields to tx=0, rx=0xffff. Subsequent
frames increment tx and echo the peer's received rx_seq field. These are
separate from each TCP-like connection's sequence and acknowledgment. Our
previous SETUP used tx=1/rx=0 and later frames always wrote rx=0. The host now
matches the source behavior and only sends SETUP after accepting a complete
version-2 reply. Missing replies time out, malformed replies fail, and version 1
is explicitly rejected because this host implements v2 framing only.

The low-level TCP builder initializes rx_seq to zero as a placeholder; the
stateful host fills it from the negotiated connection-wide mux state.

## Supplied board evidence

On the previous image, `carplay_usb mux` reported version/setup ret=0 and
Lockdown connect ret=-4. `carplay_usb pair` also returned -4, stage=pair,
without a remote error string. This does not establish a Pair rejection or
that a trust prompt was sent. The pair stage is entered before opening its
Lockdown stream, so it includes connection-open failures. No capture yet
identifies whether VERSION replies or SYN replies were received.

The changes add version reply/peer, cumulative RX bytes/frame counts and
pending reassembly length to the console. They are a source-backed protocol
correction and better evidence collection, not a demonstrated hardware fix.
Pairing stays in this application; the independent Ethernet driver is unchanged.

## Automated checks

The USBMUX tests cover source-derived VERSION vectors, fragmented replies,
missing/truncated/malformed replies, unsupported version 1, no SETUP or SYN
before valid negotiation, idempotent start on the same host object, initial
SETUP/SYN sequences and distinct sentinel values for peer tx_seq/rx_seq echo.
Existing stream, reset, overflow, fragmentation and optional-padding cases
remain enabled. Host Make builds use ASan/UBSan and warning-as-error flags.

## Board procedure

With the new image booted, start with the phone unplugged:

```text
carplay_usb start
```

Connect the unlocked phone and capture enumeration. Then test the read-only
Lockdown query first:

```text
carplay_usb mux
ipheth_status
usb_ehci_status 1
```

Record `version_rx`, `peer`, `rxbytes`, `frames`, `pending`, Lockdown connect and
GetValue results. Expected negotiation is version_rx=1, peer=2.x and a received
VERSION frame; this does not imply trust or Ethernet readiness.

For a separate pairing attempt, unplug/replug to start a fresh device-side mux
session, then run `carplay_usb pair` directly. Both shell commands currently
initialize their own mux test session; calling them in succession on one
attachment is not a test of an existing-session reuse mechanism. Unlock the
phone and accept a trust dialog if it appears. Preserve all output and finish
with `ipheth_status` and `usb_ehci_status 1`. Do not delete trust records merely
to collect this evidence.

If VERSION itself has no reply, investigate the USBMUX bulk transport before
attributing the failure to trust. If VERSION succeeds but Lockdown SYN has no
reply, compare the v2 stream handshake and endpoint traffic. A remote plist
error or a later service-start error is a different failure stage. Actual
network traffic and reconnect stability still require hardware validation.


## V8 result and DATA PID lifetime correction

The v8 board logs now establish a complete VERSION 2.0 reply: 20 bytes, one
parsed frame. Both fresh-attachment `mux` and `pair` tests then time out opening
the Lockdown stream (62078), with RX still at exactly 20 bytes/one frame and no
pending reassembly. No Pair request/remote plist error is demonstrated. IPHETH
still has one pending first RX and no frames; this is not evidence of a trust
rejection. One unplug/replug rebinds USBMUX and IPHETH and reuses i0, but does
not validate reconnect under active traffic.

A separate application USB-port defect was then found by checking official
CherryUSB at a0054795c44037ace8a2b77b6e610adbff2257ea:

- `core/usbh_core.h`, `usbh_bulk_urb_fill`: fills transfer fields while retaining
  `urb.data_toggle` for reuse.
- `port/ehci/usb_hc_ehci.c`, `ehci_bulk_urb_init`: seeds the QH overlay DATA toggle
  from `urb.data_toggle`.
- The EHCI completion path writes the next DATA toggle back into the URB.
- Upstream host classes such as `class/cdc/usbh_cdc_ecm.c` keep their IN/OUT URBs
  across calls, retaining that state. Linux/libusb/usbmuxd instead rely on their
  USB stack to preserve endpoint state underneath each userspace transfer.

Our port allocates and zeroes a private DMA URB on every transfer, so it lost the
completed toggle. After a short VERSION OUT at DATA0, the next SETUP must use
DATA1. Resending DATA0 can be ACKed as a duplicate without delivering SETUP to
the device's consumer. That is compatible with transport send success followed
by absent stream responses. This is a concrete software defect and a plausible
explanation of the observed sequence; no bus capture yet confirms that it was
the hardware failure mechanism.

V9 stores IN and OUT toggles independently across owned DMA URBs, reading and
writing them while holding the port mutex. State resets only at USB attachment
or detachment, not at each mux frame or shell command. Generation checks and
critical sections prevent a detected stale completion from overwriting the
reset state. Control transfers retain their independent Setup/DATA rules.
No independent IPHETH driver or official HCD source is changed by this fix.

The actual port regression models receiver duplicate-PID handling and verifies
DATA0/1/0 across 20/17/36-byte OUT transfers, independent short IN transfers,
a full packet followed by ZLP, failed submit, a no-progress timeout, state reset
and an old-generation completion. Restoring only the old per-transfer DATA0
behavior makes this regression fail at the second transfer; the fixed path
passes ASan/UBSan with official CherryUSB headers. Existing alignment, bounds,
allocation-failure and failed-cancel/late-callback cases remain enabled.
This is a host-side model, not electrical/HCD hardware validation.

On a timeout after partial USB progress, the HCD cancellation path may not
report the final toggle; this change does not claim transparent recovery from
that case. Use a fresh attachment for these diagnostic runs rather than
assuming that a partially cancelled stream can resume safely.

Use the board procedure above with v9: collect VERSION, Lockdown connect and
GetValue first, then perform a fresh-attachment pairing attempt. Only after a
successful external session/readiness transition should Ethernet traffic be
assessed. If the stream opens but a later Pair/SSL/service error appears,
record its exact stage; that is new evidence rather than the same SYN timeout.


## V9 result and asymmetric RX header validation

V9 board evidence differs from v8: after the valid 20-byte VERSION reply,
36 more bytes arrive. Both fresh-attachment tests reach RX bytes=56 but
frames=1, then fail with `frame desynchronised`. The complete extra transfer
was rejected before TCP dispatch. Its contents were not captured, so neither
SYN|ACK nor RST nor a particular third-word value is yet established. IPHETH
remains without first RX; this does not identify a pairing rejection.

Rechecking usbmuxd's `device_data_input` shows no equality check between the
peer's third header word and the host's outgoing 0xfeedface marker. Our RX
parser and optional-padding plausibility check imposed that unsupported
symmetry, and all earlier synthetic peer builders used our host marker.
V10 treats that inbound word as opaque while retaining recognized protocol,
bounded length and protocol-specific minimum size checks. VERSION stays
exactly 20 bytes; TCP requires at least a 16-byte mux + 20-byte TCP header;
SETUP requires its byte; CONTROL is recognized separately and logged by type
and length, without dumping its contents or delivering it as TCP. Host TX
still uses 0xfeedface.

New first-frame diagnostics report protocol, length, third word and mux
sequences, then TCP source/destination/flags. Invalid headers log only complete
available header fields before dropping, without reading truncated storage.
These logs contain no plist/TLS/pair-record payload. The successful header/TCP
logs are limited to the first two post-VERSION frames. Their purpose is to
identify the actual board reply, including the possibility of RST rather than
SYN|ACK, without inferring it solely from a 36-byte length.

An independent 36-byte SYN|ACK vector tests opaque third words 0,
0x12345678 and 0xfeedface; each reaches stream establishment at total 56 RX
bytes/two frames. Separate tests retain rejection of unknown protocols and
undersized TCP headers and acceptance of CONTROL without TCP dispatch. These
are constructed regression vectors, not bytes captured from this iPhone.
The source-backed RX correction still requires board confirmation.

## V10 result and framed GetValue exchange

The supplied v10 reply is now identified: protocol 6, length 36, peer third
word `faceface`, source port 62078, destination port 1, flags 12 (SYN|ACK).
Lockdown stream establishment returns 0. This confirms that the earlier
symmetric 0xfeedface RX requirement rejected a valid peer header.

After `GetValue sent ... bytes=317`, the phone emits CONTROL type 3 with
46 payload bytes, and no XML reply. Type 3 is a mux-level error in usbmuxd's
`device_control_input`, not a Lockdown plist `Error` or proof of trust refusal.
The old diagnostic omitted its 45 bytes of text, so its actual reason remains
unknown. V11 retains the type/count and up to 64 sanitized printable text bytes,
printing that diagnostic on a separate bounded line. It does not dump TCP,
TLS, certificate or pair-record payloads. Like usbmuxd, it does not assume that
one CONTROL packet means all connections must be reset.

A definite smoke-command defect was found independently of the missing error
text: `cmd_mux` sent the XML document directly over the stream. Lockdown
requires a four-byte big-endian length before the document. The reference is
libimobiledevice `src/property_list_service.c`, `internal_plist_send`, at
fa0f79190142bc309307967c058f89c1b36eb6b8: it writes `htobe32(length)` and then
the content. Our pairing path already uses `rtcp_lockdown_exchange` with that
framing, but the smoke command bypassed it. V11 reuses that same function for
GetValue, including bounded length-prefixed reply reading. The bad request is
corrected; without the actual CONTROL text it is not yet proven to be the
cause of the observed type-3 error.

The shared stream reader also previously returned a timeout if one successful
pump delivered only an ACK or unrelated frame. The portable
`rtcp_usbmux_connection_read_wait` now waits for this connection's bytes up to
an injected monotonic clock deadline; the command and pairing both use it.
Initial TCP diagnostics include seq/ack on RX and the first few TX headers so
an actual mux/TCP error can be distinguished from plist framing failure.

The regression combines the actual mux host and Lockdown exchange with a fake
peer: an ACK-only packet and CONTROL warning precede a reply whose four-byte
prefix is split across TCP frames. It verifies the outgoing prefix/document,
stream sequence advance, correct reply extraction, no false timeout and clock
wrap. Separate tests bound/sanitize/truncate CONTROL text and clear stale
text on an empty CONTROL. Plain and logging-enabled ASan/UBSan builds exercise
these paths. Real device error text and successful GetValue still require the
v11 board run. The independent Ethernet driver remains unchanged.

## V11 board result: successful GetValue

The next user-supplied UART run confirms VERSION 2.0, a SYN|ACK from 62078,
seq=0/ack=1, and successful Lockdown connection establishment. The host sends
its four-byte length prefix at seq=1, then the 317-byte XML request at seq=5.
The phone returns a 365-byte mux TCP frame with seq=1/ack=322, containing a
four-byte length prefix and 325-byte plist. `GetValue exchange ret=0`,
`complete=1`, and parsed `ProductVersion=27.3` confirm successful framed
request/reply exchange. No CONTROL type-3 error appears in this supplied run.
The old malformed-request failure has not been proven equivalent to a specific
CONTROL message because its text was never captured.

The console truncates the long XML print, but the exchange's validated reply
length and parsed Value independently establish success; an incomplete-looking
console dump is not a failed reply. No new firmware is needed to proceed.

IPHETH still reports RX=TX=0, submitted=1/completions=0, ready=no/carrier=down.
GetValue success does not establish host trust, successful Pair, StartSession
or network readiness. Next, use the same image on a fresh attachment for
`carplay_usb pair`, accept any trust prompt, and capture the complete stage/error
output plus `ipheth_status`, `ifconfig` and `carplay_usb net`. A later carkit
service error must be distinguished from Pair/StartSession failure. Peer IPv6
communication and reconnect-under-load remain unvalidated.

## V11 pairing result and request-level diagnostics

The supplied fresh-attachment Pair run establishes Lockdown, receives a
400-byte TCP frame after SetValue UntrustedHostBUID, and later reaches total
RX=1486 bytes/six mux frames before returning -4 at the broad `pair` stage.
Earlier first-frame logging stops before those later replies, so the counts
alone cannot identify the failed subrequest. There is no successful Pair,
StartSession, or first IPHETH RX demonstrated by these logs.

V12 records and logs operation names and request/reply lengths for
SetValue/UntrustedHostBUID, GetValue/DevicePublicKey, GetValue/WiFiAddress, Pair,
StartSession and StartService. Pair-record generation has begin/end markers;
connection opening is separately named. Metadata remains available after
failure cleanup, with transport exchange status distinct from a remote plist
Error. An incomplete framed read logs received/wanted byte counts without
printing the underlying plist, key or certificate contents.

The shell command's per-read timeout is changed from 5000 to 30000 ms, matching
libimobiledevice `property_list_service_receive_plist` in the audited
`src/property_list_service.c` revision. The core's configurable default is not
changed. This removes a shorter-than-reference wait but does not establish
that the observed timeout was caused by a slow Pair reply or user interaction.
No automatic retry is added after a transport timeout: partially consumed
length-prefixed messages are not safely retried as new requests on the same
stream. Existing explicit PairingDialogResponsePending handling is retained.

A timeout-injection test confirms that a first-request timeout retains the
operation and correct zero reply length, releases resources and closes the
connection once. The denied-Pair test distinguishes a successfully received
remote error reply from a transport timeout. Full pairing/session/TLS tests
remain enabled. Next, use v12 on a fresh attachment and run `carplay_usb pair`
directly, accepting any phone trust prompt; capture all request/reply markers,
the final operation/stage/error, and IPHETH status. Driver and HCD behavior are
unchanged by this diagnostic build.

## V12 board results: Pair accepted; intermittent partial replies remain

The first supplied run completes SetValue, DevicePublicKey, WiFiAddress and
pair-record generation, then receives a complete 316-byte Pair plist and
advances to StartSession. By the session implementation's checks, the Pair
reply had no parsed Error and the local `paired` flag is true. StartSession
then reads a length of 354 but fails before completing the body with RTCP_EIO.
Concurrent EP0 logs include raw -12 (USB_ERR_IO) and -4 (USB_ERR_NOTCONN).
There is no completed StartSession plist, TLS attempt or service-start result.
Those errors must not be called a certificate/TLS failure.

A second attachment completes the first two plist requests but reads only the
335-byte length for WiFiAddress before timing out on its body. With varied
failure position and already received prefixes, these runs justify transport
investigation; they do not establish one specific HCD, cache, device or pairing
cause. The later IPHETH snapshot still has no completed first RX. Acceptance of
a Pair reply alone has not produced demonstrated network traffic.

V13 prints raw submit/completion errors, endpoint, actual length and DATA toggle.
On a wait timeout, an optional weak SDK hook captures the existing read-only
EHCI register/QH/qTD snapshot before cancellation, while the pending USBMUX QH
is still available. Ordinary snapshots taken after cancellation cannot show
that QH. A callback completing during the dump is checked before trying to kill
an already completed URB; the caller still returns timeout and does not assume
a partially consumed stream can be resumed. Tests cover that completion/dump
race and retained failed-cancel storage.

USBMUX IN requests are capped at 16384 bytes, matching usbmuxd `USB_MRU` in
`src/usb.h` and its `libusb_fill_bulk_transfer` in `src/usb.c`. The existing
mux reassembler handles continuation; this changes USB transfer chunk size,
not framing or plist format. The application-port test checks the 16 KiB return
and a canary after it. This source-backed limit reduces the number of qTDs
per ordinary USBMUX read but is not a demonstrated hardware fix. IPHETH's
64 KiB NCM RX is unchanged. No new HCD scheduling or cache patch is added.

Use v13 on a fresh attachment, run `carplay_usb pair` directly, and capture
through the final operation/stage/error and any `USB completion error`,
`USB wait timeout`, `EHCI before cancel` and qTD output. The failure summary now
explicitly shows `paired=0/1`. Then record `ipheth_status`, `carplay_usb net`
and `usb_ehci_status 1`. Do not erase trust records or assume TLS began when
StartSession's body was not fully read.


## V13 board result: activation and wired network traffic succeed

The supplied run completes SetValue, DevicePublicKey, WiFiAddress, Pair,
StartSession (354-byte reply) and StartService (383-byte reply). The command
reports an open service stream and session/service TLS enabled. IPHETH reports
RX=79, TX=18, 81 receive completions, one control completion, ready=yes and
carrier=up. DHCP leases 172.20.10.4/28 with gateway/DNS 172.20.10.1; the local
IPv6 Link-Local address is valid. There is one unclassified RX error and no USB
or TX errors. This establishes a working combination, not a controlled proof
that the 16 KiB chunk limit alone cured all earlier intermittent failures.

The preserved image is v13, SHA256
`72b542a84c1600799df01e696ebc017e46051a35a1cb2c5a019621799f3c989d`;
its manifest pins application e168c31, SDK a6c3a34a4 and driver 25792b9.
No new image is needed to continue connectivity testing.

### Subsequent wired application run

`carplay_start wired` opens the IPv6 RTSP listener. `carplay_usb iap2` reports
link ready, MFi authenticated and identification accepted over the TLS carkit
stream. The advertised endpoint is [FE80::DCB5:4FFF:FE73:DD03]:7000. A client
connects and exchanges three `/pair-setup`, two `/pair-verify` and one
`/auth-setup` request. All six responses are locally reported as status 200;
RTSP encryption is enabled after pair verify. The local MFi signature check
returns verified=1 and auth-setup emits a 1076-byte response, followed by peer
closure at connection lifetime 1602 ms. No media SETUP or streaming is shown.
Local signature verification and a sent 200 response do not prove that the
phone accepted the complete authentication response. This failure boundary
belongs to the application authentication/session layer and needs a reference
protocol/response comparison before claiming a cause.

The `peer=254.128.0.0` log is a formatting defect: server_accept unconditionally
prints the first four address bytes as IPv4, including for IPv6 peers. It is not
an IPv4 route observation. The complete peer address was not captured here.

Repeated endpoint-85 timeouts occur while iAP2/RTSP still make progress. The
iAP2 stream interface explicitly treats a no-data read timeout as a usable,
idle link. An Active qTD retaining all bytes cannot prove a controller failure
or a device NAK without a bus trace. The current port dumps every timeout,
including idle polls; treat these as diagnostic noise unless accompanied by
lost progress or a failed framed exchange. Do not conflate USBMUX endpoint 85
with IPHETH endpoint 86, or infer that the phone's RTSP close was caused by
these snapshots. The touch-device open failure is another independent issue.

Next collect `ipheth_status`, `ping 172.20.10.1`, `carplay_usb net` and
`carplay_status` with the working session left open. Preserve request/response
metadata around any subsequent RTSP connection. IPv6 peer reachability under
load, physical unplug/replug, and media startup remain separate open checks.


### Later v13 run: service-stage timeout with working gateway ICMP

StartSession returns a complete 354-byte reply and StartService a complete
383-byte reply. The command subsequently fails with RTCP_ETIMEDOUT at
stage=service, paired=1. Source inspection shows this stage covers both
opening the returned service port over USBMUX and its optional TLS handshake.
The retained operation=StartService and last exchange ret=0 describe the last
plist exchange, not a failed StartService request. These diagnostics do not
distinguish service connection opening from service TLS negotiation; do not
claim a certificate failure or erase trust based on this output. The EP85
snapshot alone cannot resolve that distinction.

All four subsequent gateway ICMP replies succeed (2, 1, 1, 1 ms, TTL 64).
IPHETH is ready/carrier-up with RX=85, TX=16, rxerr=4 and zero TX/USB errors.
Multiple NCM datagrams can be delivered per USB completion. This later run
shows USB address 4 rather than the earlier 2, so the different error/frame
counts do not establish growth on a single attachment. Collect counters
before and after traffic on one attachment to assess the RX errors.

The receiver remains running with zero audio packets. A running local
receiver does not imply that the remote service or media session succeeded.


## V14 source investigation and bounded transport fix

The [wired authentication investigation](wired-auth-investigation.md) records a
reproduced data-loss race in v13's post-timeout diagnostics, its actual-port
ASan/UBSan regression, the correction and independent Python/OpenSSL peer
checks of the MFiSAP/control-cipher code. A completed read is now delivered
normally if its callback runs during diagnostics or just before a failed
cancel; its next DATA PID is retained. Idle bulk-IN timeout dumps are opt-in.
Service-connect and service-TLS failures retain distinct operation names.

This is a confirmed source defect with a deterministic reproduction. The
supplied UART does not prove this race caused its service timeout or RTSP peer
close; those hardware conclusions remain open. No IPHETH framing, USBMUX wire
sequence, TLS version, MFi response format or RTSP frame-size policy changes
are included. All 48 host tests, platform boundary, actual-port sanitizer
regression, and independent peer crypto checks passed.


## V14 board result: service succeeds, authentication close repeats

Service TLS opens successfully, iAP2 authenticates, and the phone again closes
RTSP about 25 ms after the auth response, with no media SETUP. Investigation
then identifies the SDK's silent no-op native AES-CTR backend, which the prior
software-AES interop check did not cover. See the
[backend investigation](wired-auth-investigation.md#v14-hardware-result-and-sdk-aes-ctr-defect)
for the source chain, deterministic reproduction and separate SDK correction.
The responder now validates its real AES-CTR backend with a NIST known-answer
check before replying. No control-channel framing change is part of this fix.

## V15 hardware: post-auth media transport established

The supplied run opens carkit TLS port 63567, authenticates iAP2 and completes
AirPlay auth-setup with the runtime AES-CTR check verified. The phone proceeds
to initial SETUP, event connection, /info, RECORD and type-110 screen SETUP.
The screen receiver continues through 720 frames. The blocking failure has
moved to the local MPP sink (NALs=14, frames=8, FIFO pending=15/13,
errors=242/244), with duplicate resets and an invalid frame allocation.
See wired-auth-investigation.md for the independently reproduced sink and
SDK-parser fixes in v16. No USBMUX or IPHETH code changes are required for
these fixes; audio, command handling and usable display remain unvalidated.
