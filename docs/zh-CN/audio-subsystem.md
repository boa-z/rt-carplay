[English](../audio-subsystem.md) | [简体中文](audio-subsystem.md)

# CarPlay 音频子系统开发

## 历史范围与当前集成

本文保留音频实现、实测 Host 开销和历史构建结果，包括 2026-11-11 镜像。下文“当前”、测试数量和待执行板端检查均指对应基线；历史 v30 视频／交接确认不能证明音频验收。D213 Sink、采集／诊断 Worker 和组合现归属 Luban-Lite 宿主 `application/rt-thread/rtcarplay/`，SDK 保留可移植协商、Codec、Jitter 和混音代码。参见[宿主迁移](luban-lite-migration.md)及[音频调试交接](audio-debug-handoff.md)。V2.1 冻结音频协商和输出行为，本次文档更新不是新的音频修复。旧镜像路径／校验值保留用于历史追溯，不是当前烧录建议。

从现行 Luban-Lite 根目录运行实际板级适配 Host 回归 `make -C application/rt-thread/rtcarplay -j8 check`，协议回归为 `make -C packages/third-party/rt-carplay -j8 check`。下文历史 SDK 内 `check-lubanlite-*` 命令已迁至宿主，不应为恢复命令将板级代码重新引入 SDK。实机音频、采集、声学调校及实时余量仍需单独验证。

分支：`main`，音频修改已集成。硬件基线：用户确认过无线 v30 视频与蓝牙交接。该分支的音频修改**尚未在 D213 硬件上测试**。

后续 Discovery Mask 和启动时钟回绕修复、连接时蓝牙身份刷新、当时镜像校验值及板端诊断见[音频路由修复](audio-routing.md)。首次路由修复后，用户后续板端 Trace 仍没有 Audio SETUP；身份修复及新能力 Trace 也未在下一次用户测试中确认 Audio SETUP。Formatter 修复、真实 PCM/AAC 标志、双采样率 Media Offer 和当时测试镜像见[协商调查](audio-negotiation.md)。

## 实际代码和开源参考中的发现

* `/info` 只提供 LPCM，完全缺少 `102/media`，而 DiPlay 在该角色提供 AAC-LC。这是明确的协商缺口，也是音乐留在手机上的合理候选原因，但不是硬件确认的根因。
* RTP L16 样本字节此前原样提交给小端设备；DiPlay Renderer 显式交换字节序。Receiver 现转换 L16。
* 未知／复合协商格式此前被静默当作 PCM，现拒绝协商，避免输出噪声。
* 三种 Stream Type 共用一个字节环形缓冲区，格式变化会冲掉其他流；现使用独立有界 Jitter Queue 和 Decoder。
* 格式验证期间失败可能关闭描述符零，因为未完成构造的 Stream 没有将 fd 字段初始化为 -1。

参考文件（NOTICE 已引用的 GPL 源码）：https://github.com/shihabal3amri/DiPlay 中的 `AirPlayInfoPlist.kt`、`AudioStream.kt`、`CarPlayMediaEngine.kt`、`MicrophonePacketizer.kt`、`AndroidMediaSink.kt`。未引入 Apple 私有 SDK 或认证材料。

## 当前已实现的下行

`RTSP SETUP -> ChaCha20-Poly1305 RTP authentication/replay filter -> bounded per-role jitter queue -> audio worker -> AAC-LC/Opus/LPCM decode -> resample and mix -> sound0`。

