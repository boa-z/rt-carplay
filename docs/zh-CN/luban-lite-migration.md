[English](../luban-lite-migration.md) | [简体中文](luban-lite-migration.md)

# Luban-Lite 宿主所有权迁移

## 源码归属

D213 实现位于 [boa-z/luban-lite-carplay](https://github.com/boa-z/luban-lite-carplay/tree/carplay-oss/application/rt-thread/rtcarplay)。下列路径均相对于该仓库。SDK 仍以 Git 子模块方式位于 `packages/third-party/rt-carplay`；cherryusb-ipheth 仍是独立子模块和驱动。应用不复制协议实现或外部驱动。

| 宿主目录 | 职责 |
| --- | --- |
| `application/rt-thread/rtcarplay/app/` | Receiver 组合、连接管理器、iAP2 Pump、USB 命令、配对文件及信任缓存 |
| `application/rt-thread/rtcarplay/platform/` | Realtek Wi-Fi、Zephyr 蓝牙与 Bond、CE 熵源、MFi、CherryUSB 接口、MPP/DE 视频、音频输出采集及触摸读取 |
| `application/rt-thread/rtcarplay/ui/` | LVGL 启动页与 UI 可见性策略 |
| `application/rt-thread/rtcarplay/tests/` | 实际板级组合源码回归，以及 Host 设备、调度接口 |

这些实现此前位于 SDK 的 `runtime/rtthread/`、`port/luban-lite/` 和 `port/luban-lite/app/`。保留文件名以便审查迁移。`port/luban-lite/` 现仅包含集成文档。SDK `SConscript` 收集通用 Runtime、协议、RT-Thread Port 及可选 Codec，不选择硬件，也不调用示例应用。宿主 `SConscript` 编译组合实现和硬件后端。通用 Kconfig 符号由 SDK `Kconfig` 唯一定义，宿主应用仅 Source 一次；板级选项由宿主持有。

## 集成与兼容

使用公共 `rtcp/receiver.h` 的 Platform、Media、iAP2 描述及 Owner 线程生命周期操作。宿主组合使用公开诊断和输入输出接口，不持有内部 RTSP Server 指针。原有 `carplay_app_*` 声明现位于宿主 `app/carplay_app.h`。使用该板级 API 时，将该目录加入包含路径。旧 `examples/rtthread_app` 头文件和构建入口报告明确迁移错误，避免静默链接不完整的 Receiver。废弃的 `examples/sdk_app` 板级模板由持续维护的宿主应用及其 Target Defconfig 取代。

本次迁移保留 Wi-Fi Handoff 时序、蓝牙 Bond 文件、USB 信任记录、MPP 队列、设备操作和音频行为，不增加 USB 与无线同时待命或 Launcher V2 行为。后续宿主修改补充了单例快照副本、Owner 命令队列、持久端点存储和确认 Worker 退出。已实现的边界和 Host 证据见宿主生命周期契约。

## 可复现验证

在 Luban-Lite 仓库中，初始化子模块和固定版本的 Host 密码学依赖，然后执行实际板级组合测试集：

```sh
git submodule update --init --recursive
packages/third-party/rt-carplay/scripts/fetch-mbedtls.sh
make -C application/rt-thread/rtcarplay -j8 check
```

宿主 Makefile 构建可移植 SDK 库，执行迁移的五个音频、组合和持久化测试程序，以及视频、队列、UI 策略、触摸、管理器与启动页、Bond 恢复、USB DMA、Handoff、重连及能力回归。设备与调度接口使用模型，协议、媒体和适配函数均为实际源码。音频 Sink 继续读取 SDK 已有 AAC Fixture，通过显式路径定位，不依赖进程工作目录。`RTCP_ROOT` 可选择其他 SDK 工作副本。CI 的 Host 回归 Job 在 ASan/UBSan 下运行此命令。

SDK `make -j8 check` 和启用 Sanitizer 的 CMake/CTest 保持独立，执行 54 个测试程序及旧配置加密变体，共 55 个 CTest 用例。五个板级程序已迁移，没有用空测试替换。通用 RT-Thread 时钟测试保留在 SDK；`make check-rtthread-multicast` 使用独立 SAL 头文件接口。

GUI 与 Headless 固件 Job 交叉编译持续维护的两个 D213 Defconfig，并使用同一源码树构建的匹配裸机 Bootloader 打包完整镜像。准确源码版本、CI 结论及产物 Hash 必须另行记录。Host 回归和编译不证明开发板显示、USB 与无线重连、音频播放或麦克风工作正常。本次迁移的实机验证状态为 NOT_RUN。
