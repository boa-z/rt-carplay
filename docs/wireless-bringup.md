# D213 无线 CarPlay 与新 iPhone 配对验证

本次改动接通双模式 LVGL 启动流程，并修复已找到的蓝牙交互、密钥持久化和
无线地址发布缺口。主机测试和完整 SDK 构建通过不等于新 iPhone 已实测稳定；
尚没有这版固件在板上的蓝牙认证、Wi-Fi 接管日志。

## v28：修复 v27 首次启动被误判为存储故障

v27 板端日志在 `bt_settings_init()` 报 `settings_subsys_init failed (err -5)`，
随后 `bt_enable()` 退出；尚未进入 H5 控制器启动或手机 SSP 配对。
实际调用链为 SDK `hci_core.c::bt_enable()` → `bt_settings_init()` → 应用的
`settings_subsys_init()` → `file_load()`。SDK `dfs_posix.c::open()` 将底层
负数错误直接传给 `rt_set_errno()`，newlib `_open_r()` 不转换符号。
因此刷机后缺失的 `bt_settings.bin` 会产生 `errno=-ENOENT`。v27 的
`errno != ENOENT` 判断把它当成读盘失败，再统一映射成 `-EIO`（-5）。

v28 对主文件和备份都识别正／负 `ENOENT`；其他错误仍返回失败，并记录
失败路径及原始 errno。不关闭持久化，也不把所有存储失败当作首次启动。
SDK 同时在这条早期失败路径清除 `BT_DEV_ENABLE`，防止重试被误报成
`-EALREADY`；此时尚未打开 HCI 资源，不涉及运行中控制器的强制复位。
同一份实际后端测试分别模拟 POSIX 正 errno 和 SDK 负 errno，覆盖首次无文件、
备份恢复、真实 I/O 错误阻止初始化，以及错误恢复后重试保留身份。
板端下一步应先看到 `settings storage ready`，再观察蓝牙控制器初始化结果；
此修复不代表 SSP、Wi-Fi 接管或此前 Wi-Fi reserved-page 告警已经完成实测。
SDK 的 `python3 packages/third-party/zephyr-bluetooth/tests/run_enable_settings.py`
隔离编译实际 `bt_enable()`，验证 settings 失败时不打开 HCI、恢复后可重试，
以及真正完成启用后的重复调用仍返回 `-EALREADY`。

## v29：已关联 AP、mDNS 有应答，但没有 CarPlay 画面

板端新日志已有 WPA2 四次握手完成及 mDNS 回复；这说明 AP 关联和一部分
UDP 处理已运行，不能据此证明 RFCOMM iAP2、MFi 或 CarPlay 会话邀请成功。
`OnDisassoc reason=8` 后随即重新关联，单凭该片段无法解释离网的原因。

代码中确认 Control 单播查询的候选地址仅从已建立的 RTSP 连接取得。
因此 RTSP 尚未建立时，即使手机已查询本机 AirPlay 服务，仍会反复打印
`unicast=none`，无法启用原本用于绕过 AP 组播问题的单播发现路径。
这是发现流程的缺口，尚不能认定它是这台手机无法激活 CarPlay 的唯一根因。

v29 在 responder 成功解析本机记录的查询后提取来源。应用只使用 AP 同一
/24 内的非本机、非网络／广播 IPv4 地址作为候选（与本 SDK DHCP 的固定
`255.255.255.0` 一致）。候选只用于向其 5353 端口查询 `_carplay-ctrl`，
不会推定端口、伪造 DNS-SD 服务或直接发送 CarPlay 激活请求。
Control connect-out 仍要求 browser 解析出 SRV；实际 RTSP IPv4 peer 优先。
无关查询、响应包、畸形包、自身回环不会提供候选。

同时保留本次无线服务的 MFi 成功、Wi-Fi 配置发送、Availability、Invite、
重邀和链路 ACK 计数，RFCOMM 在 Wi-Fi 接管时正常关闭也不会抹掉这些计数。
它们随无线服务重新启动清零。Invite 表示发送路径成功，不代表手机接受；
link ACK 也只确认 iAP2 链路收到，不等同于 CarPlay 激活成功。

验证时从选择 Wireless 前开始保存日志，卡住时执行：

```text
carplay_iap2 status
carplay_bt status
carplay_ctrl status
carplay_status
```

* `MFi_completed=0`：先排查 RFCOMM/iAP2/MFi；手机系统显示蓝牙已连接不够。
* MFi 完成但 `availability=0`：尚未收到手机的 CarPlay availability，保留早期
  identification/subscriptions 日志，不用修改 Wi-Fi 密码来试错。
