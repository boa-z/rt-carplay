[English](../lifecycle-and-ownership.md) | [简体中文](lifecycle-and-ownership.md)

# 生命周期与所有权

## Owner 线程

每个 Receiver 实例由宿主选定的一个 Owner 独占。Create、Start、Poll、状态查询、Stop 和 Destroy 均在此线程执行。UI、触摸、无线与 USB 请求必须由宿主串行处理后再进入协议。Receiver 不创建 Detached Worker。状态回调可以复制、查询状态，但不能重入生命周期操作。禁止在媒体回调中销毁 Receiver。需要关闭的回调应提交宿主请求后返回。

## 自有资源与借用资源

Create 复制公共平台、媒体、iAP2 描述和初始能力文档。配对字符串、Accessory 标识与公钥、OS 和网络接口表、回调上下文及 Provider 存储均为借用，销毁前必须保持有效、不可变。可选兼容转换函数复制同样的描述，不取得所有权。Start 拥有 RTSP 监听器及协商建立的控制、Timing、Event、Screen、iAP2 和音频端点。各端点拥有自己的协议缓冲区与密码学状态。配对文件仍由持久化 Provider 持有；停止 Receiver 不会删除 Bond 或信任记录。

Screen Annex-B 缓冲区以及解密、编码音频缓冲区只在对应回调返回前有效。异步 Sink 必须在返回前复制数据，或完成接口明确支持的所有权转移。状态回调的指针同样为借用指针。`rtcp_receiver_get_state()` 复制值，宿主可以在自己的同步机制下发布此副本，不能将活动 Receiver 指针交给缺乏同步的 UI 查询。

## 关闭与失败处理

Stop 在协议 Owner 中执行，关闭所有端点，并在返回前完成同步拆除回调。宿主拥有的媒体 Worker 必须在设备、上下文释放前处理完已复制的数据。Stop 不负责 Join 宿主创建的 Worker；宿主必须在 Destroy 前请求 Worker 退出，并通过 Join 或确认机制等待结束。Start 失败后对象保持停止，可再次尝试。重复 Start、Stop 具有幂等性。只有成功 Start 才递增 Generation。

## 过渡期 RT-Thread 组合实现

`application/rt-thread/rtcarplay/app/carplay_app.c` 仍以旧单例、Poll Worker、触摸队列和板级初始化封装 Receiver。其生命周期和诊断 API 早于可移植 Owner 线程契约。无线、有线端点查询现将字符串复制到调用方存储；每条 iAP2 链路保留自己的端点副本，供回调及身份指针使用。只有版本常量仍指向静态 SDK 数据。LVGL 启动页与 Connection Manager 现已由 Luban-Lite 持有。宿主现已针对实际组合实现和适配层，独立测试回调、Worker 退出管理及诊断副本。仅凭可移植 Runtime 测试不能证明开发板调度或设备行为。

包装层现在通过非阻塞原子入口门串行化生命周期操作：并发或重入 Start/Stop 返回 `RTCP_ESTATE`。Poll Worker 内部调用 Stop 也返回 `RTCP_ESTATE`，避免等待自身退出。Worker 所有权在调度前发布，线程创建或启动失败会回滚资源。原子完成标志放在可重置会话存储之外，拆除不再于 Worker 最后访问状态前清空单例，因此即使 Worker 从未调度，成功 Stop 也会等待其退出。蓝牙所有权领取、释放采用 Compare-and-Exchange，避免启动页和 Shell 同时取得传输资源。CarPlay Control Shell 现向 Poll Owner 排队请求并读取状态副本，不自行操作 Browser。USB 和无线 iAP2 Worker 串行处理命令，在超时与延迟清理期间保留 Provider 和端点所有权。触摸、音频输出、视频和麦克风 Worker 均有独立退出回归。

包装层不再保存原始 RTSP Server 指针。对端及媒体、Timing 诊断在 Poll Owner 中通过公开 Receiver API 复制快照。`carplay_ab` 命令使用同一生命周期保护和队列互斥锁，发布请求的分帧值，并报告已应用值和待处理状态；仅 Poll Worker 调用 Receiver 设置接口。Stop/Restart 后待处理请求被丢弃，保持默认分块。旧的指针式 PIN 查询限定在串行化生命周期 Owner 内使用，跨线程调用方改用复制的显示、PIN 查询。音频输出和视频拒绝等待自身退出，在删除资源前等待读取者返回，并在停止超时后保留资源；麦克风 Stop 在 Worker 确认退出前保留借用的网络上下文。应用关闭 Owner 重试失败的退出流程，再释放依赖。

复制的元数据和调试查询使用位于会话可重置存储之外的共享读取计数，可与其他读取者及输入并发；生命周期操作禁止新查询进入，以休眠方式等待已有读取者返回，再销毁、重置资源。Shell 状态查询与生命周期互斥，并在队列锁下读取队列计数。HID 计数和加密事件就绪状态使用原子变量。配对存储独立发布实际写入结果，不将查询竞争误报为持久化失败。链路端点刷新只在成功后提交，失败不破坏 iAP2 已保留的指针。参见宿主 [快照契约](https://github.com/boa-z/luban-lite-carplay/blob/carplay-oss/application/rt-thread/rtcarplay/docs/session-snapshots.md)。

## 回归证据

触摸与 Siri 入队操作使用和 Start/Stop 相同的非阻塞入口门。生命周期或输入操作占用入口时，请求直接返回 `RTCP_ESTATE`，不会读取会话互斥锁；Stop 会持有入口门，直到该锁销毁。Worker 在发布退出完成前清空线程句柄，避免把已退出 Worker 的句柄误认为后续调用者。调用方需要依据自身输入策略处理入队拒绝；API 不保证关闭期间交付输入。

Host 生命周期回归检查拒绝重入 Destroy、配置复制、监听失败后重试，以及客户端连接期间反复拆除。加密端到端回归检查认证、独立端点就绪状态和重连。这些测试不验证 RT-Thread 调度、硬件设备关闭、DMA 有效期、USB Host 取消或物理重连；这些内容需要分别执行对应适配层和实机验证流程。

`test_rtthread_app` 还使用真实 Linux 网络、熵源和协议实现编译实际 RT-Thread 组合源码，仅模拟 RT-Thread 调度和互斥锁。覆盖线程创建、启动失败回滚，拒绝生命周期重入，入队和锁销毁期间的输入与生命周期互斥，拒绝自停止，首次调度前 32 轮启动、立即停止、重试，以及两个 pthread 竞争者进行 64 轮蓝牙所有权领取。此测试现通过 Luban-Lite 宿主的 `make -C application/rt-thread/rtcarplay check` 执行，并与板级回归一同迁移。这属于 Host 调度模型证据，不是真实 RT-Thread 开发板运行结果。
