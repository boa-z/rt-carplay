[English](README.md) | [简体中文](README.zh-CN.md)

# RT-Thread Port

本 Port 将 rt-carplay Core 对接到普通 RT-Thread（或 Luban-Lite）源码树，不包含 CarPlay 逻辑，仅实现 `include/rtcp/{os,net,bt}.h` 声明的三个平台接口。

| 文件 | 平台接口 | 说明 |
|------|------|-------|
| `rtcp_os_rtthread.c` | `rtcp_os` | 基于 Tick 的 `now_ms`、`rt_thread_mdelay`、`rt_malloc/free`、`rt_thread_create` 及可删除的互斥锁 |
| `rtcp_net_rtthread.c` | `rtcp_net` | SAL（`RT_USING_SAL`）或直接 lwIP BSD socket，包括 mDNS 使用的 IPv4 UDP |
| `rtcp_bt_rtthread.c` | `rtcp_bt` | **桩实现**：所有调用均返回 `RTCP_ENOTSUP` |

相对此 Port 的 `include/rtcp_rtthread.h` 导出三个常量表、`rtcp_net_rtthread_local_port()` 及 `rtcp_net_rtthread_multicast_interface()`。必须在打开 Discovery socket 前配置组播接口；Owner 只能在这些 socket 关闭时修改它。`NULL` 恢复基于路由的接口选择。

## 接线方式

新应用应通过 SDK 根 `SConscript` 使用公共 [Receiver API](../../docs/zh-CN/receiver-api.md)，由单个 Owner 负责 create/start/poll/stop/destroy。[独立 RT-Thread Headless 示例](../../examples/rtthread_headless/README.zh-CN.md) 提供完整组合，包括真实身份、持久化、熵源检查及清理。以下底层 RTSP 片段保留用于说明平台接口接线，不是完整 Receiver 应用，特意省略启动检查、配对实现、停止和资源销毁。不要将该无限循环作为生命周期模板。

从应用层向下传入平台接口；Core 从不按名称查找它们：

```c
#include "airplay_rtsp_server.h"
#include "rtcp_rtthread.h"

airplay_rtsp_server_config_t config = {
    .os = &rtcp_os_rtthread,
    .net = &rtcp_net_rtthread,
    .pairing = { /* Pair-Setup / Pair-Verify identity providers */ },
    .info_plist = info_plist,
    .info_plist_len = info_plist_len,
    .on_screen_message = on_screen_message,
};
airplay_rtsp_server_t *server = NULL;
airplay_rtsp_server_start(&config, &server);

for (;;) {
    airplay_rtsp_server_poll(server, 100); /* or interleave with your loop */
}
```

RTSP Server 通过该单次 poll 调用驱动其全部 Endpoint（Listener、Event Channel、Timing、Screen DataStream）。

`rtcp_os_rtthread.thread_create` 在返回前启动线程，栈大小单位为字节，优先级使用 RT-Thread 单位。这种线程为 Detached：入口函数返回时不得释放句柄。

Socket Seam 默认使用固定 12 个描述符的 `poll()` 数组，可容纳 RTSP、Event、Timing、Screen 及三组音频 Data/Control，并留有余量。会话需要更多描述符时，可增大 `rtcp_net_rtthread.c` 中的 `RTCP_NET_POLL_MAX`。TCP 监听、已接受及出站连接请求 TCP_NODELAY。仅启用 `NETDEV_IPV6` 或 `LWIP_IPV6` 时支持 IPv6，否则 IPv6 操作返回 `RTCP_ENOTSUP`。这些 socket 策略不能证明板端延迟或吞吐量。

## 随机数

`rtcp_os_rtthread.random` 为 `NULL`：RT-Thread 没有可移植 CSPRNG，Pair-Setup 不得回退到 `rand()`。启用配对前，复制常量 OS 表并安装真实平台熵源回调。现行 Luban-Lite 宿主通过 `rtcp_lubanlite_random` 使用 ArtInChip CE TRNG（`hal_ce.h` 中的 `ALG_TRNG`）；这是宿主实现，不是通用 Port 的依赖。应用必须检查 `rtcp_os.random`，熵源缺失或失败时不得发布 AirPlay 服务。Headless 示例对缺失熵源返回 `RTCP_EINVAL`，对提供者失败返回 `RTCP_EIO`，即使复用已有身份也会检查。

