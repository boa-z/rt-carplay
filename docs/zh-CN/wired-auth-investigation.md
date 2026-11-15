[English](../wired-auth-investigation.md) | [简体中文](wired-auth-investigation.md)

# 有线服务与认证调查

## 历史范围与当前集成

本文保留 v13 至 v19 的调查，以及各版本对应的测试数量和开发板反馈。这些记录不构成维护中固件重新通过实机验收的证据。USB Transport、MPP 视频、GT911 所有权和开发板回归现归 Luban-Lite 宿主的 `application/rt-thread/rtcarplay/` 负责；通用认证与协议处理仍归 SDK。参见[宿主迁移](../luban-lite-migration.md)。原复现章节的命令是历史入口；当前对应的宿主命令从 Luban-Lite 根目录执行：

```sh
make -C application/rt-thread/rtcarplay check-lubanlite-usb CHERRYUSB_ROOT=/path/to/CherryUSB
make -C application/rt-thread/rtcarplay check-lubanlite-video SDK_ROOT=/path/to/luban-lite
make -C application/rt-thread/rtcarplay check-lubanlite-touch
```

这些命令验证模拟的平台边界，不验证物理 USB 认证、解码器输出或触摸响应。下文后续协议／后端变更均为历史修复，不是架构重构中对已冻结音频行为的新修改。

## 证据边界

D213 v13 UART 证明以太网 DHCP、四次成功的网关 ICMP 回复、外部 Lockdown 激活，以及其中一次运行中的 TLS carkit/iAP2 会话。在该次运行中，手机发送加密 RTSP `/auth-setup` 请求，Receiver 返回 200，随后对端在媒体 SETUP 前关闭连接。另一次运行在完整 StartSession 和 StartService 回复后，更早地失败于 stage=service。

两者的故障边界不同。挂起的 Bulk IN qTD 或正在运行的本地 Receiver，都不能证明远端关闭的原因。确认下列软件缺陷不需要新的手机测试。

## 已确认的传输缺陷：超时后丢弃已完成读取

原有自持 URB Port 等待回调信号量。等待超时后，同步打印 UART/EHCI 快照，再次检查信号量。如果传输在此期间完成，代码虽然避免取消，却仍返回 ETIMEDOUT 并释放 DMA Buffer，既不复制收到的字节，也不保留已完成 URB 的下一个 DATA PID。

影响不仅是警告误导：USBMUX 重组器会丢失多路复用 TCP/TLS 流中的一个片段。后续读取无法恢复已从 USB Endpoint 消耗的字节。DATA PID 丢失还可能导致重复包处理。握手期间可能形成截断的 TLS Record 并最终超时；iAP2 期间则可能破坏底层 TLS 流。

一项回归使用官方 CherryUSB 头文件编译实际 Port，并从诊断 Hook 注入七字节完成事件。原 v13 Port 无法通过“读取返回七字节”的断言。修复后的 Port 在 ASan/UBSan 下通过：字节送达调用者，保留下一个 DATA PID，紧随其后的 Canary 不变。测试还覆盖零字节／错误完成、断连代次变化，以及取消失败后的隔离。

最后一次信号量检查与 EHCI kill 之间还存在第二个竞态：完成事件可能先释放 hcpriv，随后 kill 返回 INVAL。修复在 kill 失败后重新检查回调信号量，只有信号量确认时才将 URB 当作已完成；否则维持原有隔离机制。回归验证此竞态中的三字节能交付且没有泄漏。

这些结果证明真实传输缺陷存在，但不能证明它出现在所提供的 UART 运行中。日志没有捕获超时窗口内成功完成的传输。真正未完成的超时传输仍返回超时；修复不会捏造或重放部分收到的流数据。

## 空闲轮询与服务阶段诊断

iAP2 读取者通常每 200 ms 轮询一次，并明确允许无数据读取超时。V13 Port 每次超时都打印完整 EHCI 快照，在持有 Hubport 互斥锁时增加阻塞 UART 输出，可能干扰调度并扩大上述竞态窗口。Bulk IN 超时跟踪现需显式启用 `RTCP_USB_TRACE_IN_TIMEOUTS=1`；提交／完成错误及 EP0/OUT 超时仍记录日志。普通空闲超时仍返回调用者。Port 回归启用跟踪，以持续覆盖原有竞态。

