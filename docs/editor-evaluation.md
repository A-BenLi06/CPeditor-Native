# 原生编辑器评估

研究日期：2026-11-16（UTC+08:00）。以下要求和许可已对照不可变源码快照检查，但未构建这些组件。清单声明、README 描述与实际运行验证分别记录。

## 建议

Windows 首先使用 WinUIEdit，并通过可替换适配器接入。macOS 使用相同的 C++ 测试夹具比较 CodeEditorView 与 STTextView，保留 CodeEditSourceEditor 作为第三候选。参考 CodeEdit 和 BriskEdit 的应用边界及生命周期模式。通过下列验收条件后再选择生产组件，不直接 Fork 完整 IDE。

根据已检查的提交，用户提供的部分项目描述需要修正：WinUIEdit 根许可声明使用自定义、宽松的 Scintilla 风格条款，并非 MIT；STTextView 提供 GPLv3／商业许可；CodeEditorView 使用 Apache-2.0、词法分析器／正则语言配置，且没有附带 C/C++ 语言配置。

## Windows 源码基线

WinUIEdit `main`：`d9f60dbca65c2af54ced590d873d9438de32ce77`。

| 观察结果 | 证据与影响 |
| --- | --- |
| 基于 C++ 的原生 Scintilla WinUI 3/UWP 移植，提供 C++/WinRT 和 C# 示例 | [README](https://github.com/BreeceW/WinUIEdit/blob/d9f60dbca65c2af54ced590d873d9438de32ce77/README.md)、[C++ WinUI 3 示例](https://github.com/BreeceW/WinUIEdit/tree/d9f60dbca65c2af54ced590d873d9438de32ce77/CppDemoWinUI3)；适合作为首个 Windows 候选 |
| README 明确表示尚未达到生产要求，并预期 API 会出现破坏性变更 | 固定版本，将 API 限制在适配器内部；不承诺组件稳定性 |
| WinUI 3 示例的最低 Windows 版本为 `10.0.17763.0`，目标 SDK 为 `10.0.22621.0` | [项目配置](https://github.com/BreeceW/WinUIEdit/blob/d9f60dbca65c2af54ced590d873d9438de32ce77/CppDemoWinUI3/CppDemoWinUI3.vcxproj)、[应用清单](https://github.com/BreeceW/WinUIEdit/blob/d9f60dbca65c2af54ced590d873d9438de32ce77/CppDemoWinUI3/Package.appxmanifest)；Windows 10 22H2 在声明范围内；目标 SDK 版本不是最低运行系统版本 |
| 已检查源码包中的 Windows App SDK 为 `1.7.260224002` | [packages.config](https://github.com/BreeceW/WinUIEdit/blob/d9f60dbca65c2af54ced590d873d9438de32ce77/WinUIEditor/packages.config)；这是观察到的参考版本，尚未选定为产品依赖锁定版本 |
| 根目录自定义宽松许可声明，以及 Scintilla／Lexilla 声明 | [根 LICENSE](https://github.com/BreeceW/WinUIEdit/blob/d9f60dbca65c2af54ced590d873d9438de32ce77/LICENSE)、[Scintilla](https://github.com/BreeceW/WinUIEdit/blob/d9f60dbca65c2af54ced590d873d9438de32ce77/scintilla/License.txt)、[Lexilla](https://github.com/BreeceW/WinUIEdit/blob/d9f60dbca65c2af54ced590d873d9438de32ce77/lexilla/License.txt)；保留准确声明，不分配未经核实的 SPDX ID |
| XAML 包装层通过 WinRT 暴露编辑器操作 | [控件 IDL](https://github.com/BreeceW/WinUIEdit/blob/d9f60dbca65c2af54ced590d873d9438de32ce77/WinUIEditor/CodeEditorControl.idl)、[包装层 IDL](https://github.com/BreeceW/WinUIEdit/blob/d9f60dbca65c2af54ced590d873d9438de32ce77/WinUIEditor/EditorWrapper.idl)；这些类型不得进入共享 Core |

[原生平台层](https://github.com/BreeceW/WinUIEdit/tree/d9f60dbca65c2af54ced590d873d9438de32ce77/scintilla/winui) 实现指针转换、DirectWrite 绘制、文本存储输入和 DPI 转换。它是 Windows 移植，没有 macOS 后端。包清单包含 WebView2 SDK，但已观察到的编辑器宿主路径采用原生 Scintilla；仅存在该包，不能证明编辑器在 Chromium 运行时中执行。

源码检查中发现的具体风险：

- [EditorBaseControl 生命周期／重新激活](https://github.com/BreeceW/WinUIEdit/blob/d9f60dbca65c2af54ced590d873d9438de32ce77/WinUIEditor/EditorBaseControl.cpp#L30)：关闭／生命周期，以及快速重新激活后按 Ctrl+A 的注释，需要针对性测试。
- [ScintillaWin 输入实现](https://github.com/BreeceW/WinUIEdit/blob/d9f60dbca65c2af54ced590d873d9438de32ce77/scintilla/winui/ScintillaWin.cxx#L821)：存在 IME 撤销 TODO、屏幕坐标／DPI 不确定性，以及文本存储锁定期间的指针事件排队。这些是源码检查结果，尚未复现卡顿。
- [自动化对等体](https://github.com/BreeceW/WinUIEdit/blob/d9f60dbca65c2af54ced590d873d9438de32ce77/WinUIEditor/EditorBaseControlAutomationPeer.cpp)：不完整的辅助功能／输入组合行为需要验证。
- [示例窗口](https://github.com/BreeceW/WinUIEdit/blob/d9f60dbca65c2af54ced590d873d9438de32ce77/CppDemoWinUI3/MainWindow.xaml.cpp)：Mica 和焦点规避处理属于示例行为。产品代码在 Windows 10 上使用受支持的原生背景／焦点能力。

## macOS 源码基线

| 组件 | 提交版本 | 已观察到的许可／要求 | 选型价值 |
| --- | --- | --- | --- |
| CodeEditorView | `2394a48f919d6570cfbfbd53d047a3eaeb95b59e` | Apache-2.0；清单声明 Swift tools 5.9、macOS 14+；README 标为预发布；实际最低工具链尚未验证 | 集成 SwiftUI/AppKit 编辑器、消息／Minimap／补全接口；需接入 C++ 词法配置和 clangd |
| STTextView | `c7d519318b06ebb7a99a641e4aa8831105d3b7d4` | GPLv3／商业许可；macOS 14+；清单声明 tools 5.9；README 要求 Xcode 26+ | 基于 TextKit 2 的编辑基础，提供插件和 SwiftUI 包装；高亮／语言服务层仍需集成 |
| CodeEditSourceEditor | `1fa4d3c3ffba007482111466cb9721416f97ae00` | MIT；清单声明 tools 5.9、macOS 13+；README 表示尚未达到生产要求 | 功能更广的 Tree-sitter 编辑器，提供 AppKit／SwiftUI 接口；第三个对照候选 |
| CodeEdit | `fa2aebd86373211c78626074b53ab75010767575` | MIT；README 标为开发中／预发布 | 原生工作区／命令／窗口组织参考；不 Fork 完整 IDE |
| BriskEdit | `47ea9076469b67436c2d322a90c0f5cadbbfde05` | MIT；项目配置采用 macOS 15+、Swift 6、Xcode 26 | 轻量应用边界、延迟服务初始化、文档版本与大文件策略 |

CodeEditorView 的 [清单](https://github.com/mchakravarty/CodeEditorView/blob/2394a48f919d6570cfbfbd53d047a3eaeb95b59e/Package.swift)、[README](https://github.com/mchakravarty/CodeEditorView/blob/2394a48f919d6570cfbfbd53d047a3eaeb95b59e/README.md) 和 [LICENSE](https://github.com/mchakravarty/CodeEditorView/blob/2394a48f919d6570cfbfbd53d047a3eaeb95b59e/LICENSE) 支持上述声明。当前源码使用 SwiftUI `@Entry`，因此不能将 tools-version 5.9 宣称为已验证的最低构建工具链。其 [LanguageConfiguration](https://github.com/mchakravarty/CodeEditorView/blob/2394a48f919d6570cfbfbd53d047a3eaeb95b59e/Sources/LanguageSupport/LanguageConfiguration.swift) 提供词法分析器配置；此快照附带的语言配置缺少 C/C++。其 [LanguageService](https://github.com/mchakravarty/CodeEditorView/blob/2394a48f919d6570cfbfbd53d047a3eaeb95b59e/Sources/LanguageSupport/LanguageService.swift#L194) 提供同步／补全／信息接口，没有附带 clangd 服务端或完整的 LSP 传输实现。需要评估自定义 C++ 配置，以及到共享语言服务的适配器。

STTextView 的 [清单](https://github.com/krzyzanowskim/STTextView/blob/c7d519318b06ebb7a99a641e4aa8831105d3b7d4/Package.swift)、[README](https://github.com/krzyzanowskim/STTextView/blob/c7d519318b06ebb7a99a641e4aa8831105d3b7d4/README.md)、[许可条款](https://github.com/krzyzanowskim/STTextView/blob/c7d519318b06ebb7a99a641e4aa8831105d3b7d4/LICENSE.md#L1) 和 [SwiftUI AppKit 包装](https://github.com/krzyzanowskim/STTextView/blob/c7d519318b06ebb7a99a641e4aa8831105d3b7d4/Sources/STTextViewSwiftUIAppKit/TextView.swift) 支持其作为较底层的备选方案。集成前记录采用的具体许可选项。原生编辑基础的质量同样要经过编辑／IME 测试。

CodeEditSourceEditor 的 [清单](https://github.com/CodeEditApp/CodeEditSourceEditor/blob/1fa4d3c3ffba007482111466cb9721416f97ae00/Package.swift)、[README](https://github.com/CodeEditApp/CodeEditSourceEditor/blob/1fa4d3c3ffba007482111466cb9721416f97ae00/README.md)、[许可](https://github.com/CodeEditApp/CodeEditSourceEditor/blob/1fa4d3c3ffba007482111466cb9721416f97ae00/LICENSE.md) 和 [SourceEditor 包装](https://github.com/CodeEditApp/CodeEditSourceEditor/blob/1fa4d3c3ffba007482111466cb9721416f97ae00/Sources/CodeEditSourceEditor/SourceEditor/SourceEditor.swift#L14) 支持其作为第三候选。协调器回调应保留在适配器内部，避免同时引入第二套语言服务所有权。

[CodeEdit README](https://github.com/CodeEditApp/CodeEdit/blob/fa2aebd86373211c78626074b53ab75010767575/README.md) 提供原生应用组织方式的参考背景。BriskEdit 的 [架构文档](https://github.com/jx-grxf/BriskEdit/blob/47ea9076469b67436c2d322a90c0f5cadbbfde05/ARCHITECTURE.md) 仍将运行流程描述为计划中，而 [RunService 源码](https://github.com/jx-grxf/BriskEdit/blob/47ea9076469b67436c2d322a90c0f5cadbbfde05/Sources/BriskEdit/Services/RunService.swift) 已实现执行。应以代码判断当前行为。其 [项目配置](https://github.com/jx-grxf/BriskEdit/blob/47ea9076469b67436c2d322a90c0f5cadbbfde05/project.yml) 及 README 中的阈值属于 BriskEdit；未测量本产品前，不应复制其最低系统版本或大文件字节策略。

## 两端编辑器适配器的共同要求

原生编辑器接口共享语义测试夹具，类型则保留在各自平台。必需操作包括打开／捕获文档、校验版本的编辑、保存点／未保存状态、类型化范围定位、诊断、编辑器命令、焦点、外观、能力查询和释放。事件包括已提交文本变化，以及选区／保存点状态。

引擎接收不可变 UTF-8 文本和类型化版本。Scintilla 字节偏移、AppKit UTF-16 偏移及 LSP 协商的位置单位，需要按文档版本转换。缓冲区所有权、IME、选区、撤销、高亮、布局、辅助功能和指针处理保留在原生端。编辑器专用语言／语法 ID 通过适配器内部的配置映射解决。大文件能力调整不得破坏文本，也不得使源码／检查结果失效。

## 原型验收条件

以下条件均为拟议要求，尚未执行。

| 验收项 | 所需证据 |
| --- | --- |
| 构建／依赖 | 在拟议最低系统和选定编译器上还原依赖、构建并启动每个固定版本候选；记录解析后的传递依赖与许可声明 |
| C++ 工作负载 | 使用真实语法配置／语法规则、代表性片段、注释／字符串／预处理器、多行／长行测试夹具；不能以空白编辑器代替 |
| 中文 IME | 输入组合、转换、取消、提交、替换选区、多行输入、撤销／重做；滚动及显示器变化后候选窗口位置正确；不丢字、不重复字符 |
| Unicode／范围 | CJK、代理对、组合字符、制表符、CRLF/LF；保存／快照往返一致；诊断／编辑命中正确范围；拒绝过期范围及拆分字符的操作 |
| 焦点／生命周期 | 快速重新激活后 Ctrl+A、标签页打开／关闭、菜单、Alt+Tab 或 Spaces／全屏、最小化／恢复、退出；不残留指针捕获，也无释放后的回调 |
| DPI／辅助功能 | 原生缩放变化、适用情况下的混合 DPI 窗口、选区／补全／上下文菜单／IME 几何定位；键盘与辅助功能工具 |
| 性能 | 普通／大文件／长行 C++ 测试夹具；冷／热启动、首次编辑、滚动／输入延迟分布、内存／CPU；分别启用／禁用高亮／Minimap／LSP；依据基线确定预算 |
| 快照／异步服务 | 输入期间编译、组合文本期间诊断、格式化冲突、撤销保留、存在待返回服务响应时关闭文档 |
| Windows 原问题回归 | 相同主机／文件／硬件，出现问题的 Qt 5 构建、空 WinUI 宿主、普通原生控件、WinUIEdit，以及可运行时的当前 Qt 6；重复 UU 断开及桌面切换 |
| Windows 高频输入 | 设备可提供的 125–8000 Hz 设置；测量事件／分发延迟、锁定／积压情况；队列有界且选区／滚动正确 |

macOS 的两个初始候选使用相同文件、clangd 结果和源码偏移测试夹具。只有具体的能力缺失／构建／许可／延迟发现，才构成增加第三候选的依据。编辑器选择需记录集成成本和实测结果，包括平台／工具链限制。Windows 系统鼠标指针症状应与编辑器绘制分别追踪；源码检查不能证明问题已解决。

## 补充参考

[官方 WinUI Gallery](https://github.com/microsoft/WinUI-Gallery) 用于参考原生控件与交互。[MonacoEditor.WinUI3](https://github.com/luttje/MonacoEditor.WinUI3) 是基于 WebView2 的备选，位于初始路线之外。[SwiftEd](https://github.com/glaforge/swifted) 可为后续文本缓冲区实验提供参考；它尚未被选为依赖，本次也未对其进行源码审计。[CP-IDE](https://github.com/j-o-s-e-p-h-h/CP-IDE/tree/fe014bfd040c237f6c3fec8e4a0fed35231d43af) 是补充竞赛工作流参考，尚未批准将其用于编辑器集成。CPH-NG 和 Competitive Companion 的源码支持工作流观察见 [上游源码审计](upstream-audit.md)。
