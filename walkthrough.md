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

## 2026-11-16T23:40:39+08:00 — macOS 原生编译运行与测试面板

### 改动与思路

- 添加 Swift C 头桥接及单一串行 Core actor，直接链接 CMake 聚合归档，不通过 CLI 中转；后台提交／排空／取消，解码后释放 buffer 和终态 job，退出时异步等待引擎清理。
- 添加原生测试输入／预期输出编辑、启用／删除／添加、严格 JSON 导入、比较策略，以及工具链选择／加载／JSON 编辑。配置及默认 C++17 参数来自资源，用户文本仍为英文。
- 运行使用已提交源码快照，面板按文档 ID／revision 判断结果与当前源码关系，编译运行期间仍可编辑；原生编辑器及撤销历史持续挂载。关闭文档时取消相应工作。
- 构建脚本先构建 Core，可用 CPN_CORE_LIBRARY_DIR／CPN_CORE_PREBUILT 指定已有库；打包并保留实际启动与签名检查结果。

### 验证与限制

- 13 项 Swift／原生／真实 C ABI 测试通过，包含未保存 C++→AC、运行期间源码变化→旧结果、1 job／1 buffer 配额下取消后重跑和资源释放；既有 IME／撤销回归保持通过。
- 最新 script --verify、真实 app 启动和 codesign --verify --strict 通过。根 CUA 实际导入三个样例并点击 Run All，sample-1／sample-2／empty-stdin 均 AC，面板显示源码匹配。
- 当前测试用例和运行配置仍仅存于工作区内存；共享会话恢复接入在下一变更。没有把局部原生链路当作完整 M4／M6 验收。

## 2026-11-16T23:57:23+08:00 — 共享会话恢复、合作锁与 ABI

### 改动与思路

- 添加严格版本化 WorkspaceSession：稳定文档 ID／revision、未保存源码与摘要、保存点、原路径、测试和原样保留的有版本原生偏好。恢复文件与用户源文件／编译产物隔离。
- FileStore 新增可选读取及 POSIX flock／Windows LockFileEx 合作锁；持久锁文件不删除，超时／取消独立处理。保存采用预期摘要、备份、再次校验和原子替换，损坏／未来版本保持原件。
- 新增 session load／save C ABI；写入前预留返回 buffer，容量不足不产生隐藏的磁盘副作用。替换已发生但持久化未确认有独立状态。
- 修复 libc++ 对两个缺失路径的 equivalent 特殊错误，先检查存在再判断别名；不把普通 I/O 错误忽略为无别名。中文文档记录精确格式、限额与并发边界。

### 验证与范围

- macOS 普通及完整 -Werror／ASan+UBSan 构建成功，10/10 CTest 入口通过。会话覆盖原始字节、数字与schema校验、损坏保留、提交前后故障、两实例冲突及取消；真实 helper 验证跨进程锁。
- 合作锁不保证绕过锁的外部writer无法竞争最终replace；文档明确此限制。Windows编译／运行与两端原生会话接入保持待验收，未纳入本次完成声明。

## 2026-11-17T00:21:45+08:00 — Windows 验证入口与 WinUIEdit 快照事务补丁

### 改动与思路

- 按用户“先提供 Windows 验证脚本”的选择，新增 PowerShell 7 工具发现、固定源码准备和统一 verify 入口。脚本建立官方 MSVC 开发环境，构建 Core／执行 CTest，构建源码控件和宿主，并可显式部署／请求启动。
- 逐阶段保存退出码、日志、版本、源码／依赖／产物 SHA256、binlog 和报告；中文手工实测模板的 10 个场景默认未执行。启动请求与真实UI验收独立；空CTest计划被拒绝，交叉架构不伪称执行测试。
- 固定 WinUIEdit 源码以git archive导出独立副本，应用哈希锁定的patch／overlay；不修改研究子模块，身份或文件变化拒绝复用且保留改动。修正Windows receipt路径分隔符规范化。
- 补齐Scintilla committed事务的原生锁／队列barrier、净文本revision和原子字节／光标快照，覆盖先SetText再composition-start。新增快照跟踪可用性，永久fault与暂时pending分开；异常在原生边界处理，跟踪失效不阻断后续原生输入。
- 统一CppWinRT依赖、保留实际WIL、移除未使用WebView2构建残留；通过正常WinMD项目引用连接源码控件。当前宿主仍为M0，Core UI草稿没有纳入本阶段提交。

