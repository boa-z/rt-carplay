# 已配对 iPhone 在 READY 状态重连失败

## READY 自动重连与“手动 Connect 无日志”的后续修正

产品行为上，已经配对并允许无线 CarPlay 的手机，在双方蓝牙／Wi-Fi 开启、
车辆系统允许连接时，应能自动恢复连接。协议栈保存 bond 并不等于应用已经
实现主动重连。此前 `rfcomm_accept()` 无限等待手机接入，根本没有调用
`bt_conn_create_br()` 主动寻呼已配对地址；因此不能承诺它会自动连接。

本轮新增实际行为：

* 注册 SDP/RFCOMM 后使用 HCI Read Scan Enable 读回控制器状态；只有
  page/inquiry scan 均已开启（0x03）才发布 READY。发现 host 缓存与控制器
  不一致时，通过公开接口 off/on 修复并再次读回，失败则返回错误。
* 仅在无线 iAP2 owner 空闲时，启动后等待 2 秒主动寻呼已恢复的 BR bond。
  多个 bond 轮询，常规失败重试间隔 30 秒；从暂停状态恢复时重新等待 2 秒。
  不执行额外的 inquiry 搜索。
* ACL 成功后请求 L2 加密，复用原 link key，不强制重新配对；保留密钥已有的
  更强认证属性。随后等待手机连接已注册的 iAP2 RFCOMM 服务。不猜测手机
  RFCOMM channel，也不把 ACL 成功宣称为 CarPlay 已成功。
* 20 秒内未形成 RFCOMM 时，取消本 owner 发起的待完成连接并释放引用。
  已有入站 ACL、配对交互时不并发寻呼；入站连接赢得竞争时不取消它。
* 将 owner 的 accept 改成 500 ms 有界轮询，使其持续检查接收器、AirPlay
  event 和 iAP2 tunnel 状态。Wi-Fi 接管后暂停主动寻呼；同一手机已经接入
  的 RFCOMM 不受重连定时器影响。原阻塞 accept 接口保留兼容。
* 通过应用层日志记录 incoming Classic page，不依赖 SDK info 日志过滤设置。
  `carplay_bt status` 增加 scan、radio error、尝试数、active ACL 数及配对状态。

同时修复 SDK 的同步 HCI 并发问题：所有命令原来共用一个静态 semaphore，
并发的扫描查询与蓝牙认证可能互相唤醒。现在每个命令池槽位使用独立等待对象，
槽位由命令缓冲区引用生命周期保护，零初始化并复用，避免每次调用泄漏 RTOS
对象。当前 SDK ELF 中该静态数组为 624 字节；运行时 semaphore 对象另计，
尚无板上 CPU／射频或重连时间测量。此处没有新增工作线程。

新增验证命令：

```sh
make check-lubanlite-reconnect
cd ../luban-lite-jc-carplay-oss
python3 packages/third-party/zephyr-bluetooth/tests/run_hci_sync.py
```

前者提取实际 adapter 函数，覆盖读回／修复／失败、停止竞争、有界 accept、
寻呼轮询／节流、加密、Wi-Fi 暂停、入站竞争、引用平衡和时钟回绕。后者运行
实际 HCI 同步命令函数，以两个线程倒序完成命令，验证结果、错误和引用隔离，
连续复用槽位三次；修改前的 SDK 不能通过独立等待对象检查。
这些是 ASan/UBSan Host 测试，尚不证明 iOS 会在 ACL 建立后自动打开 RFCOMM。

启动及自动尝试的新增标记：

```text
READY radio address=... scan=0x03 (page+inquiry)
bond reconnect enabled: first page in 2s, retry 30s; Wi-Fi handoff pauses paging
reconnect paging bonded peer=... scan=0x03
ACL connected peer=...
reconnect ACL established; security request rc=0; awaiting phone RFCOMM
```

保留旧配对进行验证，结合下文的 `carplay_bt status`、`carplay_iap2 status`：

