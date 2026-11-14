[English](README.md) | [简体中文](README.zh-CN.md)

# RT-Thread Receiver 应用

## 已退役模板与历史范围

此 SDK 目录不编译任何板级实现。Receiver 组合、平台辅助模块和 Launcher 现归 Luban-Lite 宿主的 `application/rt-thread/rtcarplay/` 负责。新集成使用根目录正式 SConscript 和[宿主迁移说明](../../docs/luban-lite-migration.md)；独立可移植示例为 [RT-Thread Headless](../rtthread_headless/README.md)。边界参见[架构](../../docs/architecture.md)。

下文参考记录保留早期调试步骤、能力、测量及开发板反馈，包括已被取代的说明。原布局使用 `runtime/rtthread/` 和 `port/luban-lite/app/`；`carplay_app.c` 将库组合为 Luban-Lite RT-Thread Receiver，可复用代码保留于 `core/` 和 `port/`。这些是历史位置和职责，不是当前 SDK 构建输入。下文旧 `examples/sdk_app/` 复制步骤和 Package/Application 双重所有权方式不得用于新集成：维护中的宿主拥有板级 Kconfig，并直接调用 SDK 正式入口。

对历史叙述的当前修正：宿主会持久化 USB 信任记录，不是仅放在 RAM；宿主已支持压缩音频及其现有诊断；当前触摸／首帧交接属于应用策略。下文 PCM-only/Opus/AAC 说明、初始独占触摸行为、Feature Mask 实验，以及带日期的仅构建或待复测报告，均描述各自阶段。更晚的开发板画面／触摸及音乐异常反馈记录于[有线认证](../../docs/wired-auth-investigation.md)和[音频参考审查](../../docs/audio-routing-reference-review.md)。它们都不构成维护中固件重新通过实机验收，也不宣称音频故障已修复。架构重构冻结现有音频协商和输出行为。

宿主通过 SConscript 提供需要的 ChaCha20/ChaChaPoly/HKDF 定义；SDK TLS 配置已启用 Poly1305。未检查当前配置前，不要照搬早期统一修改 `tls_config.h` 的指令。历史“三分钟信任等待”描述的是重试间隔总和，不是整个流程的实际时间上限；单次请求还可能等待最多 30 秒。

从 Luban-Lite 根目录运行当前开发板边界检查，从 SDK 根目录运行 Multicast 检查：

```sh
make -C application/rt-thread/rtcarplay check-lubanlite-demo SDK_ROOT=/path/to/luban-lite
make -C application/rt-thread/rtcarplay check-lubanlite-bonds SDK_ROOT=/path/to/luban-lite
# From the SDK root:
make check-rtthread-multicast
```

它们检查模拟边界，不验证物理授权、输出或 Panel 转换。下文带模式选择的演示是现有基线，不是后续同时发现连接的 Launcher V2 功能。

RT-Thread App 现将 Features 限制为明确使用的 CarPlay 能力集，并与实际 Audio Offer 取交集。启用压缩音频时，TXT 和 `/info` 均使用 `0x44540280,0x61`。编译时定义 `CARPLAY_APP_LEGACY_FEATURES=1` 可恢复此前较宽的 Mask。这是针对从不请求 Audio SETUP 的 iOS 27.3 手机进行的受控兼容比较，尚不是实机验证的路由修复。Audio Format、Initial Mode 和后端播放行为不变。参见[参考审查](../../docs/audio-routing-reference-review.md)。

| 步骤 | 行为 |
|------|------|
| Entropy | `rtcp_lubanlite_random()`（CE TRNG）替代 RT-Thread 的 NULL RNG |
| Identity | 从 `/data/rt-carplay.seed` 加载 Ed25519 Seed，仅生成一次 |
| Persistence | Controller Table 保存在 `/data/rt-carplay-pairings.bin` |
| Wi-Fi | 先按配置启动 SoftAP，再添加 Apple Vendor IE，最后启动 DHCP |
| Discovery | 通过 `mdns_responder` 提供 `_airplay._tcp`，TXT 来自 `airplay_txt_build` |
| AirPlay | 端口 7000 的 `airplay_rtsp_server`，提供 `/info` Plist |
| Audio | 解密 PCM -> Ring Buffer -> `sound0` 写线程（RTCARPLAY_AUDIO） |
| Input | Panel Reader（`RTCARPLAY_TOUCH_PANEL`）或 `carplay_touch` 将事件入队；Poll Thread 发送 HID Report |

每次启动用 CE TRNG 生成 Pairing PIN，并打印到控制台；产品应在显示屏上呈现。Poll Thread 拥有全部 Socket 和 Cipher，因此不会并发写 HID Report。

## SDK 集成

最快方式是 `examples/sdk_app/` 中的独立 App 模式：

1. 将 `examples/sdk_app/` 复制到 Luban-Lite 的 `application/rt-thread/rtcarplay/`，包括其中的 `Kconfig`、`main.c` 和 `SConscript`。
2. 在 `packages/third-party/rt-carplay` 提供仓库，Clone 或 Symlink 均可；也可在 Menuconfig 中将 `RTCARPLAY_DIR` 指向其他位置。此模式**不要**同时 Source Package Kconfig，因为 App Kconfig 自行定义 `LPKG_RT_CARPLAY_*` 开关。
3. 将 `examples/sdk_app/d21x_demo128-nand_rt-thread_rtcarplay_defconfig` 复制到 `target/configs/`，再运行 `scons --apply-def=d21x_demo128-nand_rt-thread_rtcarplay_defconfig`；若镜像步骤要求，应先构建匹配 Bootloader。也可在 `scons --menuconfig` 中选择选项。
4. 启用 `CONFIG_REALTEK_FULL_FNC_MODE=y`，使 `wext_add_custom_ie()` 可用，并将 `port/luban-lite/README.md` 所列四个 mbedTLS 定义加入 `tls_config.h`。