### 验证与限制

- macOS ARM64上的PowerShell7.7语法解析4文件通过；实际准备、重复校验、故意篡改拒绝且未覆盖、恢复后校验通过。最终patch SHA256为 1c5d596b71af578dd07708a6a87bf0fb0c600da5ccb3fbb61e3838aa5319d32e；9个after文件和overlay哈希独立核对一致。
- Clang C++20严格警告和ASan／UBSan便携状态测试通过，含组合输入、取消、嵌套、NUL及分配／消费者异常注入。根重新编译运行了状态测试。
- 非Windows执行verify实际退出1并生成失败报告，自动通过标志false，10项实测均not_run／null；子模块保持干净。Windows MSVC／MIDL／WinRT／UI／TSF／IME／DPI／UU和性能尚未执行，必须使用目标机验证入口采集。

## 2026-11-17T00:26:18+08:00 — 外部 checker、批量接入与严格 CLI

### 改动与思路

- 添加独立 ExternalCheckerService，raw input／actual／expected 三文件、结构化 argv、单次模板展开、独立工作区及清理。默认testlib0为AC、1／2为WA，其余包含7为CheckerError；显式自定义二元映射可覆盖，未实现评分解析。
- 将CommandTemplate／ProcessExecutor提取为公共command.hpp，消除Batch与checker头循环。批量请求按值拥有可选配置，解答正常且有答案才运行；结果与JSON／C ABI保留独立checker进程、原始输出、限额和取消。
- checker输出共享batch总保留预算。独立审查复现违约执行器100字节输出使3字节预算下溢并误判AC，修复build／solver回收时的防御截断、totals／truncated与OLE分类，补 regression并防御剩余空间减法。
- CLI增加--checker；原有默认TestDocument宏会忽略字段并将缺input置空，改为共享parse_test_cases。统一strict schema、base64、空／缺答案、4096测试限额和参数拒绝；中文文档记录实际能力与限制。

### 验证与范围

- macOS普通及完整-Werror／ASan+UBSan构建成功，12/12 CTest通过。真实helper、受控异常／取消、预算故障、Unicode／NUL／0xFF、C ABI终态与释放均覆盖。
- 子agent执行17次真实CLI验证；根另执行raw-byte AC／空答案／Unchecked以及7项严格schema／参数检查。checker崩溃／超时／超限保持独立错误，不伪装解答TLE／RE。
- Windows真实运行、原生checker设置／诊断面板、checker编译、完整评分、压力／交互和其余高级服务仍未完成，未将此局部功能当作完整产品验收。

## 2026-11-17T00:35:08+08:00 — 关联 GitHub 远端仓库

### 改动与思路

- 将 origin 关联至 https://github.com/A-BenLi06/CPeditor-Native.git，获取远端 main，并设置本地 main 的 upstream 和 origin/HEAD。
- 更新 README 的仓库地址与同步状态；仅提交本次文档记录，保留主线程正在进行的原生应用改动及其暂存状态。

### 验证与范围

- git ls-remote 和 fetch 成功；远端 main 为 76b7d5b8710c71b5668bf77c539ed657a48e288f（Initial commit）。
- git merge-base 确认当前本地与远端没有共同祖先。此次完成远端关联，不合并、不覆盖远端，也不推送尚未整合的代码；后续同步需要保留并整合双方历史。

## 2026-11-17T00:36:24+08:00 — 会话请求信封严格校验

- 独立检查发现会话本体已严格校验，但C ABI保存信封和paths对象仍忽略未知字段。新增允许字段校验，拼错expected token或文件路径键在任何磁盘操作和buffer配额分配之前被拒绝。
- 新回归在已占用全部buffer配额的情况下提交未知外层／paths字段，确认返回InvalidArgument、无buffer且未写恢复文件，不能被Capacity错误掩盖。
- 普通及完整严格警告ASan／UBSan构建的session_api、C header检查均2/2通过；其他未改逻辑沿用上一变更12/12证据。同步更新中文会话文档。

