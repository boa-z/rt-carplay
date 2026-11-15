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

宿主明确提供 accessory、service 和 characteristic 的 IID/type、归属、可变性、单位、可选数值范围与步长，以及初始值。SDK 不包含猜测的车辆目录。当前最多八个特征，每个字符串或二进制值最多 256 字节。支持布尔、整数、浮点、UTF-8 字符串及二进制数据，不声明字典/数组特征或无效化能力。Uint64 在当前 OPACK 模型中限制为 INT64_MAX。数值元数据和值必须符合声明格式。整数范围及步长使用精确整数比较和取模校验，覆盖大于 2^53 的值；浮点步长使用有限双精度运算及 1e-7 商容差，不要求嵌入式工具链提供 long-double libm 入口。描述符和值均复制保存，不保留请求缓冲区。

手机拥有的值拒绝本地设置，车辆拥有及固定值拒绝手机写入。共享值可由双方写入，受声明的范围、格式及宿主纯校验回调约束。写入是对宿主的请求，成功状态或提交后回调均不能证明车辆执行器已动作。真实执行权限、反馈和安全策略由宿主集成负责。

## 消息处理与通知

处理器接收完整解密后的 Type 130 comm 消息体，包含 pluginID 和二进制 OPACK pluginData。实现 configRequest/configResponse、readRequest/readResponse、writeRequest/writeResponse、registerRequest/registerResponse 和 updateNotify。要求显式提供有符号整数或 null 的 transactionID，并原样回传。插件编号必须与配置一致。消息结构错误或重复编号会拒绝整个请求，未知值和拒绝写入使用参考实现中的错误值 -6727。不声明任何控制命令，controlRequest 返回参考实现的未知命令状态 -6735。未知命令返回本地不支持错误，不构造猜测的协议响应。

读取和订阅返回已知当前值及按 IID 区分的不可用错误。已知但尚无值的特征可以订阅，随后有效的手机写入能够产生首次通知。只有已订阅且发生变化的值产生通知，相同值重复写入不会产生通知。变化合并为最新值，通过高优先级通道输出。序列化成功会消费待发送变化，调用方必须保存生成字节，直到发送成功或认证会话结束。编码失败会保留待通知数据。会话重置清除订阅及待通知标记，保留车辆当前值。参考实现没有确认取消订阅的线格式，因此不自行发明。

请求先暂存再序列化。输出空间不足时，值、订阅和写入回调均不提交。写入策略校验必须是纯函数，可能在序列化失败之前执行；提交后回调只在成功后执行。全部调用由同一个串行所有者执行，回调不得重入对象。响应沿请求通道返回。公共状态包含请求、拒绝、写入、订阅及通知计数，本地错误区分非法输入、未知插件/IID、未知命令、归属不符及有界工作区或输出空间耗尽。

## 双向 DataStream 传输基础

内部 airplay_data_stream API 提供独立的认证双向 APTransportPackage 传输。所有者提供分别派生的 Pair-Verify DataStream Output/Input 密钥与对端地址，只处理已就绪事件。每个实例只接受一次匹配的连接，包括 IPv6 scope，不会以相同密钥接受替换连接。下述 Receiver 集成现已提供新流 ID、密钥派生及 SETUP 路由，现有 iAP 隧道保持原样。

只有完整且通过认证的包体进入回调。Sync 包先以完全相同的八字节 token 排队回复，再交付 comm/cmnd 包体；回复及未知消息类型不交付应用。传输接收与应用执行结果独立，应用输出采用 async/cmnd 包。每个包体最多 65,536 字节，复制保存的 FIFO 最多八包及 131,072 个明文字节。应用发送为传输回复保留一包和 32 字节，应用排队失败不会改变原队列。对端突发消息耗尽回复容量时，明确终止流，避免静默丢弃回复。此处不自行定义协议超时、重传策略或应用序号协议。

短写和临时 RTCP_ETIMEDOUT 保留加密字节，使用同一 nonce/密文继续。每个分块只加密一次，只有全部密文被 socket 接受后才计为已发送包。认证、格式、回调、socket 和接收队列失败会关闭流并清除密钥、待发送包体及组装缓冲区。排队成功代表接管发送责任，不能证明手机已接收。状态包含生命周期、终止错误、拒绝对端数、接收/交付/忽略/发送包数及已排队同步回复。全部调用由同一个串行所有者执行；借用完整包体的回调可以排队响应，但不能 poll、stop 或以其他方式重入对象。集成端口必须限制 socket 操作阻塞时间。