* 每次协商恰好一个格式位。LPCM 支持 8/16/24/32/44.1/48 kHz、单／双声道。
* 可选定点 FAAD2 AAC-LC Raw Access Unit，双声道 44.1/48 kHz。
* 可选定点 Opus 单声道，以 48 kHz 解码；独立保留 RTP 时钟（16/24/48 kHz）。不声明不支持的 Codec。
* 八个语义 100/101/102 队列相互独立，每队列 16 个不超过 4096 字节的条目，按认证后的 RTP 时间戳排序，处理回绕，重排等待为 20 ms。
* 单个音频 Worker 解码并写入设备；网络通过 Try-lock 入队，从不等待 Codec 或播放。锁争用作为丢包计数。
* 48 kHz 双声道输出、线性插值、饱和混音、逐 Lane 增益渐变、主音量／静音和导航／媒体 Ducking 策略。可选有界占用率 Servo 最多修整 300 ppm；不声称 D213 漂移已验证。
* 预留本地安全 Lane，绕过远程娱乐音量／静音／Duck。
* 选择性 TEARDOWN 只清空对应队列／Decoder／Mixer Lane。
* 设备短写会继续补完；零／错误写入终止当前 Chunk。
* Writer 启动标志在调度前设置，防止首次进入前的停止释放线程仍需的资源。设备 Writer 卡住时隔离保留资源，不释放仍在使用的内存。

Codec 源码／版本／许可见 `third_party/audio/README.md`。FAAD2 仅支持 LC，不支持 AAC-ELD。尚未声明 Enhanced Buffered Audio。

## 已有自动化验证

`make -j8 check` 在 ASan/UBSan 下执行 Core 测试，包括真实合成 AAC-LC 44.1/48 kHz 解码、真实 Opus 编解码、时间戳回绕、乱序／重复／过晚／溢出队列、流隔离、混音、削波、增益渐变、重采样和本地告警保护。`test_audio_sink` 使用 pthread 支撑的 RT-Thread／设备模型编译实际板级 Sink，验证播放、线程启动失败和重复清理。SDK 在 `CONFIG_LPKG_RT_CARPLAY_AUDIO_CODECS=y` 时也通过 `scons -j8`，FAAD/Opus 从源码构建。

这些结果证明 Host 行为及 SDK 可构建性，不能证明 iPhone 路由选择、扬声器输出、麦克风接线或 D213 实时余量。

有界槽位映射为 100/default、101、102、100/telephony、100/speechRecognition、100/alert、100/compatibility、100/media，第九个 Lane 预留本地音频。多个 Type 100 角色可并存；所有 101 角色共用一个槽位，102 也如此。这是单 Controller Receiver，不是任意多 Controller 混音。扩展 start/stop 回调携带角色；旧回调仍可用于每种 Type 单流。选择性 TEARDOWN 匹配 Connection ID／角色，陈旧拆除不会关闭新流。仅指定 Type 的拆除会关闭该 Type 全部角色。

## 上行和控制实现进展

* 独立的 DataStream Input Encryption Key HKDF，不使用 Output Key。
* 单声道 L16 大端或定点 Opus Encoder，具有 RTP/AEAD Header、Nonce Tail、符合 MTU 的分包、独立 16/24/48 kHz RTP 时钟，以及 48 kHz Opus 采集。
* SETUP 验证 Input Port、audioType、framesPerPacket，只向已认证 Control Peer 发送。相同 ID 但不同选项的重新协商被拒绝；回调可能发送前先预留 Key ID，多流事务中的重复 Key 被拒绝。
* 显式启用 `RTCARPLAY_MICROPHONE` 后，使用专用 Source／Encode／Send Worker，独占 SDK dmic0，以 48 kHz 双声道采集，转换为单声道并重采样。Pipe 非阻塞读取，协议 Owner 异步请求停止，有界等待退出，Worker 仍忙时隔离资源。`carplay_mic mute|unmute` 在保持 RTP 连续的同时将发送样本静音。
* SpeexDSP 定点 NS、有界 RMS AGC。实验性 AEC 以实际提交的混合播放帧作为参考，延迟可配置（`RTCARPLAY_MIC_AEC_DELAY_MS`）。AEC 显式启用，并需实机校准。
* 规范化 modesChanged 的 Call/Siri/Resource 状态，以及通过验证的 duckAudio/unduckAudio 音量／时长，驱动 Worker 拥有的增益策略和渐变。
* `/feedback` 现返回活跃 Stream Metadata；仍缺少真实 DAC sampleTime 和与 NTP 关联的显示时间戳，不把接收包数或墙钟计数伪装为硬件播放时钟。
* `carplay_siri` 将 Event Channel requestSiri 动作 2/3 排队到 RTSP Owner。

