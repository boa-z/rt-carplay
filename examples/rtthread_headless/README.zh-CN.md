[English](README.md) | [简体中文](README.zh-CN.md)

# 独立 RT-Thread Headless 接收器

## 范围

`headless.c` 是可在普通 RT-Thread Worker 中执行的 Receiver 组合实现。它创建或加载软件 Ed25519 身份，加载和保存真实 SDK 控制器表，构建仅视频的 `/info` 文档，启动公共 Receiver，并可通过 mDNS 提供 `_airplay._tcp.local` 服务。同一份源码在 Host 回归中使用真实 Linux OS 和网络 Provider 运行。它不依赖 Luban-Lite、SoC、GUI、屏幕、声卡或触摸设备，与已退役的 `examples/rtthread_app` 板级模板以及当前维护的 D213 应用相互独立。

这是局域网协议验证示例。视频会被有意统计后丢弃；不声明音频能力，也不打开音频设备。示例不启动 USB 枚举、蓝牙、MFi 或 Wi-Fi Handoff，不实现 CarPlay Control 反向连接策略，也不声称能够自动激活手机。这些服务仍由宿主负责。完整 D213 有线和无线集成应使用当前维护的 Luban-Lite 应用。本示例不改变该应用的媒体协商或输出行为。

## 依赖与构建

需要 C11 编译器、启用软件 Ed25519 的可移植 SDK、mbedTLS、已初始化的 RT-Thread BSD Socket 网络，以及已挂载且支持标准 C 文件接口的文件系统。通过 SDK 根目录 `SConscript` 构建，启用 `LPKG_USING_RT_CARPLAY`、`LPKG_RT_CARPLAY_ED25519` 和 `LPKG_RT_CARPLAY_HEADLESS_EXAMPLE`，并将 `examples/rtthread_headless` 加入应用头文件搜索路径。最后一个选项只编译示例，不会自动启动第二个 Receiver，不使用旧板级模板的 SConscript。

通用 `rtcp_os_rtthread.random` 有意保持 NULL。复制该 OS 表，并接入实际平台可用的密码学安全熵源回调。缺少熵源返回 `RTCP_EINVAL`；Provider 执行失败返回 `RTCP_EIO`，已有身份文件时也会检查。本示例不使用弱随机数、模拟认证、固定私钥或返回成功的占位 Provider。下方 MAC 格式的设备 ID 是公开示例值；每台配件应配置稳定且唯一的 ID。

## RT-Thread 集成

先完成文件系统和网络初始化，再从串行化的应用控制线程调用以下包装函数。将真实平台 CSPRNG 传给 `start_headless()`。包装函数在唯一所有者 Worker 上运行真实阻塞示例；另一个线程可以请求停止并等待结束。32 KiB Worker 栈沿用现有 Receiver 组合的预算；产品验收前仍需测量实际板卡的栈和内存使用。

```c
#include <rtthread.h>
#include "rtcp_rtthread.h"
#include "headless.h"

static struct rtcp_os headless_os;
static rtcp_headless_config_t headless_config;
static rtcp_headless_result_t headless_result;
static atomic_bool stop_requested = ATOMIC_VAR_INIT(false);
static atomic_bool done = ATOMIC_VAR_INIT(true);
static int last_result;

static void headless_worker(void *argument)
{
    last_result = rtcp_headless_run(argument, &stop_requested, &headless_result);
    atomic_store_explicit(&done, true, memory_order_release);
}

int start_headless(bool (*secure_random)(uint8_t *, size_t))
{
    if (secure_random == NULL) return RTCP_EINVAL;
    bool expected = true;
    if (!atomic_compare_exchange_strong(&done, &expected, false)) return RTCP_ESTATE;
    headless_os = rtcp_os_rtthread;
    headless_os.random = secure_random;
    headless_config = (rtcp_headless_config_t){
        .os = &headless_os, .net = &rtcp_net_rtthread,
        .name = "Headless receiver", .device_id = "02:00:00:00:00:01",
        .seed_path = "/data/headless.seed", .store_path = "/data/headless.controllers",
        .width = 1024, .height = 600,
    };
    atomic_store(&stop_requested, false);
    rt_thread_t worker = rt_thread_create("headless", headless_worker,
                                         &headless_config, 32 * 1024, 15, 20);
    if (worker == NULL) { atomic_store(&done, true); return RTCP_ENOMEM; }
    if (rt_thread_startup(worker) != RT_EOK) {
        rt_thread_delete(worker);
        atomic_store(&done, true);
        return RTCP_EIO;
    }
    return RTCP_OK;
}

int stop_headless(void)
{
    atomic_store_explicit(&stop_requested, true, memory_order_release);
    for (unsigned waited = 0; !atomic_load_explicit(&done, memory_order_acquire) && waited < 3000;
         waited += 10) rt_thread_mdelay(10);
    return atomic_load_explicit(&done, memory_order_acquire) ? last_result : RTCP_ETIMEDOUT;
}
```