## 2026-11-17T00:44:44+08:00 — macOS 原生会话、源文件保存与关闭流程

### 改动与思路

- 通过独立后台Core actor接入会话load／save，稳定工作区／文档／测试身份、保存点、未保存正文、工具链／样例／选区和原生偏好完整恢复。候选全部验证及真实native buffer匹配后一次应用；撤销历史和preedit不序列化。
- SourceDocument与ID／快照按UTF-8字节比较，修复Swift规范等价导致版本或身份合并；revision耗尽明确拒绝／报告并可复制实际live buffer为新文档，不能回绕。
- 未知原生偏好原样保留。审查发现自身未来namespace追加v1导致重复，修复为保留未来版本而不追加旧版，显示设置兼容性提示；源码／样例仍可恢复保存。
- 源文件后台NSFileCoordinator、有界循环读取、原磁盘摘要校验和原子写入；BOM／CRLF／NUL不转换。保存旧快照时的新编辑仍dirty。关闭单文档冻结native输入／Undo，重新检查保存期间新编辑，恢复移除成功才释放容器。
- 窗口／退出先捕获committed buffer、flush恢复，再关闭两Core引擎。I/O失败保留原件并提供独立恢复导出／取消关闭；sticky取消跨引擎创建时点保留。打开／导入／导出／备份加载的异步任务协调关闭，避免丢失晚完成的操作。
- 单Window暂时控制默认恢复文件归属，新增--recovery-file用于独立工作区；产品仍英文资源，项目文档中文。

### 验证与范围

- 30项Swift测试通过（7模型／15真实原生集成／8 Core ABI，双编辑器参数化执行两组件）；包含BOM／CRLF／NUL、未知与自身未来偏好、max版本、源文件冲突／Save As、受控writer期间新编辑、持锁flush／shutdown、取消先于引擎创建等回归。
- root实际重新打包并--verify启动、严格codesign验证通过。CUA在独立out恢复文件／源码上验证未保存中文／emoji／组合字符、样例7+8→15 AC、退出重启恢复；注入外部修改后Save拒绝覆盖，另存新文件并保持原始文件，恢复后Run All再次AC。
- 磁盘独立核对UTF-8正文、SHA256、revision、稳定document ID、保存点、样例原始字节和另存路径一致。参考fixture和用户默认恢复文件未修改。
- macOS14／Intel、系统IME候选、多显示器／Spaces／性能和发布仍未验收；Windows原生全链路和其余竞赛／高级功能继续实施，目标未标完成。

## 2026-11-17T01:01:12+08:00 — 提交后自动推送要求

### 改动与思路

- 在 AGENTS.md 中新增每次 Git 提交后自动 push 的要求，目标沿用当前分支配置的上游远端，避免固定仓库地址或分支名。
- 仅在 push 成功后报告已同步；失败时保留本地提交并记录、报告实际原因。

### 验证与范围

- 检查目标文档不存在其他待提交改动，并执行 Markdown 空白检查；本次提交只包含 AGENTS.md 和改动记录。
- git push --dry-run origin main 与首次提交后的 git push origin main 均实际被 non-fast-forward 拒绝：本地与远端历史尚未整合。本地提交已保留；补充失败记录后再次执行普通 push，最终同步状态以推送结果为准。

## 2026-11-17T01:05:02+08:00 — Windows 多标签、Core 运行与可靠保存基线

### 改动与思路

