# Portable audio dependencies

Sources are unmodified, with only the build's required C/header subset copied.
No SDK binary libraries are used.

* FAAD2 2.11.2, `673a22a3c7c33e96e2ff7aae7c4d2bc190dfbf92`,
  https://github.com/knik0/faad2. GPL-2.0-or-later; compatible with this
  GPL-3.0-or-later program. See `faad2/COPYING` and file notices.
  **Code from FAAD2 is copyright (c) Nero AG, www.nero.com**.
  This build selects fixed point, LC only, no SBR/PS. No AAC-ELD support.
* Opus 1.5.2, `ddbe48383984d56acd9e1ab6a090c54ca6b735a6`,
  https://github.com/xiph/opus. BSD 3 clause, see `opus/COPYING`.
  Fixed point CELT + SILK encoder and decoder, no architecture assembly,
  float API, DRED or runtime CPU dispatch. Sources selected from upstream
  OPUS_SOURCES, CELT_SOURCES, SILK_SOURCES, SILK_SOURCES_FIXED.

`sources.json` records the compiled upstream files. Files retain upstream
copyrights. Codec licensing does not confer patent rights. The build makes
no assertion about deployment-specific patent requirements.