名为 `service` 的 Session 阶段包含两个操作。诊断现分别在 USBMUX 连接前保留 `OpenServiceConnection`，在 TLS 握手前保留 `ServiceTLS`，并打印返回的服务端口和 SSL 标志。TLS 握手失败还记录原始 mbedTLS 结果及映射后的应用错误，不记录密钥、Record、证书或 TLS Payload。注入连接和 TLS 读取超时时，清理后仍保留正确操作、paired=1，以及最后一次成功的 StartService 交换。

## 独立认证／密码检查

已审查的参考实现：

- [DiPlay MfiSapAuthSetup.kt](https://github.com/shihabal3amri/DiPlay/blob/615b32ce0b40cd0b99ce94ba23a512001553684f/shared/src/main/java/com/shilapi/xcertplay/airplay/MfiSapAuthSetup.kt)，本地检出 `615b32ce0b40cd0b99ce94ba23a512001553684f`。
- [LIVI auth_setup.rs](https://github.com/f-io/LIVI/blob/870d26441e073cdb802c427c5ae8833dba694951/native/crates/livi-cp/src/auth_setup.rs) 及其 `crypto.rs`，本地检出 `870d26441e073cdb802c427c5ae8833dba694951`。
- usbmuxd `src/device.c`，本地检出 `3ded00c9985a5108cfc7591a309f9a23d57a8cba`。
- 官方 CherryUSB EHCI 取消实现，版本 `a0054795c44037ace8a2b77b6e610adbff2257ea`。

对于提供的 MFi 2.0C 请求，两份应用参考实现与本项目线上布局一致：配件 X25519 公钥、大端证书长度及证书、大端加密签名长度及签名。它们也一致采用 SHA-1(accessory public || phone public)、基于 SHA-1 的 AES-KEY/AES-IV 标签、小端 X25519 共享秘密字节，以及 AES-128-CTR。这些代码提供比较证据，不能证明与该手机兼容。

旧单元测试检查回复长度，并确认签名不是明显的明文，但没有独立解密回复。新增的可选 `tests/interop/test_auth_crypto.py` 加载实际 C MFiSAP/Control Cipher 代码，使用 Python cryptography/OpenSSL 模拟手机端，独立执行：

- 使用手机私钥和返回的公钥推导共享秘密。
- 检查 SHA-1/SHA-256 签名摘要，推导 AES Key/IV，解密返回的签名，并使用独立生成的 RSA 身份验证签名。
- 覆盖协议 2/RSA-1024 和协议 3/RSA-2048；修改后的签名必须验证失败。
- 解密 1166 字节、两帧的 RTSP 回复，检查长度 AAD 和逐帧 Nonce 连续性，再使用下一个 Nonce 解密后续回复。

全部通过。这能实质检查密码边界，避免仅由本项目两份代码对同一个错误达成一致。它不验证真实芯片证书链／授权、手机策略、实时发出的密文或 D213 运行时内存完整性。没有对 MFi 线上格式、Hash、签名顺序或帧大小策略进行推测性修改。

## 复现与构建

```sh
make -j8 check
make check-lubanlite-usb CHERRYUSB_ROOT=/path/to/CherryUSB
# Optional; requires Python cryptography and the host mbedTLS library above.
python3 tests/interop/test_auth_crypto.py
```

全部 48 个 Host 可执行测试及平台边界检查通过。实际 Luban-Lite USB Port 的 ASan/UBSan 回归和独立对端检查通过。D213 SDK 构建与该应用检出独立进行。

尚未解决的设备相关问题是：对端为何在认证回复后立即关闭。已确认的竞态已修复，但不能证明它解释了该次关闭。进一步定位需要修复后的实际服务／TLS 边界或设备／参考实现跟踪，而不是继续网关 Ping 或任意改变协议。

## V14 实机结果与 SDK AES-CTR 缺陷

提供的 v14 运行成功打开服务 TLS 流，完成 iAP2 MFi 认证／识别，并再次交换相同的六个 RTSP 请求。手机在 1508 ms 关闭，约在 auth-setup 回复处理完成后 25 ms：Body 1076 字节、明文 1166 字节、线上 1202 字节、两帧。IPHETH 持续工作：RX=187、TX=23、rxerr=1，TX/USB 错误为零。没有发生媒体 SETUP。因此，已修复的 USB 超时竞态并未被证实是该可重复 auth-response 边界的原因。

进一步检查发现，实际 SDK 通过 RT_HWCRYPTO_USING_AES 启用了 MBEDTLS_AES_ALT，却禁用了原生 AES-CTR。ALT CTR 函数仍选择 HWCRYPTO_TYPE_AES_CTR。CE 驱动的原生 CTR 分支为空并返回 RT_EOK。根据对齐处理路径，输出可能保持原状，或只是复制未改变的输入。这是实际加密路径中的明确故障，解释了为何本地加密前 RSA 验证可能成功，而手机无法验证加密签名。此前 Python/OpenSSL 检查使用软件 AES，成功结果没有覆盖该平台后端。现已明确补齐此验证缺口。

独立 SDK 修复按照 mbedTLS 标准 Counter/Keystream 机制，使用可工作的 AES-ECB 组合 CTR，并拒绝直接调用不支持的 CE CTR。实际 ALT Host 回归复现旧版静默成功，并在 ASan/UBSan 下使用 NIST 向量、签名长度输入、分段调用、Counter 进位／回绕及失败传播验证修正。

Responder 现会在生成回复前，用实际运行时后端检查 NIST SP 800-38A F.5.1 的首块，记录 `auth_setup=aes_ctr_check verified=1`。失败时返回密码错误，不再发送误导性的成功认证 Body。一项 Host 回归使用“无操作却返回成功”的后端编译实际 Responder，确认它拒绝该后端、不发回复，也不请求签名。MFi Port 日志还明确将签名前缀标为签名前缀；之前的 "challenge ... head" 文字实际上在成功时打印签名字节。

这识别并修复了与观测边界直接相关的加密路径缺陷。手机接受和媒体建立仍是硬件验证项目，不能从软件复现结果推断。

修正后的 SDK 和 Responder 一起通过完整 D213 固件构建验证。全部 49 项应用 Host 测试，包括后端拒绝，以及边界检查通过；独立 Python/OpenSSL 对端检查和原始／修补后的实际 ALT ASan/UBSan 测试也通过。这些结果证明后端修正和集成成立，不宣称下一次实机结果之前已在手机上成功建立媒体会话。

## V15 设备结果：认证已接受，实时画面停滞

在 `aes_ctr_check verified=1` 后，手机继续越过 auth-setup：初始 SETUP、Event Channel 连接、GET /info、RECORD、Screen SETUP（type 110）和 Screen TCP 连接均成功。手机发送 33 字节 H.264 配置，以及被 Screen AEAD 路径接受的加密帧。这证明在实际 AES 后端修复后，该次运行的手机已接受认证。此前第六个请求后的可重复关闭在这次运行中已解决。

POST /command 和 /feedback 仍收到 405，但后续仍进行 Screen SETUP。两者语义仍待解决，音频和触摸也一样。不能宣称完整可用的视频会话已建立。

Video Sink 又收到一份配置并连续重置两次。随后 MPP 报告负帧缓冲请求（-1867127296 字节）、分配 18 个帧缓冲失败，以及 Co-Located Buffer 失败。后续计数为 pending=15/13、NALs=14 不变、frames=8 不变、dropped=440 不变、errors=242/244；网络 Screen Receiver 达到第 720 帧。输入传输持续，解码进度却停滞。Sink 的八帧计数表示 MPP 返回的 Buffer，不是独立验证的可见画面。

使用实际 Sink 源码和两个 Slot 的 MPP 模型复现了两个缺陷：

* 一份配置中的 SPS 和 PPS 是两个队列条目，且都标记为在此前产生输出后重建解码器。为 PPS 再次重建会丢弃刚提交的 SPS。配置现以一个完整 Annex-B Packet 入队，由 MPP 内部遍历，最多重置一次。FIFO 满时拒绝整份配置，不再只接收一半。
* Worker 先耗尽 FIFO 再推进解码器。MPP 只有两个 Slot 时，第三个 Packet 等待的 Slot 只能由该 Worker 释放。Worker 现每次提交后都会推进解码器，并在重试 Slot 时继续推进。

SDK PPS 解析器还会解引用缺失的 SPS。独立回归运行实际 h264_nal.c：原版在 ASan/UBSan 下因空 SPS 访问失败；修补版解析器返回错误。Slice 解析拒绝缺失 SPS，Frame Manager 分配失败会向上传播，部分分配的帧缓冲不会在后续 Slice 中进入硬件解码。尚未捕获负数分配请求背后的精确硬件值；重置／缺失 SPS 序列解释了一条明确的无效状态路径，不能证明每一个观察到的分配数值。

成功的 Timing Sample 仍每秒处理一次，但最多每 30 秒打印一次。Screen SETUP 日志用 32 位片段打印 64 位数值，因为 RT-Thread Formatter 不支持之前的 `%llu` 用法，会把后续参数，例如绑定的 Screen 端口，打印错误。

验证：全部 49 项 Host 测试和平台边界检查通过。使用 SDK 公共头文件的实际 Sink 回归在 ASan/UBSan 下覆盖原子配置、输出后重置、FIFO 溢出、Packet Slot 饥饿和渲染背压，全部通过。两项原版 Sink 用例均按预期失败。实际 SDK PPS 解析器回归通过，原版则因空指针失败。完整 D213 构建通过。V16 设备视频进展和可见输出仍待验证。

```sh
make check-lubanlite-video SDK_ROOT=/path/to/luban-lite
# In the SDK checkout:
packages/artinchip/mpp/tests/run-h264-missing-sps.sh
```

## V16 可见视频与 V17 触摸所有权修复

用户刷入 v16，确认 CarPlay 画面可见。认证、Event SETUP、RECORD 和 type-110 Screen 会话仍成功。此时报告的故障转为触摸：启动仍显示 `cannot open gt911`，随后是 `touch panel unavailable rc=-1`。已证明视频可见；音频以及入站 /command 或 /feedback 处理仍是独立未解决事项。

配置的 SDK 启用了 LVGL v9、GT911 和 RTCARPLAY_TOUCH_PANEL。LVGL 的 lv_port_disp_init() 调用 tpc_run()，在 carplay_start 前已打开同一个 GT911。rt_hw_touch_register() 用 STANDALONE 注册设备，第二次 rt_device_open() 因而返回 -RT_EBUSY。Touch Class 自身的 open 函数返回成功；所有权检查是明确冲突路径。重命名设备或再加一个 I2C Reader 都无法解决。

启用 RTCARPLAY_TOUCH_PANEL 时，SDK 两个 LVGL Display Port（v8/v9）均跳过自己的独立触摸读取者。在此应用配置下，Receiver 是唯一 Owner，stop/start 之间也维持此规则。Receiver 停止时释放设备；LVGL 不动态重新取得所有权。其他 SDK 配置仍照常调用 tpc_run。构建出的 v9 Object 没有未定义 tpc_run 引用，证明选中的固件采用该路径。

Receiver 报告设备打开错误码，在安装 Buffer／Callback／Thread 时禁用 IRQ，就绪后显式启用。读取者生命周期在调度前标记，因此立即停止会等待读取者退出；启动失败清理未启动线程和设备。已有 DOWN/MOVE/UP -> input_bridge -> HID Event Channel 路径保留。Input Bridge Descriptor/Report 和 Event Command 机制已重新对照 DiPlay 的 AirPlayHid.kt 和 AirPlaySession.kt 检查。

carplay_status 现报告 Panel Reads/Events、排队的 Touch Events、满队列拒绝，以及 HID 发送成功／失败。HID_sent 表示本地 Channel 写入成功，不代表手机侧已处理触摸。计数属于调试遥测，不是原子化跨线程快照；queue_full 不包含锁争用造成的拒绝。不逐个打印输入事件日志。

实际 Panel 模块通过 Host ASan/UBSan 回归，覆盖 Busy Open、IRQ 就绪、带 Track ID 和坐标缩放的 DOWN/MOVE/UP 映射、读取者进入前立即停止、正常清理和启动失败。已有 input_bridge、Event Port、Control Port 测试和平台边界检查通过。完整 D213 构建通过。V17 Panel IRQ 交付和手机侧触摸响应需要下一次开发板运行验证。

```sh
make check-lubanlite-touch
```

## V17 实机基线：画面与物理触摸工作

纠正误刷另一镜像的问题后，用户刷入 v17，确认可见视频和物理触摸响应均工作。GT911 现以五点、1024x600 几何信息打开。认证、Event SETUP、RECORD 和 Screen SETUP 成功。这验证了触摸所有权修复。

剩余报告问题为严重视频卡顿。日志有 Screen Count 120，却没有该计数对应的耗时测量，因而不能据此推导源帧率或解码帧率。入站 /command 和 /feedback 仍收到 405。该次运行的 Timing Offset 日志超出 int 毫秒表示范围；仅凭这一行不能证明播放调度故障。性能修改前应保留 v17 作为已知画面／触摸可用基线。

第一轮性能跟进记录于 wired-video-performance.md。V18 移除有线每次 recv 的固定 Sleep，让解码器优先于 Producer 调度，合并已解码输出，并添加阶段统计。这些修改保留 v17 认证／触摸修复，不修改 IPHETH。

随后 V18 实机在第二次配置重置后立即暴露视频参考链故障。V19 缓存／比较配置，并在配置变化、解码失败或输入丢失后从 IDR 重新同步。证据、限制和集中开发板检查见 [wired-video-performance.md](../wired-video-performance.md)。该报告中的失败阶段不是认证或触摸。

## 集成后的有线邀请恢复

有线会话保留 CarPlayAvailability 响应，直到认证及有效端点允许发送。后续每次 availability 都可以请求新的响应，不能被之前成功发送的标志抑制。已经接受 AirPlay TCP 或建立 event 连接后，停止继续发出有线邀请。宿主在 `iap2_session_airplay_t` 中分别提供 `transport_connected` 与 `event_connected`，使启动恢复可以在媒体 SETUP 前停止。

`iap2_session_config_t.wired_reinvite_delay_ms` 可选地启用一次额外启动邀请，条件是尚未观察到连接。SDK 默认为零，即关闭。延迟及是否启用属于宿主兼容策略，不是 Apple 协议要求。端点暂不可用不消耗补发机会；成功补发或观察到连接后停止重试。无线节奏保持原有策略。链路 ACK 只确认 iAP2 链路交付，不等于 AirPlay 接受。

集成应用在 2026-11-14 的用户日志包含首次邀请 ACK、五秒快照仍无 TCP、一次补发及 ACK，随后建立 RTSP、配对/认证、event/screen 连接、视频可见及运行阶段。用户确认该次有线 CarPlay 正常。这支持启动恢复，但不能确定首次邀请未产生连接的原因，也不能证明持续性能或声学验收。同一日志中的互联网共享定时器池断言及音频时间戳缺口仍作为独立未解决问题保留。

## 可选协议观察

`iap2_session_config_t.observe_message` 是可选同步观察回调，用于收到的会话消息及成功排队的发送消息。消息体仅借用至回调返回；需要时复制有界元数据，不得保留指针、重入或销毁会话。发送通知表示已排队，不等于链路 ACK 或 AirPlay 接受。配置零初始化时保留现有流程。过滤、隐私与有界日志由宿主负责；USB 网络选择、NCM/NTB 帧、lwIP 及接收器身份策略不属于此协议 API。
