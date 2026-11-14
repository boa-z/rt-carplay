[English](README.md) | [简体中文](README.zh-CN.md)

> 实现迁移：所有板级源码和 Receiver 组合现归属 Luban-Lite 宿主。遵循[宿主迁移](../../docs/zh-CN/luban-lite-migration.md)；除明确更新的部分外，下列命令描述早期移植。本 SDK 目录不再编译板级实现。

# Luban-Lite Port

## 现行集成与历史范围

以下移植记录保留原有寄存器细节、配置片段、命令和限制。其中“待验证”“从未运行”和“当前”描述原始基线，不代表现在的验收状态。后续 RFCOMM/MFi/Wi-Fi/视频历史反馈见[配对重连](../../docs/zh-CN/bluetooth-bond-reconnect.md)；这些反馈不能证明现行固件已重新通过实机验收。下表列的是历史文件名；具体实现现位于宿主 `application/rt-thread/rtcarplay/platform/`，其中 `rtcp_lubanlite.h` 直接位于该目录，不在 SDK 的 include 子目录。SDK 根构建入口编译可移植 Core/Runtime 和通用 RT-Thread 平台接口，不再添加 `port/luban-lite/include`，也不从本目录编译板级源码。宿主拥有板级 Kconfig 符号，并且只引入一次通用 SDK Kconfig。不要复制历史 Package 模式配置而产生第二个 Owner。

现行宿主通过 `application/rt-thread/rtcarplay/SConscript` 中的 `CPPDEFINES` 启用 ChaCha20/ChaChaPoly/HKDF，`tls_config.h` 已启用 Poly1305。下方四行配置块说明原有依赖要求，并非要求所有现行配置都手动修改。下方 CE 测试必须验证 `ALG_HASH_ACCELERATOR`，不能验证已被否定的 Unit 3 推测。Host 测试及完整 GUI/Headless 交叉构建与实机启动、熵源、媒体和生命周期验收分别计证，现行镜像的这些实机项目仍待验证。

从 Luban-Lite 根目录运行现行实际源码 USB 回归，默认选择 SDK 官方 CherryUSB 源码树：

```sh
make -C application/rt-thread/rtcarplay check-lubanlite-usb
```

在普通 RT-Thread Port 之上补充 ArtInChip SDK 特有的平台接口：

| 文件 | 用途 |
|------|---------|
| `rtcp_wifi_lubanlite.c` | 基于 Realtek RTL8733BS SoftAP API 的 `rtcp_wifi` |
| `rtcp_random_lubanlite.c` | 基于 ArtInChip CE TRNG 的 `rtcp_os.random` |
| `rtcp_bt_lubanlite.c` | 基于 SDK Zephyr Classic/RFCOMM Host 的 `rtcp_bt`（iAP2 Transport） |
| `rtcp_mfi_lubanlite.c` | 基于 I2C 2.0C 认证协处理器的 `rtcp_mfi` |
| `include/rtcp_lubanlite.h` | `rtcp_wifi_lubanlite`、`rtcp_bt_lubanlite`、`rtcp_mfi_lubanlite`、接口设置函数和 `rtcp_lubanlite_random` |

首个硬件目标为 ArtInChip D213 搭配 RTL8733BS（SDIO Wi-Fi + HCI H5 蓝牙）。板级设置遵循该板早期硬件移植结果。`target/d21x/demo128-nand-carplay/` 下各配置文件均与该参考对齐（只有提交的预构建 `pack/bootloader.bin` 不同，镜像使用本地构建的 Bootloader）；应用 defconfig 遵循其已验证的网络／栈配置（`LWIP_NO_RX/TX_THREAD`、256 个 pbuf、RX 线程优先级 3、16 KB lwIP/SDIO/MMC 栈、4 字节 lwIP 对齐、4 KB WLAN Workqueue）。

## Wi-Fi 平台接口

| `rtcp_wifi` 调用 | Luban-Lite 调用 |
|------------------|-----------------|
| `ap_start(ssid, pass, channel)` | `rt_wlan_config_autoreconnect(RT_FALSE)`、`rt_wlan_set_mode(RT_WLAN_DEVICE_AP_NAME, RT_WLAN_AP)`、`rt_wlan_start_ap_adv()`，轮询 `rt_wlan_ap_is_active()` |
| `ap_stop()` | `rt_wlan_ap_stop()` |
| `set_vendor_ie(ie, len)` | 先 `wext_del_custom_ie()`，再以 `PROBE_RSP \| BEACON` 调用 `wext_add_custom_ie()` |
| `sta_info()` | 驱动 API 未提供，返回 `RTCP_ENOTSUP` |