需要的板级选项：

```
CONFIG_AIC_WIRELESS_LAN=y
CONFIG_AIC_WLAN_REALTEK=y
CONFIG_REALTEK_FULL_FNC_MODE=y
CONFIG_AIC_USING_CE=y          # CE TRNG for the identity and pairing PIN
CONFIG_LWIP_USING_DHCPD=y
CONFIG_RT_USING_WIFI=y
CONFIG_RT_USING_NETDEV=y
CONFIG_AIC_USING_AUDIO=y       # selects RT_USING_AUDIO; registers "sound0"
CONFIG_RTCARPLAY_AUDIO=y       # default on when RT_USING_AUDIO is set
```

`CONFIG_AIC_AUDIO_EN_PIN_LOW`/`_HIGH` 和 SPK 选择属于 Board Target；demo128-nand-carplay 使用 `AIC_AUDIO_EN_PIN_LOW` 和 `AIC_AUDIO_SPK_1`。

Bluetooth（无线 iAP2 Transport，对 AirPlay Receiver 可选）：

```
CONFIG_LPKG_USING_ZEPHYR_BLUETOOTH=y
CONFIG_LPKG_ZEPHYR_BLUETOOTH_CHIPSET_RTL8733=y
CONFIG_LPKG_BT_UART="uart7"
CONFIG_LPKG_BT_H5=y            # H4 must stay off
CONFIG_LPKG_BT_CLASSIC=y
CONFIG_LPKG_BT_RFCOMM=y
CONFIG_LPKG_RT_CARPLAY_LUBANLITE_BT=y
CONFIG_RTCARPLAY_IAP2=y         # carplay_iap2 (requires RTCARPLAY_BT)
# CONFIG_LPKG_BT_ASSERT is not set   # see port/luban-lite/README.md
```

通过 D213 USB1 Host 的有线 CarPlay（阶段 1–3：USB、iAP2、IPv6 AirPlay）：

```
CONFIG_AIC_USING_USB1=y
CONFIG_AIC_USING_USB1_HOST=y
CONFIG_LPKG_CHERRYUSB_HOST=y
CONFIG_LPKG_CHERRYUSB_HOST_NCM=y   # phone's CDC-NCM function -> netdev "u1"
CONFIG_RT_USING_LWIP_IPV6=y        # link-local address CarPlayStartSession uses
CONFIG_RTCARPLAY_USB_HOST=y        # carplay_usb (app mode)
```

SDK CherryUSB 必须提供 `usbh_get_configuration_index()`；缺少此 Weak Hook 时，Stack 始终枚举 Configuration Index 0，永远不会选中手机 CarPlay Configuration。OSS SDK 分支在 `packages/third-party/cherryusb/core/` 携带该 Hook。

也支持 Package 模式：仓库位于 `packages/third-party`，Source Package Kconfig。SDK 自动构建库，App SConscript 跳过重复 Include。

## Shell 命令

```
carplay_start                 # wireless: SoftAP + mDNS + AirPlay, prints the PIN
carplay_start wired           # wired: IPv6 AirPlay on the NCM link, no AP/mDNS
carplay_ab single|chunked     # wireless A/B: ControlCipher reply framing
carplay_status                # running/stopped, display size, PIN, audio counters
carplay_touch down 640 240    # inject touch in display pixels (panel runs too)
carplay_touch move 700 260
carplay_touch up 700 260
carplay_stop

carplay_randtest              # CE TRNG self-test (two distinct 32-byte reads)

carplay_bt start              # Zephyr Classic RFCOMM bring-up (iAP2 UUID, echo)
carplay_bt status             # enable state, SDP/RFCOMM channel, link state
carplay_bt stop               # drop the link and stop accepting

carplay_iap2 start            # real iAP2 session over RFCOMM (needs carplay_start)
carplay_iap2 status           # serving/link/coprocessor/identification/MFi counters
carplay_iap2 stop             # stop the session and disconnect the phone

carplay_video status          # decoder counters, queue depth
carplay_video start 1024 600  # exercise MPP + DE without a controller
carplay_video play /data/sample.h264   # decode an Annex-B file
carplay_video stop            # release decoder; notify registered host policy

carplay_usb start             # arm the enumeration hook; plug the iPhone after
carplay_usb status            # device, selected configuration, interfaces
carplay_usb request           # re-send Apple 0x52, print bNumConfigurations
carplay_usb mux               # USBMUX -> lockdown 62078, GetValue, print reply
carplay_usb pair              # wired pairing + StartSession + carkit service
carplay_usb iap2              # iAP2 over the open carkit stream (ID + MFi)
carplay_usb iap2stop          # stop the wired iAP2 session
carplay_usb net               # NCM netdev state and IPv6 link-local address
carplay_usb unpair            # close the carkit service and lockdown session
carplay_usb stop              # disarm the hook (device stays configured)
```

`carplay_usb pair` 执行完整有线 Lockdown 序列：SetValue UntrustedHostBUID、GetValue DevicePublicKey/WiFiAddress、本地 Pair Record 生成（两个 RSA-2048 Key，预期需数秒）、Pair（手机显示信任对话框，命令每秒重试，最多三分钟）、StartSession、TLS、StartService `com.apple.carkit.service`，再连接报告的端口。成功后打印生成的 Host/System UUID、Wi-Fi Address 和 Service Port，并保留 carkit Service Stream 供下一阶段使用；`unpair` 关闭它。模块需要 mbedTLS 的 `MBEDTLS_SSL_CLI_C`、X.509 Create/Parse 和 RSA Key Generation，SDK 的 `ports/inc/tls_config.h` 已启用这些功能；也需要 CE TRNG（`rtcp_lubanlite_random`，USB-only 配置同样构建）。Pair Record 仅保存在 RAM，下次启动重新生成，因此手机会再次请求信任。