新增 Host 测试使用虚拟 PCM 采集和真实 Loopback UDP 运行实际麦克风 Worker，解密其包，验证 PCM/Opus 时钟及停止／重启。合成对齐回声衰减独立测试，不是 D213 实测回声性能。SDK 麦克风／AEC 构建也与默认配置分开检查。

## 功能与验证矩阵

| 功能 | 实现 | 证据／边界 |
| --- | --- | --- |
| 100/101 LPCM 输出 | 已启用 | Host 加密 UDP、字节序转换、实际 Sink Worker 模型 |
| 102/media AAC-LC 输出 | 启用 Codec 依赖时支持 | Raw AAC 44.1/48 kHz 测试，真实 iPhone 路由待板端验证 |
| 单声道 Opus 输出／输入 | 定点编解码 | 16/24/48 kHz RTP 时钟、48 kHz Codec、认证 Loopback 输入测试 |
| Main/Alternate/Media 混音 | 独立队列／Decoder、重采样、增益渐变 | 数值和 Worker 测试，九个有界 Lane |
| 本地 HMI／关键告警 | 公共本地 PCM 入队、安全增益优先级 | 本地 Lane 绕过远程静音／音量；HMI 必须使用该 API，不得并发写 sound0 |
| Call/Siri/Navigation Focus | modesChanged、活跃角色策略、Duck/Unduck | 状态／命令测试，不伪造远程资源授予 |
| 麦克风 | 显式启用 dmic0 采集、单声道转换、RTP 上行 | 实际 Worker 配合虚拟 Source 测试，板级采集路径通过编译检查 |
| Siri 触发 | carplay_siri -> requestSiri 动作 2/3 | 加密命令、CSeq 和 Binary Plist 测试 |
| NS/AGC | Speex 定点 NS、有界 RMS AGC | Host 信号测试，声学调校待执行 |
| AEC | 可选 Speex 回声抵消 + 混合输出参考 | 合成对齐回声衰减测试，实际设备延迟／双讲未验证 |
| 输入静音 | 在连续 RTP 时钟上编码静音 | Host Sender 和 Worker 测试 |
| 重连／清理 | 逐槽位代次、已消耗 Key ID、有界 Worker 关闭 | 重连、启动失败、选择性拆除测试；驱动阻塞时隔离仍在使用的资源 |
| 占用率时钟 Servo | Core API，变化率受限 +/-300 ppm | 120 s 内模拟 +/-100 ppm 源漂移，不自动在 sound0 启用 |
| AAC-ELD | 不可用、不声明 | 所选 FAAD LC-only Decoder 不支持 ELD，Voice 回退至 LPCM/Opus |
| Main Buffered / Auxiliary / Enhanced Siri | 不声明 | 不同缓冲／Feedback／Capability 语义需要验证过的线上证据 |
| 完整 Take/Borrow Resource 协议 | 未实现 | 跟踪入站状态，现有参考对附件 Take 的路由效果说法不一致 |
| RTCP Resend／DAC 播放 Feedback | 未实现 | 排空 Control Datagram，没有已证明的重发或硬件播放时钟契约 |

上行遵循已认证 AirPlay SETUP Peer/DataPort 和独立 Input Encryption Key，不是 Bluetooth HFP。只有一个采集 Worker 拥有 dmic0。竞争的 Input SETUP 返回 503，不停止正在运行的输入；拥有该流的 TEARDOWN 请求异步停止。旧 Worker 仍在退出时，替换请求可能返回 503，重试需要新的 Connection ID，可能已经发送的 ID 永不复用。这是有界安全回退，不是无缝 Siri／电话麦克风仲裁。

## 可重现的 Host 检查

从 rt-carplay 仓库运行：