* `availability>0`、`invites=0`：检查发邀请所需状态和参数，保留 iAP2 日志。
* `invites>0`、`link_acks=0`：检查邀请是否还在队列中及 RFCOMM 链路是否断开。
* 已有 ACK、无 RTSP：检查 Control 发现结果、手机激活／用户确认和网络接管。
  新日志应有 `source=mdns_query` 及之后的 `unicast_rc`；没有 SRV 时仍不会
  connect-out。请保留 `carplay_ctrl status` 的 packets、updates 和 endpoints。
* RTSP 已进入配对：按该连接的请求／错误继续排查，不再停留在蓝牙状态上。

已对比 DiPlay 的 0x4301 参数布局及 LIVI 的 mDNS socket 设置；未找到据此
需要修改本地 0x4301 编码的证据。实际 SDK `udp_new()` 已把组播 TTL 初始化为
`UDP_TTL`，本方案 `UDP_TTL=255`，因此 v29 不改 TTL、广播能力或协议公告值。
主机回归验证了候选提取及邀请 ACK 事件，板端是否因此进入 RTSP 仍待验证。

## 已确认的代码问题及修正

| 问题 | 实际源码和机制 | 修正 |
| --- | --- | --- |
| 配对没有显示／确认交互 | `port/luban-lite/rtcp_bt_lubanlite.c` 未注册认证交互回调；SDK `subsys/bluetooth/host/classic/ssp.c` 的 IO capability 分支因此选择 `BT_IO_NO_INPUT_OUTPUT` | 启动页构建注册 display、compare、consent 回调；数字核对由用户确认。其他手动构建保持原 IO 模式 |
| 服务注册前已可发现 | `enable()` 先开 inquiry/page scan，`rfcomm_listen()` 随后才注册 RFCOMM/SDP | 完成服务注册后再发布；停止时禁止新的 RFCOMM accept |
| 默认配置不保存蓝牙密钥 | 应用已有 `rtcp_bt_settings.c`，但未启用 settings，D213 链接脚本也未保留静态处理器 | 默认启用 `RTCARPLAY_BT_PERSISTENCE`、SDK settings 宏及 handler section，恢复密钥后发布服务 |
| 换手机后密钥池可能已满 | 原 SDK `.config` 的 `LPKG_BT_MAX_PAIRED=1`，且未开启 `KEYS_OVERWRITE_OLDEST`；`classic/keys_br.c::bt_keys_get_link_key()` 无空位返回 NULL，`classic/ssp.c::bt_hci_link_key_notify()` 因而不能保存新手机 link key | 演示 defconfig 改为 4 台；UI 显示已用／上限，满后由用户忘记旧配对，不静默淘汰 |
| 满池恢复缺少 NULL 检查 | SDK `classic/keys_br.c::link_key_set()` 恢复有效记录时，未检查分配失败便执行 `memcpy(link_key->storage_start, ...)` | 满池返回 `-ENOMEM`；应用 settings 后端向上传播 handler 错误，避免崩溃或虚报恢复成功 |
| 存储失败被当成功 | 旧 settings 后端忽略追加失败，压缩先删除主文件，损坏尾部阻断后续记录重放 | 返回 I/O 错误，保留备份，修复有效前缀后再追加；UI 显示持久化错误 |
| 无线接管可能发布旧 IP | `start_wifi()` 在 `dhcpd_start()` 修改 AP IP 前读取地址 | 先等 AP netif 和 DHCP 启动，再读最终地址；无法启动 DHCP 时返回失败 |
| AP/STA 或 USB 默认路由混淆 | Wi-Fi netif 曾按名称／非零地址猜选，mDNS 按默认接口加入组播 | 按 WLAN 设备绑定找 AP，mDNS 加组和发送明确选择 AP 地址 |
| 异步启动成功被当成服务就绪 | `carplay_iap2_start()` 只创建线程；蓝牙启动可在后面失败 | 独立发布 running/listening/connected/identified/authenticated/error，UI 据此显示阶段 |

这些是已确认的实现缺口；其中哪一个导致某台新 iPhone 的失败，仍需设备日志确认。
不能仅因 IO capability 改变就断言所有 iOS 版本兼容问题已解决。

## 生命周期与数据边界

启动页只有一个工作线程管理 USB 和无线。切换前依次关闭旧 USB 会话、等待无线
iAP2 释放借用的 receiver 字符串／MFi provider，再停止接收器。蓝牙控制器启动
期间收到 stop，退出路径仍关闭可发现状态。配对请求持有 `bt_conn` 引用，UI 只
提交带 token 的确认，由工作线程调用栈 API；过期点击不应用到另一台手机。

