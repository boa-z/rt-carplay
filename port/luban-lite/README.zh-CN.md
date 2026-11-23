[English](README.md) | [简体中文](README.zh-CN.md)

# Luban-Lite 集成

从 [luban-lite-carplay](https://github.com/boa-z/luban-lite-carplay/tree/carplay-oss/application/rt-thread/rtcarplay) 构建持续维护的 D213 固件。其应用负责连接管理器、CherryUSB 和蓝牙适配、MFi 提供者、硬件媒体和 LVGL，并将本 SDK 固定为子模块。

## 接入其他板卡

1. 调用 SDK 根目录的 `SConscript`，并且只引入一次 `Kconfig`。在宿主应用中配置板级能力。
2. 使用通用 [RT-Thread 移植层](../rtthread/README.zh-CN.md)提供时钟和套接字。宿主负责物理传输、真实熵源及身份提供者。
3. 根据接入的媒体和输入设备构建能力 plist。注册后端，在同一个线程运行 [Receiver 生命周期](../../docs/zh-CN/receiver-api.md)。
4. 执行 Host 测试及交叉构建，然后在目标板验证 USB 插拔、蓝牙配对、VSync、音频和麦克风。

## 迁移和历史资料

[迁移指南](../../docs/zh-CN/luban-lite-migration.md)列出旧板级文件现在的归属。早期寄存器实验及已过时的构建步骤可通过 `git log -- port/luban-lite/README.md` 查阅。
