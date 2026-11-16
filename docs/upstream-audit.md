# CP Editor 源码审计与迁移映射

研究日期：2026-11-16（UTC+08:00）。审计结论来自固定源码版本的观察；迁移目标和改进措施属于拟议架构。

## 源码获取与基线

| 来源 | 提交版本 | 用途 |
| --- | --- | --- |
| 本地参考目录 `references/cpeditor`，上游分支 `master` | `77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c` | 当前源码的迁移基线；声明项目版本为 7.3.0 |
| CP Editor 标签 `7.0.2` | `8f8cfafae7c6f3ab4923ce0e08cdb60c2b0e0640` | 用户提供的评估采用此基线；已克隆的历史中可以访问 |

表格第一行记录的是实际获取的提交，不代表对最新发布版本的判断。本地克隆包含上游历史和全部嵌套子模块。父仓库通过 gitlink 固定参考版本，并未将上游实现复制到新的 Core 中。可使用 [README](../README.md) 中的命令检查准确版本。

[当前 CMake 配置](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/CMakeLists.txt#L7) 声明 7.3.0、C++14、Qt 6 Widgets/Network/Core5Compat，以及 KF6 语法高亮。[7.0.2 的 CMake 配置](https://github.com/cpeditor/cpeditor/blob/8f8cfafae7c6f3ab4923ce0e08cdb60c2b0e0640/CMakeLists.txt#L7) 使用 C++14、Qt 5 和 KF5。原生性能实验必须区分出现问题的 Qt 5 构建与当前 Qt 6 源码。

## 迁移清单

下列所有 CP Editor 链接均指向当前固定提交。

| 上游组件 | 已观察到的依赖或行为 | 原生架构中的目标 |
| --- | --- | --- |
| [Compiler](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Core/Compiler.cpp#L56) | 依赖 `QObject`、`QProcess`、Qt 设置／文件／编码转换；按 C++/Java/Python 的语言名称分支；解释执行的 Python 跳过编译；C++ 添加原文件的包含目录 | `BuildService`、声明式工具链配置和原生进程宿主；保留有价值的包含目录及运行规则 |
| [Runner](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Core/Runner.cpp#L60) | Qt 进程／定时器、重定向的临时输入文件、命令构造和独立终端路径 | `RunService` 与原生 `ProcessHost`；分别定义终端和交互执行能力 |
| [Checker](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Core/Checker.hpp#L31) | 为获取判定结果类型而包含 Widget 头文件；提供内置文本检查，并在运行时编译附带的 testlib／自定义 checker | 纯比较策略、领域层判定结果和外部 checker 服务 |
| [SessionManager](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Core/SessionManager.cpp#L18) | 包含生成的应用 UI 和窗口类；直接恢复标签页；使用 Qt JSON／定时器／文件行为 | 带版本的 `SessionRepository`；由两端原生外壳恢复各自视图 |
| [SettingsManager](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Settings/SettingsManager.cpp#L148) | 用户设置采用 QSettings INI/QVariant；JSON 文件驱动 Qt 设置／UI 生成 | 类型化配置模式、原生偏好设置资源和独立的旧版导入器 |
| [TestCase](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Widgets/TestCase.hpp#L38) / [TestCases](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Widgets/TestCases.cpp#L547) | 测试数据字节、选中状态、checker 设置和 AC/WA/TLE/RE 枚举位于 QWidget 类中；测试用例会重新编号 | 使用稳定 ID 的测试记录，以及独立的原生展示模型 |
| [CompanionServer](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Extensions/CompanionServer.cpp#L56) | Qt/qhttp 监听与 JSON 解析；在解析前就接受 HTTP 请求；携带原始 JSON 及 URL／时限／样例值 | 有容量限制的回环协议服务、带版本的字段映射和事务化导入 |
| [LanguageServer](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Extensions/LanguageServer.hpp#L21) | 直接依赖 `CodeEditor` 和 Qt；保存单个文档引用、同步全文进行检查，并按语言显示名称分支 | 基于原生进程传输的独立 JSON-RPC/LSP 服务；带版本的文档镜像与编辑器适配器 |
| [CodeFormatter](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Extensions/CodeFormatter.cpp#L31) | 直接处理编辑器／光标，并通过 QProcess 格式化 | 基于快照的格式化服务，以及校验版本的原生撤销事务 |
| [CFTool](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Extensions/CFTool.cpp#L42) | 外部进程、Codeforces URL 解析，以及翻译后的日志／通知行为 | 可选的评测平台／CLI 适配器；共享进程规则，由原生端展示通知 |
| [StressTesting](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Widgets/StressTesting.cpp#L315) | 在 QWidget 内协调生成器／参考程序／待测程序，依赖编辑器／设置并包含 C++/Java/Python 判断 | 使用快照／配置／随机种子数据的专用、可取消压力测试服务 |
| 编辑器／高亮器、样式、翻译器、主窗口 | Qt 文本／布局、KSyntaxHighlighting、信号、偏好设置和窗口协调 | 新的原生外壳／编辑器适配器，以及基于资源的本地化 |

将原项目作为行为对照基准和迁移来源。仅重命名依赖 Qt 的类，不能得到可移植 Core。目标契约见 [Core 契约](core-contracts.md)。

## 需要保留的兼容规则

- [Strict 与行尾空白检查实现](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Core/Checker.cpp#L233)：Strict 将 CRLF 和 CR 转为 LF。IgnoreTrailingSpaces 还会移除行尾 Unicode 空白和末尾仅含空白的行。通过测试夹具保留这些模式；逐字节精确比较和仅处理 ASCII 的比较属于独立策略。
- [Checker 模式清单](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Core/Checker.hpp#L47)：包括 `ncmp`、`rcmp4`、`rcmp6`、`rcmp9`、`wcmp`、`nyesno`，以及两个内置模式和自定义 checker。Testlib 是独立依赖，需要单独记录来源。
- [外部 checker 调用](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Core/Checker.cpp#L297)：参数依次为输入文件路径、实际输出路径、预期输出路径。
- [样例文件名规则](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Widgets/TestCases.cpp#L595)：模板包括 `${filename}`、`${basename}`、`${0-index}`、`${1-index}`。不要将所有样例文件名硬编码为 `.in/.out`。
- [源码快照／状态](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/mainwindow.cpp#L533)：已保存文本与编辑器文本是不同状态。保留未保存源码及其恢复优先级。
- [保留恢复会话](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Core/SessionManager.cpp#L113)：当前 master 可避免自动保存覆盖已保留的会话，这是 7.0.2 基线之后新增的行为。

## 明确提出的修正及兼容边界

| 已观察到的行为 | 拟议修正 |
| --- | --- |
| [Runner 定时器](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Core/Runner.cpp#L92) 在 `started` 信号之前启动，耗时统计却在之后开始 | 使用单调时间戳，分别定义进程创建超时、执行墙钟截止时间和清理／管道排空超时 |
| [输出读取](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Core/Runner.cpp#L180) 删除 NUL；保留的缓冲区按字节计数，UI 却称为字符 | 保留原始字节产物、明确上限和展示解码规则；不得在静默过滤／截断后继续正常比较输出 |
| [Checker 退出处理](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Core/Checker.cpp#L172) 将 `_fail` 和计分／部分得分结果映射为 WA；未知退出码只记录日志 | 区分 checker 故障与答案错误；仅通过明确的适配器支持计分 |
| [空答案检查条件](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/mainwindow.cpp#L1567) 使用选项决定是否检查空的预期字符串 | 通过可选的预期输出值区分“缺失”和“明确为空”；导入器记录采用的旧版解释方式 |
| 进程销毁时终止所持有的 QProcess；已审计路径未提供 Job Object／进程组／内存限额强制执行支持 | 原生进程后端实现并测试其声明的能力；不能继承或宣称已有沙箱／MLE 保证 |
| [LSP 诊断](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Extensions/LanguageServer.cpp#L232) 应用到已保存的编辑器引用 | 使用稳定文档 ID／版本、类型化位置转换并拒绝过期响应 |

普通 Runner 使用临时文件提供 stdin，属于批量执行。交互判题需要待测程序与 interactor 之间的双向通道、独立限额、EOF 处理、空闲／整体截止时间及取消机制。压力测试需要明确的生成器／参考程序／待测程序快照、可重现的随机种子／参数、有界迭代和持久化反例。这两类功能都不应仅作为未经检查的标志添加到批量 Runner 中。

## 会话与设置导入

[会话 JSON](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Core/SessionManager.cpp#L126) 包含 `currentIndex` 和 `tabs`，没有顶层模式版本。[标签页序列化](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/mainwindow.cpp#L477) 包括源码／已保存文本、路径／语言／编译命令、题目 URL、光标／选区锚点、滚动位置、输入／预期输出数组、checker 选择、测试选中标志和分隔条数值。

`editorCursor`／`editorAnchor` 来自 `QTextCursor`：应将它们视为 UTF-16 位置，并依据导入文本转换为类型化源码锚点。Qt 滚动条／分隔条的像素值是旧版展示元数据，不具备可移植含义。容易误解的 `testcasesIsShow` 是测试用例选中状态的旧字段名，需要记录其映射。校验数组对应关系，恢复可用样例并报告无效／缺失值，不能截断未保存内容。

[设置键生成规则](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Settings/SettingsInfo.hpp#L53) 将名称转为小写、将 `c++` 映射为 `cpp`，并将空格替换为下划线。[SettingsUpdater](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Settings/SettingsUpdater.cpp#L47) 还识别旧版别名。生成器的 `settings.json` 不是持久化的 INI 文件。仅使用通用 INI 解析，无法保证正确转换 Qt QVariant／字体／快捷键。

实现解码器前，应从真实的 7.0.2／当前版本导出文件建立测试夹具。对于尚不支持的旧版值，可采用可选、独立且链接 Qt 的导出工具，同时保持原生运行时不依赖 Qt。导入应保留原文件，生成带版本的原生会话／配置及报告。MVP 的兼容范围仅包括导入，不承诺导出回 Qt 格式。[FileUtil](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Util/FileUtil.cpp#L74) 已在安全模式使用 QSaveFile；新存储层必须通过原生实现保留原子保存语义。

## CPH-NG 与 Companion 参考

CPH-NG 基线：`15bc6465383be7b2133895894ebf785ed3840ad1`。其 [README](https://github.com/langningchen/cph-ng/blob/15bc6465383be7b2133895894ebf785ed3840ad1/README.md) 说明 `main` 包含 TypeScript 实现，而 Rust 内核工作位于 `refactor/rust-kernel`。它们属于不同研究基线。

已观察到且有参考价值的边界包括：[测试用例／结果模型](https://github.com/langningchen/cph-ng/blob/15bc6465383be7b2133895894ebf785ed3840ad1/packages/vscode-ext/src/domain/entities/testcase.ts)、[执行策略取消契约](https://github.com/langningchen/cph-ng/blob/15bc6465383be7b2133895894ebf785ed3840ad1/packages/vscode-ext/src/application/ports/problems/judge/runner/execution/strategies/IExecutionStrategy.ts)、[交互判题服务](https://github.com/langningchen/cph-ng/blob/15bc6465383be7b2133895894ebf785ed3840ad1/packages/vscode-ext/src/application/useCases/problems/judge/interactiveJudgeService.ts) 和 [压力测试流程](https://github.com/langningchen/cph-ng/blob/15bc6465383be7b2133895894ebf785ed3840ad1/packages/vscode-ext/src/application/useCases/webview/problem/stressTest/StartStressTest.ts)。借鉴其职责划分和工作流思路；TypeScript／VS Code 服务依赖不属于拟议运行时。

项目的 [包清单](https://github.com/langningchen/cph-ng/blob/15bc6465383be7b2133895894ebf785ed3840ad1/package.json) 声明 `AGPL-3.0-only`，而已检查的源码头使用 GPL 第 3 版或更高版本的表述。不能仅凭一个标签推断复制权限；复用实现前需确认具体文件／包的来源和许可。本次变更仅将其作为设计参考。

Competitive Companion 协议基线：`df90fabb52e8f566ea3382405e22d246af5d6a69`。其 [自定义工具格式](https://github.com/jmerle/competitive-companion/blob/df90fabb52e8f566ea3382405e22d246af5d6a69/README.md#custom-tools) 规定 HTTP POST `/`、兼容端口、以 ms／MB 为单位的限额、测试用例，以及可选的交互／输入／输出／语言／批次字段。应以此作为外部契约，不能假定 CP Editor 使用的子集包含所有受支持字段。

## 许可与证据范围

CP Editor 的 [LICENSE](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/LICENSE) 是 GPL v3；已审计的 [源码头](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Core/Compiler.cpp#L1) 允许使用第 3 版或更高版本。移植代码时按 GPL 兼容的派生实现规划，保留许可声明并追踪具体复制文件。Qt、testlib、编辑器及其他传递依赖各有独立声明。不能仅因选用的编辑器采用宽松许可，就将派生 Core 标为 MIT。

本次审计阅读了源码与清单，未编译原版或原生应用，未生成旧版导出测试夹具，也未在运行时验证进程／输入性能结论。迁移映射是后续实现的起点，所需验证见 [实施计划](implementation-plan.md)。
