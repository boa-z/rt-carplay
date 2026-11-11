# v22 pairing persistence and launch-page management

## Failure identified

The v21 trace reaches the third AirPlay Pair-Setup exchange, reports
`pair_setup result=7`, sends a short TLV error response, then the phone closes.
In `airplay_pair_setup.h`, 7 is `AIRPLAY_PAIR_SETUP_STORAGE_FAILED`.
It is distinct from USB trust, iAP2 authentication and MFi /auth-setup.

The receiver previously saved its controller table with
`rename("...tmp", "...bin")`. The SDK's DFS Elm/FatFS adapter calls `f_rename`
directly. That API rejects an existing destination (`FR_EXIST`), so updating
an existing file fails even though creating the first file succeeds.
Host POSIX rename semantics hid this discrepancy.

The app now writes and closes a temporary file, moves an existing primary to
an absent `.bak` name, then moves the temporary file into place. A failed commit
attempts rollback; loading can recover a backup if the primary is absent.
Identical controller identifier/key updates avoid another flash write. This is
best-effort interruption recovery, not a full power-loss filesystem transaction.
The launcher records storage failures and exits the connection wait promptly.
It only declares video active after the decoder has produced a frame.

## USB trust reuse

The app queries lockdown UniqueDeviceID and saves a versioned local pair record
in `/data/rt-carplay-usb-<UDID>.bin`. Subsequent connections load the existing
HostID/SystemBUID and certificates/private keys, skip Pair and RSA generation,
and proceed through StartSession/TLS/StartService. Only explicit StartSession
InvalidHostID/InvalidPairRecord/MissingPairRecord errors trigger a fresh Pair
attempt. Network and TLS failures do not delete the persisted record.

Records include host-generated private keys; the UI only lists names/sizes and
never prints or exports key material. The format includes magic/version, local
record size, checksum and validated field bounds. Its checksum detects accidental
corruption, not tampering, and this local format is not portable across ABI
changes. Never publish `/data` trust files in a public repository or image.

The first connection still requires phone consent. Resetting/revoking phone
trust, deleting a local record, changing the record ABI or reflashing/erasing the
`data` partition can require Trust again. Preserve `/data` for a reboot test.
Actual iOS trust reuse still needs validation on the device.

## LVGL controls

The English launch page uses existing built-in fonts and shows connection phase,
previous/failing phase, USB/IPv6, USB trust cache result, AirPlay storage result,
received/displayed frames, drops, decode errors, HID results and last error.
These are diagnostic snapshots, not synchronized performance measurements.

* **Pair files**: available while waiting or after failure. Opening management
  holds off automatic pairing, even if a phone is inserted. The worker lists up
  to 12 logical records from `/data`: USB records and the AirPlay controller
  table `rt-carplay-pairings.bin`. Backup-only or temporary-only entries are also
  included so they can be removed. Refresh after deletion to see further records.
* Tap an entry to see its full name and a **Cancel / Forget record** confirmation.
  USB deletion can require Trust on that iPhone again. AirPlay table deletion
  forgets all AirPlay controllers, but does not delete the receiver's identity.
* Confirmed deletion runs on the session-owning worker while USB sessions are
  closed. It stops the receiver, deletes the temporary/backup/primary copies,
  then restarts the receiver to discard the in-memory AirPlay table. Errors are
  displayed as a numeric result. The device seed is excluded by an allowlist;
  path traversal and arbitrary file deletion are rejected.
* **Refresh**, **Back** and **Retry connection** (after failure) avoid shell
  commands. Failure still does not automatically loop through trust dialogs.

The launcher owns the single GT911 reader for its lifetime. It feeds a bounded
pointer queue consumed by an LVGL input callback before video output, and
forwards touch to the CarPlay queue while video is active. GUI pointer input
tracks one finger; CarPlay retains its existing multitouch reports. The SDK
LVGL panel reader remains disabled. Receiver restart/storage failure therefore
does not disable the management page's touch input. All LVGL calls remain on the
GUI thread; TLS and file operations run on the worker. A stage lock prevents
management from racing pairing or a touch report from racing receiver teardown.

## Validation

* 50 host tests plus boundary check pass with ASan/UBSan. New tests simulate
  FatFS's no-overwrite rename, failed commit/rollback recovery, USB cache round
  trip/corruption/bounds and file-manager allowlisting/deletion of recovery
  copies while preserving the identity file. A resumed real TLS lockdown fake
  phone session issues zero Pair requests and keeps the original HostID.
* Actual launch worker tests cover trust failure, storage failure, replug,
  network readiness, first decoded frame, delayed iAP2 shutdown, pointer
  routing/release/overflow and exclusion of pairing while management is open.
* Actual video sink and touch-reader seam regressions pass against SDK headers.
  Full D213 SDK/image build passes. Host tests do not validate framebuffer
  appearance, physical panel behavior or real iOS trust retention.

Board checks require no startup commands:

1. Boot with phone unplugged; open Pair files, Refresh and Back. Confirm no seed
   is listed. Plug an unlocked phone in, accept consent once and verify picture
   and touch. The page must not claim video active without a decoded frame.
2. Replug that phone; check `Trust record: REUSED` during connection and confirm
   Trust is not requested. Reboot without erasing `/data`, then repeat.
3. Unplug, delete only that phone's USB record via confirmation, then reconnect.
   Expect a new USB pairing and possibly renewed Trust. Cancel deletion once to
   verify the record remains. AirPlay table deletion is a separate operation.
4. If connection fails, retain stage/previous/last error and storage/trust
   diagnostics; try Retry connection. A storage failure should return to a
   usable management page rather than showing a false connected state.

This release repairs persistence and demo management. It retains the earlier
video cadence diagnostics and does not claim to resolve the remaining stutter.


## v23: RT-Thread negative errno regression

The v22 board trace stops at `cannot load pairing store` with stage 7/error -1
before any phone pairing attempt. Unlike host POSIX, this SDK keeps negative
filesystem errno values: `dfs_elm.c` maps a missing file to `-ENOENT`,
`dfs_posix.c:open()` passes that result to `rt_set_errno()`, `kservice.c` stores
it unchanged and newlib `syscalls.c:__errno()` returns `_rt_errno()` directly.
V22 checked only `errno == ENOENT`, so even a clean data partition with no
controller file was treated as an I/O error. The same assumption affected
backup lookup, first save, missing-backup removal and management deletion.

V23 matches the specific error in either sign at every pairing-file call site.
Missing primary plus missing backup yields an empty table; missing primary with
an existing backup loads the backup. Genuine permission/I/O/read/size failures
remain errors; the fix does not delete trust files, reset device identity or
silently accept corrupted records. Load failures now log the path and raw errno
or read/size/close context. No USB configuration or phone setting change is
needed to address this regression.

The persistence test now runs every scenario with both positive POSIX errno
and negative SDK errno. Adding those SDK-style wrappers reproduced v22's
empty-first-boot failure before the fix and passes after it. It also tests
new-file save, repeated replacement, interrupted backup recovery, trust cache,
management deletion and preservation of real access/oversize errors in both
modes. Makefile dependencies now include the platform sources included by this
test to prevent stale test binaries after a platform-source edit.

Flash v23 with the phone unplugged: the receiver should reach the normal waiting
page rather than `cannot load pairing store`. Connect once and verify picture
and touch, then replug the same phone to check trust reuse. Keep existing `/data`
when possible; deleting the pairing table is not a workaround for v22's errno
bug because an absent table also triggers that bug. Real-device validation of
the corrected image remains required.
