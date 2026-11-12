# 音乐仍在 iPhone 本机播放：发现能力与时钟修复

## 日志能确认什么

用户实机日志的 `tunnel=1 identified=1 authenticated=1 handoff_complete=1`
表明无线 iAP2 隧道已认证并完成交接。此时 bootstrap Bluetooth `link down`
符合既有流程。`sound0 ref count=1` 和 `carplay-aud` 线程只说明音频输出
worker 已启动，不代表收到 Audio SETUP/RTP。静态画面下视频 `rx=0` 也不能
用来判断音频链路。

## 代码发现与修复

1. **无线 mDNS 与 RTSP 能力不一致。** 音频开启时 `/info` features 为
   `0x615653aee2`，但应用构建 DNS-SD TXT 时未设置 features，回落至旧的
   `0x5203a4e2,0x61`，即无音频掩码 `0x615203a4e2`。发现阶段的 Audio
   bit 9 因此为零。现在两处共享 `airplay_info_plist_features()` 的计算结果，
   DNS-SD 通过显式低/高 32 位格式化发布 `0x5653aee2,0x61`。音频设备启动
   失败时两处一起关闭音频；自定义 features 也保持一致。RT-Thread 应用与
   Linux simulator 均修复。这是确定的代码缺陷、与手机路由现象一致，但最终
   是否足以恢复这台 iPhone 的路由，仍须新固件实机验证。
2. **约 65 秒的时钟首次回绕。** SDK `kernel/rt-thread/src/clock.c` 的
   tick 初值为 `0xffff0000`，默认 1000 Hz 下约 65.536 秒回绕。将32位
   毫秒直接转64位并不能延长它。原先时间倒退会使屏幕间隔相减后饱和到
   `4294967295`，并影响共享 AirPlay timing 和音频 jitter。现在在短关中断
   临界区内扩展模32位的毫秒增量，ms/us 共用一条64位单调时间线。运行中的
   接收器须至少在一次32位毫秒回绕周期内采样一次，正常轮询远快于此要求。
   播放 jitter 和麦克风 AEC reference 也统一使用该时钟。此临界区实现面向
   当前 D213 单核平台，不宣称为任意 SMP 平台提供跨核同步。
3. **补齐音频阶段状态。** poll owner 定期快照活动音频流数、接收 UDP
   包数、鉴权失败数与最后收到的 audio modes。shell 读取快照，不访问可能
   被协议线程销毁的流对象。`carplay_status` 可区分尚未建立流与已到包但未
   输出；`entity` 只报告手机状态，不把任意实体值解释成成功取得音频资源。

本次不靠强行发送 MainAudio Take 改变手机资源归属，也不改已有音频格式集合。

## 验证

```sh
make -j8 check
make BUILD=build-pcm AUDIO_CODECS=0 -j8 check
make check-audio-discovery
cmake -S . -B build-cmake -DRTCP_SANITIZE=ON
cmake --build build-cmake -j8
ctest --test-dir build-cmake --output-on-failure
make check-lubanlite-demo check-lubanlite-handoff SDK_ROOT=../luban-lite-jc-carplay-oss
```

55 个 C 测试程序通过。新增测试编译实际 RT-Thread OS seam，模拟初次及多次
回绕、并发调用，并验证 ms/us 共用时间线。加密音频 socket 测试覆盖新增统计。
额外的 `check-audio-discovery` 启动真实 Linux 应用，读取实际网络上的 DNS-SD
TXT 与 RTSP `/info`，分别验证音频开启/关闭，两者逐位相等。它使用标准 mDNS
组播和单播回复，需要 Host 允许本机组播；不会停止现有桌面 mDNS 服务。
SDK 默认 D213 配置 `scons -j8` 构建成功，无编译警告。这些结果不能替代手机
音频路由、声卡输出和声学实机验证。

## 实机步骤与判读

使用 app `main` 与 SDK `poc/carplay-oss`，SDK 根目录执行 `scons -j8`。
刷入新镜像后重启板端，让 iPhone 重新连接本次 Wireless CarPlay，以重新
读取发现信息；无需先删除 trust/bond 文件。

启动应出现（默认启用音频编解码时）：

```text
[carplay] capabilities features=0x5653aee2,0x61 audio_output=1 compressed=1
```

从开始连接到手机点击播放保留完整日志，然后执行：

```text
carplay_status
carplay_iap2 status
```

`carplay_status` 新增示例字段（数字取决于实际流量）：

```text
carplay audio transport: streams=1 datagrams=123 auth_failures=0
carplay audio modes: entity=1 call=0 speech=-1
```

- `streams=0`：当前没有活动音频 SETUP；应检查本次从启动起的能力日志、
  手机是否曾发起 SETUP、是否被拒绝/随后 TEARDOWN，以及手机显示的输出路由。
- `streams>0, datagrams=0`：流建立了，但音频 UDP 尚未到达。
- `datagrams>0, auth_failures>0`：进一步检查音频密钥/包格式，不归因于喇叭。
- UDP 与 sink packet 都增加、无鉴权错误而仍无声：继续排查 decoder、音量、
  sound0 write errors 和硬件输出。设备 ref count 不能取代这一判断。

连接超过70秒，间隔统计不应因启动 tick 回绕跳到 `4294967295`；同时检查
音乐是否持续、是否开始建立100/101/102音频流。修复不要求一定使用102，
应以手机实际协商为准。

本次生成镜像的 SHA-256：
`ebd17f03c10d20e9013944ce6fdc3f40285e55393ce73b72c594f8fc51261222`。
路径仍为 SDK `output/d21x_demo128-nand_rt-thread_rtcarplay/images/` 下的
`d21x_demo128_nand_carplay_page_2k_block_128k_v1.0.0.img`。
