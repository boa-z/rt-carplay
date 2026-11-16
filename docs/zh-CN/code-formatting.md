[English](../code-formatting.md) | [简体中文](code-formatting.md)

# C 与 C++ 格式

SDK 和 Luban-Lite CarPlay 应用使用相同的 RT-Thread 风格 clang-format 配置：基于 LLVM，4 空格、不使用 Tab、花括号另起一行、指针星号靠右、保留 include 顺序。来源为 [RT-Thread 配置 7c974b59fdc258afbdb19c378f250bcd95511537](https://github.com/RT-Thread/rt-thread/blob/7c974b59fdc258afbdb19c378f250bcd95511537/.clang-format)。调整两项策略：`InsertBraces: false` 防止格式化器自动添加控制流花括号，`LineEnding: LF` 保证 Windows 与 Linux 输出一致。include 排序、注释重排和结构改写保持关闭，沿用上游不限制行宽的设置。采用此风格不会给可移植 SDK 引入 RT-Thread 依赖。

使用 clang-format 23.1.1 复现全量格式化提交和 CI，可通过 `python -m pip install clang-format==23.1.1` 安装。在 SDK 根目录运行：

```sh
python3 scripts/format-code.py
python3 scripts/format-code.py --check
```

在 Luban-Lite 工程中运行：

```sh
python3 packages/third-party/rt-carplay/scripts/format-code.py --root application/rt-thread/rtcarplay
python3 packages/third-party/rt-carplay/scripts/format-code.py --root application/rt-thread/rtcarplay --check
```

脚本仅枚举选定目录中已跟踪的 C/C++ 文件，`.clang-format-ignore` 使用简单相对路径通配符。第三方 `third_party/` 实现、生成的协议/音频字节数组、字体子集及上游适配生成文件保留生成器或上游格式。手写 SDK 核心、公开/内部头文件、运行时、移植层、示例、应用和已有测试源码均在范围内。新增未跟踪文件应直接运行 clang-format，或先暂存再使用脚本。

功能提交与全量格式化分开。CI 只检查两处格式，不自动改写。固件编译、现有 Host 回归和实际 LVGL 预览分别验证格式化后的代码；格式检查成功不能证明开发板或 iPhone 验收。
