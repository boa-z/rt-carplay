# LIVI 主题目录与渲染证据

[English](../livi-theme-review.md)

基于 Lasse Heitgres 的 LIVI 原版 `64a05b68358db52ed887169143afd5944fd6f1f7`，遵守 GPL-3.0-or-later 及 NOTICE。本研究补充 [Ultra 基础报告](ultra-foundation.md)，不证明 LIVI 或本 SDK 已实现官方 Ultra。

## 原版测试

2026-11-15，在 Ubuntu 24.04/WSL、Rust/Cargo 1.100.0 上运行原版源码，源码和 Cargo.lock 均保持干净，构建输出位于仓库外。[原始选定输出](../evidence/livi-theme-host-20261115.txt)记录了测试名称和结果。

| 命令 | 结果 | 覆盖 |
| --- | --- | --- |
| `cargo test --locked -p livi-core --lib projection::theme_catalog::tests` | 6 通过，349 筛除 | 模拟 package/styles 提取、缺少 cluster 的拒绝、主题 ID、模拟 JWT payload 目录选择、ZIP/deflate 辅助及本地缓存匹配。 |
| `cargo test --locked -p livi-ui-model --lib dashboard::layout::tests` | 1 通过，113 筛除 | 未知布局 ID 使用通用仪表加地图模型。 |

这是原先 179 项 livi-cp 测试以外的补充。筛选的测试不调用网络 fetch 路径，没有下载或纳入 Apple 主题包、专有资产、密钥或身份数据。这些测试不证明目录/资产真实性、真实包兼容性、渲染一致性、手机协商或呈现时序。

## 实际实现与边界

`native/crates/livi-core/src/projection/theme_catalog.rs` 的 `from_package()` 识别 centerConsole/cluster，要求存在 cluster，提取仪表布局 ID/显示名和调色板 ID，只保留包含 DCA_Map 组件的区域；中心屏布局故意置空。调色板提取只保留 accentColor 的明暗字符串。这是元数据目录提取，不能证明所有仪表图形/脚本、组件行为或呈现时钟已实现。

fetch 路径会请求目录、校验下载的公开根证书 SHA256、选择资产/版本，并通过 HTTP Range 读取 ZIP 内两个 JSON。`catalog_assets()` 在该函数内解码 JWT payload，没有应用层 JWT 签名校验；`inflate()` 在该处没有显式解压输出上限，所选 ZIP 辅助代码也不构成完整资产认证/校验流程。不能将 HTTPS 传输检查与元数据解析当作官方签名资产渲染器已经验证。本 SDK 没有复制该下载器或资产。

`ThemeId::parse()` 提取资产和数字版本，缓存文件名以二者命名；`cached()` 还要求已存主题的完整 ID 与配置一致。测试覆盖完整 ID 不匹配，但不能证明同资产/版本的不同 SDK 后缀可同时保存在不同文件。

`native/crates/livi-ui-model/src/dashboard/layout.rs` 的 `spec()` 只特别处理少数内置 livi 布局，未知或厂商 ID 默认落入通用仪表加地图模型；`pages()` 使用目录中的布局/调色板 ID 和强调色。原版测试明确验证了这个回退，因此“选择了某厂商布局 ID”不能独立证明该厂商完整本地图形已经运行。

`livi-core/src/projection/cluster.rs` 仅在配置主题 ID 与当前目录一致时生成 UI Sync 布局，将已知区域映射到地图组件。`livi-cp/src/stack/setup.rs` 在手机提供 uiSync 且存在主题显示名称时启用它，`stack/control.rs` 随后输出空 uiSyncInfo，并与 Vehicle State 信息一起添加 displayPanels。这些是参考行为和 Host 证据，不能直接当作完整官方实现的验收清单。

## 对我们实现的影响

便携 UI Sync 模型现在可复制有界目录，约束宿主和手机的布局、调色板、区域/组件选择，为后续真实宿主目录与能力组合提供接口。Host 测试明确使用模拟目录，绑定不会自行向手机声明能力。

D213 的 LVGL 速度、挡位、电量仍是独立的本地融合演示，不能因为布局标识或颜色与目录相符就称为官方 Ultra Local UI。完整目标仍需真实且获授权的主题/渲染器集成、图形与编解码可行性、呈现同步、正确的显示/plugin/车辆关系、真实 iPhone 互操作和适用的官方产品流程。本地仪表不会隐式声明 HEVC、Type 111、displayPanels 或 uiSync，未知要求继续明确记录。
