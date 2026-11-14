[English](../usbmux-validation.md) | [简体中文](usbmux-validation.md)

# USBMUX 协商审查与 D213 验证

## 历史范围与当前集成

本文保留协商审查及 v8 至 v15 开发板调查。下文版本相关命令、镜像身份、测试数量和硬件观测描述历史基线，不代表维护中固件重新通过验收。USB Transport、Lockdown 命令组合和开发板回归现归 Luban-Lite 宿主的 `application/rt-thread/rtcarplay/` 负责；USBMUX Framing 和 Lockdown 协议处理仍在可移植 SDK 中。IPHETH 仍是独立驱动。参见[宿主迁移](../luban-lite-migration.md)。维护中的 Port 回归从 Luban-Lite 根目录执行：

```sh
make -C application/rt-thread/rtcarplay check-lubanlite-usb CHERRYUSB_ROOT=/path/to/CherryUSB
```

此回归模拟传输完成／取消和 USB 状态，不证明物理重连、信任或网络稳定性。下文 v13 超时行为是历史记录，已由本文后续 v14 调查修正。调查时应保留手机已有信任记录。

## 源码参考

已审查 usbmuxd `src/device.c` 的 [3ded00c9985a5108cfc7591a309f9a23d57a8cba](https://github.com/libimobiledevice/usbmuxd/blob/3ded00c9985a5108cfc7591a309f9a23d57a8cba/src/device.c) 版本，涉及 `device_add`、`send_packet`、`device_version_input` 和 `device_data_input`。现有 GPL-3.0-or-later 实现依据这些协议行为更新，没有将 usbmuxd 源码纳入仓库。

初始 VERSION Packet 使用八字节 Mux Header，随后是三个大端 u32（Major、Minor、Padding），总长 20 字节。发出向量原本字节正确，但解析器错误地从偏移 16 返回 Body，启动代码也忽略接收结果。因此，`version/setup ret=0` 不足以证明协商成功。

usbmuxd 将 v2 SETUP 的 Mux Sequence 字段重置为 tx=0、rx=0xffff。后续帧递增 tx，并回显对端收到的 rx_seq 字段。这些字段与各 TCP 类连接的 Sequence/Acknowledgment 独立。此前本项目 SETUP 使用 tx=1/rx=0，后续帧始终写 rx=0。Host 现与参考源码行为一致，仅在接受完整版本 2 回复后发送 SETUP。缺少回复则超时，畸形回复失败，版本 1 被明确拒绝，因为此 Host 仅实现 v2 Framing。

底层 TCP Builder 将 rx_seq 初始化为零作为占位；有状态 Host 从协商后的连接级 Mux 状态填入该字段。

## 提供的开发板证据

此前镜像中，`carplay_usb mux` 报告 version/setup ret=0、Lockdown connect ret=-4。`carplay_usb pair` 也返回 -4、stage=pair，没有远端错误字符串。这不能证明 Pair 被拒绝，也不能证明已发送信任提示。pair 阶段在打开 Lockdown 流之前进入，因此包含连接打开失败。当时没有捕获能确定是否收到 VERSION 或 SYN 回复。

修改在控制台增加版本回复／对端信息、累计 RX 字节／帧数，以及待重组长度。这是有源码依据的协议修正和证据收集改进，不是已证实的硬件修复。配对保留在应用中；独立以太网驱动不变。

## 自动化检查

USBMUX 测试覆盖由源码推导的 VERSION 向量、分片回复、缺少／截断／畸形回复、不支持的版本 1、有效协商前不发 SETUP 或 SYN、同一 Host 对象幂等启动、初始 SETUP/SYN 序列，以及用不同 Sentinel 值验证对端 tx_seq/rx_seq 回显。已有 Stream、Reset、Overflow、Fragmentation 和 Optional Padding 用例继续启用。Host Make 构建使用 ASan/UBSan 和警告即错误选项。

## 开发板步骤

新镜像启动后，从手机未连接的状态开始：

```text
carplay_usb start
```

连接已解锁手机并捕获枚举过程，先测试只读 Lockdown 查询：

```text
carplay_usb mux
ipheth_status
usb_ehci_status 1
```

记录 `version_rx`、`peer`、`rxbytes`、`frames`、`pending`、Lockdown connect 和 GetValue 结果。预期协商结果是 version_rx=1、peer=2.x，并收到 VERSION 帧；这不意味着信任或以太网已就绪。

独立配对测试应拔插手机，建立新的设备侧 Mux Session，再直接运行 `carplay_usb pair`。两个 Shell 命令当时各自初始化 Mux 测试 Session；在同次连接上连续运行，不构成已有 Session 复用机制的测试。解锁手机，若出现信任对话框则接受。保留所有输出，最后记录 `ipheth_status` 和 `usb_ehci_status 1`。不要仅为收集此证据删除信任记录。

若 VERSION 本身没有回复，应先调查 USBMUX Bulk Transport，再归因于信任。若 VERSION 成功而 Lockdown SYN 没有回复，应比较 v2 Stream 握手和 Endpoint 流量。远端 Plist 错误或更晚的 Service Start 错误属于不同故障阶段。实际网络流量和重连稳定性仍需硬件验证。

## V8 结果与 DATA PID 生命周期修正

v8 开发板日志已证明收到完整 VERSION 2.0 回复：20 字节、一个已解析帧。全新连接的 `mux` 和 `pair` 测试随后都在打开 Lockdown 流（62078）时超时，RX 仍恰好为 20 字节／一帧，没有待重组数据。未证明发送了 Pair 请求或收到远端 Plist 错误。IPHETH 仍有一个挂起的首个 RX，没有帧；这不是信任拒绝的证据。一次拔插重新绑定 USBMUX 和 IPHETH 并复用 i0，但不能验证活跃流量下的重连。

随后检查官方 CherryUSB a0054795c44037ace8a2b77b6e610adbff2257ea，发现独立的应用 USB Port 缺陷：

- `core/usbh_core.h` 的 `usbh_bulk_urb_fill` 填写传输字段，同时保留 `urb.data_toggle` 供复用。
- `port/ehci/usb_hc_ehci.c` 的 `ehci_bulk_urb_init` 从 `urb.data_toggle` 初始化 QH Overlay DATA Toggle。
- EHCI 完成路径将下一个 DATA Toggle 写回 URB。
- `class/cdc/usbh_cdc_ecm.c` 等上游 Host Class 跨调用保留 IN/OUT URB，从而保留该状态。Linux/libusb/usbmuxd 则依赖底层 USB Stack 在用户态传输之间保留 Endpoint 状态。

本项目 Port 每次传输均分配并清零私有 DMA URB，因此丢失已完成的 Toggle。短 VERSION OUT 使用 DATA0 后，下一个 SETUP 必须使用 DATA1。重发 DATA0 可能被当作重复包 ACK，却不向设备消费者交付 SETUP。这与传输发送成功但缺少 Stream 回复的现象相容。它是明确软件缺陷，也是观测序列的合理解释；尚无总线捕获确认它就是硬件故障机制。

V9 在自持 DMA URB 之间分别保留 IN/OUT Toggle，读写时持有 Port 互斥锁。状态仅在 USB 连接或断开时重置，不在每个 Mux Frame 或 Shell 命令时重置。代次检查和临界区防止已检测出的过期完成覆盖重置状态。Control Transfer 保留独立 Setup/DATA 规则。此修复不修改独立 IPHETH 驱动或官方 HCD 源码。

实际 Port 回归模拟接收者的重复 PID 处理，验证 20/17/36 字节 OUT 传输的 DATA0/1/0、独立短 IN、完整包后接 ZLP、提交失败、无进展超时、状态重置和旧代次完成。仅恢复旧的每次传输 DATA0 行为，回归便在第二次传输失败；修复路径使用官方 CherryUSB 头文件，在 ASan/UBSan 下通过。已有对齐、边界、分配失败，以及取消失败／延迟回调用例继续启用。这是 Host 模型，不是电气／HCD 硬件验证。

USB 已部分推进后超时，HCD 取消路径可能不报告最终 Toggle；此修改不宣称能从这种情况透明恢复。诊断运行应使用新的物理连接，不要假定部分取消的流可安全恢复。

v9 使用上述开发板步骤：先收集 VERSION、Lockdown connect 和 GetValue，再在新连接上尝试配对。仅在外部 Session／就绪转换成功后评估以太网流量。若 Stream 打开后出现更晚的 Pair/SSL/Service 错误，应记录准确阶段；这是新证据，不再是同一 SYN 超时。

## V9 结果与非对称 RX Header 验证

V9 开发板证据不同于 v8：有效 20 字节 VERSION 回复后，又收到 36 字节。两个新连接测试均到达 RX bytes=56，但 frames=1，随后因 `frame desynchronised` 失败。额外的完整传输在 TCP 分派前被拒绝。未捕获其内容，因此尚不能确定是 SYN|ACK、RST 或特定第三字值。IPHETH 仍未完成首个 RX，不能据此认定配对被拒绝。

重新检查 usbmuxd 的 `device_data_input` 发现，它不要求对端 Header 第三个字等于 Host 发出的 0xfeedface Marker。本项目 RX Parser 和 Optional Padding 合理性检查施加了这种无依据对称要求，此前所有合成 Peer Builder 也使用 Host Marker。V10 将入站第三个字视为不透明值，仍保留已知协议、有界长度和协议专属最小长度检查。VERSION 严格保持 20 字节；TCP 至少要求 16 字节 Mux + 20 字节 TCP Header；SETUP 要求其对应字节；CONTROL 独立识别，仅记录 Type/Length，不转储内容，也不作为 TCP 交付。Host TX 仍使用 0xfeedface。

新增首帧诊断报告 Protocol、Length、第三个字及 Mux Sequence，再报告 TCP Source/Destination/Flags。无效 Header 仅记录已完整到达的字段后丢弃，不读取截断存储。日志不含 Plist/TLS/Pair Record Payload。成功 Header/TCP 日志限于 VERSION 后前两帧，目的是识别实际开发板回复，包括 RST 而非 SYN|ACK 的可能性，避免仅凭 36 字节长度推断。

独立 36 字节 SYN|ACK 向量测试不透明第三字 0、0x12345678 和 0xfeedface；各自均以总计 56 RX 字节／两帧达到 Stream 建立。独立测试保留未知协议和过短 TCP Header 的拒绝，以及接收 CONTROL 而不分派 TCP。这些是构造的回归向量，不是该 iPhone 捕获的字节。有源码依据的 RX 修正仍需开发板确认。

## V10 结果与带 Framing 的 GetValue 交换

提供的 v10 回复现已识别：Protocol 6、Length 36、对端第三字 `faceface`、源端口 62078、目标端口 1、Flags 12（SYN|ACK）。Lockdown Stream 建立返回 0。这确认此前对称 0xfeedface RX 要求拒绝了有效对端 Header。

在 `GetValue sent ... bytes=317` 后，手机发出 CONTROL Type 3，Payload 46 字节，没有 XML 回复。Type 3 在 usbmuxd 的 `device_control_input` 中是 Mux 级错误，不是 Lockdown Plist `Error`，也不证明信任被拒绝。旧诊断遗漏其中 45 字节文本，因此真实原因仍未知。V11 保留 Type/Count 和最多 64 字节经清理的可打印文本，在独立有界行上打印诊断；不转储 TCP、TLS、证书或 Pair Record Payload。与 usbmuxd 一样，不假定一个 CONTROL Packet 意味着所有连接都必须重置。

独立于缺失错误文本，确认 Smoke Command 有缺陷：`cmd_mux` 直接在 Stream 上发送 XML 文档，而 Lockdown 要求文档前有四字节大端长度。参考是 libimobiledevice 的 `src/property_list_service.c`、`internal_plist_send`，版本 fa0f79190142bc309307967c058f89c1b36eb6b8：先写 `htobe32(length)`，再写内容。本项目配对路径已使用带该 Framing 的 `rtcp_lockdown_exchange`，但 Smoke Command 绕过了它。V11 为 GetValue 复用同一函数，包括有界、带长度前缀的回复读取。错误请求已修正；没有实际 CONTROL 文本，尚不能证明它就是观测到的 Type-3 错误原因。

共享 Stream Reader 此前在一次成功 Pump 仅交付 ACK 或无关帧时也会返回超时。可移植 `rtcp_usbmux_connection_read_wait` 现等待此连接的字节，直到注入的单调时钟期限；命令和配对均使用它。初始 TCP 诊断包含 RX Seq/Ack 和前几个 TX Header，从而区分实际 Mux/TCP 错误与 Plist Framing 失败。

回归组合实际 Mux Host、Lockdown Exchange 与模拟对端：仅 ACK 的 Packet 和 CONTROL Warning 先到达，随后回复的四字节前缀跨 TCP 帧拆分。测试验证发出的前缀／文档、Stream Sequence 推进、正确回复提取、无错误超时及 Clock Wrap。独立测试检查 CONTROL 文本限长／清理／截断，以及空 CONTROL 清除旧文本。普通和启用日志的 ASan/UBSan 构建均覆盖这些路径。真实设备错误文本和成功 GetValue 仍需 v11 开发板运行。独立以太网驱动不变。

## V11 开发板结果：GetValue 成功

下一份用户 UART 运行确认 VERSION 2.0、来自 62078 的 SYN|ACK、seq=0/ack=1，以及成功的 Lockdown 连接。Host 在 seq=1 发送四字节长度前缀，在 seq=5 发送 317 字节 XML 请求。手机返回 365 字节 Mux TCP Frame，seq=1/ack=322，包含四字节长度前缀和 325 字节 Plist。`GetValue exchange ret=0`、`complete=1` 及解析出的 `ProductVersion=27.3` 确认带 Framing 的请求／回复交换成功。本次运行没有 CONTROL Type-3 错误。由于旧错误文本从未捕获，不能证明先前畸形请求对应某个具体 CONTROL 消息。

控制台截断长 XML 打印，但经过验证的回复长度和解析出的 Value 独立证明成功；控制台转储看似不完整，不意味着回复失败。继续测试不需要新固件。

IPHETH 仍报告 RX=TX=0、submitted=1/completions=0、ready=no/carrier=down。GetValue 成功不证明 Host 已获信任、Pair/StartSession 成功或网络就绪。下一步在同一镜像的新连接上运行 `carplay_usb pair`，接受任何信任提示，捕获完整 Stage/Error 输出及 `ipheth_status`、`ifconfig` 和 `carplay_usb net`。后续 carkit Service 错误必须与 Pair/StartSession 失败区分。对端 IPv6 通信及负载下重连仍未验证。

## V11 配对结果与请求级诊断

提供的新连接 Pair 运行建立 Lockdown，在 SetValue UntrustedHostBUID 后收到 400 字节 TCP Frame，之后到达累计 RX=1486 字节／六个 Mux Frame，再于宽泛的 `pair` 阶段返回 -4。此前首帧日志在这些后续回复之前停止，因此仅凭计数无法识别失败子请求。日志未证明 Pair、StartSession 或首个 IPHETH RX 成功。

V12 记录并打印 SetValue/UntrustedHostBUID、GetValue/DevicePublicKey、GetValue/WiFiAddress、Pair、StartSession 和 StartService 的操作名及请求／回复长度。Pair Record 生成有开始／结束标记；连接打开有单独操作名。失败清理后仍保留元数据，并区分 Transport Exchange 状态与远端 Plist Error。不完整的 Framed Read 仅记录已收／预期字节数，不打印底层 Plist、密钥或证书内容。

Shell 命令逐次读取超时由 5000 改为 30000 ms，与所审查 `src/property_list_service.c` 版本中的 libimobiledevice `property_list_service_receive_plist` 一致。Core 可配置默认值不变。此修改去掉比参考实现更短的等待，但不能证明观测到的超时源于 Pair 回复缓慢或用户交互。Transport 超时后不增加自动重试：部分消耗的带长度前缀消息，不能在同一 Stream 上安全地作为新请求重试。已有显式 PairingDialogResponsePending 处理保留。

超时注入测试确认，首个请求超时后保留操作名和正确零回复长度，释放资源，且仅关闭连接一次。拒绝 Pair 测试区分成功收到的远端错误回复与 Transport 超时。完整配对／Session／TLS 测试继续启用。下一步在 v12 新连接上直接运行 `carplay_usb pair`，接受手机信任提示，捕获所有请求／回复标记、最终 Operation/Stage/Error 和 IPHETH 状态。该诊断构建不改变驱动和 HCD 行为。

## V12 开发板结果：Pair 已接受，仍有间歇性不完整回复

第一次运行完成 SetValue、DevicePublicKey、WiFiAddress 和 Pair Record 生成，收到完整 316 字节 Pair Plist，并推进到 StartSession。按 Session 实现的检查，Pair 回复没有解析出的 Error，本地 `paired` 标志为真。StartSession 随后读到长度 354，却在 Body 完成前以 RTCP_EIO 失败。同时 EP0 日志包含原始 -12（USB_ERR_IO）和 -4（USB_ERR_NOTCONN）。没有完整 StartSession Plist、TLS 尝试或 Service Start 结果。不能称其为证书／TLS 失败。

第二次连接完成前两个 Plist 请求，但 WiFiAddress 只读到长度 335，便在 Body 超时。故障位置变化且前缀已到达，足以支持调查 Transport；不能据此认定某个特定 HCD、Cache、设备或配对原因。后续 IPHETH 快照仍无完成的首个 RX。仅接受 Pair 回复尚未产生可证明的网络流量。

V13 打印原始提交／完成错误、Endpoint、实际长度和 DATA Toggle。等待超时时，可选 SDK Weak Hook 在取消前、挂起 USBMUX QH 仍可用时，捕获已有只读 EHCI Register/QH/qTD 快照。取消后的普通快照无法显示该 QH。转储期间到达的完成回调会在尝试 Kill 已完成 URB 前被检查；调用者仍返回超时，不假定部分消耗的流可恢复。测试覆盖完成／转储竞态和取消失败后的存储保留。

USBMUX IN 请求限制为 16384 字节，与 usbmuxd `src/usb.h` 中的 `USB_MRU` 及 `src/usb.c` 的 `libusb_fill_bulk_transfer` 一致。已有 Mux 重组器处理续接；这只改变 USB Transfer Chunk Size，不改变 Framing 或 Plist 格式。应用 Port 测试检查 16 KiB 返回及其后的 Canary。有源码依据的限制减少普通 USBMUX Read 使用的 qTD 数量，但不是已验证硬件修复。IPHETH 的 64 KiB NCM RX 不变。没有新增 HCD 调度或 Cache 补丁。

在 v13 新连接上直接运行 `carplay_usb pair`，捕获直到最终 Operation/Stage/Error，以及任何 `USB completion error`、`USB wait timeout`、`EHCI before cancel` 和 qTD 输出。失败摘要现明确显示 `paired=0/1`。随后记录 `ipheth_status`、`carplay_usb net` 和 `usb_ehci_status 1`。不要清除信任记录，也不要在 StartSession Body 未读完整时假定 TLS 已开始。

## V13 开发板结果：激活和有线网络流量成功

该次运行完成 SetValue、DevicePublicKey、WiFiAddress、Pair、StartSession（354 字节回复）和 StartService（383 字节回复）。命令报告 Service Stream 已打开，Session/Service TLS 启用。IPHETH 报告 RX=79、TX=18、81 次接收完成、一次 Control 完成、ready=yes 和 carrier=up。DHCP 获得 172.20.10.4/28，Gateway/DNS 为 172.20.10.1；本地 IPv6 Link-Local 地址有效。存在一个未分类 RX 错误，没有 USB 或 TX 错误。这证明一组可工作的组合，不是受控证明 16 KiB Chunk Limit 单独消除了所有此前间歇故障。

保留镜像为 v13，SHA256 `72b542a84c1600799df01e696ebc017e46051a35a1cb2c5a019621799f3c989d`；其 Manifest 固定 application e168c31、SDK a6c3a34a4 和 driver 25792b9。继续连通性测试不需要新镜像。

### 后续有线应用运行

`carplay_start wired` 打开 IPv6 RTSP Listener。`carplay_usb iap2` 报告 TLS carkit 流上 Link Ready、MFi Authenticated 和 Identification Accepted。公布端点为 [FE80::DCB5:4FFF:FE73:DD03]:7000。客户端连接并交换三次 `/pair-setup`、两次 `/pair-verify` 和一次 `/auth-setup`。六次回复均在本地报告 Status 200；Pair Verify 后启用 RTSP 加密。本地 MFi 签名检查返回 verified=1，auth-setup 发出 1076 字节回复，随后对端在连接生命周期 1602 ms 时关闭。未显示媒体 SETUP 或 Streaming。本地签名验证和已发送 200 回复不能证明手机接受完整认证回复。该故障边界属于应用认证／Session 层，需要比较参考协议／回复后才能宣称原因。

`peer=254.128.0.0` 日志是格式化缺陷：server_accept 无条件将地址前四字节按 IPv4 打印，即使对端为 IPv6。它不是 IPv4 路由观测。此处未捕获完整 Peer 地址。

Endpoint-85 超时反复出现，同时 iAP2/RTSP 仍在推进。iAP2 Stream 接口明确将无数据读取超时视为可用的空闲链路。保留全部字节的 Active qTD，在没有 Bus Trace 时不能证明控制器失败或设备 NAK。当时 Port 转储每次超时，包括空闲轮询；除非伴随进展丢失或 Framed Exchange 失败，应将其视为诊断噪声。不要混淆 USBMUX Endpoint 85 和 IPHETH Endpoint 86，也不要据此推断手机 RTSP 关闭由这些快照导致。触摸设备打开失败是另一独立问题。

下一步保留可用 Session 打开状态，收集 `ipheth_status`、`ping 172.20.10.1`、`carplay_usb net` 和 `carplay_status`。保留随后 RTSP 连接附近的请求／回复元数据。负载下 IPv6 Peer 可达性、物理拔插和媒体启动仍是独立未完成检查。

### 更晚的 v13 运行：网关 ICMP 可用，但 Service 阶段超时

StartSession 返回完整 354 字节回复，StartService 返回完整 383 字节回复。命令随后以 RTCP_ETIMEDOUT 失败于 stage=service、paired=1。源码检查显示，该阶段包含经 USBMUX 打开返回的服务端口，以及其可选 TLS 握手。保留的 operation=StartService 和 last exchange ret=0 描述最后一次 Plist Exchange，不是失败的 StartService 请求。这些诊断未区分 Service Connection Open 与 Service TLS Negotiation；不要据此宣称证书失败或清除信任。仅凭 EP85 快照无法区分。

随后四次网关 ICMP 回复全部成功（2、1、1、1 ms，TTL 64）。IPHETH ready/carrier-up，RX=85、TX=16、rxerr=4，TX/USB 错误为零。一次 USB Completion 可交付多个 NCM Datagram。更晚运行的 USB Address 为 4，此前为 2，因此不同 Error/Frame 计数不能证明同次连接中的增长。评估 RX 错误时，应在同次连接上记录流量前后计数。

Receiver 仍运行，音频包数为零。本地 Receiver 运行不意味着远端 Service 或媒体 Session 已成功。

## V14 源码调查与有界传输修复

[有线认证调查](wired-auth-investigation.md) 记录 v13 超时后诊断中复现的数据丢失竞态、实际 Port ASan/UBSan 回归、修正，以及对 MFiSAP/Control Cipher 代码的独立 Python/OpenSSL 对端检查。若回调在诊断期间或取消失败前发生，已完成读取现正常交付，并保留下一个 DATA PID。空闲 Bulk IN 超时转储改为显式启用。Service Connect 和 Service TLS 失败保留不同 Operation 名称。

这是确定性复现的已确认源码缺陷。所提供 UART 未证明该竞态导致其 Service 超时或 RTSP Peer 关闭；这些硬件结论仍未确定。没有改变 IPHETH Framing、USBMUX 线上序列、TLS 版本、MFi 回复格式或 RTSP 帧大小策略。全部 48 项 Host 测试、平台边界、实际 Port Sanitizer 回归及独立对端密码检查通过。

## V14 开发板结果：Service 成功，认证后关闭重现

Service TLS 成功打开，iAP2 完成认证，但手机再次在认证回复后约 25 ms 关闭 RTSP，没有媒体 SETUP。随后调查识别出 SDK 原生 AES-CTR 后端静默不执行的缺陷，此前软件 AES 互操作检查没有覆盖该后端。源码链、确定性复现和独立 SDK 修正见[后端调查](../wired-auth-investigation.md#v14-hardware-result-and-sdk-aes-ctr-defect)。Responder 现会在回复前通过 NIST 已知答案检查验证实际 AES-CTR 后端。此修复不包含 Control Channel Framing 变更。

## V15 实机：认证后媒体传输已建立

该次运行打开 carkit TLS 端口 63567，认证 iAP2，并在运行时 AES-CTR 检查通过后完成 AirPlay auth-setup。手机继续执行初始 SETUP、Event Connection、/info、RECORD 和 type-110 Screen SETUP。Screen Receiver 持续到 720 帧。阻塞故障已转移到本地 MPP Sink（NALs=14、frames=8、FIFO pending=15/13、errors=242/244），表现为重复重置和无效帧分配。v16 中独立复现的 Sink 及 SDK Parser 修复见 wired-auth-investigation.md。这些修复无需修改 USBMUX 或 IPHETH；音频、Command 处理及可用显示仍未验证。
