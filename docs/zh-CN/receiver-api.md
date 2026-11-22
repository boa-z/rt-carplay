[English](../receiver-api.md) | [简体中文](receiver-api.md)

# Receiver API

## 生命周期

包含 `rtcp/receiver.h`。`rtcp_receiver_create()` 复制公共配置和初始 `/info` Plist，不打开 Socket。`rtcp_receiver_start()` 打开 RTSP 监听器。`rtcp_receiver_poll()` 推进已有协议与媒体端点。`rtcp_receiver_stop()` 同步关闭所有端点。`rtcp_receiver_destroy()` 停止并释放对象，销毁 NULL 成功。对运行中的对象再次 Start、对已停止对象再次 Stop 均直接成功，不重复执行。Restart 使用已复制的配置，每次成功 Start 后递增 `generation`。

```c
rtcp_receiver_config_t config = {0};
config.platform.os = &host_os;
config.platform.net = &host_net;
config.platform.pairing = host_pairing_providers;
config.platform.info_plist = info_bytes;
config.platform.info_plist_len = info_length;
config.media = host_media_backend;
rtcp_receiver_t *receiver = NULL;
int result = rtcp_receiver_create(&config, &receiver);
if (result == RTCP_OK) result = rtcp_receiver_start(receiver);
/* The host's owner loop calls rtcp_receiver_poll(receiver, timeout_ms). */
/* Stop/join any host worker before freeing its borrowed providers. */
(void)rtcp_receiver_destroy(receiver);
```

该片段说明配置与所有权，不是完整可运行的配对 Provider。可工作的 Linux 组合实现位于 `examples/linux_sim/main.c`，其中身份存储、密码学 Provider、能力文档与发现配置均为实际实现。该示例和 RT-Thread 组合实现现在都直接填写公共配置。

## 平台与会话配置

`rtcp/platform.h` 定义 `rtcp_platform_t`：OS、网络接口表，身份和控制器存储回调，初始能力字节及可选的能力刷新回调。`rtcp_pairing_provider_t` 保留独立的 Pair-Setup、Pair-Verify 上下文。随机数、Ed25519 签名验证及配对保存查询仍由宿主注入，SDK 不内置身份或认证材料。OS、网络接口表、身份数据、Provider 上下文与回调上下文均借用至 Destroy 返回，期间须保持有效和不可变。初始配置结构与 `/info` 字节可在 Create 后释放。

能力文档必须非空，且不超过 `RTCP_CAPABILITIES_MAX_BYTES`（6144）。动态 `get_info_plist` 回调在 Poll Owner 中执行，先于新控制器会话建立。返回字节必须在随后立即复制完成前保持有效、不可变；失败会拒绝本次连接。已有会话保留自己的快照。

`port` 默认 7000；`screen_port` 为零时申请临时 Screen 端点。`family` 接受零、`RTCP_AF_INET` 或 `RTCP_AF_INET6`，非法值在 Create 时拒绝。`screen_max_body_bytes` 为零时保留已有的 512 KiB 协议默认值。发现、USB 选择、蓝牙启用、Wi-Fi Handover 策略及连接仲裁由宿主负责。可选 `iap2` 回调用于注册认证后的隧道，不会启动无线硬件。隧道 Tick 返回 false 时仅关闭隧道，保留媒体。

## 媒体与输入注册

`rtcp/media.h` 定义 Receiver 实际接收的 `rtcp_media_backend_t`。`on_video` 接收配置或访问单元形式的 Annex-B H.264、可写前缀容量、发送端纳秒时间戳及本地到达微秒时间戳。这是编码视频，并非解码像素。缓冲区只借用到回调返回，异步后端须在返回前复制。视频回调为 NULL 时使用明确的丢弃 Sink，支持 Headless 运行。显示和 HMI 可见性仍由宿主决定。

`on_audio` 接收解密后的 LPCM 或编码 AAC、Opus，附带协商得到的 `rtcp_audio_format_t`、流类型、RTP 时间戳及本地到达微秒时间戳。LPCM 为交错小端 S16。编码包保留 Codec 标识、采样率、时钟率和流槽位元数据。Audio 为 NULL 时保留已有拒绝 Audio SETUP 的行为。选择性停止、带槽位停止、输入启停、资源模式、Ducking 及音量回调均保留原有协议契约；音量之外的音频回调使用 `audio_context`，音量使用 `volume_context`。

麦克风启动接收 `rtcp_audio_input_config_t`，包含协商的发送格式、输入密钥、认证后的对端、网络接口表和电话标志。采集 Worker 须在返回前复制借用配置，并在宿主释放它之前停止使用会话状态。API 迁移不改变音频能力声明、SETUP、Jitter、Codec、Mixer、重采样、麦克风加密或 AEC 行为。

