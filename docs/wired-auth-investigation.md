# Wired service and authentication investigation

## Evidence boundary

D213 v13 UART proves Ethernet DHCP, four successful gateway ICMP replies,
external Lockdown activation, and (in one run) a TLS carkit/iAP2 session. In that
run the phone sends an encrypted RTSP `/auth-setup` request, the receiver sends
200, and the peer closes before media SETUP. A later run fails earlier at
stage=service after complete StartSession and StartService replies.

Those are different failure boundaries. Neither a pending bulk IN qTD nor a
running local receiver establishes the reason for a remote close. No new
phone test is needed to establish the software defects below.

## Confirmed transport defect: completed reads discarded after timeout

The original owned-URB port waits for a callback semaphore. When that wait times
out, it prints a synchronous UART/EHCI snapshot and checks the semaphore again.
If the transfer completes during this interval, it avoids cancellation but
still returns ETIMEDOUT and frees the DMA buffer. It does not copy the received
bytes or preserve the completed URB's next DATA PID.

The effect is not just a misleading warning: the USBMUX reassembler loses a
fragment of a multiplexed TCP/TLS stream. A later read can never recover bytes
already consumed from the USB endpoint. A lost DATA PID can also cause duplicate
packet handling. During a handshake the result can be a truncated TLS record
and an eventual timeout; during iAP2 it can corrupt the underlying TLS stream.

A regression compiles the actual port against official CherryUSB headers and
injects a seven-byte completion from the diagnostic hook. Against the original
v13 port it fails the assertion that the read returns seven bytes. The fixed
port passes under ASan/UBSan: the bytes reach the caller, the next DATA PID is
retained and the following canary is unchanged. Zero-byte/error completions,
disconnect generation changes and failed-cancel quarantine are also covered.

A second race occurs between the last semaphore check and EHCI kill: completion
can release hcpriv before kill, which then returns INVAL. The fix rechecks the
callback semaphore after a failed kill. It treats the URB as completed only
when that semaphore confirms it; otherwise it retains the existing quarantine.
A regression checks delivery of three bytes in this race without a leak.

This proves a real transport defect, not that it occurred in the supplied
UART runs. There is no captured successful completion inside their timeout
windows. A genuine incomplete timed-out transfer still returns timeout; the
fix does not invent or replay partially received stream data.

## Idle polling and service-stage diagnostics

The iAP2 reader normally polls every 200 ms and explicitly permits no-data read
timeouts. V13's port prints a complete EHCI snapshot on every such timeout,
adding blocking UART output while holding the hubport mutex. This can perturb
scheduling and widen the race above. Bulk IN timeout tracing is now opt-in
with `RTCP_USB_TRACE_IN_TIMEOUTS=1`; submission/completion errors and EP0/OUT
timeouts still log. A normal idle timeout is still returned to its caller.
The port regression enables tracing so that the original race stays exercised.

The session stage named `service` covers two operations. Diagnostics now retain
`OpenServiceConnection` before USBMUX connect and `ServiceTLS` before the TLS
handshake, and print the returned service port and SSL flag. TLS handshake
failure also logs the raw mbedTLS result alongside its mapped application
error. No key, record, certificate or TLS payload is logged. Injected connect
and TLS-read timeouts preserve the correct operation after cleanup, paired=1
and the successful last StartService exchange.

## Independent authentication/crypto checks

Audited references:

