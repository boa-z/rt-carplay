[English](../classic-capability-audit.md) | [简体中文](classic-capability-audit.md)

# Classic CarPlay 能力审计

## 后续实现与参考源码

| Classic 缺口 | SDK 当前状态 | 参考源码 |
| --- | --- | --- |
| 路线摘要和转向描述 | 对 `0x5201`／`0x5202` 增量为 **Implemented / Hardware Pending**。`core/iap2/iap2_session.c` 的 Host 链路测试覆盖鉴权、UTF-8 字段、显示组件过滤和转向索引。车道提示和转向列表展示为 **Partial**。 | LIVI `iap2-csm/src/messages/route_guidance.rs`、`livi-helper-stack/src/events.rs`。DiPlay `Iap2Endpoint.kt` 注册了两条消息。 |
| 主机 GNSS 转发 | 可选的 GGA/RMC 为 **Implemented / Hardware Pending**。`iap2_identity.c` 声明提供方，`iap2_session.c` 处理 `0xFFFA`／`0xFFFB`／`0xFFFC`。宿主提供 GNSS 接收器和定位数据。 | LIVI `iap2-csm/src/messages/location.rs`、`livi-helper-stack/src/ident.rs`、`bringup.rs`、`vehicle.rs`。DiPlay `Iap2Endpoint.kt`、`Iap2Messages.kt`。 |
| 手机电量、蜂窝和时钟 | **Partial**。现有初始化流程声明或订阅了其中部分消息，仍缺可移植的宿主数据模型。 | LIVI `livi-helper-stack/src/events.rs` 分发电量、蜂窝、通话和时钟更新。 |

回调生命周期及 GNSS 启用方式见 [Receiver API](receiver-api.md)。

