# 改动与验证记录

## 2026-11-16T20:11:27+08:00 — 首次源码获取与原生架构设计

### 改动内容

- 将空的 CPEditor-Native 目录初始化为 Git 仓库，分支为 `main`。
- 将 `https://github.com/cpeditor/cpeditor` 克隆为 `references/cpeditor`，固定在 `77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c`，并初始化其全部六个嵌套上游子模块。7.0.2 标签及历史可用于用户提供的评估基线。
- 添加根目录协作要求以及构建/IDE 输出排除规则，没有修改原版 CP Editor 实现。
- 添加 README 和五份设计文档，当时以英文编写，涵盖架构、Core 契约、源码迁移、编辑器评估和实施验收条件。

### 设计思路

当前上游快照声明 7.3.0/C++14/Qt 6，而用户提供的评估基于 7.0.2/Qt 5。上游 Core、测试模型、设置、会话和扩展仍包含 Qt/UI 依赖。因此设计保留有价值的竞赛语义与明确的导入兼容性，同时规划不依赖 Qt 的 C++20 服务层及原生进程、文件和协议适配器。WinUI 3/C++/WinRT 与 SwiftUI/AppKit 分别管理原生文档、编辑器和窗口行为。Windows 调用引擎的 C++ 接口；Swift 使用稳定 C ABI，明确生命周期与事件所有权。

编辑器选择仍须通过实际原型验收。源码研究修正了 WinUIEdit 的 MIT 许可描述，确认了 STTextView 当前 GPL/商业许可及 CodeEditorView 缺少 C++ 词法配置的情况。CodeEdit/BriskEdit 作为架构参考，CPH-NG 作为工作流参考。依赖与来源说明基于不可变源码提交，而非浮动的 README 描述。

Core 规则区分空输入、缺少预期输出与预期输出为空、checker 错误和解答判题结果。保留 CP Editor 的换行符及 Unicode 空白比较语义，并引入明确的源码版本、稳定测试 ID、原始输出字节限制、进程取消和安全恢复。只读审查修正了状态图的 checker 前提、后台标签页撤销历史保留以及已完成事件的背压。

### 验证与范围

- 确认已获取的 CP Editor 版本、7.0.2 基线和全部嵌套子模块检出状态；上游工作区干净。
- 通过本地 Git 历史及固定版本的 GitHub 源码 URL 检查实现、依赖清单和许可证；独立只读审查覆盖 Core 迁移与两端编辑器方案。
- 设计文档区分已观察事实、拟议契约和未执行验收条件。
- 提交前的最终仓库与文档完整性检查记录如下。
- 执行主机为 macOS，shell 为 zsh，未安装 PowerShell 7。主要目标环境仍为 Windows 10 22H2 专业工作站版/PowerShell 7。没有执行原生/编辑器构建、原版应用构建、运行时性能测试、旧版导出样本生成或部署；这些属于文档中后续实施阶段的工作。

### 2026-11-16T20:11:53+08:00 — 最终完整性检查结果

- 八份 UTF-8 Markdown 文档、十五个本地链接及三十二条固定版本 CP Editor 源文件路径/行号引用通过完整性检查。
- 递归子模块状态确认参考版本准确固定，六个嵌套模块均已初始化；源码差异为空。
- Core 一致性审查发现的问题已修正；最终 macOS/编辑器/桥接审查未发现实质问题。
- 空白字符检查通过，完整的架构与源码获取变更可进行首次 Git 提交。

## 2026-11-16T21:17:43+08:00 — 项目文档中文化

### 改动内容

- 将 README、五份设计文档、AGENTS.md 和 walkthrough 历史记录全部翻译为简体中文，同时翻译 Mermaid 可见说明、目录布局注释和 .gitignore 注释。
- 保留原有文件路径、命令、API/类型/字段标识符、版本号、提交哈希与引用 URL；历史记录保留原始时间戳与当时的事实。
- 更新仓库语言要求：项目文档使用中文，应用默认语言为英文，用户可见文本通过原生本地化资源管理。
- 澄清判题结果段落的中文表述：仅 MemoryLimitExceeded 依赖内存限制后端，其他基础结果不能误读为可选。

### 改动思路

按用户最新要求，将文档语言统一为中文，方便直接阅读架构、迁移与验收说明。保留技术标识符和固定版本引用，使中文说明仍能准确对应源码与接口；区分文档语言要求和产品语言设置。此次没有改变架构方案或上游参考版本。

### 验证结果

- 八份 Markdown 文档均可按 UTF-8 读取，标题及说明已中文化。
- 十五个本地链接与三十二条固定版本 CP Editor 源码路径/行号引用通过检查，全部链接目标与翻译前一致。
- 技术标识符集合、原有标题/表格结构、命令代码块、Mermaid 状态转换/组件关系和拟议目录布局检查通过。
- 翻译自查及最终核对保留源码观察、拟议设计和未验证行为的区别；空白字符检查通过。
- CP Editor 及全部嵌套子模块的工作区保持干净；本次仅修改项目文档与注释，无需执行应用构建或运行时测试。

## 2026-11-16T22:35:47+08:00 — 实施启动与可移植领域基础

### 改动与思路

- 创建 CMake/C++20 构建入口、UTF-8 解码与 UTF-8/UTF-16/UTF-32 位置索引、CP Editor 兼容比较规则及独立 SHA-256 摘要实现。
- 位置转换拒绝编码字符/代理对/CRLF 内部位置；比较保留原始 NUL，并区分文本编码错误与字节精确比较。
- 定义无 UI 类型的原生进程接口，明确结构化 argv、并发管道、取消/限额/最终排空及能力报告；后端正在独立实施。
- 固定 WinUIEdit 研究/原型源码和 MIT 的 nlohmann/json v3.12.0；记录自有源码 GPL-3.0-or-later 与第三方来源。
- 添加任务所有权、问题/经验文档与实施状态，更新 README，保留完整里程碑及未完成验收项。Windows/macOS 原型及进程后端并行开发，未纳入本次已完成声明。

