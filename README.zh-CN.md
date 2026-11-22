[English](README.md) | [简体中文](README.zh-CN.md)

# rt-carplay

[![ci](https://github.com/boa-z/rt-carplay/actions/workflows/ci.yml/badge.svg)](https://github.com/boa-z/rt-carplay/actions/workflows/ci.yml)

## 项目概览

独立的 CarPlay Receiver SDK，提供 Linux Host 和 RT-Thread Port。协议核心和可移植 Receiver Runtime 无需 RT-Thread、Luban-Lite、ArtInChip 或 GUI 头文件即可构建。宿主通过公共 C 接口提供平台服务、身份与配对 Provider、媒体后端和输入所有权。

当前状态：可移植 Receiver API 和公共媒体注册已实现；D213 组合实现、板级适配和板级测试已归属 Luban-Lite 宿主。生命周期审查和完整 SDK 双语文档审查仍在进行。历史 D213 观察结果不代表当前重构已经实机验证。应用 Connection Manager 已迁入 Luban-Lite；USB、无线同时待命策略和 Launcher V2 属于后续任务。

## 功能

[Ultra 协议基础](docs/zh-CN/ultra-foundation.md)提供可选 Main Display 元数据、OPACK、有界 Vehicle State 消息处理器，以及具有显式 Receiver 注册、手机提议/认证门控和独立 Type 130 生命周期的认证双向 DataStream 传输。可选 Receiver 应用处理器现已支持 Vehicle State 和 [UI Sync](docs/zh-CN/ui-sync.md)，包含有界输出重试及认证会话重置。能力声明、实际渲染和真实手机互操作仍需分别完成，这些 API 不代表完整官方 Ultra 支持。

- AirPlay/CarPlay RTSP、Pair-Setup/Pair-Verify、加密控制与事件及媒体、H.264 和 HID 输出。
- 通过注入的传输服务提供 USBMUX、Lockdown、TLS 协议支持及无线 iAP2、MFi、Wi-Fi Handover。
- 独立的传输、认证、会话、视频、音频及输入就绪状态，明确的 Owner 线程生命周期和失败重试。
- 可注册的编码视频、LPCM/AAC/Opus 音频、麦克风、资源模式、Ducking、音量及 iAP2 回调，保持已有音频行为。
- 使用真实 Socket、密码学和生命周期的 ASan/UBSan Host 测试；可选板级适配位于协议核心之外。

## 架构

- `examples/rtthread_headless/` — 独立的普通线程 Receiver 验证示例，提供真实身份和控制器记录持久化、可选 mDNS 以及计数式 Null 视频 Sink；参见[集成与限制](examples/rtthread_headless/README.zh-CN.md)。

- `include/rtcp/`：公共 Receiver、平台、媒体、输入和事件契约，OS、网络、BT、MFi、Wi-Fi 接口及通用错误码。
- `core/mfi/`：从 Luban-Lite PoC 提取的 Apple MFi 2.0C 寄存器状态机，不依赖 RT-Thread。
- `core/iap2/`：可移植 iAP2 链路层、参数封装、Identify/IdentificationInformation、CarPlayStartSession/WiFiConfig 构建器、MFi 认证状态机及通过传输、MFi、Wi-Fi、AirPlay 回调连接这些模块的无线 Accessory 会话。移植自 `esp32-carplay-web-relay`（GPL-3.0-or-later，其自身基于 DiPlay），保留版权声明。
- `core/airplay/`：Binary Plist、RTSP Codec、TLV8、HID Plist 与描述符、Screen Codec、DataStream AEAD 解密、ControlCipher 流、Timing、Pair-Setup/Pair-Verify/SRP-6a、MFiSAP 响应器、`/info` Binary Plist 构建器、RTSP 会话（Info、SETUP、RECORD、Teardown）及持久化配对存储。上层为轮询推进的 ControlCipher Event Channel、UDP Timing、Screen DataStream 和音频 DataStream（类型 100/101/102，HKDF-SHA512 输出密钥，ChaCha20-Poly1305 RTP 数据报，12 字节头、16 字节 Tag、8 字节 Nonce 尾部）。`airplay_rtsp_server` 是单控制器 RTSP Server，拥有监听器、Pair-Verify 后的 ControlCipher 切换、会话请求的媒体端点及通过事件通道输出的 HID 报告，即触摸输入路径。
- `core/crypto/`：基于内置 orlp/ed25519（zlib）的 Ed25519 密钥、签名和验证封装，为 Pair-Setup/Pair-Verify 提供软件身份。
- `third_party/ed25519/`：未修改的 orlp/ed25519 源码及其 LICENSE。
- `core/media/`、`core/input/`：时间戳辅助函数和输入桥。
- `core/discovery/`：AirPlay Bonjour TXT 记录、`_airplay._tcp` mDNS Responder（与传输无关的数据包构建器，通过网络接口进行 UDP 轮询）及 Apple CarPlay SoftAP Vendor IE。
- `port/linux/`：POSIX OS 与 TCP 实现；桌面 Host 没有 Accessory 协议栈，因此 BT 和 Wi-Fi 接口为明确的桩实现。
- `port/rtthread/`：RT-Thread OS 与 BSD Socket 接口；通用 RT-Thread BT 接口仍为返回 `RTCP_ENOTSUP` 的明确桩实现，板级 Zephyr 传输位于宿主 `platform/`。
- `port/luban-lite/`：仅保留集成文档；D213 实现及实际源码回归属于 Luban-Lite 仓库。
- `Kconfig`、`SConscript`：RT-Thread/Luban-Lite 构建集成。
- `examples/linux_sim/`：下文 Linux Host 使用方式介绍的前台接收器测试程序，包含 mDNS、RTSP 和 Screen Dump。
- Luban-Lite 宿主在 `application/rt-thread/rtcarplay/` 下拥有 `app/` 中的 Receiver 组合、配对文件、USB 命令与 iAP2 Worker，以及 `platform/` 中的 Realtek、Zephyr、MFi、CherryUSB、MPP/DE、音频和触摸实现。保留已有协议时序与音频行为，SDK 不再编译板级实现。
- `tests/`：模拟总线 MFi 测试及使用 ASan/UBSan 编译的 Host 单元测试。端到端测试在环回网络上以真实控制器实现驱动 `airplay_rtsp_server`，覆盖 SRP-6a Pair-Setup、X25519 Pair-Verify、ControlCipher 切换、SETUP/RECORD、加密 Screen DataStream 帧及并发加密音频 DataStream 包（HKDF 输出密钥、ChaCha20-Poly1305 RTP，由 Sink 回调验证 PCM）。
- `check-boundary.sh`：平台 API 泄漏到 `core/`、公共头文件或可移植 Receiver Runtime 时失败。
- LVGL 启动页与应用 Connection Manager 属于 Luban-Lite，分别位于 `application/rt-thread/rtcarplay/ui/lvgl_launcher.c` 和 `app/connection_manager.c`。正式 SDK 构建不调用示例 SConscript。

## 支持平台

| 平台 | 实现与证据 |
| --- | --- |
| Linux Host | POSIX OS、网络 Port 和前台 Simulator，已通过真实环回配对、加密媒体与重连的 Host Tested 验证。BT、Wi-Fi Accessory 服务为明确的桩实现。 |
| 通用 RT-Thread | 提供 OS、BSD Socket 接口；BT 接口返回 `RTCP_ENOTSUP`。独立的 `examples/rtthread_headless` 组合实现不依赖 GUI 或板级服务，需要真实宿主熵源、Socket 和文件系统 Provider。 |
| D213 Luban-Lite | 可选 Realtek、Zephyr、MFi、CherryUSB 及 MPP、音频、触摸适配。已发布集成具有 GUI/Headless 交叉构建证据；当前版本构建结果必须单独核实。 |
| 物理控制器与开发板 | 子系统文档记录历史观察。当前重构的视频、重连、设备生命周期及声学表现属于 Pending Validation，并非 Hardware Validated。 |

## 环境要求

使用 C11 编译器、Make 或 CMake 3.16+、POSIX Shell、pthreads 与 mbedTLS 2.28。公共宿主配置头文件也可作为 C99、C++11 编译。Host 默认构建软件 AAC、Opus、SpeexDSP。板级构建还需要宿主集成文档指定的匹配 BSP、RT-Thread 网络、硬件服务和工具链。

AirPlay 模块需要 `MBEDTLS_CHACHAPOLY_C` 与 `MBEDTLS_HKDF_C`。可获取固定版本 mbedTLS，或将 `RTCP_MBEDTLS_DIR` 指向已有 2.28 源码。开发板验收需要真实 iPhone 及合适的 Accessory 传输、硬件；Host 测试不证明物理配对或播放成功。

## 构建与测试

### SDK 版本与构建来源

SDK 的 SemVer 只在 [`include/rtcp/version.h`](include/rtcp/version.h) 维护。当前开发版为 `0.1.0-dev`。准备正式发布时，在该头文件更新版本并清空 `RTCP_VERSION_SUFFIX`，为发布提交打上对应的 `v` 前缀标签，例如 `v0.1.0`。标签与头文件版本不一致，或指向开发版时，CI 会报错。

```c
#include <rtcp/version.h>
#include <stdio.h>

void print_sdk_version(void)
{
    printf("rt-carplay %s (%s)\n", rtcp_version_string(), rtcp_build_git_sha());
}
```

两个 API 均返回静态字符串，支持 C 和 C++。通过 `rtcp/rtcp.h` 仍可访问版本宏。Make、CMake 和 SCons 嵌入 SDK 检出的完整提交号，作为 Git 子模块集成时也读取 SDK 自身提交。源码归档或 Git 元数据不可用时返回 `unknown`。Make 和 CMake 通过 Python 3 生成提交号头文件。SDK 版本、提交号和宿主固件版本及 OTA 策略各自独立。

### Host 构建

```sh
./scripts/fetch-mbedtls.sh
make -j8 check

cmake -S . -B build-cmake -DRTCP_BUILD_TESTS=ON -DRTCP_SANITIZE=ON
cmake --build build-cmake -j8
ctest --test-dir build-cmake --output-on-failure
```

Make 使用 `config/mbedtls_rtcp_host.h` 在 `build/mbedtls` 构建私有 `libmbedcrypto.a`；mbedTLS 通过获取脚本提供，不提交源码。CMake 使用指定二进制目录。Make 默认启用 ASan/UBSan，上述 CMake 命令显式启用。测试集执行 58 个测试程序及旧配置加密端到端变体（59 个 CTest 用例）、边界检查及不使用内部包含目录的公共头文件编译检查。适配器专用回归由对应宿主集成文档说明。

## 快速开始

执行 `make` 后，使用自己的 LAN 地址运行前台协议测试程序：

```sh
./build/rt-carplay-sim --ip 192.168.1.20 \
    --seed-file ./identity.seed --pairing-store ./pairings.bin \
    --dump-h264 ./screen.h264
```

Simulator 发布 `_airplay._tcp`，并以软件 Ed25519 身份提供 RTSP 服务。Seed 首次运行时以 0600 权限创建，删除它会轮换 Accessory 身份。协议测试程序使用固定 PIN（`3939`）。跨运行保留配对状态可测试 Pair-Verify 重连。进程在前台运行，直到 Ctrl-C。

## SDK 集成

包含 `rtcp/receiver.h`，以公共 `platform`、`media`、`iap2` 描述填写 `rtcp_receiver_config_t`。一个由宿主选择的 Owner 执行创建、启动、轮询、停止和销毁。跨线程请求在宿主排队，释放上下文前停止宿主 Worker，借用的 OS、网络、Provider 接口表必须保持有效至销毁完成。该契约不需要 GUI 或内部 RTSP 配置头文件。参见 [Receiver API](docs/zh-CN/receiver-api.md) 和[生命周期与所有权](docs/zh-CN/lifecycle-and-ownership.md)。

可选 `rtcp/receiver_compat.h` 转换器用于迁移已有低层 `airplay_rtsp_server_config_t` 配置。旧 `carplay_app_*` 包装层现属于 Luban-Lite 宿主；它不等于可移植 Receiver 的线程契约。

## Linux Host 使用方式

还支持 `--name`、`--model`、`--device-id`、`--pair-code`、`--port`、`--no-dump`、`--no-audio` 和 `--help`。默认声明音频，`--no-audio` 选择纯视频。H.264 保存为 Annex-B Dump，解密音频包仅计数、丢弃，没有 Host 播放设备。

Host 验证覆盖 `_airplay._tcp.local` 的 mDNS `PTR`、`SRV`、`TXT`、`A` 记录，Binary Plist `GET /info`，按角色区分的 `audioFormats`、`audioLatencies`，加密 RTP 与负载验证，SET_PARAMETER 音量回调，篡改帧拒绝及使用已保存配对重连。可选 Codec、采集能力会改变能力文档大小和格式位图，不声明不支持的 Codec。

## RT-Thread 集成

宿主应用调用 SDK 根目录 `SConscript`，并且只 Source 一次其 `Kconfig`。`port/rtthread/` 提供通用 OS、网络接口。Luban-Lite 宿主编译自己的组合实现及硬件后端，板级 Kconfig 符号与 SDK 开关各有归属。废弃的示例应用入口会报告明确迁移错误。参见 [RT-Thread Port](port/rtthread/README.md)、[Luban-Lite 集成](port/luban-lite/README.md) 和 [宿主迁移](docs/zh-CN/luban-lite-migration.md)。

## 视频、音频与输入接口

`rtcp/media.h` 注册借用的 Annex-B 配置、访问单元，附带发送端纳秒和到达微秒时间戳。宿主负责 H.264 解码、队列、DMA 与 Cache 处理、显示及 HMI 策略。视频回调为 NULL 时使用明确的丢弃 Sink。

音频回调保留 LPCM 小端 S16 或编码 AAC、Opus，以及 Codec、采样率、RTP 时间戳和流槽位元数据。Audio 为 NULL 时拒绝 Audio SETUP。麦克风启停、选择性拆除、资源模式、Ducking 和音量保持原契约。[音频子系统](docs/audio-subsystem.md) 记录格式、流矩阵，麦克风与 AEC 配置、Host 成本测量、Sanitizer 检查及开发板命令；[音频调试交接](docs/zh-CN/audio-debug-handoff.md) 说明冻结行为及尚未解决的路由、零流症状。

`rtcp/input.h` 提供 Owner 线程 HID、Siri 输出，Receiver 还提供隧道 iAP2 写入。宿主负责触摸读取、焦点、坐标转换及跨线程队列。视频后端不能强制隐藏宿主需要保留的 HMI。

## 开发状态与已知限制

Host 回归覆盖可移植 API、公共媒体元数据、真实 Socket、发现、持久化及生命周期回滚。具体能力证据和剩余硬件检查见 [Classic 能力审计](docs/zh-CN/classic-capability-audit.md)。

宿主负责 USB 配置选择和 CarPlay 会话，并使用独立的 cherryusb-ipheth 驱动。iPhone 音频路由和声学仍需在设备上分别验证。Android Auto、CarLife 和 CarLink 在 SDK 的范围之外。

## 文档

| 领域 | 指南 |
| --- | --- |
| SDK 契约 | [Receiver API](docs/zh-CN/receiver-api.md)、[架构](docs/zh-CN/architecture.md)、[生命周期与所有权](docs/zh-CN/lifecycle-and-ownership.md)、[Classic 能力审计](docs/zh-CN/classic-capability-audit.md) |
| 集成 | [RT-Thread](port/rtthread/README.zh-CN.md)、[Luban-Lite](port/luban-lite/README.zh-CN.md)、[宿主职责迁移](docs/zh-CN/luban-lite-migration.md) |
| 传输 | [无线启动](docs/zh-CN/wireless-bringup.md)、[蓝牙重连](docs/zh-CN/bluetooth-bond-reconnect.md)、[有线信任持久化](docs/zh-CN/wired-pairing-persistence.md)、[视频会话](docs/zh-CN/video-stream-session.md) |
| 音频 | [子系统](docs/zh-CN/audio-subsystem.md)、[协商](docs/zh-CN/audio-negotiation.md)、[调试](docs/zh-CN/audio-debug-handoff.md)、[测试素材](tests/fixtures/audio/README.zh-CN.md)、[依赖来源](third_party/audio/README.zh-CN.md) |
| 实验 | [Ultra 基础](docs/zh-CN/ultra-foundation.md)、[Vehicle State](docs/zh-CN/vehicle-controls.md)、[UI Sync](docs/zh-CN/ui-sync.md)、[能力门控](docs/zh-CN/ui-sync-capabilities.md)、[LIVI 主题研究](docs/zh-CN/livi-theme-review.md) |
| 贡献 | [代码格式](docs/zh-CN/code-formatting.md) |

`docs/` 下以英文指南为主，简体中文放在 `docs/zh-CN/` 下并使用同名文件。目录入口使用 `README.md` 和 `README.zh-CN.md`。修改时同步更新两种语言，并执行 `make check-docs`。检查工具复用 cherryusb-ipheth 的 Apache-2.0 实现。`docs/documentation-exceptions.json` 保留上游 Ed25519 署名例外，技术准确性和翻译含义另行审阅。

早期调试报告和已退役的集成说明可通过 `git log --all -- docs examples/rtthread_app` 和 `git show <revision>:<path>` 查阅。

## 许可证

**GPL-3.0-or-later**（架构决策 A1，于 2026-11-08 确认）。参见 `LICENSE` 与 `NOTICE`。

- 从 `esp32-carplay-web-relay` 移植的模块保留 GPL-3.0-or-later 版权和许可声明。
- 从 LIVI 转译的代码保留其要求的署名："Based on LIVI by Lasse Heitgres"。
- 本 SDK 仓库不分发 Realtek 二进制、厂商固件 Blob 和 Apple MFi 身份材料。

## 致谢

Based on LIVI by Lasse Heitgres (https://github.com/f-io/LIVI)。

iAP2 工作源于 `esp32-carplay-web-relay` 与 DiPlay，保留原始声明。Screen 实现记录 DiPlay 的协议行为；相关音频传输参考和 LIVI 署名保留于源码与声明。orlp/ed25519、mbedTLS、FAAD2、Opus 和 SpeexDSP 保留各自第三方许可。RT-Thread、Luban-Lite/ArtInChip 和 CherryUSB 提供宿主集成基础；cherryusb-ipheth 继续作为独立维护的驱动。
