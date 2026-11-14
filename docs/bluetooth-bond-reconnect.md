[English](bluetooth-bond-reconnect.md) | [简体中文](zh-CN/bluetooth-bond-reconnect.md)

# Bonded iPhone fails to reconnect from READY

## Historical scope and current integration

This document preserves the bond-reconnect, H5 and key-persistence investigations in reverse chronological order. "Latest", "this round" and test results below refer to each section's historical version, not renewed physical acceptance of the maintained firmware. The concrete Bluetooth adapter, pairing storage and iAP2 owner now live in the Luban-Lite host under `application/rt-thread/rtcarplay/`; see [host migration](luban-lite-migration.md). Historical commands and old directory names remain for traceability, with maintained validation commands listed separately. Documentation work does not change pairing, authentication or reconnect policy.

## Latest historical board feedback: old bond reconnects; H5 fixes follow

The latest user logs for this investigation confirm that the previous READY fix took effect: the BR bond was restored, active paging began after two seconds, and both connections authenticated with the saved link key (status=0x00). On the first connection the phone ended ACL with `0x14` (remote resource shortage); the retry after 30 seconds established RFCOMM and completed MFi, Wi-Fi, the authenticated iAP2 tunnel and first-video-frame stage=5. This connection must no longer be described as an invalid pairing key. The BR address `DC:B5:4F:7B:E3:20` differing from the private Wi-Fi address is normal.

The logs also contain H5 sequence retries and `HOST_NUM_COMPLETED_PACKETS status 0x12`. SDK commit `351d2ec9d` fixes two independent defects:

* UART whole-frame transmission lacked mutual exclusion and did not respect the transmit window; retransmission moved queues and rolled sequence numbers back concurrently with ACK processing. Out-of-window ACKs could also underflow a counter into an infinite loop. The fix serializes whole frames, retransmits in place with the original sequence, validates ACKs within bounds and wakes window waiters. SLIP escape sequences support fragmented arrival.
* Receive buffers defaulted to index=0/handle=0. Duplicate or incomplete H5 ACL frames discarded before Host delivery could therefore report receive-completion credits for connection zero through their free callback. Allocation now uses an invalid index; frames not accepted by the Host generate no new credit, and connection handles are checked to prevent index-reuse confusion.

Host tests compile the actual H5 implementation, ACL allocator and completion callback, reproducing the incorrect completion count before the fix. Afterward, concurrent ACK/retransmission, 200 concurrent UART frames, window handling, sequence wrap, duplicate/incomplete ACL and encode/free balance pass under ASan/UBSan. Existing reconnect, bond, LVGL, handoff and capabilities regressions and the D213 SDK build pass. This does not establish physical validation of the new fixes, or prove that these H5 defects caused the phone's initial 0x14.

For the maintained integration location, see [host migration](luban-lite-migration.md). Image, SHA-256, build commands and the full retest checklist were historically recorded in section 12 of `../../luban-lite-jc-carplay-oss/doc/rt-carplay-build.md`; that cross-repository relative link is no longer maintained by this SDK. Preserve the bond, cold boot and select Wireless, then verify this startup marker:

```text
H5 transport: serialized frames, bounded window, in-place retransmission
```

Save at least 90 seconds after READY and logs before/after Wi-Fi handoff. Run `carplay_bt status`, `carplay_iap2 status` and `carplay_status`. Check whether automatic retry reaches stage=5, whether cumulative `H5 sequence retry` keeps growing, and whether `HOST_NUM_COMPLETED_PACKETS 0x12` remains. Sequence retries are logged at cumulative counts 1, 2, 4, 8…; disappearance of an old log string alone does not prove resolution. RFCOMM -110 during intentional handoff disconnect is not independently a failure.

In this log, `/info` bluetoothIDs, identity and TXT/plist Features match, but Audio SETUP is still absent. These fixes do not establish working music output. Further audio diagnosis still needs to determine whether type=100/101/102 appears, rather than attributing the issue to AAC, Mixer or sound0.

