[English](../wired-pairing-persistence.md) | [简体中文](wired-pairing-persistence.md)

# v22 配对持久化与启动页管理

## 历史范围与当前集成

本文保留 v22/v23 的调查和验证事实。“本版”和“当前行为”指当时基线，不代表现行版本已完成同等实机验收。D213 组合实现、文件系统辅助函数、USB Trust 缓存、Launcher 和实际源码回归现归属 Luban-Lite 宿主 `application/rt-thread/rtcarplay/`；参见[宿主迁移](luban-lite-migration.md)。保留历史测试数量和板端步骤，不声称已在当前硬件上重新执行。

## 已识别的失败

v21 日志进入第三次 AirPlay Pair-Setup 交换，报告 `pair_setup result=7`，发送短 TLV 错误响应后手机关闭连接。`airplay_pair_setup.h` 中的 7 是 `AIRPLAY_PAIR_SETUP_STORAGE_FAILED`，与 USB Trust、iAP2 认证和 MFi /auth-setup 不同。

接收器此前使用 `rename("...tmp", "...bin")` 保存控制器表。SDK 的 DFS Elm/FatFS 适配直接调用 `f_rename`，该 API 拒绝已存在的目标（`FR_EXIST`），所以首次创建可以成功，更新已有文件却会失败。Host POSIX rename 语义掩盖了这一差异。

应用现在先写入并关闭临时文件，再将已有主文件移动到不存在的 `.bak`，然后把临时文件移动为主文件。提交失败时尝试回滚；主文件缺失时可以加载备份。相同控制器标识和公钥的更新不再重复写 Flash。这是尽力恢复中断的方案，并非完整掉电文件系统事务。Launcher 记录存储错误并及时退出连接等待；只有解码器已输出一帧后才声明视频激活。

## USB Trust 复用

应用查询 Lockdown UniqueDeviceID，并将带版本的本地配对记录保存到 `/data/rt-carplay-usb-<UDID>.bin`。后续连接加载已有 HostID、SystemBUID、证书与私钥，跳过 Pair 和 RSA 生成，直接进入 StartSession/TLS/StartService。只有 StartSession 明确返回 InvalidHostID、InvalidPairRecord 或 MissingPairRecord 时才重新尝试 Pair。网络或 TLS 失败不会删除持久化记录。

记录包含宿主生成的私钥；UI 仅列出名称和大小，不打印或导出密钥材料。格式包含 Magic、版本、本地记录大小、校验和与字段边界验证。校验和用于发现意外损坏，不防篡改；该本地格式也不能跨 ABI 变化移植。禁止把 `/data` Trust 文件发布到公共仓库或镜像。

首次连接仍需手机授权。手机重置或撤销 Trust、删除本地记录、记录 ABI 改变或重新烧录并擦除 `data` 分区，都可能再次要求 Trust。重启测试应保留 `/data`。真实 iOS Trust 复用仍需实机验证。

## LVGL 控件

英文启动页使用已有内置字体，显示当前连接阶段、前一阶段或失败阶段、USB/IPv6、USB Trust 缓存结果、AirPlay 存储结果、接收和显示帧数、丢帧、解码错误、HID 结果及最后错误。这些是诊断快照，不是同步的性能测量。

* **Pair files**：等待或失败后可用。打开管理页会暂停自动配对，即使此时插入手机。Worker 从 `/data` 列出最多 12 个逻辑记录，包括 USB 记录和 AirPlay 控制器表 `rt-carplay-pairings.bin`。仅备份或临时文件存在的条目也会列出，便于删除；删除后刷新可查看更多记录。
* 点击条目可查看全名，并显示 **Cancel / Forget record** 确认。删除 USB 记录后，对应 iPhone 可能重新要求 Trust。删除 AirPlay 表会忘记所有 AirPlay 控制器，但不删除接收器身份。
* 确认后的删除由会话所有者 Worker 在 USB 会话关闭时执行：停止 Receiver，删除临时、备份和主文件，再重启 Receiver 以丢弃内存中的 AirPlay 表。错误显示为数值结果。Allowlist 排除设备 Seed，并拒绝路径穿越或任意文件删除。
* **Refresh**、**Back** 和失败后的 **Retry connection** 避免依赖 Shell 命令。失败仍不会自动循环触发 Trust 对话框。