`carplay_usb iap2` 在 `pair` 保留打开的 carkit Service Stream 上运行 iAP2 Link。这是有线 Transport：IdentificationInformation 公布 `USBHostTransportComponent`（Group 16），CarPlay NCM Control Interface Number 从所连接设备的 Active Descriptor 读取；省略 Bluetooth/WirelessCarPlay Component 和 Wi-Fi Configuration Exchange；认证后报告 PowerSourceUpdate（0xAE03）。收到 CarPlayAvailability（0x4300）后，从运行中的有线 Receiver（`carplay_start wired`）取得 AirPlay Endpoint，即 NCM Netif 的 IPv6 Link-Local 和 RTSP Port，再用该地址发送 CarPlayStartSession（0x4301）。若 Receiver 未以有线模式运行，或地址仍为 Tentative，只记录一次并暂不发送 0x4301。`iap2stop` 拆除 Link；`unpair` 也先停止 Link。

有线调试顺序：`carplay_start wired`、`carplay_usb start`、手机插入 USB1、`carplay_usb pair`（信任对话框）、`carplay_usb iap2`。有线 Receiver 启动时绑定 IPv6 Listener，手机通过 0x4301 公布的 Link-Local 地址访问它，因此不使用 mDNS 和 `_carplay-ctrl` Browse，两者保持关闭。

`carplay_usb net` 报告有线 NCM Netdev：CherryUSB 是否注册它，以及 lwIP 是否分配 IPv6 Link-Local 地址。手机连接该地址；AirPlay Endpoint 存在后通过 CarPlayStartSession 公布。`tentative` 地址尚未通过重复地址检测，不得公布。CherryUSB lwIP Glue 将 Netif 命名为 `u1`，且所选 Configuration 必须暴露手机 NCM 功能；通过 `carplay_usb status` 检查。

`carplay_ab` 切换加密 RTSP Reply 的封装方式：`chunked` 为默认，采用 Apple 的 1024 字节 `kMaxMessageWriteSize` Framing；`single` 每个 Reply 一帧，明文最多 4096 字节。曾观察到 Controller 在 1166 字节 auth-setup Reply 后立即关闭；此开关可不重新刷机，在开发板测试 Framing 假设。它仅影响切换到 ControlCipher 的回复，即 Pair-Verify 之后。

`carplay_usb start` 必须在插入手机前运行：Configuration Hook 仅在 CherryUSB 枚举内部执行，而手机只有收到 0x52 Vendor Request 后才暴露 CarPlay Configuration。若当前快照不是 CarPlay Configuration，`status` 会明确报告，必须在 Hook 启用后重新拔插。

没有 Controller 时，`carplay_video play` 是调试入口：必要时启动 Sink，用 64 KB Read Window 按 Decoder Queue Depth 控制节奏，提交完整 NAL Unit，并打印 Decoder Counter。

## 触摸输入

启用 `CONFIG_RTCARPLAY_TOUCH_PANEL=y` 时（`RT_USING_TOUCH` 打开时默认启用），`touch_panel_lubanlite.c` 打开由 `CONFIG_AIC_TOUCH_PANEL_NAME` 指定的 RT-Thread CTP Device，demo128-nand-carplay 为 `gt911`。Panel Interrupt 禁用 IRQ，专用线程读取 Point Buffer，按 Panel Track ID 解析 DOWN/MOVE/UP，将驱动的 `range_x`/`range_y` 缩放到显示尺寸，再调用 `carplay_app_touch()`。Poll Thread 将队列转换为 HID Report，因此 Panel Thread 从不操作 Event Channel。缺少设备会记录 `[carplay-touch] touch device ... not found`，并保留 `carplay_touch` 用于合成输入。

CarPlay HID Report 携带两个 Contact（`INPUT_BRIDGE_TOUCH_CONTACTS`），因此第三根同时触摸的手指不会转发。Panel 与 Display 应使用相同 Board Setting 旋转：驱动先应用 Flip/Rotate/Crop，`input_bridge` 再叠加 CarPlay Rotation。

Receiver 运行时也可使用 `carplay_video start`/`play`，前提是 Controller 没有 Streaming。

`RTCARPLAY_AUTO_START=y` 则从 `main()` 调用 `carplay_app_start()`。

## 屏幕输出

`on_screen` 接收带 Sender Timestamp 的 Annex-B H.264，先 SPS/PPS，再 Access Unit。启用 `CONFIG_RTCARPLAY_VIDEO=y` 时（VE 和 DE 驱动可用时默认启用），应用安装 `video_sink_lubanlite.c`：

- Annex-B 数据拆分为队列中的 NAL Unit，每个 NAL 对应一个 Decoder Packet，与 SDK H.264 Decoder 一致。
- Sink 自身线程使用 `mpp_decoder` 解码，经 `aicfb` Video Layer 呈现 NV12 Frame，按 `AICFB_GET_SCREENINFO` 报告的 Panel Geometry 留黑边适配；DE Layer 使用 Panel Pixel。
- 相同 SPS/PPS 消息保留 Reference Chain；配置变化或 Decoder Error 后等待 IDR，并在恢复时重放缓存配置。
- Sink 不读取或修改 UI Alpha。宿主可在停止状态下注册 `carplay_video_sink_set_display_observer()`；首次成功呈现帧和静止停止会通知该策略。Luban-Lite Launcher 显式注册全屏 UI 策略，在交接成功前保留触摸所有权。具有持续显示车辆／安全 HMI 的宿主必须提供自己的合成策略。

自行传入 `config.on_screen` 会绕过板载 Sink。没有 `CONFIG_RTCARPLAY_VIDEO`，或缺少 `aicfb` 时，Receiver 保持 Headless，仅统计帧数。

