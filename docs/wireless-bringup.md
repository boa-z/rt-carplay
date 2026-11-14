[English](wireless-bringup.md) | [简体中文](zh-CN/wireless-bringup.md)

# D213 wireless CarPlay and new-iPhone pairing validation

## Historical scope and current integration

This document preserves the v27 through v30 pairing, discovery, display and handoff investigations. "This round", "current" and "not yet physically tested" refer to each historical baseline; read subsequent results in section order rather than treating them as renewed physical acceptance of the maintained image. Concrete connection workers, Bluetooth/Wi-Fi/MFi adapters and Launcher now live in the Luban-Lite host under `application/rt-thread/rtcarplay/`, while the SDK retains protocol and Receiver boundaries; see [host migration](luban-lite-migration.md). The dual-mode flow below is the historical manual selector, not the simultaneous USB/wireless standby required by the subsequent Launcher V2 task. Historical test commands and board procedures remain; run maintained actual-source host regressions from the repository root with `make -C application/rt-thread/rtcarplay -j8 check` and independent protocol regressions with `make -C packages/third-party/rt-carplay -j8 check`. Later feedback on bonded Bluetooth reconnect is in [bond reconnect](bluetooth-bond-reconnect.md); audio status is in [audio debug handoff](audio-debug-handoff.md). Documentation updates do not change frozen audio behavior.

For the subsequent investigation of a bonded phone failing to reconnect at READY, and retest steps preserving the old pairing, see [Bluetooth bond reconnect](bluetooth-bond-reconnect.md).

These changes connect the dual-mode LVGL startup flow and fix identified gaps in Bluetooth interaction, key persistence and wireless-address publication. Host tests and a complete SDK build do not establish stable physical operation with a new iPhone; at this stage there were no Bluetooth-authentication or Wi-Fi-handoff logs from this firmware on the board.

## v28: fix v27 first boot being misclassified as storage failure

The v27 board log reports `settings_subsys_init failed (err -5)` in `bt_settings_init()`, followed by `bt_enable()` exiting before H5 controller startup or phone SSP pairing. The actual chain is SDK `hci_core.c::bt_enable()` → `bt_settings_init()` → application `settings_subsys_init()` → `file_load()`. SDK `dfs_posix.c::open()` passes a negative lower-level error directly to `rt_set_errno()`; newlib `_open_r()` does not change its sign. A missing `bt_settings.bin` after flashing therefore produces `errno=-ENOENT`. The v27 `errno != ENOENT` check treats it as a disk-read failure and maps it to `-EIO` (-5).

v28 recognizes positive and negative `ENOENT` for both primary and backup. Other errors still fail and log the path plus raw errno. Persistence stays enabled and storage failures are not universally treated as first boot. The SDK also clears `BT_DEV_ENABLE` on this early failure path so retry does not incorrectly return `-EALREADY`; HCI resources are not open yet, so this is not forced reset of a running controller. The same actual-backend test simulates POSIX positive errno and SDK negative errno, covering initially missing files, backup recovery, real I/O failure blocking initialization and retry retaining identity after recovery. On the board, first look for `settings storage ready`, then controller startup. This fix does not establish physical SSP, Wi-Fi handoff or resolution of earlier Wi-Fi reserved-page warnings. SDK `python3 packages/third-party/zephyr-bluetooth/tests/run_enable_settings.py` isolates actual `bt_enable()`, verifying that settings failure does not open HCI, recovery permits retry and duplicate calls after genuine enable still return `-EALREADY`.

## v29: AP association and mDNS replies, but no CarPlay picture

New board logs include completed WPA2 four-way handshake and mDNS replies. AP association and some UDP handling are working, but these alone do not prove RFCOMM iAP2, MFi or a successful CarPlay session invitation. Immediate reassociation after `OnDisassoc reason=8` does not identify the reason for leaving the network.

Code review confirms that candidate addresses for Control unicast queries came only from an established RTSP connection. Before RTSP, even a phone querying this accessory's AirPlay service therefore still produced repeated `unicast=none`, preventing the existing unicast-discovery workaround for AP multicast problems. This is a discovery gap, not a confirmed sole cause of this phone's activation failure.

v29 extracts the source after the responder successfully parses a query for local records. The application accepts only nonlocal, non-network/non-broadcast IPv4 candidates in the AP's /24, matching this SDK DHCP's fixed `255.255.255.0`. A candidate is used only to query `_carplay-ctrl` at port 5353; it does not infer a service port, fabricate DNS-SD or directly send activation requests. Control connect-out still requires a Browser-resolved SRV, and an actual RTSP IPv4 peer takes priority. Unrelated queries, responses, malformed packets and self-loopback do not supply candidates.