AP 必须通过 RT-Thread WLAN 框架启动，不能直接调用 `wifi_on()`/`wifi_start_ap()`。只有 `rt_wlan_set_mode()` 才执行 `rt_wlan_dev_init()` → `aic_realtek_init()` → `realtek_init()`，注册 Realtek SDIO 驱动。直接 `wifi_on()` 时 SDIO Function 未注册（`gspi_dvobj_init: get wifi sdio function fail`），驱动在 `rtw_drv_probe()` → `rt_mutex_take()` 崩溃（2026-11-09 板端证据：`mcause=4`、`mtval=0x108000177`、`mepc=rt_mutex_take+0x2c`）。框架路径也遵循板端验证过的 `poc/carplay-oss` Phase 1 顺序，包括禁用 STA 自动重连工作，避免在仅 AP 模式反复操作已链接的 Timer 对象。

Apple AirPlay Vendor IE 为 `DD 08 00 A0 40 00 00 02 00 22`（`core/discovery/airplay_vendor_ie.c`）。驱动仅保留一份自定义 IE 列表，并拒绝第二次添加，因此此平台接口总是先删除。

`rtcp_wifi_lubanlite_set_interface()` 选择承载 AP 的接口。仅 AP 模式使用 `wlan0`；板端同时运行 STA+AP 时传入真实接口名。

### 板级辅助函数

Receiver 应用还需要三项 `rtcp_wifi` 接口有意不提供的能力，它们一起声明在 `rtcp_lubanlite.h`：

| 函数 | 行为 |
|----------|----------|
| `rtcp_wifi_lubanlite_get_mac()` | `ap_start` 后读取 Realtek MAC，设备 ID／Bonjour Host Name 由其生成 |
| `rtcp_wifi_lubanlite_get_ipv4()` | lwIP 配置完成后的 AP netif 地址；未就绪时返回 `RTCP_ESTATE` |
| `rtcp_wifi_lubanlite_start_dhcpd()` | 在 AP netif 重启 lwIP DHCP Server（`LWIP_USING_DHCPD`）；未启用时返回 `RTCP_ENOTSUP` |

IP/netif 查找匹配首个具有地址的 `w*` 接口，因为 WLAN 适配层可能随初始化顺序注册为 `w0` 或 `w1`。

### 所需 Kconfig

```
CONFIG_AIC_WLAN_REALTEK=y
CONFIG_REALTEK_FULL_FNC_MODE=y
```

`CONFIG_REALTEK_FULL_FNC_MODE` 决定预构建 WLAN 库是否包含 `SetCusIE` Handler。没有该选项时，`wext_add_custom_ie()` 可以编译，但驱动运行时回答 “Command not found”，Probe Response 缺少 Vendor IE，iPhone 因而不显示 “CarPlay” 提示。

本仓库不存储 Realtek 固件 Blob 或库；它们由 SDK 提供，保留各自许可证。

## 熵源平台接口

`rtcp_lubanlite_random()` 通过公共 HAL 驱动 CE TRNG：构造对齐的 `struct crypto_task`，设置 `alg.trng.alg_tag = ALG_TRNG` 和 `data.in_len = 4`，调用 `hal_crypto_start_hash()`，等待 `ALG_HASH_ACCELERATOR` 完成位，再复制 Cache Line 缓冲区。这遵循 SDK 自身的 TRNG 实现（`packages/artinchip/aic-authorization/authorization/rng.c`）。任何失败均清零输出并返回 `false`。启动 Server 前覆盖 RT-Thread 表中的提供者：

```c
static struct rtcp_os os;

os = rtcp_os_rtthread;
os.random = rtcp_lubanlite_random;
```

依赖 `CONFIG_AIC_CE_DRV=y` 和 `CONFIG_DRIVER_HAL_EN=y`。未启用时函数编译为返回 `false` 的桩，AirPlay Server 拒绝执行 Pair-Setup，不采用弱随机回退。

两项注意事项：