```sh
make -j8 check
make BUILD=build-pcm AUDIO_CODECS=0 -j8 check
cmake -S . -B build-cmake -DRTCP_SANITIZE=ON
cmake --build build-cmake -j8
ctest --test-dir build-cmake --output-on-failure
make check-lubanlite-demo check-lubanlite-handoff SDK_ROOT=../luban-lite-jc-carplay-oss
./check-boundary.sh
```

CMake 和 Make 各覆盖 55 个测试程序。CMake Sanitizer 模式以 AddressSanitizer 检查 Core、测试和音频依赖库，Core 和测试还使用 UndefinedBehaviorSanitizer。第三方定点 Codec/Ed25519 算术保留有意的有符号回绕／移位，因此排除 UB 插桩，但不排除地址检查。mbedTLS 使用独立 Host 库构建。这些测试不是协议符合性认证。

关闭 Codec 的构建只声明 PCM，保留 RMS AGC 回退；没有依赖时，NS/AEC 和 AAC/Opus 请求不会静默成功。

## 实测 Host 开销和有界内存

```sh
cmake -S . -B build-bench -DRTCP_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-bench -j8 --target audio_bench
./build-bench/audio_bench
```

Benchmark 在 Release 下仍保留断言，并包装所链接项目／Codec 对象中的 malloc/calloc/realloc/free。报告的堆不含 libc 内部、线程栈、RTOS 分配和 DMA。样例运行记录在 [audio-host-benchmark.txt](../audio-host-benchmark.txt)：x86_64 AMD EPYC 7402、GCC、优化的通用定点实现、300 次迭代。AAC-LC 双声道解码 p95 为每 1024 帧 169 us，Opus 单声道编码 p95 为每 20 ms 761 us，NS+AGC+AEC p95 为每 10 ms 290 us。这些是观察到的 Host 耗时，不是 D213 CPU 估算。Benchmark 没有计时通过／失败阈值。

实测分配：AAC Decoder 峰值 27,992 字节；同时存在的 Opus Encoder 和 Decoder 为 42,512；NS+AGC 为 51,720；NS+AGC+AEC 为 181,342；九 Lane Mixer 加全部 Jitter Queue 为 840,640。Codec 状态在 Mixer／队列之外另计。板级输出和输入 Worker 各预留 64 KiB 栈；实验性 AEC 参考历史约预留 31 KiB。网络缓冲、应用、视频、RTOS 设备缓冲和 DMA 另计。分配失败会拒绝同步启动／协商，或由异步输入 Worker 记录。SETUP 成功不证明物理采集随后已打开，应检查麦克风 Worker 状态／日志。

配置的包重排延迟为 20 ms，输出 Chunk 为 10 ms，Decoder Ahead 上限在 48 kHz 时约 60 ms，其他采样率按源帧数计算。Mixer 容量为每 Lane 8192 个源帧。sound0 启动先为功放等待 200 ms，遵循 SDK aplay 行为。这些是软件缓冲配置值，不是实测端到端或 DAC 延迟。`audioLatencies` 仍是协商提示，必须校准。

## 板端验证清单与命令

使用 SDK [宿主迁移说明](luban-lite-migration.md)（历史构建记录位于 `../../luban-lite-jc-carplay-oss/doc/rt-carplay-build.md`），原记录对应 App `main` 和 SDK `poc/carplay-oss` 仓库。默认启用下行 AAC/Opus，关闭麦克风／AEC。只在采集测试时通过 menuconfig 启用 RTCARPLAY_MICROPHONE，可选 RTCARPLAY_MIC_AEC。

```text
list_device
carplay_status
carplay_iap2 status
carplay_bt status
carplay_mic
carplay_mic mute
carplay_mic unmute
carplay_siri
list_thread
free
```

