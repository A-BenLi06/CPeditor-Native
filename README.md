# CPEditor-Native

面向原生算法竞赛编辑器的架构设计与源码研究：**Windows 使用 WinUI 3 + C++/WinRT，macOS 使用 SwiftUI + AppKit，两端共享不依赖 Qt 的 C++20 竞赛引擎**。

当前仓库包含架构规范和完整的上游 CP Editor 参考源码，包括其嵌套子模块。原生应用与新引擎尚未实现，也未进行性能测试。

## 文档入口

| 文档 | 内容 |
| --- | --- |
| [总体架构](docs/architecture.md) | 组件边界、平台集成、依赖方向和拟议仓库布局 |
| [Core 契约](docs/core-contracts.md) | 源码快照、任务生命周期、进程限制、判题结果、桥接所有权和持久化 |
| [上游源码审查](docs/upstream-audit.md) | 有源码依据的迁移清单及兼容性差异 |
| [编辑器评估](docs/editor-evaluation.md) | 已核实的依赖、许可证、编辑器取舍和原型验收条件 |
| [实施计划](docs/implementation-plan.md) | 分阶段里程碑、验收证据和尚待解决的决策 |
| [改动记录](walkthrough.md) | 带时间戳的改动、思路和验证结果 |

## 上游源码

[CP Editor](https://github.com/cpeditor/cpeditor) 已作为 Git 子模块克隆到 `references/cpeditor`。[上游源码审查](docs/upstream-audit.md)记录了准确的研究基线；父仓库的 Git 提交固定参考源码版本。它用于研究，不参与后续原生应用的构建依赖图。

在仓库根目录执行以下命令，兼容 PowerShell 7：

```powershell
git submodule update --init --recursive
git -C references/cpeditor rev-parse HEAD
git -C references/cpeditor show 7.0.2:CMakeLists.txt
git submodule status --recursive
```

首次检出本仓库时，使用 `git clone --recurse-submodules <repository-url>`。这个新建的本地仓库尚未配置远程地址。为保证研究可复现，不要使用 `git submodule update --remote`：更新参考源码需要审查并提交 gitlink 变更，同时记录到 walkthrough。

当前上游快照声明版本为 7.3.0，使用 Qt 6。用户提供的评估基于 7.0.2 标签和 Qt 5。设计同时参考二者，并在源码审查中保留版本区别。

## 初步决策

- 首先在指定的 Windows 10 22H2 机器上验证 WinUIEdit 原型，包括原有 UU 远程及虚拟桌面问题的复现流程。
- 在 macOS 上对比 CodeEditorView 与 STTextView，将 CodeEditSourceEditor 保留为第三个候选；CodeEdit 和 BriskEdit 用于架构参考。
- 抽离竞赛业务语义，重建进程、持久化和协议适配层。编辑器缓冲区、输入法、撤销和渲染由两端原生应用分别管理。
- Swift 桥接使用稳定的 C ABI；Windows C++ 界面通过 C++ API 调用同一引擎。
- 配置与本地化保持可扩展。C++ 是首个接受验证的语言配置，而非固定的语言模型。

CP Editor 源文件头允许按 GPL 第 3 版或更高版本使用。参考项目保留各自许可证。原生实现的代码复用和分发应遵循源码审查中的来源与许可决策；首次架构变更没有将这些项目的实现复制到新 Core 中。
