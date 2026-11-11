# v25：有线视频停顿的代码级验证

## v25 板端结果与 v26 后续验证

用户回传的默认 Carrier ON 日志把候选进一步收窄：

| 候选 | 板端结果 | 当前判定 |
| --- | --- | --- |
| USBMUX 共锁→Carrier→RX 延迟 | IN 持锁常见 201ms；EP0 wait 约 86–153ms，hold 0–1ms；Carrier/rearm 最大约 144–199ms | 阻塞机制已在板上出现，仍需 off/on 验证其对帧率的贡献 |
| 同步 TX 阻塞 | tx_urb 最大通常 1ms，tx_lock 最大约 1ms | 此次运行不支持它是主要瓶颈 |
| Screen owner 未读数据 | owner 最大间隔约 17ms，recv 最大约 1ms；partial gap 可到 690ms，未完成 body 可存留超过 1 秒 | 等待主要在 socket 数据就绪之前，不能单凭此区分手机/USB/lwIP |
| USBMUX 发送缺口 | failed=0、controls=0，没有重现 Expected110 | 此次不能归因为自动 ACK 发送失败 |
| IPHETH 接收错误 | 活跃窗口每 5 秒约 12–32 次 RX error，USB/TX error 为 0 | 提升为优先调查对象；原日志未区分格式校验与网络交付失败 |

RX error 是失败的 USB 完成块处理次数，不是丢失 Ethernet 帧数，也不能除以
rx_frames 当作丢包率。代码存在两条完全不同的路径：

1. `worker → ipheth_receive → ncm` 拒绝格式、长度、DPE 等；整块不交付。
2. `ncm → frame → usbh_ipheth_net_receive` 的 pbuf 分配、复制、netif input
   失败；此前帧可能已交付，但当前及后续帧不交付。潜在地丢失 TCP 段，导致
   重传和 Screen body 等待，尚未由抓包确认该链在此运行中发生。

复核 Linux `ipheth_rcvbulk_callback_ncm` 后发现本实现还检查 wHeaderLength、
wBlockLength、NDP wLength/next、1514 帧上限、重叠等；Linux 快照没有完全相同的
检查。这个差异提供具体调查方向，不足以证明手机发送了被误拒的合法格式。
不能仅为追求帧率而取消边界检查。缓存/HCD 损坏也可能导致格式校验失败。

v26 保留全部解析约束与原返回值，增加 `ipheth_receive_diagnose` 分类：
size、signature、header、block、table、range、frame、overlap、terminator、callback；
原 `ipheth_receive` API 仍可用。5 秒窗口仅在失败时打印非零原因和最后失败的
20 字节 NTH/NDP 头字段、DPE 索引/偏移/长度、已验证和已交付数量。
oversize 单独计数；callback_remaining 是回调失败时当前及后续未成功交付的
已验证帧数，不含格式失败块中无法可靠计数的帧。
RT-Thread adapter 另报 alloc/copy/input 失败，保留最后 input 返回码。
不记录 Ethernet/TLS 内容，不在 RX worker/ISR 打印，不新增 DMA/URB。

这版同时按用户要求整理调试输出：RT-Thread debug 默认关闭，逐条 iAP2 与 RTSP
收发降为 debug；USB 自动枚举保留配置选择而不再渲染完整描述符。可显式定义
`RTCP_LOG_DEBUG` 或手动 `carplay_usb desc` 取得细节。主机回归不编入固件，保留
边界、取消、拔插与 DMA 覆盖，不删除有独立保护作用的测试。

**先用现有 v25 做 Carrier off/on 即可，不要求为此重刷。** 如需判定 RX 错误，
使用 v26，保持同一版本内 on/off/on 和持续动态场景，跳过切换后的混合窗口：

* off 后 rearm 长尾消失且 RX error 同步下降、帧率恢复，再 on 可复现：支持
  Carrier 阻塞是主要诱因；尚需解释格式/交付错误如何产生，不能仅凭相关性定案。
* off 后 rearm 改善但错误/卡顿仍在：锁是已确认问题，另有接收损失/供应问题。
* block/table/header/frame 类错误：用打印字段与该手机的实际 USB 捕获核对格式；
  构造相同头的回归后单独修解析器，归属 cherryusb-ipheth。
* callback 且 alloc/input 非零：调查 pbuf 生命周期/内存或 lwIP input 返回路径，
  不先改 TCP window。不同计数器快照略有先后，跨窗可差一项，应跨多个窗比较。
* signature/range 错误：核对线上包与 DMA 内存；线上正常、内存异常才进一步支持
  HCD/cache 问题。不能仅凭失败原因认定 DATA PID 或缓存就是根因。
