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