The wireless service also retains counts for MFi success, sent Wi-Fi configuration, Availability, Invite, reinvite and link ACKs. Normal RFCOMM closure during Wi-Fi handoff does not clear them; restarting the wireless service does. Invite means successful send-path execution, not phone acceptance; link ACK confirms iAP2 link receipt, not successful CarPlay activation.

Capture logs before selecting Wireless and run these commands when stuck:

```text
carplay_iap2 status
carplay_bt status
carplay_ctrl status
carplay_status
```

* `MFi_completed=0`: investigate RFCOMM/iAP2/MFi first; a Bluetooth-connected label on the phone is insufficient.
* MFi completed but `availability=0`: no phone CarPlay availability has arrived; retain early identification/subscription logs instead of changing Wi-Fi passwords experimentally.
* `availability>0` and `invites=0`: check invitation prerequisites/parameters and retain iAP2 logs.
* `invites>0` and `link_acks=0`: check queued invitations and RFCOMM disconnection.
* ACK present without RTSP: inspect Control discovery, phone activation/user consent and network handoff. New logs should show `source=mdns_query` followed by `unicast_rc`; connect-out still does not occur without SRV. Retain packets, updates and endpoints from `carplay_ctrl status`.
* RTSP has entered pairing: investigate that connection's requests/errors rather than remaining at Bluetooth status.

Comparison with DiPlay's 0x4301 layout and LIVI's mDNS socket settings found no evidence requiring local 0x4301 encoding changes. Actual SDK `udp_new()` already initializes multicast TTL to `UDP_TTL`, with `UDP_TTL=255` in this configuration. v29 therefore changes neither TTL, broadcast capability nor advertised protocol values. Host regressions verify candidate extraction and invitation-ACK events; whether this takes the board into RTSP remained pending.

## Confirmed code defects and fixes

| Issue | Actual source and mechanism | Fix |
| --- | --- | --- |
| Pairing lacks display/confirmation interaction | `application/rt-thread/rtcarplay/platform/rtcp_bt_lubanlite.c` did not register authentication-interaction callbacks; the IO-capability branch in SDK `subsys/bluetooth/host/classic/ssp.c` selected `BT_IO_NO_INPUT_OUTPUT` | Launcher builds register display, compare and consent callbacks; the user confirms numeric comparison. Other manual builds retain the old IO mode. |
| Discoverable before service registration | `enable()` enabled inquiry/page scan before `rfcomm_listen()` registered RFCOMM/SDP | Publish after service registration and prohibit new RFCOMM accept during stop. |
| Default configuration does not save Bluetooth keys | Application `rtcp_bt_settings.c` existed but settings were disabled and D213 linker script did not retain static handlers | Enable `RTCARPLAY_BT_PERSISTENCE`, SDK settings macros and handler section by default; restore keys before publishing service. |
| Key pool can be full after changing phones | Original SDK `.config` had `LPKG_BT_MAX_PAIRED=1` without `KEYS_OVERWRITE_OLDEST`; `classic/keys_br.c::bt_keys_get_link_key()` returned NULL when full, so `classic/ssp.c::bt_hci_link_key_notify()` could not save the new phone's link key | Demo defconfig allows four phones; UI shows used/capacity and requires the user to forget an old bond when full, without silent eviction. |
| Full-pool restore lacks NULL check | SDK `classic/keys_br.c::link_key_set()` performed `memcpy(link_key->storage_start, ...)` without checking allocation failure when restoring a valid record | Return `-ENOMEM` when full; the settings backend propagates handler errors instead of crashing or falsely reporting restoration. |
| Storage failures treated as success | Old backend ignored append failure, removed primary before compaction and let a corrupt suffix block replay of later records | Return I/O errors, retain backup and repair a valid prefix before append; show persistence errors in the UI. |
| Wireless handoff can advertise stale IP | `start_wifi()` read the address before `dhcpd_start()` changed the AP IP | Wait for AP netif and DHCP startup, then read the final address; fail when DHCP cannot start. |
| AP/STA or USB default-route confusion | Wi-Fi netif was guessed from names/nonzero addresses; mDNS joined multicast on the default interface | Find AP through WLAN device binding, and explicitly select AP address for multicast membership/transmission. |
| Asynchronous start success treated as service readiness | `carplay_iap2_start()` only created a thread; Bluetooth could fail later | Publish running/listening/connected/identified/authenticated/error separately and show stages from these states. |

These are confirmed implementation gaps. Device logs are still needed to identify which caused a particular new iPhone's failure. An IO-capability change alone does not prove compatibility with every iOS version.

