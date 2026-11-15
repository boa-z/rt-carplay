[English](../video-stream-session.md) | [简体中文](video-stream-session.md)

# RTSP 会话中的独立视频槽位

内部 RTSP 会话现支持显式配对的 start_video/stop_video 后端，分别管理 Type 110 Main 和实验性 Type 111 备用视频。这是可选第二路视频实验的控制面基础。生产 RTSP server 和公共 Receiver 尚未接入此后端，仍保持原有单 Main 监听。本里程碑没有新增 /info 声明、altScreen 能力、第二个 TCP 监听、媒体 Sink、编解码或 D213 渲染行为，不能作为 L5 或官方 Ultra 验收。

参考 LIVI 版本 64a05b68358db52ed887169143afd5944fd6f1f7 的 stack/setup.rs，其中 setup_screen 通过 open_screen(cluster, key) 打开独立接收器。Based on LIVI by Lasse Heitgres (https://github.com/f-io/LIVI)，署名要求见 NOTICE。本实现沿用 rt-carplay 现有有界连接 ID 台账和事务行为。

## 后端与生命周期契约

两个回调必须一起提供，分别接收流类型、连接 ID、独立请求端口及后端上下文，并拥有两种视频类型。Main 不会再同时调用旧 start_screen/stop_screen。缺省新回调时保持旧 Main 路径，Type 111 继续作为不支持类型从响应中省略。新增槽位仅是固定大小的会话状态，不新增帧缓冲、软件拷贝、线程或平台依赖。

Type 111 还要求已完成初始 SETUP。与现有 Main 回调一样，后端必须负责认证/密钥派生和实际端点资源。测试使用模拟回调，不能证明加密网络交付。当前公共 Receiver 尚未暴露生产备用监听。

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

## Host 证据与后续集成

tests/check_video_session_wire.py 使用 Python plistlib 独立生成二进制 plist 输入并解码实际会话响应，验证独立端口、未改变的初始能力及旧配置中省略的备用视频。C 测试覆盖混合 SETUP、重复类型/ID、视频/音频间碰撞、活动密钥冲突、幂等请求、过期选择性清理、替换/复用拒绝、任一回调失败、成功回调返回零端口、音频失败、过小响应缓冲、完整清理/销毁和不完整回调配对拒绝。

这些证据仅验证会话分派和资源归属。下一阶段仍需显式门控的公共 Receiver/server 组合、独立监听、媒体回调或明确的 Null Sink、统计、套接字断开隔离、认证密钥派生、加密并发流测试和不变的 Classic 回归。手机侧第二显示元数据及 altScreen 协商还需进一步的匹配门控。真实 iPhone Type 111、物理/官方渲染保持 NOT_RUN。D213 Vehicle State 模拟器及独立 LVGL 融合演示不会启用此备用槽位。

2026-11-15 本地完整 SDK Make、启用 RTCP_SANITIZE 的 CMake 67/67 项及完整集成 Host 套件均通过。现有加密 Vehicle State/UI Sync 和 Classic 媒体测试与新增模拟视频会话测试一起通过，但不能将备用槽位结果认定为加密 Type 111 接收证据。
