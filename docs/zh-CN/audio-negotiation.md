[English](../audio-negotiation.md) | [简体中文](audio-negotiation.md)

# Classic 音频协商

## 能力声明

D213 宿主在每个控制会话开始前刷新 `/info`。启用蓝牙时，`/info` 和 iAP2 使用同一个本地蓝牙地址。宿主检查编码后的发现 TXT 与 plist 中的 Features 值是否一致，并报告 `info_match=1`。构建未开启 `RT_KPRINTF_USING_LONGLONG` 时，协议日志将 64 位值拆成两个 32 位字输出，以保证后续参数正确对齐。

音频输出可用时声明 AudioPCM，只有编入并提供 AAC-LC 时才声明 AAC。媒体默认提供 44.1 和 48 kHz，也可显式限定采样率。Type 102 使用 media 对应的延迟项。Receiver 保持 Type 103 缓冲音频未声明。

协议对照采用 [catplay 音频默认值](https://github.com/catplay-labs/catplay/blob/66340ad4a0ec8257dafb8ed2d693d1adfc81740b/carplay/catplay_carplay/src/carplay_rx/sink/audio_defaults.rs) 和 [DiPlay AirPlayInfoPlist](https://github.com/shihabal3amri/DiPlay/blob/615b32ce0b40cd0b99ce94ba23a512001553684f/shared/src/main/java/com/shilapi/xcertplay/airplay/AirPlayInfoPlist.kt) 作为参考实现。

## 诊断

分别检查 Audio SETUP、数据报到达、鉴权和实际可听输出。只建立 Type 110 视频和 Type 130 iAP2 的会话可以显示视频，同时音乐仍由手机本机播放。Type 102 AAC SETUP 证明完成了协商，PCM/AAC 台架播放则验证宿主下游链路。按[音频调试指南](audio-debug-handoff.md)定位故障环节。

在 Luban-Lite 根目录运行 `make -C application/rt-thread/rtcarplay check-lubanlite-capabilities`，用实际宿主代码检查能力编码、蓝牙身份和日志格式。