- Windows 宿主接入持续挂载的每文档原生编辑器、独立撤销／光标／样例／运行身份及英文资源；配置定义工具链、比较策略、限额与状态映射。Python 首先使用纯文本 lexer，不声明语法高亮已完成。
- WinUIEditAdapter 使用哈希锁定补丁的 guarded committed API，原子读取正文／revision／选区。Run 先拥有样例和配置，在 composition／原生队列释放后捕获正文；待提交意图可取消，tracking 永久失效明确失败，不回退读取 preedit。
- CoreRunner 在后台协调线程拥有 C ABI 引擎；提交前预留追踪节点，取消按 token 去重，终态校验后 release，buffer RAII。关闭 join 在后台，外部并发 close 串行化；同worker回调重入 close 在改变状态前拒绝。交付异常可观察，generation过滤旧UI事件。
- 独立审查修复保存B期间编辑C、随后undo回原保存点A却错误clean的问题：以实际落盘正文为权威基线；正常使用native savepoint，失效期间每committed revision缓存比较，回到B后恢复native保存点。失败不更新基线，保存期间可继续输入。
- Windows verify增加正式Core桥接构建／测试及报告标志；Debug／Release CRT与Core一致。中文Windows说明、测试复现文档及总架构更新为两端共用C ABI；当前Windows会话与源文件外部冲突仍未接入。

### 验证与范围

- macOS ARM64中文路径上的正式桥接普通、完整Core两侧ASan／UBSan、TSan严格编译与实际子进程测试通过，命令／主机／+08:00时间戳／归档SHA记录于out/windows/research/core-adapter-validation.json。当前Apple运行时不支持LeakSanitizer，拒绝日志保留，没有泄漏检测结论。
- 根另独立以-Werror编译正式桥接并链接out/core/dev/libcpn_core.a，全部回归通过；含UINT64_MAX／2^53+1、raw NUL、空／缺答案、CE／RE、取消、有限配额下释放重跑、回调拒绝及并发close。保存基线回归复验通过。
- 根实际PowerShell7.7解析4脚本通过，非Windowsverify退出1，所有自动／UI通过标志false，10项人工not_run／null；XML／JSON、XAML事件、英文资源和实际打包profile路径检查通过。
- Windows MSVC／MIDL／WinUI／TSF／IME／DPI／UU和性能尚未执行；请按目标机脚本采集真实证据，未将便携结果计作原生验收或完整产品完成。

## 2026-11-17T01:11:09+08:00 — 共享 Companion 解码器与导入架构边界

### 改动与思路

- 新增无 I/O 的 C++20 Companion 解码器，进入cpn_services和单一Core归档。已知字段严格类型，未知外部元数据完整保留；原始payload不经DOM重序列化，Unicode／NUL／未知大整数词法和空白原样保存。
- 递归拒绝所有对象重复键，约束正文／深度／节点／各字符串／合计字符串／样例／批次边界，累计比较剩余预算防溢出。空答案不转为无答案，来源ms／MB仅存数据，不擅自改成执行限制。
- 交互／文件／regex／未知testType给出结构化普通stdio不兼容理由。源码路径、Java类名、远端filename与pattern均只保存，不打开、解释或执行。固定TaskBuilder的空taskClass合法，独立核实后补夹具；fixed TestType只有single/multiNumber，未知multiEOF明确不兼容。
- 中文Companion文档记录上游Any监听、解析前202、URL覆盖样例及扩展不检查response.ok的源码证据；设计回环HTTP、持久有界收件箱、稳定身份、去重／更新、会话先保存再Applied确认，但这些接入尚未实现。
- CTest使用相对fixture参数和明确工作目录，支持Windows中文工作区而不依赖窄argv的ACP。更新README／Core API／实施状态入口，不新增空网络构建目标。

### 验证与范围

- 子agent普通及-Werror ASan／UBSan独立测试通过；4个fixture包含官方builder默认值回归与现代／旧载荷／不支持模式。未知字段、递归与escaped重复键、raw UTF8／surrogate／NFD／NUL、UINT64来源数据及每项预算均覆盖。
- 根另在out/core/companion-review及companion-review-sanitize进行干净完整CMake构建，两套均13/13 CTest通过；sanitizer两侧完整instrument，显式detect_leaks=0，当前平台不支持LSan。
- 尚未实现HTTP监听、导入收件箱／ABI、稳定ID分配、原生导入UI或实际扩展联调；解码器通过不计为完整Companion功能／目标Windows／产品完成。
