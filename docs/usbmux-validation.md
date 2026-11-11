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