审计基线为 rt-carplay `45b3133`、[LIVI `4451f8f`](https://github.com/f-io/LIVI/tree/4451f8f8b927032dd59708ab2f1591548410139e)（Lasse Heitgres，GPL-3.0-or-later 和 NOTICE）及 [DiPlay `615b32c`](https://github.com/shihabal3amri/DiPlay/tree/615b32ce0b40cd0b99ce94ba23a512001553684f)（GPL-3.0）。参考结论来自源码。Hardware Pending 表示仍需 iPhone 或开发板验证。Host-only 表示当前证据来自模拟测试或 Socket 测试。

| 能力 | rt-carplay 基线与依据 | 参考源码 |
| --- | --- | --- |
| 有线、无线连接，配对恢复和重连 | **Implemented / Hardware Pending**。`core/usb/` 负责 USBMUX 和 lockdown，`core/iap2/iap2_session.c` 负责传输识别及 Wi-Fi 交接，`core/airplay/airplay_rtsp_session.c` 负责 AirPlay 鉴权。宿主管理配对记录和重连策略。 | LIVI `livi-cp/src/manager.rs`、`pairings.rs`。DiPlay `PairingStore.kt`、`LockdownPairingClient.kt`、`WiredSessionMemory.kt`、`HotspotJoinRepair.kt`。 |
| 日夜模式及在线切换 | **Partial**。`core/airplay/airplay_info_plist.c` 声明 UI 和地图的模式及偏好。基线缺少公开在线控制接口。 | LIVI `livi-cp/src/info.rs`、`stack/events.rs` 使用 `uiAppearanceUpdate` 和 `setNightMode`。DiPlay `AirPlaySession.kt` 在事件通道就绪后发送 `setNightMode`。 |
| 音量、静音、焦点及资源 | **Partial**。`airplay_rtsp_session.c` 接收音量、`modesChanged` 及 Duck 命令，`core/audio/` 实现 Direct/Mixer 播放策略。设备增益和焦点策略由宿主负责。 | LIVI `stack/audio.rs`、`video_playback.rs`。DiPlay `AndroidMediaSink.kt`、`NavigationPromptFocus.kt`。 |
| 音乐、电话、导航播报、Siri、麦克风 | **Implemented / Hardware Pending**。`airplay_audio.c`、`airplay_audio_stream.c`、`airplay_audio_sender.c` 和 `include/rtcp/media.h` 覆盖流协商和麦克风输入，`runtime/receiver.c` 暴露 Siri。加密传输已有 Socket 测试，板端音质和并发路由恢复待验。 | LIVI `stack/audio.rs`。DiPlay `AudioStream.kt`、`BufferedAudioStream.kt`、`AndroidMediaSink.kt`、`PhoneAudioRoute.kt`。 |
| 触控、物理键、旋钮、方向键 | **Partial**。`core/input/`、`include/rtcp/input.h`、`airplay_control_port.c` 提供触控编码、原始 HID 和 Siri，默认声明缺少通用旋钮和按键设备。 | LIVI `hid.rs`、`stack/commands.rs`。DiPlay `AirPlayHid.kt`、`AirPlaySession.kt`。HID 报告须与设备描述符匹配。 |
| Now Playing 和导航数据 | **Missing** 公开 Classic 数据模型。`airplay_rtsp_session.c` 基线没有 Now Playing 分发，`core/media/vehicle.c` 和 UI Sync 提供实验通道。 | LIVI `airplay.rs` 解析 `updateMRNowPlayingInfo`，`vehicle_state/navigation.rs` 处理实验导航。DiPlay `CarPlayHostActivity.kt` 消费媒体信息，`Iap2Session.kt` 提供 iAP2 路径。 |
| GPS、车辆状态和时间同步 | **Partial / Host-only**。`airplay_timing_port.c` 实现 AirPlay 媒体时钟，`core/media/vehicle.c` 实现实验订阅并有 Host 测试。缺少公开 Classic GNSS 转发。 | LIVI `helper_sock.rs` 提供 NMEA。DiPlay `VehicleSpeedNmea.kt`、`Iap2VehicleStatus.kt`、`NtpClock.kt` 分离车辆数据和媒体时钟。 |
| 分辨率、比例及显示能力变更 | **Partial**。`core/media/display.c` 校验初始几何，计算等比区域及映射坐标。`airplay_info_plist.c` 声明初始能力，`airplay_rtsp_server.c` 管理视频端点。Classic 运行时分辨率重协商待确认。 | LIVI `info.rs`、`stack/video.rs`。DiPlay `AirPlayInfoPlist.kt`、`AirPlaySession.kt`。 |
| 多手机和有线/无线选择 | **Partial / Hardware Pending**。`airplay_rtsp_server.c` 管理单活动 RTSP 连接。宿主选择手机和传输，配对存储与选择策略独立。 | LIVI `manager.rs`。DiPlay `CarPlayHostActivity.kt`、`WiredSessionMemory.kt`。 |
| Ultra 实验能力 | **Host-only / Hardware Pending**。`core/media/vehicle.c`、`core/airplay/airplay_ui_sync.c`、`airplay_rtsp_server.c` 提供有界的 Vehicle State、UI Sync 和 Type 111 通道，`tests/unit/test_airplay_pairing_e2e.c` 验证模拟对端的加密会话。 | LIVI `vehicle_state.rs`、`ui_sync.rs`、`stack/video.rs`。DiPlay 仪表和 HUD 含有平台专用显示逻辑，无法据此判定 Ultra 符合性。 |

## 协议边界

`ui_night` 和 `map_night` 分别表示手机界面和地图的初始明暗状态。共享的 `appearance_setting` 用 0、1、2 表示自动、浅色、深色偏好。LIVI 向主界面发送带显示 UUID、整数模式和偏好的 `uiAppearanceUpdate`。两个参考项目均使用布尔 `setNightMode` 控制主地图。Socket 写入只证明发送到事件通道，画面效果需要手机确认。环境光判断和宿主 HMI 主题由宿主决定。

最初的 `streams=0` 对应控制 SETUP。后续音频 SETUP、数据包到达和鉴权、设备播放是独立检查点。手机音量反馈和宿主应用增益也是不同动作。详见[音频协商](audio-negotiation.md)和[音频调试](audio-debug-handoff.md)。

Receiver 管理单活动会话。USB 拔出、蓝牙到 Wi-Fi 的交接、重试时机、已记忆手机优先级和传输切换策略由宿主负责。SDK 负责协议清理和端点重复建立。调用顺序见[生命周期与所有权](lifecycle-and-ownership.md)。
