# CarPlay 性能对照：Zalink、开源接收器与 v24 实验

本次以用户确认正常工作的 v23 为功能基线。目标先验证 1024×600、30 FPS
连续交互时的吞吐和延迟；静止页面的平均 FPS 不作为性能结论。
已知旧日志 4.3/6.3 FPS 缺少同时段 RX 和连续运动条件，不能据此确定瓶颈。

## 参考代码与可见范围

| 实现 | 本次阅读版本 | 用途 |
| --- | --- | --- |
| SDK `poc/carplay/zalink` | `22fdca3473de01076e35bf424f283ec7d9ff5571` | 同平台 MPP/GE/DE 接入，最有价值的平台对照 |
| [UxPlay](https://github.com/FDH2/UxPlay/tree/05b0275cb1b726592700e47a6e0307544b3d3a52) | `05b0275` | 广泛使用的 AirPlay 镜像接收器；视频接收和 GStreamer 管线组织 |
| [DiPlay](https://github.com/shihabal3amri/DiPlay/tree/615b32ce0b40cd0b99ce94ba23a512001553684f) | `615b32c` | CarPlay ScreenStream、Android 硬解码输入/输出分离、延迟诊断 |
| [LIVI](https://github.com/f-io/LIVI/tree/870d26441e073cdb802c427c5ae8833dba694951) | `870d264` | CarPlay 异步 screen proxy、流解析和媒体宿主分层 |
| [carplayd](https://github.com/lvalen91/carplayd/tree/4eb254499aa00f456aea91d00755cf150c7d18a4) | `4eb2544` | Pi/NCM 接收、IPC 和 GStreamer 显示分层的补充参考 |

SDK 工作区 `/home/boa/Documents/luban-lite-jc` 当前在 `poc/carplay/zalink`，
本次仅只读分析，没有切换分支或修改它的文件。其受版本控制的接入源码在
`application/rt-thread/helloworld/zalink/`；另一个未跟踪的
`application/rt-thread/carplay/` 目录不作为已提交实现的依据。

Zalink 协议核心为 `packages/third-party/zalib/zalink/lib/d21x/libzalink.a`。
可直接分析的是平台回调、队列、线程、显示和接口配置；不能从 socket 的 select
适配薄层推断闭源库内部的 TCP 窗口、解密效率或 RTSP/feedback 行为。
其他项目的 FPS 和低延迟参数也不能直接外推到 D213。

## Zalink 同平台实现的关键点

主要文件：`application/rt-thread/helloworld/zalink/src/zalink_video.c`。

1. **接收回调直接投入 MPP 包池。** `zalink_video_play()` 取得 packet 后复制一次，
   立即 put packet 并通知解码。不会等待 VE 或 vsync 完成。池满时释放主锁，
   每次等 5 ms，最多 40 次；因此回调仍可能阻塞约 200 ms，并非完全无阻塞。
2. **解码、显示各有线程。** `decode_once()` 驱动 VE；`render_once()` 取得图像后
   GE 旋转/复制，再交 DE 并等待 vsync。MPP 的 get/put frame 管理器内部有锁，
   生命周期另由 decode_lock/render_lock 串行化。不能在另一个线程仍访问 MPP 时
   销毁 decoder。
3. **显示缓冲双缓冲。** GE 完成读源图像后即可归还 MPP frame；DE 扫描独立的
   rotated[2]，新地址经过 vsync 才复用前一块。其收益是解码/扫描生命周期解耦；
   成本是 GE 调度和额外图像读写。即使不旋转，这个路径仍走 GE，不能等同零拷贝。
4. **有界 MPP 缓冲。** bitstream 1 MiB、packet_count=10、extra_frame_num=2。
   额外包槽有利于接收和 VE 重叠，也可能增加积压延迟。不存在“队列越大越流畅”
   的普遍结论。
5. **优先级和监测。** 解码 5、显示 6、统计 25；每 5 秒输出 RX/show FPS、
   KiB/s、队列、回调/锁等待、解码/旋转/显示耗时。统计线程不持 decoder 锁打印。
   这些优先级不能原样移植：当前 OSS TCP/IP 为 10，IPHETH 默认 12，解码 14，
   receiver 15，直接照搬 5/6 可能影响网络和系统任务。
6. **协商条件不同。** `zalink_carplay.c` 注册的是 wireless CarPlay，关闭 audio；
   静态 cfg 未设置 fps，而 `zalink.h` 声明 0 表示默认 60。这里只能确认接口的默认
   约定，不能确认该手机实际发送 60 FPS。OSS 明确公告 30 FPS，不能直接比较两者。

## 与当前 OSS 路径的对照

| 环节 | Zalink 接入 | 当前 OSS / v23 | 影响与判断 |
| --- | --- | --- | --- |
| 接收 | 库回调直接写 MPP 包池 | 控制/Screen 共用 poll owner；帧转 Annex-B 后复制到 FIFO | 多一次堆分配/复制和调度；需测接收与回调耗时 |
| 解码输入 | MPP 10 包槽、1 MiB | 软件 FIFO 最多 15 项，再复制到 MPP 2 包槽、1 MiB | 缓冲层数不同；不能直接扩容全部队列 |
| 解码/显示 | VE、GE/DE 分线程 | 同一视频线程负责 decode、get frame、DE/vsync | 存在可消除的串行等待，但不等于当前低 FPS 的唯一根因 |
| 显示 | GE 输出双缓冲 | MPP NV12 直接交 DE，持有扫描帧直到下一次 vsync | 当前无旋转路径省掉全图复制；这一优势应保留 |
| 积压处理 | 每次 render 取一个 frame | 最多 drain 8 个已解码图像，显示最新一个 | 当前允许丢弃已解码旧图，不随意丢 P 帧参考链 |
| 码流恢复 | 库行为不可见 | 重复配置去重，输入丢失/解码错误后等 IDR | 不能为速度退回任意丢压缩帧或重复重建 decoder |
| 测量 | 自动 5 秒同窗统计 | 手动两条状态命令，窗口可不同 | v24 补齐自动同窗统计 |

现有平均 decode≈8 ms、present≈8 ms，粗略串行成本约 16 ms，低于 30 FPS 的
33.3 ms 帧预算；这些是 wall time，包含等待/抢占，不能直接当 CPU 利用率。
max queue≈192 ms 说明有突发积压，但不能解释全部 4–6 FPS。

## 开源实现能借鉴什么

* **UxPlay**：`lib/raop_rtp_mirror.c` 有独立镜像接收线程；
  `renderers/video_renderer.c` 将 appsrc → queue → parser → decoder → sink 分层，
  区分是否按时间戳同步。它是 AirPlay 镜像协议参考，并非 CarPlay MFi/控制协议模板。
  独立接收线程和媒体调度值得借鉴，`sync=false` 不能解释为可安全删除 DE 的 vsync
  生命周期屏障。
* **DiPlay**：`shared/.../airplay/ScreenStream.kt` 单独读取完整 header/body 并解密；
  `shared/.../media/AndroidMediaSink.kt` 有输出线程和独立 output lock，阻塞的
  dequeueOutputBuffer 不占用全局输入锁；配置重复时保持 decoder。
  低延迟硬解码参数有能力检查/失败回退。其可选 smooth video 会主动增加缓冲，
  是平滑与交互延迟之间的取舍，不能直接搬进当前触摸场景。
* **LIVI**：`livi-cp/src/airplay.rs:screen_proxy` 运行独立异步任务，数据代理给媒体宿主；
  `livi-screen-stream/src/lib.rs` 累积 chunk 后循环取出完整消息，允许一块输入包含
  多帧；状态/控制不会为每一小片 Screen 重新完整调度。
* **carplayd**：接收和显示通过 IPC/GStreamer 分层，文档有其 Pi 平台的接收/显示 FPS
  记录。它的硬件、分辨率、系统和时钟条件不同，不能作为 D213 已达到相同性能的证明。

## 已落地的第一步：v24 有限批量读取

v23 每次 Screen owner 调度只执行一次 recv，128-byte header、body 及额外分片
都要回到完整 RTSP/event/timing/audio 调度器。v24 在初次就绪后连续处理已排队数据，
每次追加 recv 前对该 fd 做零超时就绪检查；最多 16 次 recv 或约 2 ms 后返回 owner。
这样减少完整控制循环的重复开销，仍保留触摸和控制调度机会。

这个时间上限是协作式的：一次 recv、解密回调或高优先级抢占可能超过 2 ms，
不会中断半次解密。额外的单 fd poll 仍存在，因此这里减少的是完整 owner dispatch，
并不声称所有系统调用都减少。Linux 突发测试中，20 个认证帧用 40 次 recv、4 次 owner 调度完成，旧路径至少需
40 次 owner 调度。这个结果不能换算成板端 FPS 提升。

不改动分辨率、公告 FPS、配对、触摸、MPP 引用链或 decoder 生命周期。
暂不增加独立 Screen 线程：那需要把 socket/加密 nonce/退出回调/统计快照的所有权
一并迁移，不能只把现有对象丢给另一个线程。

v24 每 5 秒由低优先级 demo owner 输出两行 `[carplay-perf]`，不在解码或网络线程打印：

```
[carplay-perf] dt=... rx=... show=... KiB/s=... reads=... batches=... budget_yields=... queue=... peak_total=... drop=... err=...
[carplay-perf] avg_us rx_gap=... sender_gap=... assembly=... process=... | avg_ms queue=... decode=... present=...
```

所有速率/均值为该报告窗口内的计数差值；`queue` 是采样时长度，`peak_total` 明确为
本次启动累计峰值。RX 使用 owner 每 100 ms 发布的快照，窗口边界有采样偏差；
视频计数在单核短临界区复制，避免读取被打断的 64-bit 累加值。
`sender_gap` 是手机时间戳之差，不是手机到屏幕的端到端延迟；无样本时均值显示 0。
串口打印仍有微小扰动，低优先级和低频率只能降低影响。

## 下一步如何按证据选择

| 连续拖动时的结果 | 下一步 |
| --- | --- |
| RX≈30、show 明显偏低、队列增长 | 拆 VE 解码与 DE 显示线程；保留直接 NV12 扫描、帧引用和重置屏障 |
| sender_gap≈33 ms，但 rx_gap/assembly 明显更大 | 查 IPHETH RX 单 URB 重挂间隙、NCM 聚合、lwIP mailbox 和接收线程抢占，必要时抓包 |
| RX 低、sender_gap 本身也大 | 查源端出帧节奏、协商及控制反馈；时间戳大不能单独排除上游背压 |
| process 明显大，队列不满 | 分离解密/Annex-B/复制成本，再决定专用 Screen worker 或复用内存池 |
| FPS 良好但触摸仍迟缓 | 测触摸→HID→对应帧的时间；FPS 不等价于交互延迟 |

已检查 SDK lwIP 2.1.2 的 `src/api/api_msg.c:recv_tcp()`：TCP 明确不按
`recv_bufsize` 限制已 ACK 数据；默认 8192 不是本路径的 TCP 硬上限。当前 TCP_WND
已为 65535、TCP 接收 mailbox 为 128，不能仅见 SO_RCVBUF 默认小就认定它是根因。
IPHETH 当前使用单个 64 KiB RX URB，收完/解析后重挂；这是后续观测项，不代表已经
证实 USB 吞吐受限。本版不修改 USB 驱动。

## 验证与回板

50 项主机测试和边界检查、实际视频 sink 和 demo worker 回归、D213 完整构建通过。
新增真实 TCP 加密突发测试验证同 nonce 顺序、批量读取、16-read/2-ms 让出、
半包不盲目阻塞和认证失败关闭。主机结果不证明板端性能提升。

刷 v24 后照常自动连接，持续拖动地图或滚动列表 15–20 秒，保留期间 3–4 组
`[carplay-perf]`。不需要重新测 ping、USB 枚举或输入状态命令。静止页面和启动阶段
不要混入性能结论。刷机清空 `/data`，首次 Trust 属正常流程；本版保留 v23 的
空配对目录启动修复。Zalink 分支和 USB 驱动均未修改。