Launcher 在其生命周期内独占唯一 GT911 读取器。视频输出前，它向有界指针队列送入事件，由 LVGL 输入回调消费；视频激活后，将触摸转发到 CarPlay 队列。GUI 指针跟踪单指，CarPlay 保留原有多点触摸报告。SDK 的 LVGL 面板读取器仍禁用，因此 Receiver 重启或存储失败不会禁用管理页触摸。所有 LVGL 调用均在 GUI 线程，TLS 和文件操作在 Worker 执行。阶段锁防止管理与配对竞态，也防止触摸报告与 Receiver 拆除竞态。

## 验证

* 当时的 50 项 Host 测试及边界检查在 ASan/UBSan 下通过。新增测试模拟 FatFS 不覆盖重命名、提交失败和回滚恢复、USB 缓存往返与损坏及边界，以及文件管理 Allowlist、恢复副本删除和身份文件保留。恢复的真实 TLS Lockdown 模拟手机会话发送零次 Pair 请求，并保留原 HostID。
* 实际启动 Worker 测试覆盖 Trust 失败、存储失败、重插、网络就绪、首个解码帧、延迟 iAP2 退出、指针路由与释放和溢出，以及管理页打开时排斥配对。
* 实际视频 Sink 和触摸读取器接缝回归使用 SDK 头文件通过；完整 D213 SDK 与镜像构建通过。Host 测试不验证帧缓冲外观、物理面板行为或真实 iOS Trust 保留。

板端检查无需启动命令：

1. 手机未插入时启动，打开 Pair files，使用 Refresh 和 Back，确认未列出 Seed。插入已解锁手机，首次接受授权后验证画面和触摸；无解码帧时不得声明视频激活。
2. 重插同一手机，连接期间检查 `Trust record: REUSED`，确认不要求 Trust。保留 `/data` 重启，再重复测试。
3. 拔出手机，经确认仅删除该手机 USB 记录，再连接。应发生新的 USB 配对，并可能重新要求 Trust。先取消一次删除，验证记录仍保留。AirPlay 表删除是另一项独立操作。
4. 连接失败时保留阶段、前一阶段、最后错误和存储、Trust 诊断，尝试 Retry connection。存储失败应返回可用管理页，不能呈现虚假已连接状态。

本版修复持久化和示例管理，保留此前视频节奏诊断，不声称解决了剩余卡顿。

## v23：RT-Thread 负 errno 回归

v22 板端日志在任何手机配对之前就停在 `cannot load pairing store`，阶段 7、错误 -1。此 SDK 与 Host POSIX 不同，保留负的文件系统 errno：`dfs_elm.c` 将文件不存在映射为 `-ENOENT`，`dfs_posix.c:open()` 将结果传给 `rt_set_errno()`，`kservice.c` 原样保存，而 Newlib `syscalls.c:__errno()` 直接返回 `_rt_errno()`。v22 只判断 `errno == ENOENT`，因此干净数据分区没有控制器文件也被判为 I/O 错误。同样假设还影响备份查询、首次保存、删除缺失备份及管理删除。

v23 在每个配对文件调用点识别特定错误的正、负两种符号。主文件与备份都缺失时返回空表；主文件缺失但备份存在时加载备份。真实权限、I/O、读取或大小错误仍返回错误；修复不删除 Trust 文件、不重置设备身份，也不静默接受损坏记录。加载失败会记录路径及原始 errno，或读取、大小和关闭上下文。解决该回归无需改变 USB 配置或手机设置。

持久化测试现对每个场景分别使用 POSIX 正 errno 和 SDK 负 errno。增加 SDK 风格包装函数后，在修复前复现了 v22 首次空目录启动失败，修复后通过。两种模式均覆盖新文件保存、反复替换、中断备份恢复、Trust 缓存、管理删除，以及真实访问和超大记录错误的保留。Makefile 依赖也包含测试实际 include 的平台源码，避免修改后复用过期测试二进制。

烧录 v23 时先拔出手机：Receiver 应进入正常等待页，而非 `cannot load pairing store`。连接一次验证画面和触摸，然后重插同一手机检查 Trust 复用。尽量保留已有 `/data`；删除配对表不是 v22 errno 错误的规避办法，因为文件缺失同样会触发错误。修正镜像仍需真实设备验证。