AirPlay Display Mode（`config.width_pixels`/`height_pixels`）通过 `/info` 公布，必须与 Panel 一致。demo128-nand-carplay 为 1024x600；App-mode Kconfig（`RTCARPLAY_SCREEN_WIDTH/HEIGHT`）和 `carplay_start` 使用相同数值，Panel Reader 也把 GT911 Range 缩放到该尺寸。其他 Mode 仍可显示，Sink 会按 Panel 留黑边，但 UI Aspect 和 Touch Mapping 不再一致。

已知调试说明：视频内存来自 VE/DE Pool，所用 Frame Count 依赖 Controller H.264 Profile；首次实机运行应观察 `[carplay-vid]` 的 Decoder Reset 和 Error。

## 音频输出

启用 `CONFIG_RTCARPLAY_AUDIO=y` 时（`RT_USING_AUDIO` 打开时默认启用），应用将 `airplay_rtsp_server_config.on_audio_pcm` 接到 `audio_sink_lubanlite.c`，并在 `/info` 公布 PCM Audio，包括 Feature Bit 及 `audioFormats`/`audioLatencies` 数组。RTSP Session 在 SETUP 接受 Audio Stream Type 100/101/102，用 HKDF-SHA512 推导每流密钥，解密 ChaCha20-Poly1305 RTP Datagram。

Sink 有两个线程，这是有意设计：

- AirPlay Poll Thread 仅将每个解密 Packet 复制到 64 KB Ring 并唤醒 Writer，不阻塞等待 Codec。
- Writer Thread 拥有 `sound0`：首包时执行 `AUDIO_CTL_CONFIGURE`（Output、协商 Rate/Channels、16-bit），再以 `AUDIO_STREAM_REPLAY` 执行 `AUDIO_CTL_START`，按 aplay 数值等待功放 200 ms，随后调用 `rt_device_write()`。后续 SETUP 协商不同 Rate/Channels 时就地重配 Codec。

Ring Overflow（Codec 停滞）时整包丢弃并计数；`carplay_status` 打印 Packet、Replay Start、Dropped Byte 和 Write Error。缺少 `sound0` 时，应用记录 `audio sink unavailable` 并继续 Video-only；此时 `/info` 不公布 Audio，因此 Controller 不启动 Audio Stream。

Sender 也通过 SET_PARAMETER `volume: <dB>` 控制扬声器；0 dB 为 Full Scale，AirPlay 最低发送至 -144 dB。RTSP Session 解析参数，Server 转发至 `on_volume`，Sink 通过 Codec Mixer（`AUDIO_TYPE_MIXER` / `AUDIO_MIXER_VOLUME`）应用。[-45, 0] dB 线性映射到 Codec 0..100；低于 -45 dB 不可听。Writer 下次唤醒时应用，播放前到达的值则在 Stream 启动时应用。每次变化记录 `[carplay-aud] sender volume -<x>.<y> dB -> <pct>%`，`carplay_status` 打印最后请求的百分比。映射和 Codec Fade Curve 需实机调节。

`airplay_audio_pcm_cb_t` 交付 Signed 16-bit Little-Endian Interleaved PCM。Opus 和 AAC 不公布，SETUP 时拒绝，因此 Sender 始终协商 LPCM。

## Bluetooth Transport 与无线 iAP2

`RTCARPLAY_BT` 构建 `carplay_bt`，仅检查 Transport：启用 Zephyr Classic Host，经 SDP 公布 iAP2 Service Class UUID，并回显收到的 RFCOMM 字节。

`RTCARPLAY_IAP2` 默认启用，依赖 `RTCARPLAY_BT`，构建 `carplay_iap2`，在同一 RFCOMM Transport 上运行 `core/iap2/` 的真实 Session：

| 步骤 | 行为 |
|------|------|
| 1 | `carplay_iap2 start` 要求 Receiver 已运行（`carplay_start`），并取得唯一 Bluetooth Link 所有权 |
| 2 | 启用 Host，读取本地 BR/EDR Address 用于 IdentificationInformation，注册 iAP2 SDP Record |
| 3 | 接受一个手机，调用 `iap2_session_link_up()`，以 100 ms Tick 双向处理字节（`iap2_session_feed`、`iap2_session_advance`） |
| 4 | 用 `[iap2] ...` 记录 Identification、MFi、Subscription、Wi-Fi Configuration 和 CarPlay Invite Event |
| 5 | 观察 AirPlay Event Channel 状态，向 Session 报告转换，用于 Re-invite Pacing |

`carplay_bt` 与 `carplay_iap2` 互斥；应用跟踪 Link Owner。停止 Receiver（`carplay_stop`）也会结束 iAP2 Service。

iAP2 Session 支持构造 5 GHz Channel，SoftAP 默认 36，不再只限原 Relay 接受的 2.4 GHz 1–14。

`RTCARPLAY_MFI` 默认启用，依赖 `AIC_I2C_DRV`，加入 `application/rt-thread/rtcarplay/platform/rtcp_mfi_lubanlite.c` 的 2.0C Authentication Coprocessor。开发板 Apple 2.0C 位于 `i2c0` 的 `0x10`，PoC 已在实机验证，`0x11` 也应答。参考 Bluetooth 的 `HW_SECRUITY_I2C_ID`/`HW_SECRUITY_I2C_ADDR` 属性中 `0x6A` 地址属于厂商 RJGT204 Device Lock，不是 MFi Coprocessor。Bus/Address 由 Kconfig 的 `RTCARPLAY_MFI_I2C_BUS`、`RTCARPLAY_MFI_I2C_ADDR` 配置。创建 Session 时传入 Provider（`config.mfi = rtcp_mfi_lubanlite()`），因此可运行手机 Certificate/Challenge Exchange，并推进到 Wi-Fi Configuration 和 `CarPlayStartSession`。iAP2 Service 启动时 Probe；Coprocessor 不应答时明确记录，并退化为 Identify-only（`mfi = NULL`），而非让 Transport 路径失败。`carplay_iap2 status` 报告 Coprocessor `ready`/`absent`。私钥不离开 Coprocessor，仅使用其证书和签名。

