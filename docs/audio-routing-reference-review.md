# 无线音乐未建立音频流：参考实现逐项复核

## 最新实机结果：资源正常，仍没有音频协商

用户随后验证了 `6d0b2a5` 固件：MainAudio 的 current/permanent 均为 1，
MainScreen 的 current/permanent 也均为 1，HID_sent=2395、HID_failed=0。
视频持续接收，iAP2 隧道 identified/authenticated/handoff_complete 均为 1，
音频仍 streams/datagrams/packets 全为 0。这不支持附件抢占 MainAudio 或触摸
没有送达的解释。Owner=1 是资源实体，不能当作 iPhone 已选择 CarPlay 输出。
所示主控制请求为 200，但日志只记录前 32 个请求及后续有限错误，不等于
抓取了整段会话的所有请求与 event 通道请求。所有 SETUP 候选另有独立日志。

本次进一步检查：

* DiPlay `AirPlaySession.kt::runEventRead` 对 event 通道入站请求返回空 200，
  本项目相同；不能仅因没有语义分发就认定这里吞掉了必须的音频路由指令。
* 实际 Linux 应用 DNS-SD 与 RTSP `/info` 再用 Python `plistlib` 独立解析，
  音频开启、关闭均与编码后的 TXT Features 相符。本地 sim 当前为 PCM-only
  构建，开启时为 `0x615647aee2`，不能冒充 D213 压缩启用的 `0x615657aee2`
  实机线上抓包；后者由用户前轮日志确认。
* 参考的 PCM/AAC/Opus 位号与当前格式表一致。44.1/48 kHz、宽格式集合与
  参考的差异尚不足以解释完全没有音频 SETUP，不新增 codec 或强制资源 Take。
* 需要收窄此前无麦克风对照的结论：DiPlay 的配置默认 false，但实际
  `CarPlayHostActivity.kt` 传入 `microphoneAvailable`。源码允许无麦克风不等于
  该组合已在同一款 iOS 上实测；同样不能因此宣告虚假的输入能力。
* DiPlay `docs/ISSUE-AUDIT-2026-10-03.md` 的 #29 本身记录过所有声音留在
  手机、缺少音频 SETUP 证据的未定因报告。参考实现成功支持部分设备不代表
  它已解决所有同类路由问题。

### Timing 的可复现缺陷与修正范围

继续对照 DiPlay `NtpClock.kt` 和 catplay `clock/rtcp_timesync.rs`，两者在
会话建立后使用 UDP 210/211 的双向 timing 交换。本项目也有该交换，但此前
`timing_setup=ready` 只表明端口创建，成功 sample 在 DEBUG 级别，普通串口
记录无法判断是否收到有效应答。视频正常和 RTSP `/feedback` 空 200 都不能
代替 UDP timing 交换的证据。

发现一个能独立复现的实现错误：poll 同时遇到 socket 可读与一秒发送周期
到期时，原先先发送新探测并替换 `pending_timestamp`，再读取旧应答；原本
有效的 originate timestamp 随后匹配失败。现在先消费可读应答，再处理到期
发送。没有改变时间基准、校时算法、音频声明、AAC、Mixer 或 sound0。

新增 timing 快照在 RTSP owner 线程采集，通过应用现有 mutex 交给 shell：

```text
carplay timing: active=1 request_tx=... response_rx=... request_rx=... response_tx=... rejected=... send_errors=... timeouts=...
```

`response_rx` 只计来源地址、包形状、originate timestamp 和 RTT 校验通过的
应答，不是 UDP 收到任意包；`timeouts` 是下次探测时上一探测仍未完成。
`rejected` 包括错误来源、形状、重复/过期应答及未知类型；计数不能单独
区分这些情况。计数属于当前 timing endpoint，会话关闭后快照清零。
`response_rx>0` 证明往返交换完成，不证明完整播放时钟已经同步。本项目目前
仅采样偏差，没有像 DiPlay 那样将偏差应用于共享媒体时钟；在音频 SETUP
完全缺失时不把后续播放时钟工作冒充路由修复。

