[English](../audio-debug-handoff.md) | [简体中文](audio-debug-handoff.md)

# 音频调试交接

## 范围与冻结行为

架构迁移不主动修复已有 CarPlay 音频路由故障，保留 `/info` 能力、格式位图、RTSP Audio SETUP、资源所有权、Jitter 调度、Mixer、Direct 选择、Codec、重采样、加密麦克风上行及 AEC 行为。迁移文件或封装 Server 所有权不代表音频协商、播放已经正常。协议调整必须作为独立调试任务，并具备回归证据。

## 已有现象与复现

已有实机报告描述：无线 iAP2 Tunnel 已认证并完成 Handoff，视频可工作，但音乐仍在 iPhone 本机播放。`carplay_status` 报告 `streams=0 datagrams=0`，音频 Sink 收到零个 Packet。记录的 SETUP 候选包含 130/iAP2 与 110/Screen，没有 100、101、102 音频流。`carplay-aud` Worker 已运行或 `sound0` 已打开，不代表 Audio SETUP 已建立或媒体已送达。此前的能力、时钟、Bluetooth 身份修复记录在 `audio-routing.md` 与 `audio-negotiation.md`；这些修复没有证明音频故障已经完整解决。

在参考板上复现时，使用正常 D213 GUI 配置，完成手机授权与既有 USB、Wireless 连接流程，播放持续变化的视频并在手机上启动音乐，然后收集连接日志和下列只读快照。记录准确的固件、Submodule 版本、iOS、手机型号、模式、手机实际音频路由，以及是否涉及通话、Siri、导航流。自动启动页拥有连接时，不要混用手动 Start、Stop 命令。

```text
carplay_status
carplay_audio_path
carplay_volume
carplay_mic
```

比较播放前、播放中与停止后的快照。`carplay_perf on` 启用周期诊断，`carplay_perf off` 禁用；注意 UART 输出开销。本轮架构迁移没有执行新的实机复现。

## 数据流与源码地图

| 阶段 | 当前源码 |
| --- | --- |
| 能力文档、格式位图与 Bluetooth 身份 | `core/airplay/airplay_info_plist.c`、`application/rt-thread/rtcarplay/app/carplay_app.c` |
| RTSP 路由、Audio SETUP、RECORD、Modes、Duck、Volume | `core/airplay/airplay_rtsp_session.c` |
| Socket、端点、音频密钥、输入配置与计数器 | `core/airplay/airplay_rtsp_server.c` |
| UDP 认证、RTP 元数据与解密载荷回调 | `core/airplay/airplay_audio_stream.c` |
| Codec、Jitter、Focus、Mixer 与语音 DSP | `core/audio/` |
| 板级输出 Worker、Direct、Mixer、音量与 PCM Probe | `application/rt-thread/rtcarplay/platform/audio_sink_lubanlite.c` |
| 麦克风 Worker 与加密发送 | `application/rt-thread/rtcarplay/platform/audio_input_lubanlite.c`、`core/airplay/airplay_audio_sender.c` |
| RT-Thread 采集设备适配 | `application/rt-thread/rtcarplay/platform/rtcp_audio_capture_lubanlite.c` |
| 独立本地 Tone、AAC、麦克风诊断 | `application/rt-thread/rtcarplay/platform/audio_test_lubanlite.c` |
| Receive、Jitter、麦克风共用的时钟扩展 | `port/rtthread/rtcp_os_rtthread.c` |

接收数据流为：已协商端点 → UDP Datagram → 认证解密 → 携带 Stream Type、Slot、Codec Format、RTP Timestamp 与单调到达时间的回调 → 有界 Sink Queue → Jitter、Decoder → Direct 或 Mixer → `sound0`。LPCM 回调携带 Little-endian 交错 int16 样本；AAC、Opus 回调保持编码 Access Unit。不能在没有解码、没有保留元数据的情况下直接将该回调接入纯 PCM 后端。

麦克风数据流为：`dmic0` 采集 → 语音处理 → 已协商的单声道 LPCM、Opus → 独立 Input Encryption Key → 已认证 Peer 的 UDP。`RTCARPLAY_MICROPHONE` 是显式可选功能，正常参考配置当前启用。`RTCARPLAY_MIC_AEC` 保持禁用，除非显式配置并完成声学校准。Host 合成回声测试不证明装车 AEC 质量。

## 线程模型与资源所有权

RTSP Poll Owner 接收数据并调用协议回调。`on_audio_pcm` 将数据排入板级 Sink 后返回。`carplay-aud` Worker 拥有 Decoder、Jitter、Mixer、Direct 处理及输出设备配置、写入。麦克风采集与发送使用独立 Worker。本地音频测试由启动页连接 Worker 拥有，只在 CarPlay Session 静止退出后运行，不能竞争 `sound0`。

Server 在停止 Sink 前关闭音频端点；Server 拆除后不应再产生新的 PCM 回调。设备、队列、上下文释放前必须完成 Worker 退出。Receiver 的 Owner 线程契约不会自动修复所有旧的跨线程封装；见[生命周期与所有权](lifecycle-and-ownership.md)。

## Direct、Mixer 与诊断 Probe

`carplay_audio_path` 查询当前选择；`carplay_audio_path direct` 与 `carplay_audio_path mixer` 是已有对比控制。Direct 保留受支持的原始采样率媒体输出，在关键本地声音需要混音时让出通路。Mixer 使用已有的 48 kHz 通路，包含重采样、Gain 与 Ducking。架构工作中保持协商与选择策略不变。

`carplay_audio_probe capture` 将解码后、Gain 前的媒体录入有界 RAM Probe，`replay` 在没有新网络数据时重放，`stop` 停止 Probe 动作，`clear` 释放，`status` 报告状态。本地 Tone 或 Replay 成功可以缩小输出设备通路的排查范围，不能证明手机已建立音频流。这些操作会改变播放行为，应放在受控音频调试中，不属于上述最初的只读复现。

## 证据解释

`streams=0` 表示没有活动的已协商音频端点，不是解码器速度慢的证据。存在 Stream 但 Datagram 为零时，应调查 Sender、网络与端点投递。Datagram 存在但认证失败时，需要独立的 Key、Nonce、Format 证据。载荷已接受但无输出时，需要 Sink、Queue、Decoder、Device 证据。资源、Modes 事件、iAP2、MFi 认证、AirPlay 认证、端点就绪和可听播放是不同事实。不能将任意 Resource Entity 值推断为成功取得所有权，也不能在没有受支持需求时强制 MainAudio Take。

保留阶段、错误码、计数器、时序、Role、Type、Format 和 Teardown 原因。不要发布配对记录、Identity Seed、私钥、认证材料、原始证书或任意捕获的请求正文。当前诊断用于描述状态，不应输出这些材料。

## 回归与剩余工作

`make -j8 check` 包含协议、已认证 UDP、Host 设备适配下的实际板级 Sink Worker、麦克风上行、音频引擎和本地诊断测试。CMake、CTest 现在也包含 `test_audio_diagnostics`；`test_audio_engine` 和 `test_audio_sink` 从源码目录执行，以解析已提交的音频 Fixture。端到端协议测试独立于 D213 硬件验证加密音频。

正常 D213 固件与 Headless 配置必须分别报告。Headless 配置禁用音频、视频和触摸，不能验证扬声器或麦克风。待实机检查包括手机 Audio SETUP、真实输出路由、可听播放、重连时设备打开关闭、麦克风采集、资源切换和校准后的 AEC。本轮不伪造任何 Stream 来掩盖已有的零 Stream 现象。