- **完成单元（已修正代码）。** 2026-11-09 首次板端运行显示，等待 Unit 3 会超时（该初步推测来自 `hal_crypto_init()` 清除四个 ISR 位）。SDK 自身 TRNG 实现使用 `hal_crypto_start_hash()` 及 `ALG_HASH_ACCELERATOR` 位，Port 现与其一致。该修复的板端复测仍待执行；`carplay_randtest` 必须输出两份不同的数据及 `carplay_randtest: OK, distinct output`。
- **共享引擎。** CE 同时只能运行一个 Task。其他 CE 用户（例如 `drv_ce`、硬件 TLS 栈）活跃时，不要使用此提供者。参考配置的 mbedTLS 为纯软件，因此 TRNG 是唯一 CE 用户。

## 蓝牙 RFCOMM 平台接口

`rtcp_bt_lubanlite` 使用 `packages/third-party/zephyr-bluetooth` 提供的 Zephyr Classic BR/EDR + RFCOMM Host 实现 `rtcp_bt` Transport。无线 CarPlay 的 iAP2 over RFCOMM 路径需要它；AirPlay 镜像 Receiver 本身不需要。

| `rtcp_bt` 调用 | Zephyr 调用 |
|----------------|-------------|
| `enable(name, NULL)` | `bt_enable(NULL)`、`bt_br_set_connectable(true, NULL)`、`bt_br_set_discoverable(true, false)`，可选 `bt_set_name()` |
| `rfcomm_listen(channel, name, uuid, &chan)` | `bt_rfcomm_server_register()` 及通过 `bt_sdp_register_service()` 注册的运行时 SDP 记录 |
| `rfcomm_accept(1, &fd)` | 阻塞到 DLC `connected` 回调；支持一条链路 |
| `send(fd, buf, len)` | 按协商 DLC MTU 分块调用 `bt_rfcomm_dlc_send()` |
| `recv(fd, buf, len)` | 由 DLC `recv` 回调填充的字节环形缓冲区；`0` 表示链路关闭，`RTCP_ETIMEDOUT` 表示空闲 |

除上述接口外，Port 还导出 iAP2 Pump 所需的 `rtcp_bt_lubanlite_recv_timeout()`（显式空闲超时，使 `advance()` 按 100 ms Tick 运行）、`rtcp_bt_lubanlite_local_bdaddr()`（以显示顺序提供控制器 BR/EDR 地址，用于 iAP2 BluetoothTransportComponent；Zephyr 的 `bt_addr_t` 为小端存储），以及 `rtcp_bt_lubanlite_drop_link()`（不停止 Listener，只断开一条已接受链路）。停止后重新启动会重新启用 Listener：`rfcomm_listen()` 清除内部停止标志。

SDP 记录声明一个 128 位 Service Class UUID（传入 iAP2 UUID；`NULL` 使用 Serial Port Profile，便于移植调试）、动态分配的 RFCOMM Channel 和服务名。使用 128 位 UUID 时也携带 BluetoothProfileDescriptorList（iAP2 UUID + 版本 0x0100），这是 Apple iAP2 记录在 Service Class 之外的要求。Zephyr SDP 编码器会交换 UUID128 字节序，因此 Port 以规范顺序的逆序存储。

### 所需 Kconfig

```
CONFIG_LPKG_USING_ZEPHYR_BLUETOOTH=y
CONFIG_LPKG_ZEPHYR_BLUETOOTH_CHIPSET_RTL8733=y
CONFIG_LPKG_BT_UART="uart7"
CONFIG_LPKG_BT_H5=y
# CONFIG_LPKG_BT_H4 is not set
CONFIG_LPKG_BT_CLASSIC=y
CONFIG_LPKG_BT_RFCOMM=y
CONFIG_LPKG_BT_BUF_ACL_RX_SIZE=1024
CONFIG_LPKG_BT_RFCOMM_L2CAP_MTU=1011
CONFIG_LPKG_BT_DEVICE_NAME_DYNAMIC=y
CONFIG_LPKG_RT_CARPLAY_LUBANLITE_BT=y
```

- `LPKG_BT_H4` 必须关闭：H4 和 H5 默认都开启，均注册 `"hci"` 设备；启用 H5 时 `bt_uart.c` 将 UART 配置为 H5（偶校验）。混用会使 H4 Host 运行在 H5 配置的线路上。
- `bt_uart_init()` 以 115200/8E1 打开 `LPKG_BT_UART`；Realtek Vendor 初始化随后提高控制器和 Host 波特率，并从 `drivers/bluetooth/hci/realtek/` 下载固件。必须启用板级 `uart7` Pinmux 和 `AIC_WIRELESS_BT_PWR_GPIO` 电源引脚。
- 该 SDK 版本必须关闭 `LPKG_BT_ASSERT`。`BT_ASSERT` 展开为 `k_oops()`，但没有 `zephyr/fatal_types.h` 时，`kernel.h` 看不到 `K_ERR_KERNEL_OOPS` 枚举，导致 Classic SDP 源码编译失败。关闭自定义 Assert 后回退至内核 `__ASSERT_NO_MSG` 宏。
- ACL/RFCOMM 值决定 RFCOMM 帧大小。SDK 默认值更小；1011 与常见 iAP2 实现一致。