`rtcp/input.h` 提供 `rtcp_receiver_send_hid_report()` 与 `rtcp_receiver_request_siri()`。`rtcp_receiver_send_iap()` 提供隧道 iAP2 输出。这些操作在 Receiver Owner 中执行，允许由该 Owner 上的回调调用，同步消耗传入缓冲区。加密事件通道不存在或写入失败时返回 false。触摸设备读取、焦点、坐标转换、HID 描述符及跨线程队列属于宿主。已有可移植输入桥可以生成协商的触摸报告；RT-Thread 包装层现在通过 Receiver 发送，不再直接调用内部 RTSP Server。

## Classic 运行时控制

在 Owner 线程调用 `rtcp_receiver_set_appearance()`，传入 `RTCP_APPEARANCE_AUTOMATIC`、`RTCP_APPEARANCE_LIGHT` 或 `RTCP_APPEARANCE_DARK`。自动模式接受宿主当前的界面与地图日夜状态，可由环境光和车灯状态决定。浅色和深色覆盖这两项状态。更新使用已协商的主显示 UUID 和加密事件通道。`RTCP_OK` 表示两条消息均已写入。重连后重新应用偏好，在 iPhone 上验证渲染后的界面和地图。宿主独立管理本机 HMI 主题。

宿主实际应用增益变化后调用 `rtcp_receiver_set_volume_db()`，包括收到 `on_volume` 请求的情况。这会记录实际增益，供手机后续发起 `GET_PARAMETER volume` 查询。静音使用 `-144` dB。Duck、资源和活动流各自保留独立约定。初始 `streams=0`、缺少 Audio SETUP、完成 SETUP 后播放异常是不同的检查点。

Owner 可用 `rtcp_receiver_send_touch()` 发送完整的触点快照，或用 `rtcp_receiver_send_knob()` 发送旋钮、方向、主页、返回及确认状态。`rtcp_receiver_send_media_button()` 支持播放和切歌。会话建立前声明相应的可选 HID 设备，并在按下后发送释放。宿主负责实体键、触摸映射、焦点和本机音量策略。

通过 `rtcp_receiver_set_now_playing_callback()` 接收增量 `rtcp_now_playing_t`。回调返回前复制非空的曲名、艺人和专辑字符串，仅在 `has_playing` 已设置时更新播放状态。会话结束时清除累计快照。Classic 导航数据、GNSS 转发和运行时分辨率重协商仍需公开线包依据和独立互操作验证。参见[能力审计](classic-capability-audit.md)。

自行管理 iAP2 会话的宿主也可使用 `iap2_session_config_t.on_now_playing`。此可选回调在身份识别和 MFi 验证后由 iAP2 Owner 调用，将 `0x5001` 以同一增量类型上报。空字符串会清除字段。Luban-Lite 在无线 DataStream 隧道接入该回调，与 AirPlay 共用 Owner。诊断会记录 `0x5000` 是否入队，以及 `0x5001`、`updateMRNowPlayingInfo` 或 `updateMRPlaybackState` 解出了哪些字段。日志只记录字段是否存在。

Classic 路线引导可由 iAP2 Owner 注册 `on_route_guidance`（`0x5201`）和 `on_route_maneuver`（`0x5202`）。`rtcp/navigation.h` 定义的增量类型包含限长的 UTF-8 文本和原始路线状态或转向类型，文本仅在回调期间有效。宿主应保留未出现的字段，遇到显式空文本时清除对应字段，状态为 `0` 或断线时清除路线。维护转向列表时可用转向索引关联更新。回调会过滤发给其它显示组件的更新。

有 GNSS 数据源的宿主可在创建 iAP2 会话前设置 `identity.provide_location`。在同一 Owner 上调用 `iap2_session_location_requested()` 查询手机订阅的 GGA/RMC 类型，再用 `iap2_session_send_location()` 传入一条带校验和且不含 CR/LF 的 NMEA 语句。宿主管理数据采集、定位质量、发送节奏和重试。手机停止订阅、链路断开或重新鉴权时，SDK 会清除订阅。这些传输接口定义在 `iap2_session.h`。

## 诊断与对端地址

`rtcp_receiver_get_diagnostics()` 将屏幕、音频、Timing 计数及 ControlCipher 分帧标志复制到 `rtcp/diagnostics.h` 定义的 `rtcp_receiver_diagnostics_t`。计数保留已有含义、字段类型和顺序，低层 AirPlay 诊断类型名继续作为别名。屏幕耗时包含回调和抢占。音频 Datagram 计数发生在认证之前，活动流计数表示 Audio SETUP 端点数量。Timing 交换计数不代表时钟已经同步。停止的 Receiver 返回零计数。`rtcp_receiver_get_peer()` 复制当前 IPv4、IPv6 RTSP 对端；未连接时清空输出并返回 `RTCP_ENOENT`。参数为 NULL 时返回 `RTCP_EINVAL`。