| 结果 | 判断范围 |
| --- | --- |
| 缺少 READY radio / bond reconnect enabled 标记 | 不能据此认定运行了本轮代码，或服务启动尚未成功；先核对固件和完整启动日志 |
| bonds=0、attempts 不增长 | 协议栈没有已恢复的 BR bond；仅存在文件不足以主动重连 |
| scan=unknown 或 radio error 非零 | HCI 查询／配置未得到有效结果，先处理控制器和 H5 路径 |
| active_ACL 非零且没有新 page | 存在连接／关闭中的 ACL，本轮主动策略会让路；保留前后的 ACL/HCI 日志 |
| 有 reconnect paging，随后 HCI 0x04 | Page Timeout，尚未进入 link key 验证；检查手机可连接状态、地址和射频／共存 |
| 有 saved-key reply，随后 HCI 0x05/0x06 | 已进入旧密钥认证，继续调查双方 bond 失配 |
| ACL/security 成功但持续 awaiting phone RFCOMM | 仍缺 profile 建立，不能称自动 CarPlay 成功；保留手机类型、iOS 版本和完整 SDP/RFCOMM 日志继续定位 |
| 手动 Connect 没有 incoming Classic page，但有主动 page 结果 | 使用主动方向结果缩小范围；HCI 扫描读回只证明配置值，不证明空中收发成功 |

以下保留上一轮的密钥更新／持久化调查，不能把其中的候选原因直接套用到
“连接时没有任何事件”的新现象上。

本次提供的日志只到 RFCOMM channel 6 监听和 stage=1，没有 ACL 请求、认证
或 RFCOMM 连接结果。它能证明服务启动，不能证明手机已经请求连接，也不能
确认两端 link key 一致。Forget Device 同时清掉手机密钥和服务缓存，并触发
一次新的发现／SSP 流程；重新配对有效不能单独证明故障就在密钥文件。

## 已检查的路径

`rtcp_bt_lubanlite_enable()` 在 `bt_enable()` 后加载 settings，再注册／发布
RFCOMM 和 SDP；顺序符合 SDK 对身份初始化的要求。本机 BR 地址由控制器提供，
iAP2 使用同一地址。没有为重连更改地址、关闭认证或降低安全级别。

`/data/bt_settings.bin` 包含多种记录。手机的 Classic bond 必须有
`bt/link_key/<12 位地址><类型>`，只有 `bt/id`、名称或 LE/CCC 记录并不足够。
SDK `keys_br.c` 恢复的内存 key pool 才是 HCI Link Key Request 的查询来源。
首次配对的 pairing_complete 回调不会在每次成功重连时再次触发，因此不能
把缺少该回调视为重连认证失败。加密结果通过 security_changed 观察。

## 本次修改

* 修复 SDK `bt_hci_link_key_notify()` 对 Changed Combination Key（HCI 类型
  `0x06`）的处理。此前它落入 unsupported 分支，清除 flags 并把 16 字节密钥
  置零后持久化；手机仍使用更新后的真密钥，下一次重连就会失配。现在从现存
  bond 继承认证／Secure Connections 属性，更新并保存新密钥；缺少原 bond
  或不满足 SC-only 要求则拒绝更新。其他不支持的 key type 不再覆盖原密钥。
  本地测试已重现修改前失败、修改后通过；尚无实机日志证明本次手机触发过
  `0x06`，因此它是已确认的故障路径，不能宣称已确认实机唯一根因。
* 加载失败时不再发布服务、进入 READY；可在错误排除后重试。
* 已保存 BR 记录缺少处理器时返回错误，记录恢复失败和 commit 失败均可见。
* settings 回放遇到 RAM 表容量不足时失败并保留原文件，避免后续压缩永久
  删除未装入的记录。当前表上限为 16 条，不能把它理解为 16 台手机。
* SDK 恢复 BR 记录时先核对完整长度。非空但长度不兼容的记录以前可能被
  清除，或被 read_cb 截断后错误接受；现在保留记录并报错。零长度删除仍支持。
  不猜测旧格式，不生成替代密钥。
* 启动显示实际恢复的 BR bond 数；`carplay_bt status` 增加 bond 数、存储和
  HCI/security 错误。SDK 新增 incoming connection、saved key 命中与认证结果
  日志；应用新增 ACL 建立和加密级别日志。日志不输出 link key 内容。

上述问题是代码审查确认的缺陷，尚不能认定其中某项就是这台手机的实际根因。
若手机和板端现存密钥已经不同，保留原记录不会自动修复双方状态。

## 自动化验证