- [DiPlay MfiSapAuthSetup.kt](https://github.com/shihabal3amri/DiPlay/blob/615b32ce0b40cd0b99ce94ba23a512001553684f/shared/src/main/java/com/shilapi/xcertplay/airplay/MfiSapAuthSetup.kt),
  local checkout `615b32ce0b40cd0b99ce94ba23a512001553684f`.
- [LIVI auth_setup.rs](https://github.com/f-io/LIVI/blob/870d26441e073cdb802c427c5ae8833dba694951/native/crates/livi-cp/src/auth_setup.rs)
  and its `crypto.rs`, local checkout
  `870d26441e073cdb802c427c5ae8833dba694951`.
- usbmuxd `src/device.c`, local checkout
  `3ded00c9985a5108cfc7591a309f9a23d57a8cba`.
- Official CherryUSB EHCI cancellation at
  `a0054795c44037ace8a2b77b6e610adbff2257ea`.

For the supplied MFi 2.0C request, both application references agree with our
wire layout: accessory X25519 public key; big-endian certificate length and
certificate; big-endian encrypted signature length and signature. They also
agree on SHA-1(accessory public || phone public), SHA-1-based AES-KEY/AES-IV
labels, little-endian X25519 shared-secret bytes and AES-128-CTR. Their code is
comparison evidence, not proof of compatibility with the particular phone.

The old unit test checked response sizes and that the signature was not visibly
plaintext; it did not independently decrypt the response. The new optional
`tests/interop/test_auth_crypto.py` loads the actual C MFiSAP/control-cipher code
and uses Python cryptography/OpenSSL as the phone side. It independently:

- Derives the shared secret using the phone private key and returned public key.
- Checks the SHA-1/SHA-256 signing digest, derives AES key/IV, decrypts the
  returned signature and verifies it with a separately generated RSA identity.
- Covers protocol 2/RSA-1024 and protocol 3/RSA-2048; a modified signature fails.
- Decrypts a 1166-byte two-frame RTSP response, checks length AAD and per-frame
  nonce continuity, then decrypts the following response with the next nonce.

All pass. This substantially checks the crypto boundary without two copies of
our own code agreeing on the same bug. It does not validate the real chip's
certificate chain/authorization, the phone's policy, the live transmitted
ciphertext or runtime memory integrity on D213. No speculative change to the
MFi wire format, hash, signature order or frame-size policy is included.

## Reproduction and build

```sh
make -j8 check
make check-lubanlite-usb CHERRYUSB_ROOT=/path/to/CherryUSB
# Optional; requires Python cryptography and the host mbedTLS library above.
python3 tests/interop/test_auth_crypto.py
```

All 48 host executables and the platform-boundary check passed. The actual
Luban-Lite USB port ASan/UBSan regression and the independent peer checks
passed. The D213 SDK build remains separate from this application checkout.

The outstanding device-specific question is why the peer closes immediately
after the authentication response. The confirmed race is repaired, but it is
not a proven explanation of that close. Further resolution needs the actual
post-fix service/TLS boundary or a device/reference trace, rather than another
round of gateway ping or arbitrary protocol variations.


## V14 hardware result and SDK AES-CTR defect

The supplied v14 run successfully opens the service TLS stream, completes iAP2
MFi authentication/identification, and again exchanges the same six RTSP
requests. The phone closes at 1508 ms, about 25 ms after the handled auth-setup
response (1076-byte body, 1166-byte plaintext, 1202-byte wire, two frames).
IPHETH remains active: RX=187, TX=23, rxerr=1, zero TX/USB errors. No media
SETUP occurs. Thus the repaired USB timeout race is not an established cause
of the repeatable auth-response boundary.

Further inspection finds that the real SDK enables MBEDTLS_AES_ALT through
RT_HWCRYPTO_USING_AES, while native AES-CTR is disabled. Its ALT CTR function
still selects HWCRYPTO_TYPE_AES_CTR. The CE driver's native CTR case is empty
and returns RT_EOK. Depending on alignment handling, this silently leaves the
output untouched or copies unchanged input. This is a concrete failure in the
actual encryption path, and explains why local pre-encryption RSA verification
can succeed while the phone cannot verify the encrypted signature. The earlier
Python/OpenSSL checks used software AES; their success did not cover this
platform backend. That validation gap is now addressed explicitly.

The separate SDK fix composes CTR from working AES-ECB, following mbedTLS's
standard counter/keystream mechanism, and rejects raw unsupported CE CTR calls.
Actual-ALT host regressions reproduce the old silent success and test the
correction under ASan/UBSan using NIST vectors, signature-sized input, partial
calls, counter carry/wrap and failure propagation.

The responder now checks NIST SP 800-38A F.5.1's first block with its actual
runtime backend before producing a response, logging
`auth_setup=aes_ctr_check verified=1`. On failure it returns a crypto error
rather than sending a misleading successful auth body. A host regression
compiles the actual responder with a no-op-success backend and verifies that
it rejects that backend, emits no response and never requests a signature.
The MFi port log also now labels the signature prefix as such; its previous
"challenge ... head" wording actually printed signature bytes on success.

This identifies and fixes an encryption-path defect with direct relevance to
the observed boundary. Phone acceptance and media setup remain hardware
validation items, rather than implied by the software reproduction.


The corrected SDK and responder were validated together by a complete D213
firmware build. All 49 application Host tests (including backend rejection)
and the boundary check passed, as did the independent Python/OpenSSL peer
checks and both original/patched actual-ALT ASan/UBSan tests. These results
establish the backend correction and integration; they do not claim a
successful on-phone media session before the next hardware result.
