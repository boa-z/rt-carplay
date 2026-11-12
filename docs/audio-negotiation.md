# 无线 CarPlay 无音频 SETUP：协商核验与修复

最新实机反馈仍无音频 SETUP。后续参考实现逐项复核、当前／永久资源归属解析、
请求状态追踪及下一轮验收见 [参考实现复核](audio-routing-reference-review.md)。
本页下方 SHA 为此前固件，iOS 27.3 无线 explicit 已确认，当前 SETUP 提议采集镜像见 SDK 构建文档第 16 节。

本轮基线为 main `bffc3be`。实机反馈仍为 `entity=1`、`streams=0`、
`datagrams=0`、`packets=0`，连接日志只有110/screen和130/iAP2。
说明本次记录里尚未进入下行音频传输阶段。不能据此归因于AAC解码、
Mixer、jitter queue或sound0；本轮未改变这些后端的功能。

## 已确认的日志错误

SDK `.config` 未开启 `RT_KPRINTF_USING_LONGLONG`。其 `rt_vsnprintf()`
遇到 `%llx` 只处理一个 `l`，会原样输出 `%lx`，且不消费对应64位参数；
下一次 `%u` 随后把该参数的低32位当成格式条目数。
用户看到的 `audio_formats=1448324834` 正好等于 `0x5653aee2`，是旧Features
低32位，**不是手机收到14亿个格式，也不能据此认定plist被破坏**。

协议日志现使用两个显式32位参数 `%08x%08x`，不需要修改全局RT-Thread
printf配置。同步修复了session/server/音频UDP端点中格式和connection ID
的64位日志，避免SETUP失败时后面的端口、状态又被错读。未改变这些ID的
协议编码、密钥派生或任何音频数据。

`make check-lubanlite-capabilities SDK_ROOT=../luban-lite-jc-carplay-oss`
会提取本SDK真实的formatter和app真实的能力检查/协议日志函数，在Host上
以long-long关闭的配置运行ASan/UBSan测试。覆盖64位高低字、后续格式数量、
蓝牙ID、编码后的TXT/plist匹配，以及错值、重复Features和截断TXT拒绝。
这比只用glibc printf测试更接近本次故障，但仍不等同于D213实机运行。

## 蓝牙身份和Features核验

- main在接受控制连接时刷新 `/info`。BT已启用时，应用与iAP2身份构建均
  使用 `rtcp_bt_lubanlite_local_bdaddr()`，输出相同显示字节序的真实地址，
  没有硬编码MAC。server复制后保持session文档不变。
- 新日志从**实际返回的plist**读取 `bluetoothIDs` 数量和地址，同时iAP2
  在构建身份时打印其地址。实机必须两处一致；只有启动时的一条地址日志
  不能代替这一核验。无法连接开发板时不能声称已抓到该手机收到的字节。
- Features经同一函数生成。应用另解析**编码后的plist和TXT**作比较；
  只有唯一且匹配的Features字段才允许启动/刷新，记录 `info_match=1`。
  malformed、重复、截断、错值均在Host回归中被拒绝。

## 开源接收端协议对照

对照的是公开源码，不引入Apple私有SDK、认证材料或新的codec依赖。

| 行为 | 参考与当前发现 | 本轮处理 |
| --- | --- | --- |
| AudioPCM Features位18 | catplay接收端设置该位；原main宣告PCM格式却遗漏该位 | 有音频输出时补齐；AAC位20仅在编译并提供AAC-LC时设置 |
| 媒体采样率 | catplay同时提供44.1/48 kHz PCM和AAC-LC；原应用只选44.1 | 默认允许两种已实现的速率，由手机选择；显式44100/48000配置仍可限制 |
| 102/media延迟条目 | catplay使用102/media；DiPlay的旧表使用102/default，原main沿用后者 | 使延迟表与实际102/media格式条目一致 |
| 初始资源模式 | DiPlay使用MainScreen/MainAudio Take、NiceToHave、Anytime，原main一致 | 保持现有资源模式，不强行发送额外MainAudio Take |
| entity=1 | catplay `EntityEnum::Controller=1`、`Accessory=2` | 表示报告的当前控制器实体，不代表收到音频，也不是向附件抢占MainAudio的证据 |
| mainBuffered/type103 | 参考实现有独立的TCP/key/anchor协议，需同时宣告并协商 | 本项目未宣告该能力，不添加空实现来诱使手机选择103 |