### 板端验证

`carplay_bt start`（examples/rtthread_app）启用 Host、注册 iAP2 UUID 并回显接收字节；`carplay_iap2 start` 将 Echo 替换为真实 iAP2 Session（`core/iap2/iap2_session`）。预期控制台输出依次为 Zephyr H5 初始化（波特率、固件下载）、`RFCOMM iAP2 service on channel N`，以及手机连接后的 `[iap2] link up`。能通过 SDP 浏览 iAP2 UUID 的蓝牙串口终端也可覆盖此路径。

**[历史基线中尚待板端验证的未知项]**

- 该 RTL8733BS 模块上的 H5 握手、固件下载及波特率切换。参考蓝牙实现使用其自己的 H5 Transport；在本节最初编写时，此 Zephyr 路径尚未在板上运行。已验证参考分支下载 65,660 字节的 Realtek 固件（`rtl8733bs_8723fs_fw`、`Realtech` Magic），而 Zephyr Host 编译 `fw_rtl8733bs_d7b8_0da7.h` 中 57,020 字节的 `RealtechC` Patch 数组；两者都是本仓库以外的 Realtek Blob。板端下载失败时，先比较 Zephyr 驱动选择的 Chip/ROM ID 和 Vendor 日志，再怀疑 UART 或 H5 握手。
- 手机对 iAP2 链路的安全要求。该历史基线的 DLC 使用栈默认安全级别，未配置 SSP I/O 能力；应用级认证仍是 iAP2 MFi 交换。
- 从不发出 `RTCP_BT_EV_LINK_KEY`：Host 没有读取 BR/EDR Link Key 的公共 API，只报告连接／断开事件。
- 只支持一个 RFCOMM Link 和一个 Listener；已注册 Listener 时 `rfcomm_listen()` 幂等，该 Zephyr 版本不能注销 SDP 记录。

## MFi 2.0C 协处理器（I2C）

`rtcp_mfi_lubanlite.c` 将 `struct rtcp_mfi` 对接至 RT-Thread I2C 总线上的 2.0C 兼容认证协处理器。启用配置为：

```
CONFIG_LPKG_RT_CARPLAY_LUBANLITE_MFI=y   # package mode
CONFIG_RTCARPLAY_MFI=y                   # app mode (sdk_app Kconfig)
CONFIG_RTCARPLAY_MFI_I2C_BUS="i2c0"
CONFIG_RTCARPLAY_MFI_I2C_ADDR=0x10
```

总线／地址默认值来自板端证据，并非猜测。CarPlay PoC 在板上验证 Apple 2.0C 于 `i2c0` 的 `0x10` 应答（`0x11` 也应答）：探测寄存器 `0x4E` 返回 ID `0x00000200`，可读证书，20 字节 Challenge 生成 128 字节签名。同总线的 `0x6A` 是 Vendor RJGT204 设备锁，由参考 `HW_SECRUITY_I2C_ID`/`HW_SECRUITY_I2C_ADDR` 属性指定，**不是** MFi 协处理器（Vendor 关于探测 `0x68` 失败的注释属于 RJGT204 路径）。板端以 100 kHz 启用 `i2c0`（PD.6/PD.7 Pinmux）。

事务形态很重要，按器件要求实现：

- 写：一次 `rt_i2c_master_send()` 发送 `[register, data...]`，随后 STOP。签名载荷（`{0x00, len}` + 20 字节 Challenge = 22 字节）超过 `rt_i2c_write_reg()` 固定命令缓冲区（`AIC_I2C_CMD_BUF_LEN` 为 8，最多 7 个数据字节），因此直接发送。
- 读：`rt_i2c_read_reg()`，先写寄存器（STOP），再单独读（STOP），以 `aicos_msleep()` 延时重试。器件不应答 Repeated-start Combined Transfer。
- 寄存器引擎为 `core/mfi/mfi2c.c`（探测 `0x4E`、版本 `0x00..0x03`、大端 ID 于 `0x04`、签名请求于 `0x20`、状态 `0x10`、签名 `0x12`、证书 `0x31`）。Challenge 必须恰好 20 字节。

