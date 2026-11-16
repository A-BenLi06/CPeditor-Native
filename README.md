# CPEditor-Native

面向原生算法竞赛编辑器的架构设计与源码研究：**Windows 使用 WinUI 3 + C++/WinRT，macOS 使用 SwiftUI + AppKit，两端共享不依赖 Qt 的 C++20 竞赛引擎**。

当前仓库包含架构规范、完整的上游 CP Editor 参考源码和正在实施的原生应用/共享引擎。C++20 领域、原生进程、批量判题和有界 C ABI 已可独立构建并通过测试；完整应用、目标平台性能与发布验收尚未完成。当前证据见 [实施状态](docs/status.md)。

## 文档入口

| 文档 | 内容 |
| --- | --- |
| [实施状态](docs/status.md) | 当前已完成部分、实际验证证据与剩余工作 |
| [总体架构](docs/architecture.md) | 组件边界、平台集成、依赖方向和拟议仓库布局 |
| [外部 checker](docs/external-checker.md) | testlib 二元退出协议、原始字节、限额与 CLI 使用 |
| [Companion 解码与导入](docs/companion.md) | 已实现的无副作用解码器，以及回环监听、去重与持久收件箱边界 |
| [会话恢复](docs/session.md) | 工作区格式、原生偏好、保存点与并发恢复文件事务 |
| [当前 Core API](docs/core-api.md) | 已实现的 C/C++ 接口、请求格式、任务与缓冲区限额 |
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

首次检出本仓库时，使用 `git clone --recurse-submodules <repository-url>`。远端仓库为 [A-BenLi06/CPeditor-Native](https://github.com/A-BenLi06/CPeditor-Native)，Git 远端名为 `origin`，本地 `main` 追踪 `origin/main`。当前本地与远端初始化提交的历史独立，代码同步前需要整合两边历史。为保证研究可复现，不要使用 `git submodule update --remote`：更新参考源码需要审查并提交 gitlink 变更，同时记录到 walkthrough。

当前上游快照声明版本为 7.3.0，使用 Qt 6。用户提供的评估基于 7.0.2 标签和 Qt 5。设计同时参考二者，并在源码审查中保留版本区别。

## 初步决策

- 首先在指定的 Windows 10 22H2 机器上验证 WinUIEdit 原型，包括原有 UU 远程及虚拟桌面问题的复现流程。
- 在 macOS 上对比 CodeEditorView 与 STTextView，将 CodeEditSourceEditor 保留为第三个候选；CodeEdit 和 BriskEdit 用于架构参考。
- 抽离竞赛业务语义，重建进程、持久化和协议适配层。编辑器缓冲区、输入法、撤销和渲染由两端原生应用分别管理。
- 两端原生桥接目前统一使用稳定的 C ABI：Swift 通过 actor 管理句柄，Windows C++ 通过后台协调线程提交和消费事件。底层服务仍提供 C++ 接口。
- 配置与本地化保持可扩展。C++ 是首个接受验证的语言配置，而非固定的语言模型。

CP Editor 源文件头允许按 GPL 第 3 版或更高版本使用。参考项目保留各自许可证。原生实现的代码复用和分发应遵循源码审查中的来源与许可决策；首次架构变更没有将这些项目的实现复制到新 Core 中。

## 当前 Core 构建

仓库根目录执行：

```powershell
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

该检查覆盖文本／比较／摘要、真实子进程、文件事务、配置／JSON、批量判题、并发任务和 C ABI。Windows 需要已配置的 MSVC 开发环境和 Ninja；macOS 需要 Xcode 命令行工具。原生界面、最低系统和发布仍按实施状态继续验收。

Windows 目标机验证入口（PowerShell 7）：

```powershell
pwsh -File ./tools/windows/verify.ps1 -Configuration Debug -Platform x64 -Deploy -Launch
```

工具前提、分阶段报告与中文实测模板见 [Windows 说明](apps/windows/README.md)。当前主机上的静态／便携检查不替代真实 Windows 构建和 UI 实测。

macOS 应用入口为 `./script/build_and_run.sh --verify`，Windows 原型入口与前提见 [Windows 说明](apps/windows/README.md)。[macOS 说明](apps/macos/README.md)记录双编辑器及真实 UI 验证范围。无界面工具可通过 `out/core/dev/cpn-cli --help` 查看配置、源码和样例参数，语言／工具链定义位于 `resources/profiles`。