管理配对前先停止发现和会话，关闭管理页后才重新启动。蓝牙“忘记”先断开并
等待现有 BR ACL 退出，超时则返回失败；随后调用 `bt_br_unpair(NULL)` 清除
内存 BR 密钥，再压缩 settings，只删除 `bt/link_key/*`，
保留 `bt/id` 等本机数据。USB/AirPlay 文件仍按原白名单处理。此操作只清除板端，
不能替手机删除其配对记录。

settings 后端覆盖普通重启恢复、文件尾部截断及备份恢复；并未证明 NAND/FAT
突然断电时具备事务持久性。存储错误会显示，避免把仅在 RAM 中的 bond 当作
已经可靠写入。整包刷机会清空 `/data`，刷机后的首次配对不是重连测试。

## 板端验证顺序

1. 刷入镜像后，先在 iPhone 蓝牙设置中忘记旧 `rt-carplay`。开机默认 USB，
   点击 Wireless，等待 AP 地址和 Bluetooth listening 出现。
2. 从 iPhone 蓝牙设置选择 `rt-carplay`；若出现六位数字，两端核对并确认。
   记录机型、iOS 版本、失败停留阶段，以及 `BT last HCI/security` 和存储状态。
   不要把蓝牙成功、MFi 成功或线程创建成功当作 CarPlay 已加载。
3. 依次检查 Bluetooth → iAP2 identification → MFi → Wi-Fi → AirPlay event →
   first frame。Wi-Fi 接管时手机关闭蓝牙 RFCOMM 可以正常发生；应检查后续
   AirPlay 是否建立。`HCI 0x13` 仅表示对端关闭连接，不直接判定配对失败。
4. 不刷机，正常重启板子，测试同一手机保存的 bond 能否复用。重复至少五次，
   区分系统询问“使用 CarPlay”和重新进行蓝牙配对。
5. 在等待页打开 Pair files，执行 Forget Bluetooth pairs；在手机也忘记设备，
   重新配对，确认原本机身份和 USB/AirPlay 文件未被一起删除。
6. 分别验证 USB→无线、无线→USB、配对时取消、手机取消、断开 Wi-Fi、管理页
   打开期间手机尝试连接。USB Trust 正在阻塞时，切换要等待该操作返回。
7. 至少一台已工作的旧 iPhone 和一台容易失败的新 iPhone 做相同测试。需要
   比较成功率，记录每台五次独立连接的阶段和错误，不能只提交一次成功截图。

## 错误如何收敛

* 蓝牙认证回调失败：优先分析 `security_reason`，检查两端旧 bond、数字确认
  是否超时、密钥池是否已满及存储失败；保留失败前后的 ACL/HCI 结果。
* 已有 RFCOMM，iAP2 identification/MFi 失败：查 iAP2/MFi，不再归因于 SSP。
* MFi 成功，未建立 AirPlay：核对最终 AP IP、手机是否关联 AP／获取地址，
  再查 Wi-Fi 配置交换与 mDNS。地址正确仍失败需板端网络日志或抓包。
* 已经有画面但 FPS 低：属于后续视频供给问题。本版没有改变解码、公告 FPS、
  TCP 参数和 USB/HCD 行为，不能用它宣称完成 60 FPS 优化。

## 主机验证

`make -j8 check` 为协议回归；`make check-lubanlite-demo SDK_ROOT=...` 驱动实际
启动页工作线程，覆盖切换、异步失败、首帧、蓝牙→Wi-Fi 接管、管理页停机和
超时。`make check-lubanlite-bonds SDK_ROOT=...` 使用实际 settings 后端测试恢复、
重入回调、压缩、损坏尾部修复、I/O 传播、备份恢复、删除和身份保留。
`make check-rtthread-multicast` 用实际 RT-Thread socket port 及主机 socket
验证显式组播出口、复位和失败返回。均不运行实际蓝牙控制器或渲染 LVGL。
SDK 的 `python3 packages/third-party/zephyr-bluetooth/tests/run_br_keys.py` 编译
实际 `keys_br.c`，分别验证 1／4 个槽位、满池恢复返回错误及主动删除后释放槽位；
仅替换其外部依赖，不运行 SSP 或控制器。

## v30：已有画面但 iPhone 一直显示 Connecting

v29 的实机日志已证明无线视频可用：动态阶段约 21～25 FPS，解码约
9～10 ms、显示约 7～8 ms。随后静止界面的低帧率，且 assembly 接近零、
sender_gap 随之增大，不能单独当成接收停顿。

但连接完成链路存在已确认的代码缺口：

- 初始 SETUP 的 `enabledFeatures` 声明 `iAPChannel`，实际却没有接受
  `type=130 (0x82)`；手机请求后返回空 streams。
- `modesChanged`、`disableBluetooth` 被统一当成未知命令返回 405。
- RFCOMM 启动链路没有由 AirPlay 触发的安全交接出口。