从已有 LVGL Demo 启动 Wireless，正常配对并播放音乐，从启动开始记录。预期 AAC Media 出现 `audio_setup=ready type=102`，协商的 PCM/Voice 路径为 100/101，随后为 `writer running: independent streams, 48kHz stereo mixer`，`carplay audio` 包计数增长且没有写错误。检查实际声音，不把 SETUP 当作可听输出。麦克风命令仅在启用麦克风的构建中存在；支持输入的 SETUP 后应看到 `[carplay-mic] started dmic0=48000/stereo ...`。不能仅凭 dmic0 注册推断麦克风可用。

在 D213 验证：音乐路由与 sound0 声道顺序／增益、实际 DMIC 引脚／声道／增益、48 kHz 同时采集播放、Siri 和电话往返、输入静音、导航 Duck／恢复、远程静音时的本地关键告警、快速 USB/Wireless 切换和重连、CPU／线程栈高水位及持续堆占用、声学回声延迟／双讲／噪声质量，以及 AAC 加采集／AEC 时的视频流畅度。选择 sound0 占用率 Servo 目标前至少播放 30 分钟评估漂移。不要以接收 RTP 样本计数替代真实 DAC Feedback。

## 剩余外部依赖

上述仅板端检查需要硬件，Host 开发不需要。完整资源借用／归还语义、Enhanced Stream Framing 和准确播放 Feedback 需要相互印证的协议 Trace 或公共契约。AAC-ELD 需要协商的 Profile/ASC 和完整 Decoder 测试向量，FAAD LC-only 不能靠声明 ELD 位扩展。已研究 FFmpeg 7.1.5 作为可能的 LGPL 路径：它解析 ER AAC-ELD，但 `libavcodec/aac/aacdec.c:decode_eld_specific_config` 明确拒绝 Low Delay SBR 和 Data-resilience 标志。缺少真实 CarPlay ELD Profile／向量时，这种部分支持不足以声明完整 ELD。来源：https://github.com/FFmpeg/FFmpeg/blob/n7.1.5/libavcodec/aac/aacdec.c 。没有打包 FDK-AAC；其附加许可条款需要针对本 GPL 项目进行兼容性／分发审查。该分支没有 Apple 私有 SDK、密钥或认证材料。已实现路径为 LPCM/Opus Voice 和 AAC-LC Media。

## 初始 SDK 构建结果（2026-11-11，路由修复前）

两种配置均完成 `scons -j8`，无编译器警告。以下 GNU size 描述的是**整个链接固件**，不是音频独占分配：

| 配置 | text | data | bss | 总字节 |
| --- | ---: | ---: | ---: | ---: |
| 默认 Codec、麦克风关闭 | 2,591,024 | 1,050,404 | 3,663,968 | 7,305,396 |
| Codec + 麦克风 + 实验性 AEC | 2,735,184 | 1,054,340 | 3,695,072 | 7,484,596 |

Voice 验证后工作区恢复默认 defconfig。生成的默认镜像为 `../luban-lite-jc-carplay-oss/output/d21x_demo128-nand_rt-thread_rtcarplay/images/d21x_demo128_nand_carplay_page_2k_block_128k_v1.0.0.img`，该次构建 SHA-256 为 `3623f14f82f263dd0e144b2a26017c83162d03a299bfcc6ac7eae2e8022a43b2`。镜像字节可能随构建时间戳／工具链变化。未烧录镜像，不声称执行过 D213 音频测试。

## 接收阶段诊断

owner 线程的 rtcp_receiver_get_diagnostics 快照提供活动音频流数量、datagrams、auth_failures、malformed、replay_drops、delivered、sink_rejected、sequence_missing、sequence_reordered 与 poll_gap_max_us。delivered 仅表示回调接受，不等于采样已解码或播放。正向序号缺口属于暂时的高水位观察，可能被后续乱序填补；RTP 序号字段本身不在 AEAD 关联数据内。调度间隔包含无可读音频/控制流量的时段。这些计数单独不足以证明网络丢包或 DAC 音质。前三次被拒绝的 AAC 帧会报告解码错误和格式/消费字节详情，不泄露音频负载，校验与重放保护保持不变。