Host 测试覆盖独立 TCP/加密帧/包分片、65,536 字节包体、短 sync 包每一处截断、方向密钥、精确回复 token、忽略回复/未知类型、短写及临时收发超时、复制 FIFO 顺序及字节/包数限制、回复预留容量与过载失败、回调拒绝、异常长度/种类、错误认证标签、断连、IPv4 对端/IPv6 scope 约束及清理。这些属于模拟测试，不是真实手机证据。

## Receiver 注册及 SETUP 生命周期

公共 rtcp/data_stream.h 接口与 Receiver 配置接收显式通道掩码、完整包体处理器及可选关闭处理器。注册为零时保留 Classic 行为。三类传输对应 Vehicle State 普通/高优先级和 UI Sync UUID。注册不会自动生成应用协议、enabledFeatures 项、vehicleStateProtocolInfo、uiSyncInfo 或 displayPanels 声明。集成宿主只能注册已实现的处理器，D213 开发配置当前仍不注册任何此类处理器。下述显式 Vehicle State 处理器接入已经实现；UI Sync 应用集成仍待完成。

仅在初始手机 SETUP 成功后保存能力提议快照。已注册通道还要求对应的 vehicleStateProtocol 或 uiSync 提议、完成 Pair-Verify、活动的加密控制连接及初始 SETUP，缺失或无效前提会拒绝建立。每次 Type 130 SETUP 当前只包含一条流，与原有 iAP 限制一致；混合/批量 Type 130 请求在启动端点前被拒绝。分别发送请求可以同时建立三条通道，各自拥有独立端口、流 ID、密钥、加密计数器、队列及状态。iAP 保留 streamID 1，注册传输采用递增且大于一的 ID。完全一致的活动 SETUP 返回原有端点，不能通过改变 seed 或连接元数据隐式替换。

Pair-Verify 共享密钥与十进制 seed 通过 DataStream-Salt<seed> 及 Input/Output 标签派生 HKDF-SHA512 方向密钥。共享会话 salt 清单覆盖屏幕、音频、iAP 及注册传输。新的传输尝试在创建监听器之前预留 salt 和 ID，创建失败也会消耗。关闭、选择性 TEARDOWN、输出编码失败及完整 TEARDOWN 均不会释放该配对会话内已消费的 salt。新 seed 可以用新 ID 重开已关闭通道。选择性 TEARDOWN 按 streamID 定位，省略 streamID 时选择全部 Type 130 端点。关闭一条通道保留其他通道及媒体，完整会话销毁也会关闭尚未建立连接的监听器。旧 poll 就绪事件和旧流 ID 不能作用于替换后的通道。

rtcp_receiver_send_data_stream 将包体复制到指定已连接队列，区分非法参数、无连接及队列耗尽，可在完整包体回调中调用。rtcp_receiver_get_data_stream_status 提供注册、手机提议、端点状态/ID/端口、终止错误及传输计数；已关闭计数保留到下一个 RTSP 控制器连接，停止的 Receiver 返回空闲计数并保留注册。关闭回调对每个已拥有端点仅调用一次，不能证明对端接收。Linux/RT-Thread 的 poll 容量现为 24，包含全部可选端点，启用时自定义端口也必须支持该容量。

加密 Host 端到端控制器独立派生流密钥，通过原生和兼容 Receiver 配置驱动真实 TCP socket，检查认证/初始 SETUP/手机提议门控、非法 features、三条并行双向通道、精确同步回复与回调响应、重复 SETUP、跨类型/媒体 salt 冲突、监听失败后 salt 消耗、新 seed 重开、旧 ID 及选择性/全部 Type 130 关闭、错误认证标签隔离，以及控制器重连时清理未连接监听器。交互使用模拟传输包体，不验证真实 Vehicle State/UI Sync 应用交互或真实 iPhone。

## Vehicle State Receiver 集成

将借用的处理器赋给 rtcp_receiver_config_t.vehicle，可同时安装两条 Vehicle State 通道的应用处理器。处理器仅接入一个 Receiver，保留到 Receiver 销毁，并由 Receiver 所有者串行访问。与任一车辆通道重叠的原始注册会被拒绝；独立原始 UI Sync 后端仍可使用。不提供处理器时保持 Classic 路由和回调，不分配车辆发送缓冲区。显式接入在堆上分配约 48 KiB 的有界发送缓冲区，不会声明 /info 能力。D213 配置尚未接入处理器。

