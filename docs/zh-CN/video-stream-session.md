[English](../video-stream-session.md) | [简体中文](video-stream-session.md)

# 独立视频会话与实验性 Receiver 传输

RTSP 会话支持独立 Type 110 Main 和实验性 Type 111 备用槽位。公共 Receiver 可通过 experimental_alt_video 显式接入独立且经过认证的 H.264 TCP 接收器，默认配置仍使用现有单 Main 路径。此传输实验不新增 /info 声明、altScreen 协商、HEVC 或 D213 渲染行为，不能作为 L5 或官方 Ultra 验收。

参考 LIVI 版本 64a05b68358db52ed887169143afd5944fd6f1f7 的 stack/setup.rs，其中 setup_screen 通过 open_screen(cluster, key) 打开独立接收器。Based on LIVI by Lasse Heitgres (https://github.com/f-io/LIVI)，署名要求见 NOTICE。本实现沿用 rt-carplay 现有有界连接 ID 台账和事务行为。

## 后端与生命周期契约

两个回调必须一起提供，分别接收流类型、连接 ID、独立请求端口及后端上下文，并拥有两种视频类型。Main 不会再同时调用旧 start_screen/stop_screen。缺省新回调时保持旧 Main 路径，Type 111 继续作为不支持类型从响应中省略。会话槽位是固定大小状态。启用 Receiver 传输后按需分配独立 Screen 接收器、密钥/nonce 周期和有界组帧缓冲，不新增线程或平台依赖。每个监听只接受一个连接，随后关闭；新端点必须使用新连接 ID。共享 Linux/RT-Thread poll 容量从 24 增至 25，以容纳备用视频及原有最大端点集合。

Type 111 还要求已完成初始 SETUP。与现有 Main 回调一样，后端必须负责认证/密钥派生和实际端点资源。会话测试使用模拟回调，下述公共 Receiver 测试使用真实加密套接字。备用后端还要求 ControlCipher 已激活，才可分配监听。

启动回调返回 false 前必须清理自身部分分配的资源。如返回 true 但端口为零，会话将拒绝并调用该类型的停止回调。成功端点随后由会话拥有，直到替换、清理、回滚或销毁。

| 触发条件 | 行为 |
| --- | --- |
| 混合 Main/备用视频 SETUP | 启动端点前验证视频/音频描述，返回各自类型/端口。 |
| 同一事务重复类型或使用相同 ID | 两个回调启动前拒绝。 |
| 同一类型和活动连接 ID | 返回原有端口，不重新启动。 |
| 跨类型或退役后复用 | 经共享媒体/Type 130 密钥台账拒绝。 |
| 使用新 ID 替换 | 只停止对应槽位后启动替代，失败时不恢复已关闭端点。 |
| 回调失败、零端口或响应编码失败 | 停止本次事务中新启动端点，保留幂等沿用的其他槽位；预留 ID 保持退役，包括前一个回调失败后尚未启动的另一槽位。 |
| 新视频启动后音频失败 | 回滚两个新视频及本次新启动的音频。 |
| 选择性 TEARDOWN | 只停止所选类型；携带的连接 ID 必须匹配当前槽位，旧请求不能关闭替代端点。 |
| 完整 TEARDOWN 或销毁 | 每个活动槽位只停止一次。 |

后端可能立即传输，因此在回调前预留 ID。部分事务后继续退役 ID 是保守的复用防护，不代表推测出的手机重传规则。台账仍有界，新的认证会话仍是重建台账的边界。缺省初始 SETUP enabledFeatures 字节仍为 iAPChannel/viewAreas，手机仅提出 altScreen 不会增加已启用能力。

## 会话 Host 证据

tests/check_video_session_wire.py 使用 Python plistlib 独立生成二进制 plist 输入并解码实际会话响应，验证独立端口、未改变的初始能力及旧配置中省略的备用视频。C 测试覆盖混合 SETUP、重复类型/ID、视频/音频间碰撞、活动密钥冲突、幂等请求、过期选择性清理、替换/复用拒绝、任一回调失败、成功回调返回零端口、音频失败、过小响应缓冲、完整清理/销毁和不完整回调配对拒绝。

## 公共 Receiver 传输契约

设置 receiver_config.experimental_alt_video=true，可选 alt_screen_port（零表示独立临时端口）。media.on_alt_video/media.alt_video_context 只接收备用 Annex-B H.264；开关为 true 且回调为 NULL 时明确选择 Null Sink。Main media.on_video 和 video_ready/diagnostics 含义不变。开关为 false 时只填写备用回调不会启用任何能力。旧配置迁移复制新增显式传输字段，默认仍不启用。

rtcp_receiver_get_alt_video_status 在所有者线程返回复制快照，停止时也可查询，包括 configured/ready、端口、打开/关闭次数、last_error 及备用 Screen 统计。帧统计在关闭后保留至下一次成功打开，打开/关闭次数跨控制器重连保留，在 Receiver stop/start 时重置。意外套接字/加密/组帧失败报告通用 RTCP_EIO，不声称更精确的错误分类；监听启动失败报告启动错误。正常选择性/完整清理清除 ready 和端口。

备用意外退出只关闭自身端点和会话槽位，已消费 ID 仍保留。Main 丢失保持现有完整控制器清理并关闭备用。失败在 RTSP 分派前退役，避免旧退出信号关闭替代密钥。独立监听和密钥不代表双路硬件解码或第二显示能力。

## 加密 Host 证据与剩余工作

--alt-video 和 --alt-video-null 端到端场景使用公共 Receiver、真实 TCP、Pair-Setup/Pair-Verify、ControlCipher 及分别加密的 H.264 帧，验证认证/初始 SETUP 门控、不同标记/密钥的并发交错交付、独立端口/Sink/计数器、幂等请求、过期选择性清理、ID 退役、监听分配失败、仅错误就绪时避免阻塞 accept、错误密钥/认证标签隔离、EOF、使用新 ID 恢复、Main 丢失清理和 stop/start 重置。两个场景均继续原有 Classic 音视频/重连回归，已接入 Make check 和 CTest。现有 Python plistlib 检查独立验证不变的初始能力及会话响应结构。

手机侧第二显示元数据和 altScreen 协商仍需匹配的能力门控及真实手机证据。真实 iPhone Type 111 和物理/官方渲染保持 NOT_RUN。D213 Vehicle State 模拟器及独立 LVGL 融合演示不会启用此实验性备用传输。当前实验只支持现有 H.264 线格式，不能证明官方 Cluster 编解码、资产、呈现时序或官方 Ultra。
