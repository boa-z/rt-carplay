[English](../classic-capability-audit.md) | [简体中文](classic-capability-audit.md)

# Classic CarPlay 能力矩阵

**Implemented** 表示生产代码已提供该能力。**Host Tested** 表示自动化测试已用样本、回环连接或平台桩检查所列行为。**Hardware Pending** 表示本版本仍需在 D213 和 iPhone 上验证。**Experimental** 表示互操作证据尚不完整的可选基础能力。

| 能力 | 状态与证据 |
| --- | --- |
| 连接和会话所有权 | **Implemented、Host Tested、Hardware Pending。** Receiver 管理 RTSP 会话、建立和拆除。Luban-Lite 管理传输选择、重试、有线与无线恢复以及配对持久化。Host 测试覆盖监听回滚、重连、资源释放和传输回退。 |
| H.264 投屏和恢复 | **Implemented、Host Tested。** Classic 事件通道支持在 H.264 输入丢失后请求关键帧。早期 D213 固件日志记录了 Siri 启动后从队列溢出恢复。本版本 60 FPS 的显示节奏和从接收到锁存的延迟为 **Hardware Pending**。 |
| Classic 音频 | **Implemented、Host Tested。** D213 此前已测试音乐、通话和 Siri。本版本的播放、麦克风、Direct/Mixer 和音频路由恢复为 **Hardware Pending**。 |
| HID、外观、OEM 图标和投屏 | **Implemented、Host Tested、Hardware Pending。** SDK 提供双触点、原始 HID、旋钮、方向与媒体控制、Siri、动态外观和投屏命令。能力声明须与实际输入设备匹配。通过手机事件确认 OEM 焦点与投屏请求。 |
| 显示几何 | 初始几何、视口适配和坐标映射为 **Implemented、Host Tested**。D213 边缘对齐为 **Hardware Pending**。运行时视区选择与编码分辨率重协商是两个不同的协议问题，后者仍待确认。 |
| 定位和车速 | **Implemented、Host Tested、Hardware Pending。** 可选 GGA/RMC 和 `$PASCD` 根据手机选择器发送，在取消订阅或复位时停止。Luban-Lite 支持无线虚拟 GNSS 和车速注入。有线输入需宿主提供数据源。请求间隔字段和其他传感器仍待确认。 |
| 手机状态和设备时间 | **Implemented、Host Tested、Hardware Pending。** iAP2 提供电量和电源、蜂窝网络、通话状态及 `DeviceTimeUpdate` 增量。RTC 适配器在链路断开时作废待应用时间。D213 可选诊断会输出状态类别。 |
| Now Playing | **Implemented、Host Tested、Hardware Pending。** 保留原有文本和播放状态。扩展回调提供时长、进度、AirPlay 内联封面，以及通过 USB 和无线 iAP2 隧道组装的文件传输封面。 |
| 路线引导 | **Implemented、Host Tested、Hardware Pending。** `0x5201`、`0x5202` 和 `0x5204` 提供路线增量、有序索引、转向描述和有界车道角度。路线结束或断线时清理缓存。D213 可选诊断会输出字段存在标志、索引和计数。 |
| Classic 之外的 Vehicle State 和 UI Sync | **Experimental、Host Tested、Hardware Pending。** Host 测试覆盖有界通道及数据模型。完整 Ultra 渲染和验收不在本轮范围内。 |

## 协议证据和待确认事项

`$PASCD` 语句格式和选择器依据 DiPlay 的 `VehicleSpeedNmea.kt` 和 `Iap2LocationClient.kt`。已观察到的固定字段沿用参考值，其通用语义和定位间隔的处理仍需证据。LIVI 的 iAP2 消息定义和事件分发提供了电源、通信、Now Playing、时钟和路线引导的数据布局。OCBM 的配置与音视频文档区分已声明的视区能力和编码后的 Screen 流。

参考版本为 [DiPlay](https://github.com/shihabal3amri/DiPlay/tree/615b32ce0b40cd0b99ce94ba23a512001553684f)、[LIVI](https://github.com/f-io/LIVI/tree/64a05b68358db52ed887169143afd5944fd6f1f7) 和 [OCBM](https://github.com/lvalen91/ocbm/tree/45f5a637d2c59c7b779d47c9ccefa8138764dbf4)。回调契约参见 [Receiver API](receiver-api.md)，关闭顺序参见[生命周期与所有权](lifecycle-and-ownership.md)，媒体检查点参见[音频调试](audio-debug-handoff.md)。

## D213 验收

1. 分别建立有线和无线会话。播放音乐、拨打并结束电话，再调用 Siri 并使用麦克风。预期视频稳定、播放和采集可用、原音频路由恢复。用已有 Direct 和 Mixer 选项重复。在 30 和 60 FPS 下，分别在触发 Siri 前、动画期间和结束后执行 `carplay_video status`。选择有动画的可比时段，记录队列溢出、实际显示帧率和从接收到锁存的延迟。检查负载瞬时上升时画面是否保持可见，以及之后能否恢复正常显示节奏。
2. 在无线模式启用虚拟 GNSS，开始导航，再停车、停止定位请求并重连。用 `carplay_gnss` 检查当前订阅和 PASCD 计数。预期只发送订阅对应的数据，按请求发送零速 Park 样本，重连后无旧订阅。有线测试使用宿主数据源。
3. 分别在有线 USB 和无线隧道重复测试。拖动进度、切歌，切换手机电源和蜂窝状态，开始并结束通话。运行含多个转向和车道的导航后取消。预期看到进度更新、转向和车道记录，以及路线结束或断线时的失效通知。使用 `carplay_classic_diag on` 打印字段存在标志和计数，使用 `off` 停止打印。将 `DeviceTimeUpdate` 与手机时区对照，并确认退役链路中待处理的时间不会被应用。
4. 投屏时拔插 USB，断开 Wi-Fi 后由已配对手机重连，并在会话进行时切换有线与无线。预期只有一个活跃 Owner，旧的媒体和输入资源释放，建立失败后可重试。
5. 检查屏幕四角及双指触控、已声明的旋钮和媒体键、日夜模式、OEM 图标和投屏焦点。预期坐标映射与焦点变化正确，音频和 iAP2 各自保持独立生命周期。