* off 后重挂快、RX 无错误但 body 仍长等：进一步观察 TCP 序号/ACK/窗口和 USB
  传输完成，区分手机暂停与 socket 之前的交付问题。

另有独立问题：首轮 carkit `rc=-1` 后自动重建收到 67 字节却没有 version frame，
物理重连后 Trust cache reuse 正常。这提示同一次附件连接内的 USBMUX 服务恢复
需要单独审查，不能据此删除配对记录；尚未确认该 67 字节的内容或断流原因。

v26 已通过协议 ASan/UBSan 与边界检查、USB DMA/取消回归、demo worker 回归、
IPHETH 解析原因与 callback 中途失败/剩余帧统计回归、实际 worker 分类快照/窗口
清零与原生命周期测试、CMake upstream EP0 回归，以及 D213 完整编译打包。
没有对 v25 未修改的 HCD 再次改动。v26 的板端分类日志和 Carrier OFF 结果仍待回传，
不能把构建成功或主机注入测试写成卡顿已修复。

以下保留 v25 初始代码审查与实验设计，候选可信度以上述板端结果为准。

目标是定位第一个长等待，保持v24的分辨率、公告30 FPS、VE/DE线程、TCP参数、单RX
URB、超时和取消规则不变。本版提供统计和Carrier单变量开关，不宣称已修复板端卡顿。

## 核对版本与实际编译路径

已fetch核对rt-carplay/main（本轮基线`f41f035`）、cherryusb-ipheth/main（`c4c9a00`）、
rt-thread的`origin/bsp/artinchip-d213-dev`（`7f3e174361`）及实际SDK `poc/carplay-oss`
（`1a3fd0c`）。没有切换rt-thread工作区分支或修改Zalink工作区。

镜像实际使用SDK内的`kernel/rt-thread`及`cherryusb-luban/generated`，不是另一个
rt-thread分支的全部文件。SDK CherryUSB官方快照是`a0054795`；实际EHCI包含既有
short-IN修复。因此不能只读未打补丁的HCD便重复认定“早期short IN不完成”为新根因。

SDK开启`LWIP_NO_TX_THREAD`、`LWIP_NO_RX_THREAD`；lwipopts还显式开启
`LWIP_TCPIP_CORE_LOCKING`和`LWIP_TCPIP_CORE_LOCKING_INPUT`。优先级包括TCP/IP 10、
IPHETH 12、视频14、receiver15、iAP2 20；优先级本身不会消除持锁等待。

## 候选排名与因果链

### 1. USBMUX共锁、Carrier控制阻塞RX推进：高概率机制，待板端因果确认

位置：`rt-carplay/port/luban-lite/rtcp_usb_lubanlite.c:dma_transfer()`约251行；
`cherryusb-ipheth/port/cherryusb/usbh_ipheth.c:worker()`约110行；SDK官方
`packages/third-party/cherryusb/core/usbh_core.c:usbh_control_transfer()`727行。

调用链：iAP2线程 → TLS read → USBMUX pump → Bulk IN → 持`hport->mutex`等待完成/
200ms超时。IPHETH worker每秒Carrier GET → `usbh_control_transfer`等待同一把锁 →
同步EP0操作。该worker同时负责消费RX completion、解析NCM和重挂唯一RX URB。

直接确认的结构问题是无数据Bulk IN也占用EP0公用锁，以及Carrier控制和RX推进共用
worker。不是“HCD在等这把锁才能执行RX”：在途RX可以完成，但worker可能无法消费/
重挂。Carrier即使在RX仍在途时开始，也可能在控制请求阻塞期间积下一个完成。

实际worker主机测试注入120ms Carrier耗时，期间完成独立RX，重现wake/rearm大于50ms；
off/on及原readiness、拔插、取消隔离回归通过。它证明机制，不证明板上Carrier确实慢。
一次200ms/秒的损失也不足以单独解释所有0.1FPS窗口，必须以实测等待总量验证。

### 2. 同步TX与lwIP core锁/RX解析闭环：高概率机制，待实测时长

位置：`cherryusb-ipheth/port/rtthread/ipheth_rtthread.c:transmit()`33行；
`port/cherryusb/usbh_ipheth.c:usbh_ipheth_send()`217行；SDK
`kernel/rt-thread/components/net/lwip/port/ethernetif.c`约490行的直接`eth_tx`分支。

lwIP发送 → adapter `n->lock` → driver `tx_lock` → timeout=1000ms的同步Bulk OUT →
EHCI完成等待。1000ms是上限，不是每次固定耗时，正常提交可能很快。

