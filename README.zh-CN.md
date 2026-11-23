[English](README.md) | [简体中文](README.zh-CN.md)

# rt-carplay

[![ci](https://github.com/boa-z/rt-carplay/actions/workflows/ci.yml/badge.svg)](https://github.com/boa-z/rt-carplay/actions/workflows/ci.yml)

rt-carplay 是用于实现 Classic CarPlay 接收端的可移植 C SDK。SDK 处理 CarPlay 协议会话，并向宿主交付编码媒体及元数据。宿主负责设备 I/O、蓝牙和 Wi-Fi 策略、USB 枚举、视频解码及显示、音频设备和 UI。

## 从这里开始

| 目标 | 文档 |
| --- | --- |
| 了解 Classic 支持范围和实机验收状态 | [Classic 能力矩阵](docs/zh-CN/classic-capability-audit.md) |
| 把 SDK 接入应用 | [Receiver 接入](docs/zh-CN/receiver-api.md)及[生命周期和所有权](docs/zh-CN/lifecycle-and-ownership.md) |
| 了解代码布局和模块如何协作 | [架构](docs/zh-CN/architecture.md) |
| 移植到 RT-Thread 或使用无界面示例 | [RT-Thread 移植层](port/rtthread/README.zh-CN.md)和[RT-Thread 无界面示例](examples/rtthread_headless/README.zh-CN.md) |
| 在 ArtInChip D213 上运行或接入其他 Luban-Lite 板卡 | [Luban-Lite 集成](port/luban-lite/README.zh-CN.md) |

## 运行 Linux 测试程序

需要 POSIX Shell、C11 编译器、Python 3、Make、pthread 及指定版本的 mbedTLS 2.28 源码。测试程序使用真实套接字和可持久化的软件身份。收到的 H.264 写入文件，程序不驱动显示器或扬声器。

```sh
./scripts/fetch-mbedtls.sh
make -j8 check
./build/rt-carplay-sim --ip 192.168.1.20 \
    --seed-file ./identity.seed --pairing-store ./pairings.bin \
    --dump-h264 ./screen.h264
```

将示例地址换成宿主的局域网地址。用 `--help` 查看参数，默认配对 PIN 为 `3939`。保留身份种子和配对数据库，以便测试重连。在真实设备上使用 CarPlay，需要为 Linux 或通用 RT-Thread 移植层接入相应附件适配器。

## 接入 Receiver

1. 引入 `rtcp/receiver.h`。提供时钟、套接字、配对及身份回调和能力 plist，再注册媒体后端。完整示例见 [Linux 组合实现](examples/linux_sim/main.c)。
2. 在宿主指定的同一个线程中调用 `rtcp_receiver_create()`、`rtcp_receiver_start()` 和 `rtcp_receiver_poll()`。UI 和输入命令通过队列交给该线程。回调参数只在回调返回前有效，需要交给其他线程处理的数据应先复制。
3. 在 Receiver 所属线程调用 `rtcp_receiver_stop()`，并在释放借用的提供者之前销毁 Receiver。宿主设备线程使用的内存应在线程停止并退出后释放。

[接入指南](docs/zh-CN/receiver-api.md)说明 H.264、音频、HID、Classic 通知和低层 AirPlay、iAP2 调用的兼容方式。根据所用传输查看相应平台指南，由宿主管理硬件策略。

## 开发和验证

Make 回归默认启用 ASan/UBSan。也可用 CMake 3.16 或更高版本运行 Host 回归。

```sh
cmake -S . -B build-cmake -DRTCP_BUILD_TESTS=ON -DRTCP_SANITIZE=ON
cmake --build build-cmake -j8
ctest --test-dir build-cmake --output-on-failure
make check-docs
make docs
```

`make docs` 使用 Doxygen 在 `build/doxygen/` 生成公共 C API 的 HTML。构建版本来自 `include/rtcp/version.h` 和当前检出的提交。使用 `rtcp_version_string()` 和 `rtcp_build_git_sha()` 分别获取两者。

修改公共 API、选项或外部可见行为时，同步更新接口契约和中英说明。功能状态统一记录在能力矩阵。内部重构或仅调整测试时，可以按实际影响决定是否更新 README。参见[代码格式](docs/zh-CN/code-formatting.md)。

## 协议资料和研究

协议调试可查阅[音频协商](docs/zh-CN/audio-negotiation.md)和[视频会话](docs/zh-CN/video-stream-session.md)。[无线调试](docs/zh-CN/wireless-bringup.md)、[蓝牙旧配对重连](docs/zh-CN/bluetooth-bond-reconnect.md)、[USB 信任](docs/zh-CN/wired-pairing-persistence.md)以及[音频调试交接](docs/zh-CN/audio-debug-handoff.md)保留历次调查。Classic 之外的实验见 [Ultra 基础](docs/zh-CN/ultra-foundation.md)。

## 许可证和致谢

**GPL-3.0-or-later**。组件和第三方许可见 [LICENSE](LICENSE) 和 [NOTICE](NOTICE)。Based on LIVI by Lasse Heitgres (https://github.com/f-io/LIVI)。iAP2 实现源于 `esp32-carplay-web-relay` 和 DiPlay，并保留原始声明。