Register Engine 和 Transaction Shape 已在 PoC 的 i2c0/0x10 2.0C 上验证：`core/mfi/mfi2c.c` 在 0x4E Probe、0x20 Sign、0x31 Certificate，ID 为 `0x00000200`。与真实手机的完整 MFi Exchange 仍需开发板验证；Security Level、Link Key Reporting 和 H5 Firmware/Baud 路径也一样，参见 `port/luban-lite/README.md`。AirPlay Receiver 路径独立于 MFi。

## 验证状态

- Host 协议测试在 ASan/UBSan 下通过，包括 Socket-level Pair-Setup/Pair-Verify、Screen DataStream、Encrypted Audio、SET_PARAMETER Volume，以及篡改帧后的完整第二个 Session。
- **构建验证（2026-11-08）**：此 Defconfig 下，Luban-Lite d21x 项目在独立 SDK Worktree 中编译、链接为 `output/d21x_demo128-nand_rt-thread_rtcarplay/images/d21x.elf` 和完整镜像 `d21x_demo128_nand_carplay_page_2k_block_128k_v1.0.0.img`（4.0 MB）。ELF 中验证了 `carplay_app_start`、`carplay_*` MSH 命令、`airplay_rtsp_server_start`、`rtcp_lubanlite_random`、`wifi_start_ap`、`wext_add_custom_ie`、`dhcpd_start` 及 mbedTLS ChaCha20-Poly1305/HKDF。仅构建，未执行。
- **Video Sink**：MPP H.264 + DE 路径已按固定 SDK API（`mpp_decoder`、`AICFB_UPDATE_LAYER_CONFIG`）通过构建验证。尚未解码或显示内容；首次带真实 Controller 的开发板运行才是验证。
- **Bluetooth iAP2 Transport（2026-11-08）**：`rtcp_bt_lubanlite` 已与 SDK Zephyr Classic + RFCOMM Host 和 RTL8733 H5 Driver 一起构建验证。ELF 包含 `bt_rfcomm_server_register`、`bt_sdp_register_service`、`h5_hci_init`、`bt_h5_vnd_update_controller_firmware` 及已注册的 `carplay_bt` 命令。仅构建；开发板 `carplay_bt start` 才是 H5 Firmware Download、SDP Record 和 Data Path 的首次验证。
- **无线 iAP2 Session（2026-11-08）**：`iap2_link_lubanlite.c` 将 `iap2_session_t` 接到 RFCOMM Transport。d21x ELF 包含 `carplay_iap2`、`carplay_iap2_start`、`iap2_session_create/feed/advance`、`iap2_identity_build_wireless_omitting`、`iap2_carplay_build_start_session`、`rtcp_bt_lubanlite_recv_timeout` 和 `rtcp_bt_lubanlite_local_bdaddr`。Host Suite（含新增 5 GHz Channel 用例）在 ASan/UBSan 下通过。仅构建；开发板应观察 `[iap2] link up` 和 `identification accepted`。
- **MFi 2.0C Provider（2026-11-08）**：`rtcp_mfi_lubanlite` 将 `core/mfi/mfi2c` 适配到 RT-Thread I2C API：`rt_i2c_master_send` 用于单次 Register Write，`rt_i2c_read_reg` 用于先写寄存器再读，`aicos_msleep` 用于 Poll Delay。ELF 包含 `rtcp_mfi_lubanlite_start`、`rtcp_mfi_lubanlite_ready`、`rtcp_mfi2c_probe/sign/read_cert` 和 `rt_i2c_read_reg`；镜像 4,012,544 字节，零构建警告。Transaction Shape、Register Address 和 2.0C ID 已在 PoC 实机验证；I2C 默认值为已验证的 i2c0/0x10，0x11 也应答，0x6A 是 RJGT204 Device Lock 而非 MFi。Provider 本身仅构建；开发板首次 `carplay_iap2 start` 应记录 `I/mfi: 2.0C at i2c0 0x10: id=0x00000200 ...` 和 `MFi certificate sent`，手机连接后应有 `MFi authenticated`。
- **Audio（2026-11-08）**：Audio DataStream 已通过 Host 端到端验证。`test_airplay_audio_stream` 覆盖 Format Table、Start Validation、真实 ChaCha20-Poly1305 Datagram 解密、篡改／短 Datagram 拒绝和 Control Port 排空；`test_airplay_rtsp_session` 覆盖 Multi-stream SETUP（一个请求内 Screen+Audio、Alt Stream、Key Reuse 503、Audio-only TEARDOWN）；Socket-level Pairing 测试经真实 Server 发送加密 Audio RTP Packet，检查解密 PCM 和 Format。Session 测试还覆盖 SET_PARAMETER Volume Parsing（`volume: <dB>` 值及 Clamp，忽略非 Volume 参数，GET_PARAMETER 不回调）。SDK 构建链接 `carplay_audio_sink_start/pcm/stop/stats/set_volume_db/volume_percent`、`airplay_audio_stream_start/poll/stop`、`sound0` 和 `RT_USING_AUDIO`，带 Touch Milestone 的镜像 4,030,976 字节，零警告。仅构建；Codec 尚未播放 Sample。首次带 Controller 的开发板运行应记录 `[carplay-aud] playback <rate> Hz <ch> ch 16-bit`，`carplay_status` 应显示增长的 Packet Counter；仅当 iOS 发送 SET_PARAMETER，移动手机音量滑块才会记录 `[carplay-aud] output volume -<x>.<y> dB -> <pct>%`。CarPlay 可将 Volume Control 留给车辆：用 `carplay_volume 30`、`carplay_volume 0`（静音）或 `carplay_volume`（查询）检查本地 Software Gain。0..100 使用现有 -45..0 dB 映射，100 为 Unity。Local Critical Alert 绕过娱乐静音。后续开发板反馈已确认启用 Microphone 时 Type102 音乐路由成立；当前 Playback 修复和剩余实机检查见 `docs/audio-routing-reference-review.md`。
- **Touch Panel（2026-11-08）**：`touch_panel_lubanlite.c` 打开由 `CONFIG_AIC_TOUCH_PANEL_NAME` 指定的 RT-Thread CTP Device，demo128-nand-carplay 为 `gt911`；按 Panel Interrupt 读取 Point，RX Indication 禁用 IRQ，Reader Thread 排空 Panel 后重新启用，将带驱动 Track ID 的 DOWN/MOVE/UP 转发到与 `carplay_touch` 相同队列。坐标从 `range_x`/`range_y` 缩放至显示尺寸。ELF 包含 `carplay_touch_panel_start/stop/points`、`panel_thread`、`panel_rx`、`rt_touch_read/control`、`touch_irq_callback` 和 `gt911_read_point`。仅构建；首次开发板运行必须记录 `[carplay-touch] gt911: <n> point(s), 1024x600 -> <w>x<h>`，且手机 UI 必须跟随 Panel。
- **实机验证参考对齐（2026-11-09）**：使用这块开发板上运行的较早 Luban-Lite 固件作为参考。Board Target Configuration 字节完全一致，只有预构建 `pack/bootloader.bin` 不同，镜像使用本地构建 Bootloader。随附 Defconfig 现跟随已验证的 Network Profile：lwIP 处理 Wi-Fi Burst 不使用独立 RX/TX Thread，256 Pbuf，RX 优先级高于 TCP Thread，16 KB lwIP/SDIO/MMC Stack，4-byte Alignment，4 KB WLAN Workqueue。此应用仅构建；Receiver 自身 Wi-Fi/RTSP 路径仍需实机运行。
- **首次实机运行（2026-11-09）**：镜像启动且 MSH 命令可用，但 `carplay_randtest` 失败；最初猜测 CE TRNG Completion Unit 3 导致超时。SDK 自身 TRNG 实现 `packages/artinchip/aic-authorization/authorization/rng.c` 使用 `hal_crypto_start_hash()`、`ALG_HASH_ACCELERATOR` Bit 和 `data.in_len = 4`；Port 现与其一致，失败时清零输出。随后复测通过（`carplay_randtest: OK, distinct output`）。
- **第二次实机运行（2026-11-09）**：`carplay_start` 在启动 Wi-Fi 时崩溃，表现为 `CPU Exception NO.4`、`mcause=4`、`mtval=0x108000177`、`mepc=rt_mutex_take+0x2c`、`gspi_dvobj_init: get wifi sdio function fail`。Seam 直接调用 `wifi_on()`，跳过 `rt_wlan_dev_init()`/`realtek_init()`，导致 SDIO Driver 未注册；与 `poc/carplay-oss` Phase-1 Report 记录的崩溃相同。Seam 现使用 Framework 路径（`rt_wlan_set_mode(RT_WLAN_DEVICE_AP_NAME, RT_WLAN_AP)` + `rt_wlan_start_ap_adv()` + `rt_wlan_ap_is_active()`），禁用 STA Auto-reconnect；App 仅在 AP Active 后读取 MAC，此前使用 Seed-derived Device ID。待复测。
- **iOS Screen Mirroring 不是支持路径（2026-11-09）**：`_airplay._tcp` TXT Record 公布 `features=0x5203a4e2,0x61`，Video（Bit 0）和 VideoFairPlay（Bit 2）清零，Stack 没有 `/fp-setup` FairPlay Handler。因此 iOS 在 Control Center Screen Mirroring 列表隐藏 Receiver；手机听到 mDNS 回复但不显示设备。CarPlay 通过 `carplay_iap2` 的 Bluetooth/iAP2 进入，不经 AirPlay Screen Mirroring。
- **尚未经实机验证**：Wi-Fi 启动、Vendor IE、真实 Controller 配对、Touch Input、CE TRNG 修复（Unit 已明确，待复测）、Bluetooth H5/RFCOMM、真实手机 MFi Exchange、iAP2 Session 和 Audio Playback 均需硬件运行。首次启动应视作 Bring-up，持续观察控制台。