包装函数属于普通线程操作，应用必须串行调用。禁止从 Receiver 回调中调用 `stop_headless()`，也不能在停止返回 `RTCP_ETIMEDOUT` 时重置或释放配置。超时表示 Worker 仍拥有资源。`start_headless()` 成功只表示 Worker 已被调度，不代表协议监听器已就绪；通过 `on_ready` 或最终 Worker 返回值观察启动结果。仅在以 acquire 语义读到为 true 的 `done` 标志 后读取 `last_result` 和 `headless_result`。OS、网络表、配置字符串和回调上下文必须存活至 `rtcp_headless_run()` 返回。

设置 `advertise=true`、`host_name` 和真实本地 `ipv4` 可启用 mDNS。网络 Port 如需选择组播发送接口，应在启动前配置。`on_ready` 在所有者线程收到绑定的 RTSP 端口和公钥；如需保留公钥，必须在回调返回前复制。`on_state` 通过公共 Receiver 状态分别提供 Transport Connected、Authenticated、Session Established、Video Ready、Audio Ready 和 Input Ready，不会用单一连接标志推断就绪状态。回调不得阻塞，也不得递归启动或停止同一 Receiver。

## 身份、持久化与所有权

使用具备适当访问控制的私有挂载目录。示例仅在身份文件不存在时创建 32 字节 Seed，绝不静默替换损坏或无法访问的身份数据。即使复用现有 Seed，启动也会检查真实熵源。控制器记录采用 SDK 整体 Blob 格式和真实保存、查询回调。记录损坏时拒绝启动。配对记录写入失败会从运行循环向外传播，不会被视为已持久化成功。

写入采用同目录 `.tmp` 文件，先将已有主文件移动到不存在的 `.bak`，再将临时文件重命名为主文件。提交失败时尝试恢复原主文件；读取仅在主文件缺失时恢复备份。该策略支持 FatFS 不覆盖目标的重命名语义，同时识别 POSIX 正 errno 和 DFS 负 errno。主文件损坏时仍返回错误。宿主必须保证两个路径的独占访问；主文件、临时文件、备份路径和符号链接别名不能冲突。本示例不实现跨进程文件锁、掉电日志、`fsync`、私有文件权限配置或加密存储。有这些保证要求时应使用产品存储后端。禁止将 Seed 文件放入仓库、产物包或诊断日志。测试临时 Seed 在运行时生成并在结束时删除。

示例同时只允许一个运行实例。SDK 控制器存储是进程全局状态，因此宿主还必须排除其他使用该存储的应用组合。Worker 拥有 create/start/poll/stop/destroy、身份、控制器记录和 mDNS。另一个线程只能设置原子停止请求，然后等待或 Join。清理先关闭发现服务并销毁 Receiver，再退出控制器存储并清除软件私钥。函数返回后不再触发协议回调。结果计数器和诊断由所有者写入，只能在 Join 后读取，不能当作实时并发快照。

## 验证与限制

`make -j8 check` 包含 `test_rtthread_headless`；可以通过 `make -j8 build/test_rtthread_headless && build/test_rtthread_headless` 单独运行。CMake 测试目标同名：

```sh
cmake -S . -B build-cmake -DRTCP_BUILD_TESTS=ON -DRTCP_SANITIZE=ON
cmake --build build-cmake -j8
ctest --test-dir build-cmake --output-on-failure
```

测试将实际示例与真实 Linux Socket、熵源、Ed25519 和控制器存储实现链接，检查 TCP `/info` 及其 1024×600 二进制 Plist 几何信息、真实 UDP/mDNS 查询和端点解析、三次重启中的稳定身份与控制器记录持久化、提交回滚、仅备份存在时的身份和控制器恢复、嵌套运行排斥、带活动客户端的停止、预先请求停止、缺失或失败的熵源、无法访问的路径、损坏的 Seed 或记录、监听和发现服务失败，以及持久化写入失败。控制器存储测试在所有者线程直接调用配置使用的同一套 SDK 保存、查询回调，不声称新增了完整线上 Pair-Setup 测试。已有 SDK 加密 Pair-Setup、Pair-Verify 和媒体测试仍独立执行且实现不变。每个场景分别在 POSIX 正 errno、DFS 负 errno 和 FatFS 不覆盖目标的重命名语义下执行。本次 Host 执行启用 ASan/UBSan。

当前维护的 D213 Headless defconfig 同时启用该源码在真实 RT-Thread/BSP 工具链中的编译，但不启动这个额外示例。Host Tested 与 Cross-Compiled 是不同证据，均不能代表 Hardware Validated。非 D213 RT-Thread 板卡上的实际启动、配对、媒体接收、堆栈使用和停止重启仍为 Pending Validation。本示例也不代表 D213 GUI 或 Launcher V2 已通过实机验收。