## READY automatic reconnect and the subsequent "manual Connect produces no logs" fix

As product behavior, a paired phone allowed to use wireless CarPlay should reconnect automatically when Bluetooth/Wi-Fi are enabled on both sides and vehicle policy allows connection. A stored stack bond does not mean the application initiates reconnect. Previously `rfcomm_accept()` waited indefinitely for the phone and never called `bt_conn_create_br()` to page the bonded address, so automatic connection could not be promised.

The following behavior was added in that round:

* After SDP/RFCOMM registration, HCI Read Scan Enable reads controller state back. READY is published only when page and inquiry scan are both enabled (0x03). A mismatch between Host cache and controller is repaired through public off/on interfaces and checked again; failure returns an error.
* Only while the wireless iAP2 owner is idle, page restored BR bonds after an initial two-second delay. Cycle through multiple bonds, retry ordinary failures after 30 seconds, and wait another two seconds when resuming from pause. No additional inquiry discovery is performed.
* After ACL succeeds, request L2 encryption using the saved link key without forced re-pairing; retain any stronger authentication properties already attached to the key. Then wait for the phone to connect to the registered iAP2 RFCOMM service. Do not guess the phone's RFCOMM channel or equate ACL success with CarPlay success.
* If no RFCOMM link forms within 20 seconds, cancel this owner's pending outbound connection and release references. Do not page concurrently with an existing inbound ACL or pairing interaction, and do not cancel an inbound connection that wins the race.
* Replace the owner's accept with bounded 500 ms polling so it continues checking Receiver, AirPlay event and iAP2 tunnel status. Pause active paging after Wi-Fi handoff; an already connected RFCOMM link for the same phone is unaffected by the reconnect timer. Keep the original blocking accept interface for compatibility.
* Log incoming Classic pages at application level, independent of SDK info-log filtering. Add scan, radio error, attempt count, active ACL count and pairing state to `carplay_bt status`.

This round also fixes synchronous HCI concurrency in the SDK. All commands previously shared one static semaphore, so concurrent scan queries and Bluetooth authentication could wake each other's waiters. Each command-pool slot now has its own wait object, protected by the command-buffer reference lifetime, zero-initialized and reused to avoid leaking RTOS objects per call. In that SDK ELF the static array occupies 624 bytes; runtime semaphore objects are additional. Board CPU/radio cost and reconnect time were not measured. No worker thread was added here.

Validation commands added at the time:

```sh
make check-lubanlite-reconnect
cd ../luban-lite-jc-carplay-oss
python3 packages/third-party/zephyr-bluetooth/tests/run_hci_sync.py
```

The first extracts actual adapter functions and covers readback/repair/failure, stop races, bounded accept, paging rotation/throttling, encryption, Wi-Fi pause, inbound races, reference balance and clock wrap. The second executes actual synchronous HCI command functions, completing two threads' commands in reverse order to verify result, error and reference isolation, then reuses slots three times. The pre-fix SDK fails the independent-wait-object check. These ASan/UBSan Host tests do not prove that iOS automatically opens RFCOMM after ACL establishment.

New startup and automatic-attempt markers:

```text
READY radio address=... scan=0x03 (page+inquiry)
bond reconnect enabled: first page in 2s, retry 30s; Wi-Fi handoff pauses paging
reconnect paging bonded peer=... scan=0x03
ACL connected peer=...
reconnect ACL established; security request rc=0; awaiting phone RFCOMM
```

Validate with the old bond preserved and correlate with `carplay_bt status` and `carplay_iap2 status` below:

| Result | Interpretation boundary |
| --- | --- |
| READY radio / bond reconnect enabled markers absent | Does not establish that this code is running, or service startup has not succeeded; first verify firmware and complete startup logs. |
| bonds=0 and attempts does not grow | The stack has no restored BR bond; file existence alone is insufficient for active reconnect. |
| scan=unknown or nonzero radio error | HCI query/configuration did not produce a valid result; investigate controller and H5 first. |
| active_ACL nonzero without a new page | An ACL is connected or closing; this active policy yields. Preserve surrounding ACL/HCI logs. |
| reconnect paging followed by HCI 0x04 | Page Timeout, before link-key verification; inspect phone connectability, address, radio and coexistence. |
| saved-key reply followed by HCI 0x05/0x06 | Old-key authentication was reached; investigate mismatched bonds. |
| ACL/security succeeds but awaiting phone RFCOMM persists | Profile establishment is still missing, so automatic CarPlay is not established; retain phone model, iOS version and full SDP/RFCOMM logs. |
| Manual Connect has no incoming Classic page but active paging has a result | Use the outbound result to narrow the investigation; HCI scan readback proves configuration values, not over-the-air operation. |

The earlier key-update/persistence investigation follows. Its candidate causes must not be applied directly to the newer "no events on connection" symptom.

The logs supplied for that earlier round end at RFCOMM channel 6 listening and stage=1, with no ACL request, authentication or RFCOMM connection result. They prove service startup, not that the phone requested a connection or that both link keys match. Forget Device removes the phone's key and service cache and triggers discovery/SSP again; successful re-pairing alone does not prove a key-file fault.

## Inspected paths

`rtcp_bt_lubanlite_enable()` loads settings after `bt_enable()`, then registers/publishes RFCOMM and SDP; this matches the SDK identity-initialization order. The controller supplies the local BR address, and iAP2 uses the same address. Reconnect does not change the address, disable authentication or lower security.

`/data/bt_settings.bin` contains several record types. A phone's Classic bond requires `bt/link_key/<12-address-digits><type>` (12 address digits followed by the type); `bt/id`, name or LE/CCC records alone are insufficient. The in-memory key pool restored by SDK `keys_br.c` is the lookup source for HCI Link Key Request. The initial pairing_complete callback does not run again on every successful reconnect, so its absence is not evidence of reconnect authentication failure. Observe encryption through security_changed.

## Changes in that round

* Fix SDK `bt_hci_link_key_notify()` handling of Changed Combination Key (HCI type `0x06`). Previously it entered the unsupported branch, cleared flags, zeroed the 16-byte key and persisted it; the phone retained the real updated key and the next reconnect mismatched. It now inherits authentication/Secure Connections properties from the existing bond, updates and saves the new key, and rejects updates without an existing bond or when SC-only requirements are unmet. Other unsupported key types no longer overwrite the old key. Local tests reproduce failure before and success after the fix. No board log proves this phone sent `0x06`, so this is a confirmed faulty code path, not a confirmed sole hardware root cause.
* Do not publish services or enter READY after settings load failure; allow retry after the error is resolved.
* Return an error when a saved BR record has no handler, and expose restore/commit failures.
* Fail settings replay when the RAM table is full and retain the original file, preventing later compaction from permanently deleting records that were not loaded. The table limit is 16 records, not 16 phones.
* Check full BR-record length before restoring it. Nonempty incompatible records could previously be cleared or accepted incorrectly after read_cb truncation; retain them and report an error instead. Zero-length deletion remains supported. Do not guess an old format or generate replacement keys.
* Report the actual restored BR bond count at startup; add bond count, storage and HCI/security errors to `carplay_bt status`. Add SDK incoming-connection, saved-key-hit and authentication-result logs, plus application ACL-established and encryption-level logs. Never print link-key contents.

These defects are confirmed by code review, but none can yet be identified as this phone's actual root cause. If the existing phone and board keys already differ, preserving the record does not repair their state automatically.

## Automated validation

```sh
make check-lubanlite-bonds SDK_ROOT=../luban-lite-jc-carplay-oss
make -j8 check
make check-lubanlite-demo check-lubanlite-handoff check-lubanlite-capabilities \
  SDK_ROOT=../luban-lite-jc-carplay-oss
cd ../luban-lite-jc-carplay-oss
scons -j8
```