使用 `RTCARPLAY_TOUCH_PANEL` 时，Luban-Lite LVGL Display Port 必须跳过自己的 `tpc_run()`。RT-Thread Touch Device 为独占设备；LVGL 打开后 Receiver 再打开会返回 Busy。此配置下 SDK 集成将触摸预留给 Receiver，Receiver 停止时不会动态归还 LVGL。`carplay_status` 显示 Panel Reads/Events、Queued Input 和 Event Channel HID Write。仅 HID Write 成功不能证明手机已响应触摸。

## 自动 LVGL USB／无线演示

启用 `RTCARPLAY_DEMO_LAUNCHER`，要求 LVGL v9 + USB Host + Video。D213 SDK Application Target 现启用它。SDK `lv_user_gui_init()` 必须在该选项下调用 `carplay_demo_launcher_init()`；main() 不再另行启动 Receiver。它与 `RTCARPLAY_AUTO_START` 互斥。

启动后，英文 Launch Page 默认 USB，启用 iAP2 和 Wi-Fi Port 时提供 Wireless Selector。页面展示连接指引、诊断和配对管理。插入已解锁 iPhone，若手机请求则接受 Trust/CarPlay。Worker 启动有线 Receiver，等待连接，配对，等待可用 IPv6 Endpoint，再启动 iAP2。首个解码帧前 Launch Page 保持可见，Input 始终由 Receiver 独占。没有新增第二个 LVGL Panel Reader 或 Touch Handoff。