```sh
make check-lubanlite-bonds SDK_ROOT=../luban-lite-jc-carplay-oss
make -j8 check
make check-lubanlite-demo check-lubanlite-handoff check-lubanlite-capabilities \
  SDK_ROOT=../luban-lite-jc-carplay-oss
cd ../luban-lite-jc-carplay-oss
scons -j8
```

bond 测试使用实际 settings 后端，以及当前 SDK 的实际 `keys_br.c`、地址编解码
和 `bt_hci_link_key_req()` / `bt_hci_link_key_notify()`，控制器及 RTOS 边界
由 Host mock 代替，启用 ASan /
UBSan。覆盖三次冷启动后地址、flags、密钥一致及 HCI 使用原密钥应答，缺失／
弱密钥拒绝，长／短旧记录保留并报错，删除、表满保护、I/O、正负 errno、
备份恢复和重入回调；额外覆盖 P256 首次密钥生成、`0x06` 更新后重启及使用
新密钥应答、未知类型拒绝且不破坏旧 bond、无原 bond 的更新拒绝。
实际 SDK 处理函数分别以普通／SC-only 配置运行，检查引用平衡。
Host 通过不代表真实 RTL8733、iOS 或掉电落盘已验证。

对照 [Linux HCI event](https://github.com/torvalds/linux/blob/master/net/bluetooth/hci_event.c)
的 `conn_set_key()`，Changed Combination Key 不替换原连接 key type；SDK 的
Zephyr 上游当前 `ssp.c` 也存在相同的未处理分支，不能仅因代码来自上游就
视为该事件已受支持。本次修改遵循 HCI Link Key Notification 的更新语义，
未加入任何 Apple 私有认证数据。

## 保留配对的实机复测

先备份 `/data/bt_settings.bin`，刷入镜像时保留 data 分区和手机上的配对。
不要先 Forget Device；这会清掉本次需要观察的故障状态。文件包含密钥，
只需保留本地备份，无需上传文件内容。

从启动开始记录日志，在 READY 点击 iPhone 已配对设备连接，然后执行：

```text
carplay_bt status
carplay_iap2 status
carplay_status
```

正常的关键阶段如下，事件先后会因手机行为略有差异：

```text
BR bond restored for <phone>
Bluetooth settings loaded: BR bonds=1/4 storage_error=0
BR incoming connection: peer=<phone> type=0x01
ACL connected peer=<phone>
BR reconnect: replying with saved link key for <phone>
BR authentication complete: status=0x00 handle=...
ACL security level=... error=0
RFCOMM DLC connected
[iap2] link up ...
```

| 最后观察到的结果 | 下一步判断 |
| --- | --- |
| 文件存在但 BR bonds=0 | 文件没有有效 Classic bond；先查保存和恢复日志，不能以文件存在为成功依据 |
| restore size mismatch / table full / handler missing | 本轮明确阻止 READY；保留文件，按错误处理存储布局、容量或链接问题 |
| READY 后没有 incoming connection | 还没进入 link key 认证；检查手机是否实际发起 Classic 连接、页扫描／射频及系统缓存，不能归因于 AAC 或音频输出 |
| incoming connection 后没有 ACL connected | 查 ACL connection failed 或 SDK 拒绝／资源错误 |
| no saved link key | 核对 incoming peer 与恢复地址，确认双方仍有对应 bond |
| saved key insufficient | 安全标志／所需级别不兼容；不降低安全级别来强行连接 |
| saved key reply 后认证 status=0x05 或 0x06 | 分别为 Authentication Failure / PIN or Key Missing；重点核对两端 bond 是否失配，不能仅凭错误码断言具体哪端损坏 |
| 认证成功但没有 RFCOMM | 转查手机缓存的 SDP／RFCOMM channel、SDK 资源与连接生命周期 |
| RFCOMM/iAP2 已建立但没有画面 | 转查 MFi、Wi-Fi 配置、CarPlay invitation 和 AirPlay，蓝牙配对已过关 |

至少分别测试：板端冷启动后重连、手机关闭再开启蓝牙、一次 Wi-Fi 接管后退出
再连接、同一设备连续多次连接，以及多台已配对手机轮换。只有这些实测完成，
才能声称旧 iPhone 的 READY 重连问题已解决。