另一条路径：RX worker → `ipheth_receive` → `usbh_ipheth_net_receive` → `tcpip_input`
→ 在当前`LWIP_TCPIP_CORE_LOCKING_INPUT=1`配置中持core lock直接处理。如果产生ACK
并进入同步TX，RX解析也会等待，进而延迟RX重挂；其他socket/core操作可能一起等待，
延迟ACK和Window Update。新指标可测TX和解析/重挂的同步恶化，尚未证明板端真的慢。

### 3. EHCI前驱QH缓存写回覆盖硬件进度：模型已复现的缺陷，板端关联未知

位置：SDK官方`port/ehci/usb_hc_ehci.c:ehci_qh_remove()`158行，实际generated版本保留。
最新rt-thread D213分支的同名函数也有前驱QH cache-line clean。SDK开启descriptor
cache维护，且cache开启时`USB_NOCACHE_RAM_SECTION`为空，不能假设描述符实际不缓存。

unlink一个QH N需要修改前驱P的`hw.hlp`并clean整行；P可能仍在执行。链指针、current
qTD和overlay token共享cache line。硬件自CPU上次刷新后推进P，整行clean可能把P的
current/token/DATA toggle覆盖回旧值；屏蔽CPU中断不能停止DMA。

新增测试以独立CPU/DMA镜像调用真实`ehci_qh_remove`，复现前驱overlay覆盖；官方与
当前short-IN补丁版均有此风险。模型证明这个交错会破坏状态，不证明板上每次unlink
都触发。它可能造成USB成功而设备因重复DATA PID丢弃数据，需USB捕获证明关联。

v25不改HCD，以保持Carrier单变量实验。最终修复应使用有保证的coherent/uncached
描述符访问，或确认相关调度静止后再更新共享行，并审查IAA、取消和toggle读取。
只加一次invalidate仍存在invalidate与clean间的DMA竞态，不能算完整修复。

### 4. USBMUX自动ACK失败未传播：已确认错误传播缺口

位置：`rt-carplay/core/usb/usbmux_host.c:send_tcp()`16行、`handle_tcp()`65行。

实际单测让索引110的自动ACK发送失败，pump仍返回成功，下一包用111，已复现。
发送前递增本身不是充分错误依据：usbmuxd也先递增；关键是发送失败后继续使用会话。
在不知道是否部分发送成功时，不可盲目回退序号重发TLS字节。

v25只聚合记录attempt/failure以及失败索引/端口/flags/长度/返回值，不改变重试规则。
如果设备Expected110和host失败110对应，就建立了直接联系；如果host失败为0，则需要
继续检查短传输、DATA PID及HCD缓存。USBMUX是iAP2控制流，Screen是IPHETH TCP，不能
把两者视作同一逻辑通道，更不能直接宣布序号缺口就是视频根因。

### 5. Screen调度/分片及控制协议：证据仍不足

位置：`airplay_screen_server.c:handle_client()`151行、`screen_recv()`129行，
`airplay_rtsp_server.c:airplay_rtsp_server_poll()`及`rtcp_net_rtthread.c`的recv/poll。

代码按128字节header和body长度组装；v24追加recv前零超时检查就绪，没有发现此处
主动sleep数秒。assembly是分片等待/调度wall time，不是解密CPU时间。新增真实TCP
测试模拟body缺失1.25秒、owner每50ms运行，能区分partial gap和owner gap。

DiPlay `AirPlaySession.kt:handleCommand()`交listener后响应；LIVI
`stack/commands.rs`会对modesChanged更新模式、disableBluetooth发宿主事件；
`stack/feedback.rs`无audio时200，有audio时报告播放样本/时间戳。这说明405是功能
缺口，未证明它造成数秒TCP分片停顿。v25保持协议行为以免混入第二实验变量。

## 指标与测量边界

所有新增聚合打印由低优先级demo worker约每5秒触发。USB/driver窗口在短临界区
复制清零后打印，不持网络、USB、core或decoder锁打印。窗口接近但非原子全局同窗，
跨窗口操作在完成测量时记账。无新URB/DMA缓冲，没有改变超时、取消和DATA PID规则。