The bond test uses the actual settings backend and SDK `keys_br.c`, address encoding/decoding and `bt_hci_link_key_req()` / `bt_hci_link_key_notify()`, with Host mocks at controller/RTOS boundaries and ASan/UBSan enabled. It covers address/flags/key stability across three cold starts and HCI replies using the original key, missing/weak-key rejection, long/short legacy-record preservation with errors, deletion, full-table protection, I/O, positive/negative errno, backup recovery and reentrant callbacks. Additional cases cover initial P256 key generation, reboot after a `0x06` update and replies with the new key, unknown-type rejection without damaging the old bond, and update rejection without a prior bond. Actual SDK handlers run in normal and SC-only configurations with reference-balance checks. Host success does not validate real RTL8733, iOS or power-loss durability.

In the [Linux HCI event](https://github.com/torvalds/linux/blob/master/net/bluetooth/hci_event.c) `conn_set_key()` reference, Changed Combination Key does not replace the original connection key type. The SDK's Zephyr upstream `ssp.c` also had the same unhandled branch at the time, so upstream origin alone does not establish support for the event. This change follows HCI Link Key Notification update semantics and adds no Apple private authentication material.

## Physical retest with pairing preserved

First back up `/data/bt_settings.bin`; preserve the data partition and the phone's pairing when flashing. Do not Forget Device first, because that removes the failure state being investigated. The file contains keys; retain a local backup without uploading its contents.

Capture logs from startup, tap Connect for the paired device on the iPhone at READY, then execute:

```text
carplay_bt status
carplay_iap2 status
carplay_status
```

Normal key stages follow; phone behavior can slightly change event order:

```text
BR bond restored for <phone>
Bluetooth settings loaded: BR bonds=1/4 storage_error=0
BR incoming connection: peer=<phone> type=0x01
ACL connected peer=<phone>
BR reconnect: replying with saved link key for <phone>
BR authentication complete: status=0x00 handle=...
ACL security level=... error=0
RFCOMM DLC connected
[iap2] link up ...
```

| Last observed result | Next interpretation |
| --- | --- |
| File exists but BR bonds=0 | No valid Classic bond in the file; inspect save/restore logs rather than treating existence as success. |
| restore size mismatch / table full / handler missing | This round explicitly prevents READY; preserve the file and resolve storage layout, capacity or linkage as indicated. |
| No incoming connection after READY | Link-key authentication has not started; inspect whether the phone initiates Classic connection, page scan/radio and system cache. Do not attribute this to AAC or audio output. |
| incoming connection without ACL connected | Inspect ACL connection failed, SDK rejection and resource errors. |
| no saved link key | Compare incoming peer with restored addresses and confirm a matching bond on both sides. |
| saved key insufficient | Security flags/required level are incompatible; do not lower security to force connection. |
| Authentication status=0x05 or 0x06 after saved key reply | Authentication Failure / PIN or Key Missing respectively; compare both bonds. The code alone does not identify which side is damaged. |
| Authentication succeeds without RFCOMM | Inspect the phone's cached SDP/RFCOMM channel, SDK resources and connection lifetime. |
| RFCOMM/iAP2 established without picture | Inspect MFi, Wi-Fi configuration, CarPlay invitation and AirPlay; Bluetooth pairing has passed. |

Test at least reconnect after board cold boot, toggling the phone's Bluetooth, reconnect after leaving a Wi-Fi handoff, repeated connections of the same phone and rotation among multiple bonded phones. Only completed physical tests can establish resolution of the old iPhone READY reconnect issue.

## Maintained host validation entry points

Run these commands from the maintained Luban-Lite repository root. They replace the earlier historical board-test entry points inside the SDK; protocol SDK `make -j8 check` remains independent. These are reproducible entry points, not a claim that this translation reran their tests or physical procedures.

```sh
make -C application/rt-thread/rtcarplay check-lubanlite-reconnect check-lubanlite-bonds \
  check-lubanlite-demo check-lubanlite-handoff check-lubanlite-capabilities
python3 packages/third-party/zephyr-bluetooth/tests/run_hci_sync.py
make -C packages/third-party/rt-carplay -j8 check
```
