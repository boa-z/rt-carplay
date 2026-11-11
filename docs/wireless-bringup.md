# D213 无线 CarPlay 与新 iPhone 配对验证

本次改动接通双模式 LVGL 启动流程，并修复已找到的蓝牙交互、密钥持久化和
无线地址发布缺口。主机测试和完整 SDK 构建通过不等于新 iPhone 已实测稳定；
尚没有这版固件在板上的蓝牙认证、Wi-Fi 接管日志。

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
