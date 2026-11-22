[English](code-formatting.md) | [简体中文](zh-CN/code-formatting.md)

# C and C++ formatting

The SDK and the Luban-Lite CarPlay application use the same RT-Thread-derived clang-format configuration: LLVM base, four spaces, no tabs, Allman-style brace wrapping, right-aligned pointer stars and preserved include order. The source is [RT-Thread configuration at 7c974b59fdc258afbdb19c378f250bcd95511537](https://github.com/RT-Thread/rt-thread/blob/7c974b59fdc258afbdb19c378f250bcd95511537/.clang-format). Two policies differ: `InsertBraces: false` prevents adding control-flow braces, and `LineEnding: LF` gives Windows and Linux the same output. Include sorting, comment reflow and structural rewrites remain disabled; the upstream unlimited column width is retained. Choosing this style adds no RT-Thread dependency to the portable SDK.

Use clang-format 23.1.1 to reproduce the bulk-format commit and CI. Install it with `python -m pip install clang-format==23.1.1`. From the SDK root, run:

```sh
python3 scripts/format-code.py
python3 scripts/format-code.py --check
```

In the Luban-Lite checkout, use:

```sh
python3 packages/third-party/rt-carplay/scripts/format-code.py --root application/rt-thread/rtcarplay
python3 packages/third-party/rt-carplay/scripts/format-code.py --root application/rt-thread/rtcarplay --check
```

The helper enumerates tracked C/C++ files within the selected scope. `.clang-format-ignore` uses simple relative glob patterns. Vendored `third_party/` implementations, generated protocol/audio byte fixtures, generated font subsets and generated upstream adapters retain their source-generator or upstream form. Handwritten SDK core, public/internal headers, runtime, ports, examples, application code and existing test sources are included. New untracked files should be formatted with clang-format directly or staged before running the helper.

Keep functional commits separate from bulk formatting. CI checks both scopes without rewriting them. Firmware compilation, existing Host regressions and actual LVGL previews validate the formatted code separately from the formatting check; none establishes board or iPhone acceptance.

## API comments and documentation

Use English Doxygen comments for public declarations. Describe ownership, pointer validity, thread and callback constraints, units, significant defaults, side effects and failure semantics where they affect callers. Explain wire IDs, compatibility branches and resource ordering in the implementation when the surrounding code cannot explain them.

```sh
make check-docs
make docs
```

`make check-docs` checks documented public functions and callback types, local links and required language pairs with standard-library Python. `make docs` uses Doxygen and writes HTML below `build/doxygen`. Keep generated files out of Git. Fix Doxygen syntax warnings in public headers when editing the associated API.

Review the paired user and developer guides when changing a public API, configuration option, protocol capability or observable behavior. Update the [Receiver API](receiver-api.md) for contracts and the [capability matrix](classic-capability-audit.md) for validation evidence. Internal refactors, comment corrections and test-only changes do not require changes to the README.