这证明此前处于“视频已启动，但运行期 iAP2/交接未完成”的实现状态。
它是 Connecting 不结束的首要候选；弹窗是否因此消失仍须实机确认，
主机测试与编译不能替代该验证。

v30 按 DiPlay `CarPlayMediaEngine.kt`、`IapTunnel.kt`、
`AirPlayIapTunnelStream.kt`、`Iap2LinkEngine.kt` 和 Controller 的交接门槛补齐：

1. 只接受 iAP UUID `E9459FD0-BCAD-4C45-820F-1E72447EF2F2`，要求有效 seed；
   响应包含 `type=130 / streamID=1 / dataPort`，存在时回显 streamConnectionID。
   当前仅支持单独的 type-130 SETUP；混合请求、其他 DataStream UUID 仍拒绝。
2. 用 **seed**（不是 streamConnectionID）派生 `DataStream-Salt<seed>` 输出密钥；
   只接受当前 RTSP 对端。ChaCha20-Poly1305 解密后按 32 字节
   APTransportPackage 头的大小与 `comm` 类型解析，把链路字节交给独立 iAP2
   会话。跨 TCP/加密帧分片均可流式处理，不按 package 大小分配内存。
3. 出站完整 iAP2 链路帧通过加密 event `/command` 的 `iAPSendMessage` 发送；
   使用 1024 字节加密分帧，证书不会被原 HID 路径的 512 字节缓冲限制。
   TCP 隧道使用 control-session version 2 / zero-ACK 同步配置，并按对端同步
   参数处理无重传模式；USB、RFCOMM 的原同步配置保留。
4. `modesChanged` 作为通知确认，当前不从中推断本机的音频资源操作；
   `disableBluetooth` 校验 deviceID 后只排队。等隧道上的 MFi、Identification
   完成才释放捕获的 bootstrap ACL，保留配对与 RFCOMM listener。代次变化时
   拒绝释放新接入的手机。不能以首帧或 event TCP connected 代替交接门槛。
5. endpoint、cipher、iAP2 状态都由 RTSP owner 串行管理；错误/TEARDOWN/停止时
   清理，不复用已消耗的 seed；seed 与屏幕/音频的输出密钥使用记录共享，避免
   不同端点重复使用相同 key/nonce。MFi 完整寄存器事务也加锁，防止 bootstrap
   和新隧道的签名/证书访问交错。

本次没有实现其他 DataStream、音频播放位置反馈或未知 `/command` 的通用成功
应答；`/feedback` 等剩余协议缺口仍独立存在。这里的“tunnel authenticated”
是手机在 iAP2 上确认 MFi 成功，不是仅仅解密出一个数据包。

上板步骤（保留同一版镜像进行普通重启/重连）：

1. 刷 v30，首次按正常无线流程配对并启动。保存从蓝牙连接到首帧后至少
   30 秒的日志，同时记录手机 Connecting 弹窗出现/消失的时刻。
2. 应依次看到 `iap_setup=ready`、`Wi-Fi tunnel iAP2 starting`，再由隧道会话
   完成 Identification、MFi。`disableBluetooth accepted` 可能在前后任意一侧。
3. 成功交接的唯一新增完成日志是
   `wireless handoff complete: tunnel authenticated/identified; Bluetooth released`。
   `carplay_iap2 status` 应显示
   `tunnel=1 identified=1 authenticated=1 handoff_complete=1`。
   正常的 RFCOMM/ACL 关闭不能导致视频或 AP 被停掉。
4. 持续操作地图/列表 60 秒，验证画面和触摸；再普通断连重连、普通重启、
   USB/Wireless 模式切换各一次，确认无残留隧道或错误断开其他 ACL。
5. 若 `iap_setup=ready` 后没有 starting，检查手机是否连到 dataPort；若 starting
   后没有就绪，保留具体 iAP2/MFi 错误；若 `handoff_complete=1` 后手机仍
   Connecting，则“缺少隧道/交接足以解释弹窗”的假设被证伪，应继续检查手机
   对会话完成的其他要求，不把现有补丁描述为已实测修复。

主机验证新增：加密/网络分片、对端绑定、坏 tag、非法 package 大小、
证书大小的 event 多帧发送、TCP zero-ACK、UINT64_MAX seed、SETUP 重试、
TEARDOWN 选择 streamID、关闭后禁止重用密钥、畸形/未知控制命令。

另运行 `make check-lubanlite-handoff`：隔离编译实际 BT adapter 的交接函数和
实际 owner 的 tick/排队函数，验证认证/识别门槛、异步 HCI 关闭、新会话代次、
3 秒关闭超时、HCI 失败、对端已离开以及 bt_conn 引用平衡。只有确认 ACL
关闭后才置 handoff_complete；发出 HCI 断开命令本身不算完成。