Host 用固定时钟及真实 loopback UDP 覆盖一秒边界的应答、重复应答拒绝和
未完成探测超时。把源码中的发送/接收恢复成旧顺序后，新测试在
`requests_sent == 2 && responses_received == 1` 断言处失败；新顺序通过。
55 项 Host 测试、boundary、SDK formatter/demo/handoff 和 D213 构建通过。
**未连接实机，本缺陷与这台 iPhone 不发音频 SETUP 的因果关系未被证实。**

### 这次实机只需补充的关键证据

保留配对，使用新镜像连接并播放，间隔 5 秒执行两次 `carplay_status`，看
timing 计数是否增长；同时记录 iOS 版本，以及 iPhone 音频输出选择器中
是否存在并选中此 CarPlay 接收端。无需再反复提供静态画面性能输出。

* `response_rx` 持续增长而没有音频 SETUP：不能继续归因于无 timing 往返；
  应转向同一 iPhone 的输出路由是否注册/选中，与参考接收端作同条件比较。
* 选中 CarPlay 后立刻出现音频 SETUP：说明路由选择是触发条件，不说明
  timing 修正就是原因；需记录自动选择和手动选择各自的行为。
* `request_tx` 增长、`response_rx=0`：才有依据进一步抓 timing UDP 或查看
  rejected/send_errors/timeouts，仍不能直接断言它阻止了 iOS 的 SETUP。

最新固件和 SHA 见 SDK `doc/rt-carplay-build.md` 第 14 节。本轮不是已经通过
实机验证的音乐接入修复；验收目标仍为真实 iPhone 发出 100/101/102 SETUP。

## 本轮实机证据和结论边界

本轮用户日志来自含 H5 修复标记的固件，保存的 link key 认证成功，RFCOMM、
MFi、Wi-Fi、AirPlay Pair-Verify、event channel、认证后的 iAP2 tunnel 和视频
首帧均建立。记录中仍然只有 130/iAP2 和 110/screen，没有 100/101/102
音频候选；`/info` 为 4278 字节、9 个输出格式条目。媒体 AAC 位图 0xc00000、
PCM 位图 0xcffc 均非空。Bluetooth ID 与 iAP2 一致，TXT/plist Features 一致。

这定位了“该记录尚未进入音频 SETUP”阶段，不能唯一确定 iOS 为何没有选择
CarPlay 输出。新记录仍有 H5 重传，但没有上轮的 Host NCP 0x12；控制通道和
Wi-Fi 接管成功，不能把蓝牙重传直接当作音频不路由的原因，也不能宣称 H5
已经完全稳定。本轮没有修改 AAC、Mixer、sound0 或能力位来冒充修复。

## 使用的公开参考与可核对位置