## 依赖要求

* `RT_USING_HEAP`
* `RT_USING_SAL`（推荐），或具有兼容 `SAL` 的 `closesocket()` 的 lwIP。
* mDNS 需要 UDP socket 和 IGMP 组播成员关系（lwIP 中的 `LWIP_IGMP`）；不可用 `IP_ADD_MEMBERSHIP` 时，`udp_join` 返回 `RTCP_ENOTSUP`。
* mbedTLS 2.28.x，启用以下配置：

  | 配置符号 | 用途 |
  |--------|-----|
  | `MBEDTLS_ECP_DP_CURVE25519_ENABLED` | Pair-Setup / Pair-Verify 密钥协商 |
  | `MBEDTLS_ECDH_C` | 共享秘密 |
  | `MBEDTLS_AES_C` + `MBEDTLS_CIPHER_MODE_CTR` | MFiSAP 签名加密 |
  | `MBEDTLS_SHA512_C` / `MBEDTLS_SHA1_C` | SRP、HKDF |
  | `MBEDTLS_MPI_C` | SRP-6a |
  | `MBEDTLS_CHACHA20_C` | DataStream 和 ControlCipher |
  | `MBEDTLS_POLY1305_C` | DataStream AEAD |
  | `MBEDTLS_CHACHAPOLY_C` | DataStream AEAD |
  | `MBEDTLS_HKDF_C` | AirPlay 密钥派生 |

Luban-Lite 早期移植需要向较旧的 `tls_config.h` 添加配置；历史配置参见 [Luban-Lite 集成说明](../luban-lite/README.zh-CN.md)（英文路径相对 SDK 根为 `port/luban-lite/README.md`），应用前先核对现行宿主提交的配置。不要假定所有当前 SDK 仍需要这些补充。

## 蓝牙（Classic BR/EDR + RFCOMM）

AirPlay 媒体路径使用 TCP/IP，Headless LAN 测试台可以不使用蓝牙运行。但这不提供完整无线 CarPlay 发现和激活。完整无线附件仍需蓝牙处理：

* 无线附件发现（Classic Inquiry + SDP）。
* 手机选择 BT Transport 时的 iAP2 链路。
* 宿主特有的 RFCOMM HID 输入转发。现行 D213 组合使用 AirPlay Event Channel 发送 CarPlay 触摸报告；并非每条 HID 报告都需要蓝牙。

`rtcp_bt_rtthread.c` 明确返回 `RTCP_ENOTSUP`，不会假装链路存在。具体的 RTL8733BS H5/Zephyr Classic/RFCOMM Adapter 已在 Luban-Lite 宿主 `application/rt-thread/rtcarplay/platform/rtcp_bt_lubanlite.c` 中实现；参见[宿主迁移](../../docs/zh-CN/luban-lite-migration.md)。不应因为误认为实现缺失就在此通用 Port 内复制一份。新板级 Adapter 必须实现并在硬件上验证以下流程：

1. 启动 H5（`hci_h5`），确认 `bt_hci` 报告 `HCI_READY`。
2. 为 iAP2 UUID 和 CarPlay Accessory UUID 注册 SDP 记录。
3. 接受 `bt_rfcomm_dlc` 连接，将 DLC 暴露为 `int` 句柄，由 `send`/`recv` 回调对接 `bt_rfcomm_dlc_send()` 和环形缓冲区。
4. 通过 `set_event_cb` 交付 `RTCP_BT_EV_CONNECTED` / `RTCP_BT_EV_DISCONNECTED`。

没有此 Adapter 的板卡可保持 `rtcp_bt` 未设置，或在 Host 测试中使用替身；Core 检查 `RTCP_ENOTSUP` 并跳过仅蓝牙功能。测试替身不能证明硬件支持。Headless 示例 Host 回归和 D213 交叉构建与其他 RT-Thread 板卡的实机验收分别计证。