`rtcp_mfi_lubanlite_start()` 探测一次并保持幂等，iAP2 Glue 在服务启动时调用。失败时 Session 使用 `mfi = NULL`，因此没有器件的板卡仍可验证 Transport，但在 IdentificationInformation 后停止。`rtcp_mfi_lubanlite_ready()` 报告探测结果，`carplay_iap2 status` 会输出它。

**历史验证状态**：PoC 已在此板验证 2.0C 寄存器引擎及事务形态（ID `0x00000200`、31 字节探测、证书读取、20 字节 Challenge 签名）。当时外设包装层仅通过构建，尚需在板上运行 `carplay_iap2 start`，观察 `I/mfi: 2.0C at i2c0 0x10: id=0x...`。与真实手机的完整 MFi 交换当时未验证。私钥从不离开协处理器；Port 只读取证书和请求签名。

## mbedTLS 配置

Luban-Lite 在构建时将 `packages/third-party/mbedtls/ports/inc/tls_config.h` 复制为 `mbedtls/include/mbedtls/config.h`。该历史文件早于 ChaCha20 和 HKDF，因此当时需要在 `tls_config.h` 中（`MBEDTLS_CONFIG_H` Guard 内）加入四行：

```c
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_HKDF_C
```

`MBEDTLS_CHACHA20_C` 和 `MBEDTLS_POLY1305_C` 是 `MBEDTLS_CHACHAPOLY_C` 的硬性要求，由 `check_config.h` 强制检查。其余所需功能 ECP/Curve25519、ECDH、AES-CTR、SHA-1/SHA-256/SHA-512 和 MPI 当时已启用。

库源码从 `mbedtls/library/*.c` 编译，因此只需改变配置，不涉及源码补丁。

## 构建集成

1. 将本仓库放入（或 Clone 至）SDK，例如 `packages/third-party/rt-carplay`。
2. 让 Kconfig 引入本仓库：在 `packages/third-party/Kconfig` 添加 `source "packages/third-party/rt-carplay/Kconfig"`，或把 `Kconfig` 复制到已有 Package 菜单。
3. 选择：

   ```
   CONFIG_LPKG_USING_RT_CARPLAY=y
   CONFIG_LPKG_RT_CARPLAY_LOG=y
   CONFIG_LPKG_RT_CARPLAY_LUBANLITE_WIFI=y
   CONFIG_LPKG_RT_CARPLAY_LUBANLITE_MFI=y
   ```

4. 从应用（或 Package）的 SConscript 引入构建脚本：

   ```python
   SConscript('<path-to-rt-carplay>/SConscript')
   ```

Group 名为 `rt-carplay`；历史构建向 Include Path 添加 `include/`、所有 `core/*/include`、`port/rtthread/include`，并在启用任一 Luban-Lite 选项时添加 `port/luban-lite/include`。现行构建边界以开头迁移说明为准。

## 板端验证（历史基线待硬件）

取得 D213 + RTL8733BS 板卡后执行以下独立检查：

1. **SoftAP + Vendor IE**
   ```
   wifi_ap_start
   ```
   等待超过 10 秒，再通过手机／PC 扫描：SSID 必须声明 Apple IE（`DD 08 00 A0 40 00 00 02 00 22`）。SDK 的 `wifi_ie_probe` Shell 命令输出当前自定义 IE 列表。