| 日志字段 | 定义 |
| --- | --- |
| usb-lock in_wait/in_hold/out_wait/out_hold | USBMUX等待/持有共享hport锁，ms |
| usb-lock ep0_wait/ep0_hold | 实际EP0函数内部Carrier GET锁等待/持有；hold含原有重试 |
| ipheth-time carrier | 整个Carrier调用，包括等EP0锁 |
| wake | RX ISR回调到worker消费completion |
| rearm | RX回调到下一次成功提交的开始，含错误恢复，排除等新数据的空闲 |
| parse | NCM解析及帧交付，含配置要求的同步lwIP/ACK处理 |
| tx_lock / tx_urb | 等driver TX锁 / 同步HCD提交全过程；不含adapter外层锁等待 |
| ipheth-perf submit/complete/frame/bytes | 窗口计数；错误为累计计数的窗口差值 |
| screen-wait owner_calls/ge100ms | 每次owner循环（即使Screen fd空闲）；本窗超过100ms间隔数 |
| partial_gaps/ge100ms | 仅未完成消息内的分片间隔及本窗超过100ms数，排除完整帧之间空闲 |
| recv_ge10ms | 就绪后的recv自身超过10ms次数，wall time含抢占/core等待 |
| pending_header/body_left/age_ms | 最近owner快照时的未完成消息状态 |
| mux-perf | 本会话累计失败和control数，A/B看增量 |

IPHETH时间报告为本窗n/avg/p95_upper/max；p95上界用0/1/4/16/64/256/1000/>1000ms桶
估算，最大值为本窗观测值。Screen的`max_total_us`是启动累计最大值，不能要求它在B
阶段下降。没有将IN空闲等待当成worker阻塞。SDK console buffer仅128字节，因此
本版拆短性能行，避免关键字段被截断；串口仍有少量扰动，低频不等于零开销。

## 回板验证：同一连接on/off/on

刷v25，解锁手机、首次Trust、自动进入CarPlay，等启动活动结束；保持地图持续拖动
或列表滚动，保存完整串口日志，标明动作起止。A/B中不要拔插或重新配对。

1. 默认Carrier **on，30秒**，收集稳定4–6组数据。
2. 输入`ipheth_probe carrier off`，重复相同动作 **30秒**。
3. 输入`ipheth_probe carrier on`，重复动作 **30秒**，检查现象能否恢复。

切换后第一组可能混有前一阶段分片/在途控制，跳过再比较。off只跳过初次查询之后的
周期GET，不强制carrier up；暂时保留最后链路状态，仅作诊断。USB拔插仍正常清理，
重连恢复默认on。命令可与自动LVGL demo一起使用。

| 观察结果 | 判定/下一步 |
| --- | --- |
| on时ep0_wait/Carrier与wake/rearm长；off时重挂和RX改善；恢复on又退化 | 强支持共享锁→Carrier→RX重挂链，优先拆错误共锁/控制与RX推进 |
| off时Carrier样本归零，rearm和RX基本不变 | 证伪“周期Carrier是主要瓶颈”，不排除其他USB问题 |
| tx_urb p95/max长，parse/rearm也长 | 优先同步TX闭环，再抓TCP ACK/窗口，不能靠优先级替代 |
| owner本窗ge100增长，RX wake/rearm短 | 查owner中的RTSP/event/HID发送或core等待，结合recv耗时 |
| owner及时、recv快、partial长、RX completion/bytes低但rearm快 | 数据供应在socket之前不足，USB捕获区分源端未发与HCD未交付 |
| host失败索引与设备一直等待的110相同 | 关联发送错误；下一版安全关闭/重建，不原地重发TLS |
| host失败0但设备继续报缺口 | 不能以send返回成功证明交付；查HCD/DATA PID缓存及USB捕获 |

本补丁不读lwIP私有PCB，所以没有宣称已测出TCP窗口/重传。若需进一步区分源端与
网络背压，要抓TCP序号/ACK/窗口/重传及USB事务，并与完成时间对齐。仅凭现有日志和
主机测试，不能判定板上唯一根因或第一处阻塞。

## 修复归属与验证结果

* rt-carplay：Bulk使用独立端点/方向序列化，避免长持EP0公用锁；保持generation、
  toggle、cancel隔离。自动ACK错误传播并安全重建会话，不盲目重试TLS。
* cherryusb-ipheth：若Carrier A/B成立，控制查询移出RX关键路径；若TX闭环成立，
  使用有界异步TX队列/独立owner，core锁外等待，保证DMA所有权、拔插drain和失败
  quarantine。不能提交异步URB后立即复用单个TX缓冲。
* CherryUSB/SDK：解决QH共享cache line一致性，审查调度静止、IAA回收及toggle；
  rt-thread分支有同源代码时同步修复。本次不改调度器或core-lock配置。

已通过：50项核心ASan/UBSan测试与boundary、实际USB DMA/取消/重连回归、实际demo
worker回归、IPHETH慢Carrier/慢TX/off-on与原生命周期测试、实际EP0锁wait/hold/重试/
时钟回绕、实际HCD缓存覆盖模型与原short-IN回归、D213完整构建。板端因果关系和FPS
收益仍等待上述单连接实验。本版不提前声称卡顿已解决。