Pairing/TLS 和 Teardown 从不在 LVGL Thread 执行。全部 UI 更新在 250 ms LVGL Timer 内进行，仅 Stage 变化时更新，无动画；Live Video Session 不持续刷新隐藏页面。拔线时，Worker 等待 iAP2 释放 carkit Stream，再关闭 Pairing 并停止 Receiver。已注册的 Host Launcher Policy 恢复保存的 UI Alpha，下一个 Receiver 等待新的手机连接。每次连接只尝试一次 Pairing/Activation；失败时页面要求拔线／解锁／重连。手机信任流程可使 Pair Operation 等待，不由 UI Timer 取消。

此演示构建禁用手动 `carplay_start`、`carplay_stop` 和会修改状态的 `carplay_usb` 命令，避免与 Owner Worker 争用。只读状态命令仍可用。禁用 Launcher Kconfig 并重建可恢复手动调试。Audio Stream Support 不变。实机验证必须覆盖首次启动、手机授权、首帧显示、物理触摸、拔线和重连；Host 测试不能验证 Panel/DE 转换。

```
make check-lubanlite-demo SDK_ROOT=/path/to/luban-lite
```

Wireless Mode 下，同一 Owner 先启动 AP/DHCP/AirPlay Receiver，再启动 Bluetooth iAP2。iPhone Bluetooth Setting 中选择 `rt-carplay` 并按 CarPlay 提示操作。请求 Numeric Comparison 时，对比六位数字并在两屏确认。Bluetooth 向 Wi-Fi Handover 可正常关闭 RFCOMM，这本身不结束 AirPlay Session。Launcher 要求 AirPlay Event Connection 和已解码帧均存在才宣告 Live。Bluetooth Startup 和 Pairing-to-first-frame 有界超时，等待手机本身没有。Mode Change 先等待旧 Owner 完成再替换 Receiver State。阻塞 USB Trust Operation 必须完成或超时，请求的模式切换才生效。

`RTCARPLAY_BT_PERSISTENCE` 启用 DFS Settings Backend 和 SDK Bluetooth Settings Handler。D213 Linker Script 必须保留 Static Handler Section。Bond 保存在 `/data/bt_settings.bin`；UI 报告存储错误。完整刷机清除 Bond，普通重启应恢复。Pairing Manager 可删除 USB Trust Record、AirPlay Controller 或全部 Classic Bluetooth Bond。Bluetooth Forget 通过 Host API 和 Settings Rewrite 保留 Local Identity，不直接删除活跃 Settings File。再次配对前，也应在 iPhone 忽略配件。

Host 测试驱动实际 Demo Worker，覆盖 Wired Rejection/Replug、Wireless Asynchronous Startup Failure、Mode Change、First-frame Gate、Handover、Pairing Manager Quiescence 和 Delayed Shutdown。UI 按 SDK LVGL 公共头文件编译，但测试不渲染。Persistence 和 Multicast 测试独立执行：

```
make check-lubanlite-bonds SDK_ROOT=/path/to/luban-lite
make check-rtthread-multicast
```

代码发现、设备测试矩阵和 Host 验证限制见 [wireless-bringup.md](../../docs/wireless-bringup.md)。

### CarPlay 音频阶段诊断

周期性能输出默认**关闭**。`carplay_perf on` 启用 `[carplay-perf]`、`[screen-wait]`、USB Performance/Probe 报告及每 120 帧的 Screen Frame 消息；`carplay_perf off` 再次静默。`carplay_perf` 查询设置。选择在 Receiver Stop/Start 间保留，重启恢复关闭。静默时统计仍累积；`carplay_status`、音频诊断、连接事件和错误仍可用。

离线 PCM/AAC 测试通过但音乐仍咔嗒时，用 `carplay_status` 检查各 Lane 的 Decoded Frame、Empty Output、Decoder Error、PCM Peak、Negative Timestamp、Inserted Silence、Mixer Drop 和 Jitter Counter。输出统计包括 Written Frame、Peak、Device Error 和 Maximum Write Time。Counter/Maximum 在本次 Receiver Run 中累积，应比较快照。Transport Counter 属于当前活跃 Stream，Stream 重建时可重置。

同一首播放曲目，运行 `carplay_audio_path mixer`，等 5–10 秒保存状态；再运行 `carplay_audio_path direct`，等待并保存状态；之后恢复 `carplay_audio_path mixer`。Direct 保留 Transport、Jitter 和 Decode，同时绕过 Timestamp Repair、Resampling 和 Mixing。独立 Stereo Type102 Track 按 Native Rate，通常 44100 Hz，使用 Software Volume 和 Media Focus Gain 输出。Call、Siri、其他音频及 Local Alert 强制 Mixing。`requested` 是请求路径，`last_output` 是最后实际写出的路径，空闲时可能滞后。Rate Change 可短暂中断声音。Direct 属诊断模式，Receiver 重启恢复 Mixer。

各 Lane Generation 最初八个 Decoded Packet 打印 Format、Payload Length、Timestamp Delta 和 Decoded Frame Count。与两份状态快照一起保存，并描述 Direct 是否改变听感。不打印 Raw Audio 或 Key。各 Lane Generation 还最多记录八次非零 Timestamp Gap，含 Expected/Actual Timestamp 和 Decoded Frame。诊断分段输出以适应 SDK Console Buffer。参见[调查记录](../../docs/audio-routing-reference-review.md)。

### 从 RAM 重放实际解码的音乐

Direct 音乐仍异常而离线 PCM/AAC 通过时，对比相同 Decoded Sample 在有无实时 Packet Delivery 下的表现：