2. **mDNS**：以 `_airplay._tcp` 和 `_raop._tcp` 启动 SDK Responder，再在 Host 执行 `avahi-browse -rt _airplay._tcp`。TXT 记录由 `airplay_txt_build()` 生成（`deviceid`、`features`、`flags`、`model`、`srcvers`、`protovers`、`pi`、`pk`）。
3. **MFi 2.0C**：运行 `carplay_iap2 start`，探测必须记录 `I/mfi: 2.0C at i2c0 0x10: id=0x...`；否则 `carplay_iap2 status` 报告 `coprocessor absent`，Session 在 Identification 后停止。连接手机后，依次出现 `[iap2] MFi certificate sent` 和 `[iap2] MFi authenticated` 确认交换。证书是公开的，私钥从不离开 2.0C，也不写入 Flash 或日志。
4. **BT H5/RFCOMM**：启用 Zephyr BT 选项后运行 `carplay_bt start`：H5 Vendor 初始化必须下载固件并就绪，出现 `RFCOMM listening on channel N`，手机（或 Zephyr `bt` Shell）能够在 SDP 找到 iAP2 UUID、连接并收到 Echo。这是当时首次验证 `rtcp_bt_lubanlite` 接口，早先的“桩实现”状态不再适用。板级设置与已验证参考分支一致：`uart7` 为 1500000 波特率、启用流控，蓝牙电源为 PD.3，Realtek 固件见上文。
5. **CE TRNG**：启用 `CONFIG_AIC_CE_DRV=y` 后运行 `carplay_randtest`（两次调用 `rtcp_lubanlite_random()`）：两次均必须成功，两个十六进制 Dump 不同，且日志无轮询超时。原记录把这写成确认 Unit 3 推测；该推测已被否定，应按开头说明验证 `ALG_HASH_ACCELERATOR`。
6. **视频显示**：启用 `CONFIG_RTCARPLAY_VIDEO=y`，先运行 `carplay_video play /data/sample.h264`，日志应显示 `[carplay-vid] panel 1024x600, AirPlay display 1024x600`，解码帧计数增长。再启动 Controller Session，检查 CarPlay UI 填满面板。保持 `CONFIG_RTCARPLAY_SCREEN_WIDTH/` `HEIGHT`、面板和 `AIC_SCREEN_REAL_X/Y_RESOLUTION` 相同（此处为 1024x600）；不同显示模式会产生黑边并导致触摸错位。
7. **音频播放**：启用 `CONFIG_AIC_USING_AUDIO=y` 和 `CONFIG_RTCARPLAY_AUDIO=y`，启动手机会话：日志应显示 `[carplay-aud] playback <rate> Hz <ch> ch 16-bit`，`carplay_status` 显示包计数增长，扬声器应播放 CarPlay 音频。移动手机音量滑块时，应记录 `[carplay-aud] sender volume -<x>.<y> dB -> <pct>%`，响度跟随变化，且 `carplay_status` 输出 `carplay audio speaker volume: <pct>%`。若出现 `[carplay-aud] sound0 not found`，说明音频驱动未注册；若包计数增长却无声，检查 PA 使能引脚／电平（`AIC_AUDIO_EN_PIN_*`、`AIC_AUDIO_SPK_*`）及 Board Target 中的 Codec 接线。
8. **触摸面板**：启用 `CONFIG_RTCARPLAY_TOUCH_PANEL=y` 和 CTP 驱动（本板为 `gt911`），`carplay_start` 应记录 `[carplay-touch] gt911: <n> point(s), 1024x600 -> <w>x<h>`，触摸面板应移动 CarPlay 指针（观察 `carplay_status` 和手机 UI）。设备缺失时日志为 `touch device gt911 not found`；仍可通过 `carplay_touch down x y` 注入事件，将 AirPlay HID 路径与面板驱动分开定位。

### 官方 CherryUSB DMA 契约

D213 官方 CherryUSB EHCI Port 要求 Setup 与传输缓冲区地址均按 `CONFIG_USB_ALIGN_SIZE` 对齐（当前 Cache 配置为 64 字节）。应用缓冲区，包括配置诊断、栈上描述符、Vendor 响应和 USBMUX 包，均通过受所有权管理的对齐分配；IN 结果只按实际返回长度复制回去。分配预留完整 Cache Line，避免 Invalidate 损坏相邻堆对象。零长度传输使用 NULL 数据指针。

每次操作拥有独立 URB 和完成信号量。超时后先取消操作再释放它们。取消失败时保留 DMA 分配及回调信号量，以容忍 HCD 延迟访问；会记录错误，并占用内存直到重启。Control 返回值不包含八字节 Setup，与官方 Core API 一致。本契约适用于应用 USB Seam；独立 Ethernet Driver 管理自己的对齐 DMA 存储。

在 Host 运行当时的实际 SDK Port 回归（默认启用 ASan/UBSan）：

```sh
make check-lubanlite-usb CHERRYUSB_ROOT=/path/to/official/CherryUSB
```

覆盖调用者缓冲区未对齐、Cache Line Padding、Control 和 Bulk IN/OUT、短响应、无效完成长度、零长度包、分配／提交失败、超时取消成功，以及取消失败后的延迟回调。官方 EHCI 检查保持启用。真实控制器 DMA 和 Cache 行为仍需板端运行验证。