认证后的 Type 130 请求进入 config/read/write/register 处理器，响应沿请求通道返回。每条通道可在传输队列之外保留一个编码响应。队列耗尽时保留原始响应字节，在 Receiver poll 中重试。如果该通道已有响应仍无法排队又收到新请求，则在处理新请求前明确拒绝并关闭通道。已提交的写入不会因随后断连而回滚；传输 ACK、应用响应和实际执行始终不同。畸形或不支持的应用消息会明确关闭该通道。

订阅变更编码为保留的 HIGH 通知。队列耗尽或 HIGH 无连接时继续保留以便重试，后续本地变更在处理器中合并，并跟在保留通知之后。选择性关闭会丢弃该通道的待发响应并增加放弃计数，但保留订阅及尚未排队的通知，供同一认证会话中的新 HIGH 端点继续发送。认证失效或 Receiver start/stop 清除订阅和全部保留消息，保留当前车辆值，并统计放弃的缓冲区。已被传输队列接管的字节不保证应用交付，不跨通道替换重放。

rtcp_receiver_get_vehicle_delivery_status 提供接入状态、待发响应/通知字节、已复制到传输队列的消息数、队列重试、请求拒绝、消息放弃和最后本地错误。计数保留到 Receiver 销毁。直接调用 rtcp_receiver_send_data_stream 向已接入的车辆通道发送会返回 ESTATE，由处理器独占输出；宿主改用 rtcp_vehicle_set 更新本地值。

专门的加密 Host 场景使用真实 TCP、Pair-Verify/ControlCipher 和独立模拟控制器，目录 ID 为测试数据。覆盖 config/read/write/register、事务回传、选择性关闭/重开后的 HIGH updateNotify、认证边界保留值并清除订阅、原始 UI 回调共存，以及 Classic 音视频交互。故障注入应用测试覆盖保留原始字节及顺序、队列满重试、第二次写入前拒绝、放弃计数和畸形消息失败。这些属于 Host 测试，不代表真实 iPhone Vehicle State 互操作通过。

## 测试与验收

SDK Host 测试在 ASan/UBSan 下覆盖 LIVI configRequest 字节样本、负事务编号回传、目录结构与权限、缺失值、订阅、数值界限及步长、宿主拒绝、按 IID 返回部分写入错误、字符串复制、回调时机、输出失败回滚、通知重试/合并/重置、未知控制命令、截断及确定性畸形输入集。显示测试覆盖几何及触摸映射，元数据测试确认默认仍为 Classic 且没有额外可选能力。这些测试不是 iPhone 互操作证据。

2026-11-15，上述参考版本的原始 LIVI 源码在 Ubuntu 24.04/WSL、Rust/Cargo 1.100.0 和 OpenSSL 3.0.13 下执行 cargo test --locked -p livi-cp --lib，179 个通过，失败/忽略/过滤均为零，包括 15 个 info、27 个 Vehicle State、一个 UI Sync 综合测试及四个 iAP 隧道测试。[选取的原始输出与环境](../evidence/livi-cp-host-20261115.txt)保留测试身份及范围，源码和 Cargo.lock 未改动，构建输出位于源码目录之外。Windows 尝试无法编译 livi-base 的 Unix 文件权限 API，因此没有在 Windows 执行参考测试。成功命令不包含 livi-core 的 theme_catalog 测试，后者保持 NOT_RUN。参考 Host 通过不能证明 D213、真实手机或完整官方 Ultra 验收。

L1 仍为部分完成，Vehicle State Receiver 集成已有加密 Host 覆盖，UI Sync 基础仍需实现并通过测试。注册 Type 130 传输已有加密 Host SETUP 覆盖，尚未由真实手机验证。对当前新增实现，L2 融合、L3 真机 Ultra 协商、L4 真机 Vehicle State/UI Sync、L5 Type 111 和完整官方产品验收均为 NOT_RUN。完整 Classic 回归由集成宿主另行维护。

## 后续实现

后续协议工作为由完整处理器门控的能力声明，以及二进制 plist UI Sync 状态/ACK。现有 iAP 隧道保持原样且仅接收。宿主可显式将 Vehicle State 处理器接入 Receiver，接入不会在 /info 输出 vehicleStateProtocolInfo。UI Sync 资产、帧合成、HEVC 支持、官方接入及认证仍是独立要求。D213 构建需要先确认真实图形和编解码能力才能声明对应能力。可选 Type 111 必须有独立 Sink，不能因单屏布局而隐式声明。
