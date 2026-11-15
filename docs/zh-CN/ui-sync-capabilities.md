[English](../ui-sync-capabilities.md) | [简体中文](ui-sync-capabilities.md)

# 单 Main Display UI Sync 实验协商

本显式开关将现有 UI Sync 应用处理器及复制目录接到元数据/SETUP 协商，是便携协议实验，不提供官方渲染器或完整 Ultra。面板字段及空 uiSyncInfo 值参考 Lasse Heitgres 的 LIVI 版本 64a05b68358db52ed887169143afd5944fd6f1f7 (https://github.com/f-io/LIVI)，署名要求见 NOTICE。D213 配置保持关闭。

## 宿主契约与元数据

宿主先绑定实际安装的显示/主题目录，构建 /info 时设置 airplay_info_plist_config_t.ui_sync。生成器要求目录的显示 UUID、像素和物理尺寸与 Main Display 一致，默认物理尺寸也按有效值比较。没有目录或尺寸不匹配时返回 false 和零输出长度。可选有界堆工作区避免把额外对象偏移表放入 Classic 栈；完整文档必须放进现有 6,144 字节会话上限。Vehicle State 元数据和音频能力可以在此上限内共存。

生成文档包含空 uiSyncInfo，且 displayPanels 只有一项：真实面板 UID、尺寸、空 properties，以及在原点、zIndex 为零并引用 Main Display UUID 的唯一 initialVideoStreams 项。displays 仍只有一个 Type 110。不会新增虚拟仪表、showsInstruments、displayPlugins、Type 111、altScreen 或 hevcInfo。布局/调色板/组件 ID 仍是处理器约束，本元数据不会传输或验证主题资产，也不证明完整产品所需的车辆 plugin/主题关系已成立。

SDK 使用现有不可变会话 /info 快照，因此显式提供的 UI 元数据在 SETUP 前可用。LIVI 在接受手机提议后增加 uiSyncInfo，并在存在 Vehicle State 时提供 displayPanels。SDK 刻意限定的单面板组合与较早提供元数据的行为仅有模拟 Host 覆盖，真实手机接受情况为 NOT_RUN，不能称为完整复现 LIVI 或官方规范。

## Receiver 协商门控

将借用处理器赋给 rtcp_receiver_config_t.ui_sync；仅在宿主实现拟提供的资产、渲染及呈现行为时，将 negotiate_ui_sync 设为 true。Receiver 创建要求已绑定目录及匹配 /info，每次接受控制器时重新验证刷新后的不可变快照。此单面板门控拒绝缺失/重复字段、标量/容器类型错误、尺寸/UID/UUID 不匹配、额外面板/显示/视频流、非零视频位置，以及虚构的仪表属性或显示插件。校验只能证明受支持形状的元数据一致，不能证明资产真实性、正确渲染或物理执行。

处理器接入 Receiver 期间，目录及其实现契约应保持不可变；宿主快照刷新必须维持该契约。其他面板/插件安排需要独立实现和测试，门控不能静默接受更宽的安排。全部调用仍由 Receiver 所有者串行执行，处理器保留到 Receiver 销毁。仅接入或绑定而未显式启用协商不会开启此能力。

初始 SETUP 只有在显式门控通过且手机提供 uiSync 时，才向 enabledFeatures 加入 uiSync。Vehicle State 使用独立开关，两个开关均为 false 时保留原有 Classic SETUP 响应字节。重复初始 SETUP 保留已接受的提议，每个新认证控制器重新协商。rtcp_data_stream_status_t.enabled 记录协商接受结果，不能证明手机执行。注册、传输提议、连接、应用 ACK 与实际呈现仍分别观察。

## 验证与验收

独立 Python plistlib 检查解码生成的单面板文档，重新编码不同字典/对象布局。65 项用例覆盖尺寸/类型/标识不匹配、缺失或额外面板/显示/视频流、虚构属性/插件、NUL 后缀标识及相关字段重复。单元测试覆盖所有文档截断和输出容量不足、未绑定/不匹配输入、默认物理尺寸、音频/Vehicle 共存、缺失处理器/元数据拒绝、仅 UI Receiver 快照接受，以及四种独立 SETUP 能力组合。

加密 --ultra-apps 场景使用实际 Receiver、Pair-Verify、ControlCipher 和 TCP，验证同时启用两项能力、重复 SETUP、UI reset/layout/ACK 和拒绝/重开、继续 Vehicle 交互，以及仅因刷新 UI 元数据缺失而拒绝新控制器。后续控制器未提议时，两项协议均不启用。其他 E2E 变体保留 Classic 与原始 UI 后端覆盖。目录和身份均为模拟数据，不使用 Apple 资产或私有身份材料。

独立 LVGL 融合演示尚不满足此宿主渲染契约，因此仍不注册、不声明。完整协议范围的 L1 仍为 PARTIAL，L2 物理融合/输入和 L3-L5 真实 iPhone/Type 111 验收仍为 NOT_RUN。官方资产/运行时、完整显示/plugin/车辆组合、呈现时钟、编解码/图形可行性及官方集成/认证仍需完成。参见[整体基础](ultra-foundation.md)、[UI 状态机](ui-sync.md)和[参考渲染研究](livi-theme-review.md)。

2026-11-15，完整 SDK Make 检查通过，启用 RTCP_SANITIZE 的 CMake 通过 66/66 项。Python 除 65 项元数据用例外，还独立解码了四种初始 SETUP 能力组合。以上仅为 Host 证据。

需要按顺序将手机选择交给渲染器的宿主，可在 UI 模型启用 queue_phone_choices，并在 Receiver 所有者线程消费复制 FIFO。每项本地 generation 应与最新模型状态比较，重置后作废旧宿主工作。取走记录不能证明渲染或呈现，可选队列本身不满足资产/渲染器/时序契约。