## Lifecycle and data boundaries

The historical launcher has one worker managing USB and wireless. Before switching, it closes the old USB session, waits for wireless iAP2 to release borrowed Receiver strings/MFi provider, then stops Receiver. Stop during controller startup still disables discoverability on exit. A pairing request holds a `bt_conn` reference; UI submits only token-bound confirmation, and the worker calls stack APIs. Stale clicks are not applied to another phone.

Discovery and sessions stop before pairing management and restart only after closing the management view. Bluetooth Forget first disconnects and waits for existing BR ACLs; timeout fails the operation. It then calls `bt_br_unpair(NULL)` to clear RAM BR keys and compacts settings while removing only `bt/link_key/*`, retaining local records such as `bt/id`. USB/AirPlay files retain their existing whitelist handling. This clears only the board and cannot remove the phone's pairing records.

The settings backend covers ordinary reboot recovery, truncated file suffix and backup recovery; it does not prove transactional persistence under sudden NAND/FAT power loss. Storage errors are visible so a RAM-only bond is not presented as durable. Complete-image flashing erases `/data`; first pairing after flashing is not a reconnect test.

## Board validation order

1. After flashing, first forget the old `rt-carplay` in iPhone Bluetooth settings. Boot defaults to USB; select Wireless and wait for AP address and Bluetooth listening.
2. Select `rt-carplay` in iPhone Bluetooth settings. If six digits appear, compare and confirm on both sides. Record model, iOS version, failing stage, `BT last HCI/security` and storage status. Bluetooth success, MFi success and thread creation do not mean CarPlay has loaded.
3. Check Bluetooth → iAP2 identification → MFi → Wi-Fi → AirPlay event → first frame in order. The phone can normally close Bluetooth RFCOMM during Wi-Fi handoff; check subsequent AirPlay establishment. `HCI 0x13` means remote closure only, not necessarily pairing failure.
4. Without reflashing, reboot normally and test reuse of the same phone's saved bond. Repeat at least five times, distinguishing the system's "Use CarPlay" prompt from new Bluetooth pairing.
5. Open Pair files from the waiting view and run Forget Bluetooth pairs; also forget the device on the phone. Re-pair and confirm local identity and USB/AirPlay files were preserved.
6. Test USB→wireless, wireless→USB, accessory-side pairing cancellation, phone cancellation, Wi-Fi disconnection and phone connection attempts while management is open. A switch during blocking USB Trust waits for that operation to return.
7. Apply the same tests to at least one previously working old iPhone and one failure-prone new iPhone. Compare success rates using five independent connections per phone with stages/errors, not one successful screenshot.

## Narrowing errors

* Bluetooth authentication callback failure: analyze `security_reason`, old bonds on both sides, numeric-confirmation timeout, full key pool and storage failures; retain surrounding ACL/HCI results.
* RFCOMM exists but iAP2 identification/MFi fails: investigate iAP2/MFi rather than SSP.
* MFi succeeds without AirPlay: verify final AP IP, phone association/address acquisition, Wi-Fi configuration exchange and mDNS. Failure with correct addresses requires board network logs or capture.
* Picture exists but FPS is low: investigate subsequent video supply. This version does not change decoding, advertised FPS, TCP parameters or USB/HCD behavior and cannot establish completed 60 FPS optimization.

## Host validation

`make -j8 check` runs protocol regressions. Historical `make check-lubanlite-demo SDK_ROOT=...` drives the actual launcher worker, covering switching, asynchronous failure, first frame, Bluetooth→Wi-Fi handoff, management shutdown and timeouts. `make check-lubanlite-bonds SDK_ROOT=...` uses the actual settings backend for restore, reentrant callbacks, compaction, corrupt-suffix repair, I/O propagation, backup recovery, deletion and identity retention. `make check-rtthread-multicast` uses the actual RT-Thread socket port and Host sockets to verify explicit multicast egress, reset and failure returns. None runs the physical Bluetooth controller or renders LVGL. SDK `python3 packages/third-party/zephyr-bluetooth/tests/run_br_keys.py` compiles actual `keys_br.c`, checking one/four slots, errors when restoring into a full pool and slot release after explicit deletion. Only external dependencies are substituted; SSP/controller are not run.

## v30: picture works but the iPhone remains Connecting

v29 physical logs establish wireless video for that baseline: approximately 21–25 FPS during motion, decode about 9–10 ms and presentation about 7–8 ms. Later low FPS on a static view, with near-zero assembly and increasing sender_gap, cannot alone be called a receive stall.

However, confirmed implementation gaps remain in connection completion:

- Initial SETUP advertised `iAPChannel` in `enabledFeatures` but did not accept `type=130 (0x82)`, returning empty streams to the phone.
- `modesChanged` and `disableBluetooth` were both treated as unknown commands and returned 405.
- The RFCOMM bootstrap lacked a safe AirPlay-triggered handoff exit.

This establishes an implementation state where video started but runtime iAP2/handoff had not completed. It is the leading candidate for Connecting not ending; disappearance of the popup still requires a physical test, which Host tests/builds cannot replace.

v30 follows DiPlay `CarPlayMediaEngine.kt`, `IapTunnel.kt`, `AirPlayIapTunnelStream.kt`, `Iap2LinkEngine.kt` and Controller handoff gates:

1. Accept only iAP UUID `E9459FD0-BCAD-4C45-820F-1E72447EF2F2` with a valid seed. Respond with `type=130 / streamID=1 / dataPort`, echoing streamConnectionID when present. Only a standalone type-130 SETUP is supported at this stage; mixed requests and other DataStream UUIDs remain rejected.
2. Derive output keys using **seed**, not streamConnectionID, with `DataStream-Salt<seed>`; accept only the current RTSP peer. After ChaCha20-Poly1305 decryption, parse the 32-byte APTransportPackage header size and `comm` type and deliver link bytes to an independent iAP2 session. TCP/encrypted-frame fragmentation is handled incrementally without allocation based on package size.
3. Send complete outbound iAP2 link frames using encrypted event `/command` `iAPSendMessage`. Use 1024-byte encrypted fragments so certificates are not constrained by the old HID path's 512-byte buffer. The TCP tunnel uses control-session version 2 / zero-ACK synchronization and respects peer synchronization parameters for no-retransmit mode. USB/RFCOMM synchronization settings remain unchanged.
4. Acknowledge `modesChanged` as a notification without inferring local audio-resource actions. Validate deviceID for `disableBluetooth` and only queue the request. Release the captured bootstrap ACL only after tunnel MFi and Identification complete; preserve pairing and the RFCOMM listener. Generation changes prevent releasing a newly connected phone. First frame or event TCP connected cannot substitute for the handoff gate.
5. The RTSP owner serializes endpoint, cipher and iAP2 state, cleaning up on error/TEARDOWN/stop without reusing consumed seeds. Seed-use records are shared with screen/audio output keys to avoid identical key/nonce use across endpoints. Complete MFi register transactions are also locked to prevent interleaved bootstrap/tunnel certificate/signature access.

This round does not implement other DataStreams, audio playhead feedback or generic success for unknown `/command`. Remaining protocol gaps such as `/feedback` are separate. Here "tunnel authenticated" means phone-confirmed MFi success over iAP2, not merely decryption of a packet.

Board steps, keeping the same image for normal reboot/reconnect:

1. Flash v30 and pair/start using the normal wireless flow. Save logs from Bluetooth connection to at least 30 seconds after first frame, recording when the phone's Connecting popup appears/disappears.
2. Expect `iap_setup=ready`, then `Wi-Fi tunnel iAP2 starting`, then tunnel Identification and MFi. `disableBluetooth accepted` may appear before or after these.
3. The only new successful-handoff completion log is `wireless handoff complete: tunnel authenticated/identified; Bluetooth released`. `carplay_iap2 status` should show `tunnel=1 identified=1 authenticated=1 handoff_complete=1`. Normal RFCOMM/ACL closure must not stop video or AP.
4. Operate map/list continuously for 60 seconds to validate picture/touch, then perform ordinary disconnect/reconnect, normal reboot and USB/Wireless mode switching once each. Check for leftover tunnels or erroneous disconnection of another ACL.
5. If `iap_setup=ready` is not followed by starting, inspect whether the phone connects to dataPort. If starting is not followed by readiness, retain the specific iAP2/MFi error. If the phone remains Connecting after `handoff_complete=1`, the hypothesis that missing tunnel/handoff sufficiently explains the popup is falsified; investigate other phone requirements for session completion without calling the patch physically proven.

New Host validation covers encrypted/network fragmentation, peer binding, bad tags, invalid package size, certificate-sized multi-frame event transmission, TCP zero-ACK, UINT64_MAX seed, SETUP retry, streamID-selective TEARDOWN, consumed-key reuse rejection after closure and malformed/unknown control commands.

Also run historical `make check-lubanlite-handoff`: it isolates actual BT-adapter handoff functions and actual owner tick/queue functions, checking authentication/identification gates, asynchronous HCI closure, new-session generation, three-second close timeout, HCI failure, an already departed peer and bt_conn reference balance. handoff_complete is set only after ACL closure is confirmed; sending the HCI disconnect command is not completion.