* [DiPlay](https://github.com/shihabal3amri/DiPlay/tree/615b32ce0b40cd0b99ce94ba23a512001553684f)，
  `shared/src/main/java/com/shilapi/xcertplay/airplay/AirPlayInfoPlist.kt`、
  `AirPlaySession.kt`、`AirPlayConfig.kt` 和 `network/CarPlayBonjour.kt`。
  本轮重新 fetch origin HEAD，仍为该提交。
* [catplay](https://github.com/catplay-labs/catplay/tree/66340ad4a0ec8257dafb8ed2d693d1adfc81740b)，
  `carplay/catplay_carplay/src/carplay_rx/sink/audio_defaults.rs`、`receiver.rs`、
  `modes/change_modes.rs`、`modes/modes_changed.rs`、`modes/transaction.rs`。
* [LIVI](https://github.com/f-io/LIVI) 本地参考提交
  `870d26441e073cdb802c427c5ae8833dba694951` 的
  `native/crates/livi-cp/src/info.rs` 和 `stack/commands.rs` 作为第三份实现对照。
  不将第三方的测试或硬件结果算成本项目验证。

只使用公开协议实现作对照；没有引入 SDK、认证材料或新依赖。第三方实验注释
和同样缺失音频 SETUP 的尝试不能当作“已经验证的解决方案”。

## 对照结果

| 项目 | 参考代码行为 | 本项目及判定 |
| --- | --- | --- |
| Bonjour TXT | DiPlay 的 deviceid/features/flags/model/srcvers/protovers/pi/pk | 本项目字段集合一致；实际编码后的 Features 已逐位核对，不能继续把旧 TXT 错配当作当前根因 |
| Bluetooth 身份 | `/info.bluetoothIDs` 关联 BR/EDR 身份 | 实机已匹配；Wi-Fi 私有 MAC 不应替代 BR 地址 |
| 无麦克风 | DiPlay 配置允许 microphone=false；此时省略 audioInputFormats | 本项目已经省略该字段，日志 input=0 是读取缺失字段时的显示默认值，不能误称为线上发送显式 0 |
| 媒体 | DiPlay 提供 100/media PCM、102/media AAC-LC；catplay 也同时提供 | 当前两种媒体路径均已宣告，44.1/48 kHz 均可选择；没有媒体输出位图为空的问题 |
| 各角色格式 | catplay 为 default/alert/telephony 选择更窄的 PCM/Opus 集合；DiPlay 更宽 | 当前接近 DiPlay，宣告受已编译 codec 限制；差异存在，但没有证据说明宽集合使整条媒体路由被禁用 |
| 延迟 | catplay 的 102/media；DiPlay 该版本为 102/default | 当前 102/media 与实际格式角色相符；不能把两者差异说成已证实根因 |
| 初始资源 | DiPlay 在 `/info.modes` 对 MainScreen/MainAudio 提供 Take=1、NiceToHave=100、Anytime=100 | 当前相同；初始声明与附件运行时主动发送 changeModes 不是同一方向的操作 |
| 永久资源归属 | catplay 还提供 initialPermanentEntity；DiPlay 未提供 | 两种公开接收端有差异，不能据此宣布该可选字段为所有 iOS 必需项；本轮保持原声明 |
| modesChanged 响应 | catplay 返回空正文 200，更新资源状态；LIVI 确认通知并更新状态 | 本项目空正文 200 一致，但此前只保留 MainAudio 当前实体，丢失 screen/permanentEntity，诊断不完整，本轮补齐 |
| 运行时 Take MainAudio | catplay 将 Controller=1、Accessory=2 明确区分；资源可被临时借用 | entity=1 不等于手机本地扬声器，不表示应向手机再发附件 Take；强行 Take 可能表示本地车机音源抢占而抑制 iPhone 媒体 |
| mainBuffered/type103 | DiPlay mainBufferedAudio 默认 false；需独立 feature/SETUP/时钟/TCP 实现 | 当前未宣告；不能仅为触发流而添加假 103 或 mainBufferedInfo，缺少它也不能被断言为 102 不建立的原因 |
| 未知请求 | DiPlay 对许多剩余路径返回 200；catplay 有显式 /audio-mode 路径；本项目严格返回 405 | 是需实机核对的行为差异，但现有日志没有证明手机发出过这些请求。本轮记录路径、状态和未知命令名，不以无条件 200 隐藏不支持功能 |

MainAudio 的 **current entity** 和 **permanent entity** 必须分别观察。
`current=1/permanent=2` 与 `current=1/permanent=1` 的资源状态不同。旧版
`carplay_status entity=1` 无法区分它们，也不代表已经建立音频传输。

## 本轮实际代码变更

* 解析 `modesChanged.resources` 的 screen/current、screen/permanent、
  mainAudio/current、mainAudio/permanent，分别保存存在标志；通知是 delta，
  缺失字段不会被重置为 None。非法实体不执行回调，已知实体枚举为 0/1/2。
* 前 8 个 modesChanged 输出字段化日志，区分值和 unchanged，避免打印请求正文。
  `carplay_status` 显示累积资源快照，标注 0=None、1=iPhone、2=Accessory。
* 首次 `/info` 从实际发送的文档记录资源 ID、transfer、priority、take constraint。
* 控制连接前 32 个请求在 INFO 级别记录 method/path/body 字节数/status/encrypted；
  后续额外记录最多 8 个错误响应。原先请求路径和状态只有 DEBUG 日志，无法
  从现有串口记录判断是否有未被注意的 400/405/503。
* 未知 `/command` 记录有界、已净化的命令名称，不记录参数、密钥或任意正文。

以上改进诊断和模式解析，**不是已证实修复 iPhone 音乐输出的补丁**。

## 自动验证

```sh
make -j8 check
make check-lubanlite-capabilities check-lubanlite-demo check-lubanlite-handoff \
  SDK_ROOT=../luban-lite-jc-carplay-oss
# 在 SDK 根目录
scons -j8
```

55 个 Host C 测试及边界检查通过；模式新增用例覆盖当前/永久 owner 不同、
仅永久 owner delta、仅 screen delta、非法实体不交付。能力测试断言无麦克风时
输入字段确实缺失，并对初始资源的实际编码作语义检查。SDK 小 printf 测试
覆盖新增 info_resource 字段与原 64 位 Features 日志。LVGL/handoff 和 D213
构建通过，最终构建无 warning/error。未连接 D213 验证本轮新增日志。

## 下一次实机操作和可证伪的判断

保留配对和信任文件。冷启动，连接 Wireless，保存从连接到播放的串口日志。
在 CarPlay 自身的音乐界面选择一首歌并点击播放，再在 iPhone 输出选择器中
检查 CarPlay 是否存在、是否被选中。单独记录“从手机本地 App 点击播放”的
结果，避免把两个播放来源混为一个步骤。

播放前、播放中各执行：

```text
carplay_status
carplay_iap2 status
```

新日志示例（数值仅示意）：

```text
info_resource id=2 transfer=1 priority=100 take_constraint=100
negotiation id=1 request=... method=POST path=/command body=... status=200 encrypted=1
modes_changed count=... main=1 permanent=1 screen=1 screen_permanent=2 ...
carplay audio resource: permanent=1 screen=1 screen_permanent=2 (0=none 1=iPhone 2=accessory)
```

| 新证据 | 下一步 |
| --- | --- |
| 播放前出现 /audio-mode 或其他路径 405、已知资源命令 400/503 | 对照该请求形状补实际处理，再验证手机是否开始 SETUP；不能先改 codec |
| main/permanent 指向 accessory 或发生 borrow/return | 排查资源归属和本地音源策略；先辨明哪端永久拥有资源，再选择 Untake/Unborrow，不盲目 Take |
| owner 均为 iPhone，所有请求成功，输出选择器没有 CarPlay | 继续查 iOS 路由注册、实际能力请求和与参考接收端同一手机的对照；当前公开源码不足以唯一解释 iOS 内部选择 |
| 输出选择器有 CarPlay，但未选中 | 选择 CarPlay，观察是否随即出现 100/101/102；把路由选择与协议拒绝区分 |
| SETUP 出现但 status 非 200 | 依据候选类型及解析/端口错误定位接收端拒绝；不能只看最终 streams=0 |
| SETUP 200、streams>0、datagrams=0 | 才进入 UDP 地址、端口、peer 和网络传输调查 |
| datagrams>0 且鉴权成功 | 再检查 codec、Mixer、音量与 sound0 |

只有手机主动发出音频 SETUP，才能认为本任务的协商阻塞已解除。Host 模拟手机
能建立流只证明接收端接受了所模拟的请求，不能证明真实 iOS 会选择这条路由。