`rtcp_receiver_set_single_cipher_frame()` 是控制回复分帧的 Owner 线程 A/B 诊断接口。默认仍为分块，Stop/Start 后恢复默认；对停止的 Receiver 设置时返回 `RTCP_ESTATE`。这些 API 均属于 Receiver Owner，允许从该 Owner 的回调调用，不是跨线程访问接口。RT-Thread `carplay_ab` 命令在生命周期保护和队列互斥锁下提交请求值，由 Poll Worker 在下一次协议 Poll 前应用。输出分别显示已应用值、请求值和 Pending 标志。Shell 执行不会解引用协议对象，也不释放资源。

## 兼容迁移

早期 `.airplay` 配置过渡字段已由公共 `platform`、`media` 和 `iap2` 描述取代。迁移已有 `airplay_rtsp_server_config_t` 时，包含 `rtcp/receiver_compat.h` 并调用 `rtcp_receiver_config_from_airplay(&legacy, &config)`。该函数先清空目标，再复制所有描述与 Provider 引用，参数为 NULL 时返回 `RTCP_EINVAL`。转换后再设置 `on_state` 和上下文。已有 Provider 生命周期要求仍然适用，辅助函数不分配或打开资源。新集成应直接填写公共配置。

旧的 `rtcp_video_frame`、`rtcp_pcm` 和 `rtcp_media` 草案描述从未接入 Receiver，也不能表达压缩音频元数据，现由上述可执行后端契约取代。内部协议类型名称保留为公共媒体、Provider 类型的别名，已有低层调用者保留原函数签名。旧 `carplay_app_*` API 仍位于 `application/rt-thread/rtcarplay/app/carplay_app.h`，使用宿主 App 包含目录访问，已废弃的 SDK 示例头文件报告迁移错误。临时 `rtcp_receiver_server()` 访问入口和 `receiver_internal.h` 已删除，宿主无法再通过 Receiver 借用内部 RTSP 对象。

## 状态、错误与线程

`rtcp_receiver_get_state()` 复制 `rtcp/events.h` 中定义的状态快照。`running` 表示监听器所有权；`transport_connected`、`authenticated`、`session_established`、`video_ready`、`audio_ready`、`input_ready` 是独立观察值。RECORD 被接受不等于已经交付媒体。视频或音频就绪表示协议端点已建立，不表示解码输出或硬件播放成功。`generation` 统计成功启动次数。`last_error` 保存最近生命周期、Poll 结果，成功操作会清除它。可选 `on_event_state` 表示加密事件通道的接入，不代表整体 CarPlay 连接状态。

`on_state` 在调用生命周期或 Poll 的线程中同步执行，发生变化时通知。参数只借用到返回。回调可以查询状态，但不能 Start、Stop、Poll 或 Destroy Receiver；操作、回调活动期间，这些调用返回 `RTCP_ESTATE`。媒体和协议回调保持原缓冲区与 Owner 线程契约。Receiver 不创建异步分发队列。

API 返回已有 `RTCP_OK`、`RTCP_EINVAL`、`RTCP_ENOMEM`、`RTCP_ESTATE` 及协议、Port 错误码。启动失败后对象保持停止，可重试。启动前 Poll 返回 `RTCP_ESTATE`，负超时返回 `RTCP_EINVAL`。Poll 报错不会隐式销毁监听器，由宿主选择重试或停止。所有操作，包括快照查询，都属于同一个 Owner 线程。宿主须串行化外部请求；重入保护不是跨线程锁。

## 回归覆盖与限制

`test_receiver` 使用原生公共配置和真实环回 Socket，验证描述与能力复制、非法地址族拒绝、Bind 失败重试、拒绝重入、活动连接拆除、对端地址复制、诊断快照、分帧设置复位及 32 次 Start/Stop。`test_airplay_pairing_e2e` 使用原生 Receiver 配置执行真实 Pair-Setup、Pair-Verify、加密控制、音频、视频、HID 和重连，并检查独立就绪状态、独立回调上下文及公开音频诊断。客户端直接驱动 Receiver，不借用内部 Server 指针。其 `--legacy-config` 运行通过兼容转换执行同样交换。公共头文件编译仅提供 `-Iinclude`，不提供内部协议或平台包含目录。

RT-Thread 包装层另有跨线程生命周期回归，覆盖延迟应用分帧、停止时待处理请求及互斥锁拆除时的重入。应用 Connection Manager 和板级适配现已位于 Luban-Lite 宿主；其实际源码回归覆盖诊断副本、Owner 命令队列和 Worker 退出。设备播放、DMA 与 Cache 行为以及物理重连不属于这些 Host 测试的验证范围。