参考版本：

- [catplay audio_defaults.rs](https://github.com/catplay-labs/catplay/blob/66340ad4a0ec8257dafb8ed2d693d1adfc81740b/carplay/catplay_carplay/src/carplay_rx/sink/audio_defaults.rs)。
- DiPlay commit `615b32ce0b40cd0b99ce94ba23a512001553684f` 的
  [AirPlayInfoPlist.kt](https://github.com/shihabal3amri/DiPlay/blob/615b32ce0b40cd0b99ce94ba23a512001553684f/shared/src/main/java/com/shilapi/xcertplay/airplay/AirPlayInfoPlist.kt)。

这些参考体现了接收端的协议选择，不证明这台D213/iPhone的路由已修复。
现有实机证据不能唯一确定iOS内部拒绝音频路由的原因；本轮修正了可验证的
宣告遗漏与差异，下一步验收必须看到手机主动发送100/101/102 SETUP。

## 当前默认宣告及实机验收

默认D213打开音频及压缩codec、关闭麦克风时：

```text
[carplay] capabilities features=0x5657aee2,0x61 audio_output=1 compressed=1
[carplay] capabilities_check txt_features=0x5657aee2,0x61 info_match=1
[iap2] identity bluetoothID=<板端真实BR/EDR地址>
I/airplay_rtsp_session: info_bluetooth count=1
I/airplay_rtsp_session: info_bluetooth index=0 id=<与iAP2一致的地址>
I/airplay_rtsp_session: info=served bytes=... features=0x000000615657aee2 audio_formats=9
I/airplay_rtsp_session: info_audio type=100 role=media output=0x000000000000cffc input=0x0000000000000000
I/airplay_rtsp_session: info_audio type=102 role=media output=0x0000000000c00000 input=0x0000000000000000
```

相对旧版Features仅补上真实支持的PCM位18；PCM-only构建不再虚报AAC位20，
其掩码为 `0x615647aee2`、格式数8。无声卡时仍保持无音频宣告。
`audio_sample_rate=0` 的新语义为提供44.1和48 kHz；以前依赖零值只提供
44.1 kHz的集成方可显式填44100，或设置 `CARPLAY_APP_AUDIO_SAMPLE_RATE`。
只有实际访问 `/info` 才出现 `info=served`，不是假定手机已读取。

刷入并重启板端，重新连接无线CarPlay，在CarPlay音乐界面点击播放。
保存从连接起的完整日志，执行 `carplay_status` 和 `carplay_iap2 status`。
成功进入音频传输前必须看到例如：

```text
setup=stream_candidate index=0 type=0x0000000000000066
audio_setup=ready type=102 ...
carplay audio transport: streams=1 datagrams=<持续增长> auth_failures=0
```

100/101也可以，按手机实际选择判定；不要求必须102。若三类SETUP仍全部
缺失，保留上述身份、Features、格式表和模式日志，并记录iPhone播放输出
选择器里是否存在CarPlay。若已有候选却被拒绝，应以SETUP错误定位，而不是
把 `streams=0` 一概解释成手机没有尝试。已有UDP后才进入后端播放排查。

## 自动化与镜像

已通过55项Host测试、PCM-only配置、ASan/UBSan CTest、边界检查及SDK默认
构建；额外SDK formatter/能力检查、实际Linux应用的DNS-SD与RTSP一致性、
LVGL和无线交接回归见构建说明。网络测试套件须顺序运行，避免争用7000。
Host成功、SDK构建成功和D213/iPhone实机成功分别记录，不互相替代。

镜像：SDK `output/d21x_demo128-nand_rt-thread_rtcarplay/images/` 下
`d21x_demo128_nand_carplay_page_2k_block_128k_v1.0.0.img`。
SHA-256：`9af6c4ad1121c1372276b70ab655f8c4396a9aa996d2c12496922d6487c016b0`。