1. 持续播放，选择 `carplay_audio_path direct`，Live 和 Replay 均使用 `carplay_volume 70`。约为 21% PCM Amplitude，接近离线测试默认 20%；CarPlay Volume 未设置时默认 Unity。记录仅降低 Volume 是否有帮助。
2. 运行 `carplay_audio_probe capture`，等待 `probe ready frames=88200 rate=44100`，或 48 kHz 下 96000 Frame。上限是两秒解码 PCM，因此慢速交付可花费超过两秒实际时间。用 `carplay_audio_probe status` 查询；需要时用 `carplay_audio_probe stop` 保留部分 Capture。Stream/Rate Change 会以已有前缀结束 Capture。
3. 在手机暂停音乐但保持 CarPlay 连接，运行 `carplay_audio_probe replay`。Capture 片段播放一次；重复命令可再次收听。最终 Buffer Drain 后出现 `probe replay done`。
4. 完成后用 `carplay_audio_probe clear` 释放内存，并恢复 `carplay_audio_path mixer`。

Probe 在 Stereo Type102 PCM 成功解码后、Timestamp Repair/Resampling/Mixing/Gain 前立即复制。按需最多分配 384000 字节，不写文件，不向其他地方发送 Capture Audio；Clear 或 Receiver Stop 时释放。Replay 使用与 Direct 相同的 Media Output/Gain 函数，按 Native Rate 连续写入 Capture Segment，不等待新 Packet。Replay 期间有意忽略 Live Media，由 `ignored_live` 计数；Pending Media 被 Flush，恢复前重置 Live Decoder。Call、Siri 和其他音频会中断 Replay。`capture`、`replay`、`stop` 和 `clear` 是异步 Writer Request；`status` 显示已完成／当前状态。Receiver 重启清空 Probe。

Buffer Replay 干净时，应重点调查 Delivery/Playout Timing 或 Starvation；Replay 异常则需检查 Decoded Content 和公共 Output Path。单个结果都不能证明某个组件有故障。短离线 AAC Tone 不覆盖真实音乐所用的每种工具。SDK Audio Queue Starvation 会输出零，但不增加 Sink `write_errors`。Packet Enqueue 为短 Queue Lock 最多等待两毫秒，按 OS Tick 取整；超时增加 `queue_busy` 和 `dropped_bytes`。更早构建在 Lock Contention 时立即丢弃，且未统计该损失。

### 离线音频测试页

LVGL Launch Page 在 READY 或连接失败后可点 **Audio tests**，不需要 iPhone。Session Owner 在 Test Worker 打开 `sound0`/`dmic0` 前关闭 Receiver，直到 Back 完成才恢复接收连接。GUI Callback 仅投递请求。Stop/Back 必须完成设备清理才允许 Receiver 启动；Stop Timeout 保留所有权并显示错误。

页面提供：

- **PCM tone**：直接输出 1 kHz Stereo S16，可选 44.1/48 kHz 和 Both/Left/Right；绕过 Network、AAC、Jitter Queue 和 CarPlay Mixer。
- **PCM silence**：同一 Output Path 输出零 Sample，用于区分随声音出现的杂音与 Digital Silence 下仍存在的咔嗒。
- **AAC tone**：短内置合成 997 Hz AAC-LC Fixture，通过实际 Decoder 在 44.1 kHz 解码，再直接送至 sound0。再次 Start 重放，不需要文件或手机。
- **Mic meter**：真实 dmic0 的 48 kHz Stereo Capture，显示输入 L/R Peak 和 Bar，不打开 Speaker。
- **Mic loopback**：真实 dmic0 Input 直送 sound0，应从低音量开始。
- Volume +/-（每步 10% 的线性 Test Gain，默认 20%）、Mute、Start/Restart、Stop、Back、Frame/Byte/Write Counter、Input Peak 和实际 Error Code。

每次最多运行 10 秒，AAC Fixture 更早结束。Mode/Rate/Channel 在 Start 时应用，Volume/Mute 播放期间生效。AAC 固定 44.1 kHz，Microphone Mode 固定 48 kHz；全部 Device Audio 为 S16 Stereo。不支持的 AAC/Microphone Button 禁用。Test Gain 独立于 CarPlay dB Volume Control。Output-only 测试不打开 Microphone。

Speaker Connector 只咔嗒时，先测 PCM Both 48 kHz，再测 44.1 kHz 和 Digital Silence。Direct Tone 干净而 AAC 失败，可缩小到开发板 Decoder/Codec Operation；PCM/AAC 干净而 CarPlay 失败，可缩小到 Network/Jitter/Mixer Integration。Direct PCM/Silence 仍咔嗒，则 Device/DMA/Clock、Amplifier 和物理 Speaker Path 仍需验证。仅 Connector 名称不能确定 Output Channel Mapping，也要比较 Left/Right。

Host 测试使用虚拟 Audio/Capture Device 和真实 AAC Decoder 执行实际 Test Worker，另覆盖 Launcher Ownership 和 Stop Timeout。不证明开发板可听输出或物理 Mic Capture。

AAC Stack 后续调查：开发板 PCM Tone 工作，但原 24 KiB Test Worker 在 AAC 时溢出。RV64 反汇编表明，仅 Caller 和三个嵌套 FAAD Frame 就至少需要 25,232 字节。Offline 和 CarPlay Output Worker 现均预留 96 KiB；大型 Caller I/O Buffer 改为 Worker-owned Workspace，不放 Stack Array。Offline 测试在完成时报告 RTOS Stack Watermark；CarPlay 每 200 Packet 及退出时报告。`used_peak/free_min` 使用 RT-Thread '#' 填充模式，只是已执行内容的观测，不是全部 AAC 输入的证明。Host Diagnostic 回归除 ASan/UBSan 外，现还强制实际请求的 Native Thread Stack。早期同步 Host 测试没有覆盖 RTOS Stack Budget。物理 AAC Playback 仍待复测。
