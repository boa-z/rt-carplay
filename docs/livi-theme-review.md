[English](livi-theme-review.md) | [简体中文](zh-CN/livi-theme-review.md)

# LIVI theme inventory and renderer evidence

Review basis: original LIVI revision `64a05b68358db52ed887169143afd5944fd6f1f7`, by Lasse Heitgres, GPL-3.0-or-later with NOTICE terms. This review adds source/test evidence to the [Ultra foundation report](ultra-foundation.md). It does not establish official Ultra capability for LIVI or this SDK.

## Executed reference tests

On 2026-11-15, Ubuntu 24.04/WSL, Rust/Cargo 1.100.0, the unmodified reference passed both filtered commands below. The source and Cargo.lock remained clean, with build output outside the checkout. [Selected original output](evidence/livi-theme-host-20261115.txt) records names and results.

| Command | Result | Scope |
| --- | --- | --- |
| `cargo test --locked -p livi-core --lib projection::theme_catalog::tests` | 6 passed, 349 filtered | Synthetic package/styles extraction, missing-cluster rejection, theme ID parsing, synthetic catalog JWT payload selection, ZIP directory/deflate helpers and local cache identity. |
| `cargo test --locked -p livi-ui-model --lib dashboard::layout::tests` | 1 passed, 113 filtered | Unknown layout IDs use the generic gauges-over-map dashboard specification. |

These are additional to the prior 179 livi-cp tests, not a rerun or replacement of them. The filters do not call the network fetch path. No Apple theme package, proprietary asset, key or identity was downloaded or included. The tests do not prove catalog/package authenticity, actual package compatibility, rendering fidelity, phone negotiation or frame presentation timing.

## What the source implements

In [theme_catalog.rs](https://github.com/f-io/LIVI/blob/64a05b68358db52ed887169143afd5944fd6f1f7/native/crates/livi-core/src/projection/theme_catalog.rs), `from_package()` recognizes centerConsole and cluster displays. It requires a cluster, collects cluster layout IDs/display names and palette IDs, and retains only zones containing a DCA_Map component. Center-display layouts are deliberately left empty. Palette extraction keeps accentColor darkMode/lightMode strings. This is a useful metadata catalogue; the function does not import or execute all instrument graphics/scripts, general component behavior or presentation clocks.

The fetch path requests catalog metadata, pins the downloaded public root certificate by SHA256, selects an asset/version and uses HTTP byte ranges to read two JSON entries from a ZIP. `catalog_assets()` decodes the JWT payload without an application-level JWT signature check in that function. `inflate()` has no explicit decompressed-output bound there, and the selected ZIP helpers do not establish a complete asset-authentication/validation pipeline. HTTPS transport checks and these metadata helpers must not be presented as a demonstrated official signed-asset renderer. This SDK has not copied that downloader or its asset data.

`ThemeId::parse()` extracts the asset and numeric version; the disk filename is based on those two parts. `cached()` also requires the stored theme's full ID to match the configured string. The cache test checks mismatching full IDs, but does not establish simultaneous cache retention of multiple SDK suffixes sharing one asset/version filename.

In [dashboard/layout.rs](https://github.com/f-io/LIVI/blob/64a05b68358db52ed887169143afd5944fd6f1f7/native/crates/livi-ui-model/src/dashboard/layout.rs), `spec()` selects a few built-in livi layouts and defaults unknown/vendor layout IDs to the generic gauges-over-map specification. `pages()` uses catalogue layout/palette IDs and accent colors. The executed test explicitly verifies the unknown-layout fallback. Correctly selecting a vendor layout ID therefore does not, by itself, prove that the vendor's full local design is rendered.

In `livi-core/src/projection/cluster.rs`, the requested UI Sync layout is generated only when the configured theme ID matches the current catalogue; it maps known zones to their map component IDs. In `livi-cp/src/stack/setup.rs`, uiSync is enabled when the phone offers it and theme display names are present. `stack/control.rs` then emits empty uiSyncInfo and adds displayPanels with Vehicle State information. These are reference behavior and Host-test observations, not a complete official implementation checklist to copy blindly.

## Consequences for rt-carplay and D213

The portable UI Sync model now accepts a copied, bounded inventory and constrains local and phone layout/palette/component choices to it. This supplies a real implementation boundary for future host inventory/capability composition. Synthetic tests identify their inventory as synthetic, and binding it alone emits no capability declaration.

D213's local LVGL speed/gear/battery panel remains a separate application fusion demo. It must not be renamed official Ultra Local UI merely because its selected layout IDs or colors match a catalogue. Official work still requires actual authorized theme/renderer integration, graphics/codec feasibility, presentation synchronization, correct negotiated display/plugin/vehicle relationships, real-iPhone interoperability and the applicable official product process. Unknown requirements remain explicit; no HEVC, Type 111, displayPanels or uiSync is inferred from the local dashboard.