### 验证

- 在实际 macOS 27.0.1 arm64/Apple Clang 21 上执行 cmake --preset dev、cmake --build --preset dev；领域构建成功。
- ctest --preset dev -R '^domain$' 通过 1/1 测试入口，覆盖比较模式、混合换行、Unicode 空白、NUL、无效 UTF-8、代理对/组合字符位置往返和 SHA-256 标准向量（含百万字节多块输入）。
- Qt 6.10 的 QChar 公开实现确认空白定义包含分隔类别及 U+0085 等控制例外；仍需后续实际 Qt 导出样本验证完整迁移。
- 尚未通过 Windows 目标构建/运行、macOS 双编辑器 UI、完整进程/C ABI、性能或发布验证；这些保持进行中。

## 2026-11-16T23:13:50+08:00 — 两端原生编辑器原型与运行入口

### 改动与思路

- 添加真实 WinUI 3/C++/WinRT/WinUIEdit Windows 宿主、固定 NuGet 依赖、UTF-8 文件事务／快照诊断、英文 resw 及 PowerShell 7 工具发现和构建启动脚本。
- 添加 macOS SwiftUI/AppKit 双编辑器对照应用，固定 CodeEditorView/STTextView 及传递包锁，保留独立缓冲区与 UndoManager；添加英文原生资源、中文说明和第三方许可。
- CodeEditorView 的组合输入通知早于 marked 状态更新，通过原生主线程事务结束后的 capture 修复，未插入计时延迟或强制结束输入。真实 UI 又发现撤销菜单接收者缺失，修复标准 AppKit undo:/redo: 响应链，并持续挂载文本组件以保留切换文档后的原生历史。
- 添加可复现的 app 打包／启动／验证脚本及 Codex Run 入口。脚本保护未保存文档；根 agent 通过原生菜单处理本次创建的临时测试文本，未覆盖源码样本。

### 验证与限制

- macOS 27.0.1 arm64：Swift build、9 项模型／真实原生编辑器测试、脚本 --verify、ad-hoc codesign 与实际 app 启动通过；根 CUA 验证中文／emoji 粘贴、ST 菜单撤销及最新 CEV 菜单撤销，恢复源码与保存状态。
- Windows：9 个 XML、34 条工程引用、7 个 XAML handler、18 个资源键、2 profiles、4 包版本、6 PNG 静态检查通过。没有 Windows SDK／PowerShell 主机，未执行 Windows 编译、部署或 UI。
- M0 目标平台验收尚未通过：macOS 14／Intel、系统中文候选窗口、辅助功能、DPI／Spaces、UU／Windows 虚拟桌面及性能数据仍待采集。WinUIEdit 未公开 composition 事务，native-buffer 诊断不会被描述为已提交 Core 快照。

## 2026-11-16T23:18:49+08:00 — 原生进程、批量判题与有界 C ABI

### 改动与思路

- 实现 POSIX 进程组／Windows Job Object、结构化 argv、并发 stdin/stdout/stderr、超时／取消、有限输出保留和最终排空；进程创建失败与启动后 I/O 错误分开分类。
- 实现严格工具链 profile、独立临时 workspace、原子 FileStore、批量单次编译／多样例比较；保留稳定源码／测试 ID、revision、SHA-256、空答案与无答案以及原始 NUL 字节。保存失败前后的提交状态通过 AtomicWriteError 明确区分，路径拒绝 NUL／无效 UTF-8，POSIX 覆盖文件保留权限。
- 修复数字截断／版本溢出、重复字段、文件名别名、非法占位符／变量、检查取消和异常覆盖成功编译状态等实际审查问题；诊断内容有界，比较／解码按批次响应取消。
- 实现固定 worker JobRegistry，提交前预留容量，进度合并／轮转排空、可靠终态、精确 ack、显式 release 与并发关闭；每个接受任务预留异常终态。
- 实现 ABI 1：C 头、整数句柄、异常边界、请求复制、有限 JSON 事件／输出缓冲区和匹配释放；无效／过期句柄及容量不足均返回结构化错误，成功分配后才消费事件。添加完整静态聚合库 libcpn_core.a 供 Swift 等原生客户端。
- CLI 使用共享结果序列化，修复非法缩进字节与 stdout 写失败检查；添加数据驱动 Clang／GCC Windows／Python profiles 和中文当前 API 文档。

### 验证与限制

- macOS 27.0.1 arm64／Apple Clang 21，以 macOS 14 deployment target 构建；普通 CMake 与完整 -Werror／ASan+UBSan 构建均成功，8/8 CTest 入口通过（C header、API、jobs、files、JSON、domain、process、batch）。
- 真实进程测试 20 项通过；并发 gate 测试覆盖容量、排队取消、旧 ack、终态、异常、固定 worker／并发关闭及清理。子 agent TSan 检查覆盖 JobRegistry 与其测试，未把它报告为全 Core TSan。
- CLI 真实 C++ 编译／3 样例均 AC，输出按 UTF-8 解析成功，revision、固定摘要及原始 stdout（含空样例）一致。ABI 验证包含调用方修改请求、容量拒绝不消费终态、buffer 在 engine destroy 后继续读取／释放及双重释放错误。
- Windows 后端源码已完成，但没有 Windows 编译／运行证据。内存硬限制与硬启动期限明确不支持；POSIX 进程组不能保证清理主动脱离进程组的进程。完整原生面板、会话／Companion／高级服务和分发仍在实施。
