[English](../ui-sync.md) | [简体中文](ui-sync.md)

# UI Sync 协议基础

可选的 rtcp/ui_sync.h 处理器实现了有界 UI Sync 消息及状态基础，参考 LIVI 版本 64a05b68358db52ed887169143afd5944fd6f1f7 的 ui_sync.rs。Based on LIVI by Lasse Heitgres (https://github.com/f-io/LIVI)，署名要求见 NOTICE。处理器不提供主题资产、OpenGL 渲染器、帧合成器、呈现同步、官方授权或认证，仅接入不声明 uiSyncInfo 或 enabledFeatures；[单 Main Display 实验能力组合](ui-sync-capabilities.md)现有独立显式开关。D213 应用尚未接入该处理器。

## 宿主集成

使用实际 Main Display UUID 创建处理器，选择宿主主题目录提供的布局，并可设置深色模式。配置、布局、调色板及组件标识均复制保存。将借用的处理器赋给 rtcp_receiver_config_t.ui_sync，保留到 Receiver 销毁，并且只接入一个 Receiver。全部 Receiver 和处理器操作由同一所有者串行执行。NULL 保持 Classic 路径，不分配 UI Sync 模型工作区。SDK 不依赖 LVGL、MPP、DE 或 D213。

接入会注册 UI Sync Type 130 UUID，但仍要求 Pair-Verify、加密控制连接、初始 SETUP 成功和手机 uiSync 能力提议。与原始 UI 注册重叠会被拒绝；可与 Vehicle State 处理器或原始车辆后端共存。仅有注册和可运行状态机仍不足以向真实手机声明渲染器及主题目录，能力组合是独立门控。

allow_layout_change 是宿主显式策略，以布尔值返回手机。give_focus 默认关闭，启用时必须提供宿主实际 focus_token，限制为 INT64_MAX。不会把参考代码中的固定值 4711 当作通用协议 token。布局/外观请求及记录的手机选择本身不执行渲染或呈现。

## 消息与状态

完整解密包体为包含 displayUUID 和二进制 data 的 OPACK 字典。data 是含 type、ssn、psn、asn 的二进制 plist 头，命令 data 又是一层二进制 plist。显示 UUID 必须匹配配置。其他有界 OPACK 面板元数据会保存并回传，构造响应时不会丢弃。两层查询均拒绝重复匹配的路由字段。原有 Classic plist 查询行为不变；plist 类型查询现可正确区分布尔标记与 null。

Reset 递增本地会话，重置发送/接收/确认序号，保存可选标量手机 id，并以版本 2 回答。等待完成时重复 reset 会被忽略。当前会话的 reset-complete 协商版本 1 或 2，依次回答 reset-complete、发送 allow-layout-change、请求已选择布局，并可选交出焦点。版本 0 被拒绝，版本 1 的 reset-complete 不携带已保存 id。id 支持最多 255 字节 ASCII 字符串、最多 256 字节二进制、整数、布尔或 null；复杂及非 ASCII id 明确不支持。

Ready 状态按参考格式记录 start-layout-transition（0x03）、palette-change（0x10）、appearance-preference（0x13）的布局、调色板及外观选择。宿主启用后，request-focus（0x08）可产生 give-focus（0x07）。除 noAck 为 true 外，命令产生应用 ACK。未知命令计数并确认，不虚构行为。旧会话被忽略，已见过的命令序号仅确认，不重复应用选择。ACK 头跟踪对端接收序号及对已生成消息的确认，超出最后已生成发送序号的确认被拒绝。序号限制为 INT64_MAX，拒绝回绕。

布局请求采用 request-layout（0x05），包含显式浅色/深色、fadeInNewLayout/fadeOutOldLayout 及宿主提供的 activeComponents。rtcp_ui_sync_status 分别提供期望布局和手机选择。ACK 不能证明渲染器接受或呈现时刻。本基础未推断超时、重传窗口、主题真实性校验或帧时钟协议；序号及重复处理策略的真实手机兼容性保持 NOT_RUN。

## 输出责任与限制

模型最多保留四条各 8192 字节的预编码消息，reset-complete 可能使用全部四槽。rtcp_ui_sync_peek 借用最早输出，仅在传输已复制后调用 rtcp_ui_sync_consume 释放。存在待发输出时，新输入或宿主选择返回 ESTATE，尚未进行处理。编码/解析失败不提交协议状态或输出，但接收/拒绝计数仍可观察。实例在堆上保存有界模型、暂存状态、节点及输出，消息路径不构建逐请求堆树或整条消息的栈数组。

Receiver poll 在队列满后重试待发消息。如果保留输出仍不能排队又收到请求，则在应用选择前拒绝该请求，仅关闭 UI 通道。通道关闭、认证失效及 Receiver start/stop 重置 UI 握手、手机选择和待发输出，保留期望布局/深色模式及累计计数。新通道必须重新 reset/协商。模型统计已生成、已消费和已放弃消息，Receiver 发送查询统计复制消息、队列重试、拒绝输入及最后本地错误。向已接入 UI 通道直接原始发送返回 ESTATE。排队/消费不证明手机接收。

标识符为最多 64 字节的非空 ASCII 字符串，每布局最多八个不同活动区域。面板元数据最多 1024 编码字节。内部嵌套 plist 写入器最多接收 128 个前序节点、16 层及 16384 输出字节，支持 null/布尔/整数/ASCII 字符串/二进制/数组/字典，字典键必须是字符串。拒绝不支持的 real/date/UUID/非 ASCII 节点及非法树，支持扩展集合长度和八字节负整数。这些是显式基础限制，不是完整官方协议声明。

## 已实现的宿主目录

UI 通道协商前，宿主可通过 `rtcp_ui_sync_bind_inventory()` 绑定 `rtcp_ui_sync_inventory_t`，提供一个主题、面板、匹配的显示标识、非零像素/物理尺寸，以及各布局支持的调色板和区域/组件配对。接口将所有标识符和嵌套数组复制到可选的有界堆存储，调用方随后可以释放输入缓冲。`rtcp_ui_sync_get_inventory()` 返回借用的只读视图，在成功重绑、解绑或销毁前有效。未绑定模型保留原有协议基础行为，不分配目录存储。

上限为八个布局、每布局八个调色板、十六个可选区域/组件配对；标识符为最多 64 字节的非空可打印 ASCII。拒绝重复布局、同布局重复调色板及重复的完整组件配对。同一区域可有多个组件候选，但不能将其他区域的组件随意交叉组合。期望布局仍可只激活最多八个不同区域的子集。

仅在 IDLE 且无待发输出时允许绑定或解绑。替换目录必须支持已有期望布局、调色板和活动组件，失败保留原目录、期望状态及输出。已绑定时，宿主选择和手机的布局/调色板选择超出目录会返回 ENOTSUP，尚未提交状态或 ACK。目录与期望布局在通道/会话重置后保留。经过认证的请求被拒绝时，沿用 Receiver 的应用拒绝策略：仅关闭 UI 通道，保留其他通道，重开后重新握手。该策略的真实手机行为仍为 NOT_RUN。

Receiver 发送状态增加 `last_rejection`，与拒绝计数一起保留准确的应用/发送拒绝原因。后续通道关闭的通用错误可能覆盖 `last_error`，但不会抹掉这个原因，因此能区分队列耗尽与目录不支持。该值跨重置保留，直到下次拒绝或 Receiver 销毁。

目录是宿主实现契约，不是主题解析器、真实性校验器、渲染器或呈现回调。绑定不会生成 displayPanels/uiSyncInfo 或启用 uiSync。测试中的模拟标识不能当作已经实现的车辆主题向手机声明；独立的[实验能力门控](ui-sync-capabilities.md)要求已绑定目录及匹配 Main Display 元数据，宿主仍负责实际渲染/资产/呈现。D213 配置尚未绑定或声明此协议。[LIVI 主题补充研究](livi-theme-review.md)说明了参考元数据测试为何不能证明完整官方 Local UI 渲染。

## 验证与剩余验收

tests/check_ui_sync_wire.py 使用 Python plistlib 生成已保存的模拟输入字节，并独立解析输出 OPACK/plist。初始场景参考 LIVI 公开模拟 reset/layout 测试，使用不同的可配置焦点 token。测试覆盖版本 1/2、布局/组件/深色请求、手机布局/调色板/外观选择、焦点策略、应用 ACK/noAck、旧会话/重复消息、超前 ACK 拒绝、保留输出、重置保留、标量 id、重复路由字段、非 ASCII/复杂 id 拒绝、超大面板元数据、截断及确定性畸形输入集。plist 写入器测试覆盖 null/bool/signed/data/扩展数组、每一处输出容量截断、非法树及深度限制，Python 独立于 SDK 解析器核对输出消息。

加密 Receiver 场景通过真实 Host TCP/Pair-Verify/ControlCipher 同时使用实际 Vehicle State 与 UI Sync 应用处理器。检查四消息协商、精确传输同步 token、应用 ACK/noAck、阻塞 socket 后填满七个传输槽并保留第八个响应、应用第九个请求的布局前拒绝、新 seed 重开、错误认证标签隔离、车辆读操作继续工作及后续 Classic 音视频回归。保留一个未连接 UI 监听器，由认证会话销毁清理。这些均为模拟控制器 Host 测试，不是真实 iPhone 的 L3/L4 证据。

完整官方 Ultra 目标仍需经产品验证的 /info 能力组合、实际主题/显示目录和渲染、图形/HEVC/无线集成、呈现时序、真机协商及车辆/UI 交互，以及适用的官方集成和认证流程。参见[总体基础及 L0–L5 范围](ultra-foundation.md)。

目录单元测试覆盖嵌套数据复制、标识/尺寸/数量/重复边界、显示匹配、原子替换、准确的区域/组件候选、按布局限定调色板、宿主选择拒绝、独立编码的非法手机选择、拒绝时不提交状态/ACK 及重置保留。加密 Receiver 场景绑定模拟目录，仅拒绝并关闭收到未知布局的 UI 通道，保留准确 last_rejection，车辆读取继续，随后在新 UI 通道接受支持的布局/调色板。Python 独立解析 35 条输出。2026-11-15 该里程碑通过 SDK Make、CMake 65/65 及完整集成 Host 测试，均不是真实手机或渲染器证据。

独立 plistlib 消息还覆盖手机布局/调色板四个标识字段中的内嵌 NUL 字节。现有二进制 plist 字符串读取器会以 EINVAL 拒绝完整字符串，不能通过截断前缀匹配已安装标识。测试逐项确认拒绝后选择状态、收发/ACK 序号不变且无输出。本次补充回归证据，没有改变协议行为。
