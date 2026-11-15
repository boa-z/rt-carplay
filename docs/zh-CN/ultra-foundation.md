[English](../ultra-foundation.md) | [简体中文](ultra-foundation.md)

# CarPlay Ultra 协议基础

最终产品目标是完整官方 CarPlay Ultra。本里程碑提供可移植的显示元数据、OPACK 和有界 Vehicle State 处理器，尚不能证明 Ultra 协商、官方认证或 Ultra 渲染器可用。Classic 视频与本地仪表融合是独立的集成成果。这些 API 不会自动向手机声明新的 Ultra 能力。

## 参考与署名

Based on LIVI by Lasse Heitgres (https://github.com/f-io/LIVI)。协议研究使用 LIVI 版本 64a05b68358db52ed887169143afd5944fd6f1f7，重点为 info.rs、stack/setup.rs、stack/data_streams.rs、vehicle_state.rs、vehicle_state/wire.rs 和 ui_sync.rs。NOTICE 与相关源码保留 GPL-3.0-or-later 要求的署名，集成产品还需在法律说明或 About 界面保留可见署名。不包含 Apple 资产、身份材料或私有规范。

Apple 的 [CarPlay 开发者页面](https://developer.apple.com/carplay/)、[MFi 项目流程](https://mfi.apple.com/en/how-it-works)、[2024 架构演讲](https://developer.apple.com/videos/play/wwdc2024/10111/)和 [Ultra 发布公告](https://www.apple.com/newsroom/2025/05/carplay-ultra-the-next-generation-of-carplay-begins-rolling-out-today/)说明官方产品及集成背景，不能替代全部协议、授权和产品要求。仅与 LIVI 源码行为一致不能证明完整官方 Ultra 支持。

## 显示与 OPACK API

公共显示配置包含一个 Main Display 的 UUID、视口、安全区域和外观元数据。空配置保留 Classic 元数据。两个矩形必须位于父区域内，省略矩形时使用父区域。该配置不会增加 Type 111、altScreen、HEVC、displayPanels、Vehicle State 或 UI Sync。共享整数等比例缩放与逆向触摸映射函数会拒绝黑边区域的触摸。

OPACK 使用调用方提供的前序节点数组，校验 UTF-8、有限数值、引用、嵌套、完整容器和尾随字节，支持标量、数组与字典。限制为 512 节点、16 层及 65,536 编码字节，保留有符号事务编号。重复路由键有歧义，不能查询。编码不使用对象引用压缩，未知标签及不支持的引用会被拒绝。

## Vehicle State 模型

宿主明确提供 accessory、service 和 characteristic 的 IID/type、归属、可变性、单位、可选数值范围与步长，以及初始值。SDK 不包含猜测的车辆目录。当前最多八个特征，每个字符串或二进制值最多 256 字节。支持布尔、整数、浮点、UTF-8 字符串及二进制数据，不声明字典/数组特征或无效化能力。Uint64 在当前 OPACK 模型中限制为 INT64_MAX。数值元数据和值必须符合声明格式。描述符和值均复制保存，不保留请求缓冲区。

手机拥有的值拒绝本地设置，车辆拥有及固定值拒绝手机写入。共享值可由双方写入，受声明的范围、格式及宿主纯校验回调约束。写入是对宿主的请求，成功状态或提交后回调均不能证明车辆执行器已动作。真实执行权限、反馈和安全策略由宿主集成负责。

## 消息处理与通知

处理器接收完整解密后的 Type 130 comm 消息体，包含 pluginID 和二进制 OPACK pluginData。实现 configRequest/configResponse、readRequest/readResponse、writeRequest/writeResponse、registerRequest/registerResponse 和 updateNotify。要求显式提供有符号整数或 null 的 transactionID，并原样回传。插件编号必须与配置一致。消息结构错误或重复编号会拒绝整个请求，未知值和拒绝写入使用参考实现中的错误值 -6727。不声明任何控制命令，controlRequest 返回参考实现的未知命令状态 -6735。未知命令返回本地不支持错误，不构造猜测的协议响应。

读取和订阅返回已知当前值及按 IID 区分的不可用错误。已知但尚无值的特征可以订阅，随后有效的手机写入能够产生首次通知。只有已订阅且发生变化的值产生通知，相同值重复写入不会产生通知。变化合并为最新值，通过高优先级通道输出。序列化成功会消费待发送变化，调用方必须保存生成字节，直到发送成功或认证会话结束。编码失败会保留待通知数据。会话重置清除订阅及待通知标记，保留车辆当前值。参考实现没有确认取消订阅的线格式，因此不自行发明。

请求先暂存再序列化。输出空间不足时，值、订阅和写入回调均不提交。写入策略校验必须是纯函数，可能在序列化失败之前执行；提交后回调只在成功后执行。全部调用由同一个串行所有者执行，回调不得重入对象。响应沿请求通道返回。公共状态包含请求、拒绝、写入、订阅及通知计数，本地错误区分非法输入、未知插件/IID、未知命令、归属不符及有界工作区或输出空间耗尽。

## 双向 DataStream 传输基础

内部 airplay_data_stream API 新增独立的认证双向 APTransportPackage 传输。所有者提供分别派生的 Pair-Verify DataStream Output/Input 密钥与对端地址，只处理已就绪事件。每个实例只接受一次匹配的连接，包括 IPv6 scope，不会以相同密钥接受替换连接。新流 ID、HKDF 派生、能力协商及注册表仍由 Receiver 所有者负责，本基础尚未接入实际 RTSP SETUP，现有 iAP 隧道保持原样。

只有完整且通过认证的包体进入回调。Sync 包先以完全相同的八字节 token 排队回复，再交付 comm/cmnd 包体；回复及未知消息类型不交付应用。传输接收与应用执行结果独立，应用输出采用 async/cmnd 包。每个包体最多 65,536 字节，复制保存的 FIFO 最多八包及 131,072 个明文字节。应用发送为传输回复保留一包和 32 字节，应用排队失败不会改变原队列。对端突发消息耗尽回复容量时，明确终止流，避免静默丢弃回复。此处不自行定义协议超时、重传策略或应用序号协议。

短写和临时 RTCP_ETIMEDOUT 保留加密字节，使用同一 nonce/密文继续。每个分块只加密一次，只有全部密文被 socket 接受后才计为已发送包。认证、格式、回调、socket 和接收队列失败会关闭流并清除密钥、待发送包体及组装缓冲区。排队成功代表接管发送责任，不能证明手机已接收。状态包含生命周期、终止错误、拒绝对端数、接收/交付/忽略/发送包数及已排队同步回复。全部调用由同一个串行所有者执行；借用完整包体的回调可以排队响应，但不能 poll、stop 或以其他方式重入对象。集成端口必须限制 socket 操作阻塞时间。

Host 测试覆盖独立 TCP/加密帧/包分片、65,536 字节包体、短 sync 包每一处截断、方向密钥、精确回复 token、忽略回复/未知类型、短写及临时收发超时、复制 FIFO 顺序及字节/包数限制、回复预留容量与过载失败、回调拒绝、异常长度/种类、错误认证标签、断连、IPv4 对端/IPv6 scope 约束及清理。这些属于模拟测试，不是真实手机证据。

## 测试与验收

Host 测试在 ASan/UBSan 下覆盖 LIVI configRequest 字节样本、负事务编号回传、目录结构与权限、缺失值、订阅、数值界限及步长、宿主拒绝、按 IID 返回部分写入错误、字符串复制、回调时机、输出失败回滚、通知重试/合并/重置、未知控制命令、截断及确定性畸形输入集。显示测试覆盖几何及触摸映射，元数据测试确认默认仍为 Classic 且没有额外可选能力。这些测试不是 iPhone 互操作证据。本里程碑未运行 LIVI 自身测试。

L1 仍为部分完成，需 Type 130 协商集成和 UI Sync 基础分别通过测试。双向传输基础已有 Host 覆盖，尚无实际协商。对当前新增实现，L2 融合、L3 真机 Ultra 协商、L4 真机 Vehicle State/UI Sync、L5 Type 111 和完整官方产品验收均为 NOT_RUN。完整 Classic 回归由集成宿主另行维护。

## 后续实现

后续协议工作为带新密钥派生的协商流注册表、手机能力提议门控、二进制 plist UI Sync 状态/ACK，以及公共 Receiver 接口接入。现有 iAP 隧道保持原样且仅接收。新增 Vehicle State API 尚未连接到实际 Receiver，也不在其 /info 端点输出 vehicleStateProtocolInfo。UI Sync 资产、帧合成、HEVC 支持、官方接入及认证仍是独立要求。D213 构建需要先确认真实图形和编解码能力才能声明对应能力。可选 Type 111 必须有独立 Sink，不能因单屏布局而隐式声明。
