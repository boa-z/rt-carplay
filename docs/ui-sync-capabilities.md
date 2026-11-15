[English](ui-sync-capabilities.md) | [简体中文](zh-CN/ui-sync-capabilities.md)

# Experimental single-Main-Display UI Sync negotiation

This opt-in connects the existing UI Sync application processor and copied inventory to explicit metadata/SETUP negotiation. It is a portable protocol experiment, not an official renderer or full Ultra implementation. Panel field names and the empty uiSyncInfo value follow the reviewed LIVI source at 64a05b68358db52ed887169143afd5944fd6f1f7, by Lasse Heitgres (https://github.com/f-io/LIVI); see NOTICE. The D213 profiles leave it disabled.

## Host contract and metadata

The host binds its actual installed display/theme inventory to the processor and sets airplay_info_plist_config_t.ui_sync when building /info. The builder requires that inventory's display UUID, pixel dimensions and physical dimensions to match the Main Display. Default physical sizes are effective values and must match too. No inventory or mismatched geometry returns false and zero output length. The optional bounded heap workspace keeps the extra object offsets off the Classic stack; the complete document must fit the existing 6,144-byte session limit. Vehicle State metadata and audio capabilities can coexist within that bound.

The emitted document contains empty uiSyncInfo and exactly one displayPanels entry: the real panel UID and dimensions, empty properties, and one initialVideoStreams entry referencing the Main Display UUID at origin zero and zIndex zero. displays remains a single Type 110 entry. It adds no virtual cluster, showsInstruments, displayPlugins, Type 111, altScreen or hevcInfo. Host layout/palette/component IDs remain processor constraints; this document does not transfer or validate the theme's assets and does not establish the vehicle plugin/theme relationships required by a complete product.

This SDK uses its existing immutable session /info snapshot: explicitly supplied UI metadata is available before SETUP. LIVI adds uiSyncInfo after accepting the phone offer and supplies displayPanels when Vehicle State exists. The SDK's deliberately narrow single-panel composition and early metadata availability have only synthetic Host coverage; real-phone acceptance is NOT_RUN. Do not describe this experiment as a complete reproduction of LIVI or an official specification.

## Receiver negotiation gate

Set rtcp_receiver_config_t.ui_sync to the borrowed processor and negotiate_ui_sync to true only when the host implements the assets, rendering and presentation behavior it intends to offer. Receiver creation requires a bound inventory and matching /info metadata. Each accepted controller revalidates the refreshed immutable snapshot. Missing/duplicate fields, wrong scalar/container types, mismatched dimensions/UID/UUID, extra panels/displays/streams, nonzero stream placement, invented instrument properties or display plugins are refused by this single-panel gate. The check proves consistency with the supported metadata shape, not asset authenticity, correct rendering or physical actuation.

Keep inventory and its implementation contract immutable while attached to the Receiver. Host snapshot refresh must preserve this contract. A host needing another panel/plugin arrangement requires a separately implemented and tested extension; the gate must not silently accept a broader arrangement. Calls remain serialized on the Receiver owner. Retain the processor until Receiver destruction. Attaching or binding without the explicit negotiation flag does not enable this feature.

Initial SETUP includes uiSync in enabledFeatures only when the explicit gate passed and the phone offered uiSync. Vehicle State has its own independent opt-in. False/false preserves the original Classic SETUP response bytes. Repeated initial SETUP keeps the accepted offer, and each new authenticated controller negotiates again. rtcp_data_stream_status_t.enabled records that accepted negotiation; it does not prove execution on the phone. Registration, offered transport, socket connection, application ACK and presented frame remain separate observations.

## Validation and acceptance

Independent Python plistlib checks decode the generated single-panel document and re-encode alternate dictionary/object layouts. The 65 cases include geometry/type/identity mismatches, missing or extra panels/displays/streams, invented properties/plugins, NUL-suffixed IDs and duplicate relevant fields. Unit tests cover every document truncation and output capacity shortage, unbound/mismatched builder input, default physical sizes, audio/Vehicle coexistence, missing processor/metadata rejection, UI-only Receiver snapshot acceptance and all four independent SETUP feature combinations.

The encrypted --ultra-apps scenario uses the actual Receiver with Pair-Verify, ControlCipher and TCP sockets. It verifies both enabled features, repeated SETUP, UI reset/layout/ACK and refusal/reopen behavior, continued Vehicle exchanges, and a new controller rejected solely because refreshed UI metadata is absent. A later controller without an offer leaves both protocols disabled. The other E2E variants retain Classic and raw UI backend coverage. All inventories/identities are synthetic; no Apple assets or private identity material are used.

The independent LVGL fusion demo does not meet this host rendering contract and therefore remains unregistered/unadvertised. L1 is still PARTIAL for the complete requested protocol scope; L2 physical fusion/input and L3-L5 real-iPhone/Type 111 acceptance remain NOT_RUN. Official assets/runtime, full display/plugin/vehicle composition, presentation clocks, codec/graphics feasibility and official integration/qualification remain required. See the [overall foundation](ultra-foundation.md), [UI state machine](ui-sync.md) and [reference renderer review](livi-theme-review.md).

On 2026-11-15, the complete SDK Make check passed, and CMake with RTCP_SANITIZE enabled passed 66/66 tests. Python independently decoded all four initial SETUP feature combinations as well as the 65 metadata cases. These results are Host evidence only.
