[English](README.md) | [简体中文](README.zh-CN.md)

# 可移植音频依赖

源码未修改，仅复制构建需要的 C／头文件子集。未使用 SDK 二进制库。

* FAAD2 2.11.2，`673a22a3c7c33e96e2ff7aae7c4d2bc190dfbf92`，https://github.com/knik0/faad2。GPL-2.0-or-later；与本 GPL-3.0-or-later 程序兼容。参见 `faad2/COPYING` 及文件声明。**Code from FAAD2 is copyright (c) Nero AG, www.nero.com**。本构建选择定点、仅 LC、无 SBR/PS；不支持 AAC-ELD。
* Opus 1.5.2，`ddbe48383984d56acd9e1ab6a090c54ca6b735a6`，https://github.com/xiph/opus。BSD 3 clause，参见 `opus/COPYING`。定点 CELT + SILK 编码器和解码器，不含架构汇编、float API、DRED 或运行时 CPU 分派。源码选自上游 OPUS_SOURCES、CELT_SOURCES、SILK_SOURCES、SILK_SOURCES_FIXED。

`sources.json` 记录参与编译的上游文件。文件保留上游版权。编解码器许可不授予专利权。本构建不对具体部署的专利要求作出断言。

* SpeexDSP 1.2.1，`1b28a0f61bc31162979e1f26f3981fc3637095c8`，https://github.com/xiph/speexdsp。BSD-3-Clause；参见 `speexdsp/COPYING`。定点 NS/AEC，采用 KISS FFT；生成的 `speexdsp_config_types.h` 使用标准定宽整数类型。RMS AGC 在 Core 中实现；不假定 SpeexDSP 仅支持浮点的 AGC 可用。
