# Windows M0 原型

这是 WinUI 3 + C++/WinRT + WinUIEdit 的编辑器验证工程。当前已编写原生编辑器适配、文件事务、英文资源和构建脚本；尚未在 Windows 主机编译、部署或运行。以下行为是实现意图及待验证项，不能据此认定 M0 验收通过。

主要目标为 Windows 10 22H2 专业工作站版。当前开发主机为 macOS，Windows 构建和运行应在目标 Windows 环境完成。

## 组成与边界

```mermaid
flowchart LR
    Window[MainWindow / WinUI 3] --> Adapter[WinUIEditAdapter]
    Adapter --> Control[WinUIEdit / Scintilla]
    Window --> Profiles[editor-profiles.json]
    Window --> Files[FileStore / Win32 文件事务]
    Window --> Snapshot[SnapshotJson / 原生缓冲区诊断导出]
```

`MainWindow` 负责单文档窗口、文件选择、保存提示和命令状态。`WinUIEditAdapter` 封装原生对象及事件生命周期，原生编辑控件直接处理编辑、滚动、选择、撤销和输入法。`FileStore` 在后台线程执行文件 I/O，更新 UI 前返回窗口的 apartment。

适配器不在每次按键时复制全文，只查询字节数、位置和修改状态。打开文档生成新的文档 ID，并从 revision 1 开始；实际插入或删除文本才推进 revision。语法样式、光标和滚动事件只更新状态显示。保存或导出时才读取原始 UTF-8 字节。

这里没有编译运行和判题按钮，也没有将 Scintilla 对象传给共享竞赛 Core。本原型用于先验证编辑器接入及文件事务，后续再接入 Core 的文档快照契约。

## 数据和文件策略

应用显示文本来自 `Strings/en-US/Resources.resw`；默认语言为英文。语言 profile、扩展名映射、默认和未知扩展名 profile、tab width 和打开文件大小上限由 `editor-profiles.json` 提供。当前包含 C++ 和纯文本，可修改配置而不改窗口代码。配置字段集中于解析边界，带 `schemaVersion: 1`。

打开文件仅接受严格 UTF-8，保留 BOM 信息和原有换行字节。默认打开上限为 32 MiB；此数值由配置决定，主要限制载入文件，并非已验证的编辑器容量或性能指标。原生字节 API 可表达包含 NUL 的文档；这点仍需目标机文件往返测试。

保存以同目录 UUID 临时文件、完整写入、flush、关闭和 `ReplaceFileW` 完成；新路径使用不覆盖竞态中新建文件的 `MoveFileExW`。编辑器在整个文件事务期间暂时不可编辑，使写入字节与 savepoint 对应。失败后不会设置保存点。尚未实现外部文件修改冲突检测，也尚未验证不同文件系统上的故障行为。

导出格式为 `cpn.editor-snapshot` / `schemaVersion: 1` / `snapshotKind: native-buffer`。正文为 JSON 的 UTF-8 文本，revision、byteLength、caretByte、anchorByte 使用十进制字符串，避免 JSON number 精度丢失。坐标编码明确为 UTF-8 字节。该诊断文件不等同于共享 Core 的 committed snapshot。

WinUIEdit 当前 API 没有公开输入法 composition 事务状态。原生缓冲区变更事件可能包含组合输入中的更新；本原型不通过延时、强制失焦或屏蔽输入来猜测事务是否完成。在确认中文候选选择、撤销、取消组合和焦点变化的实际行为，或补足可靠的 composition 接口前，不能声称编译使用的快照只包含已提交文本。

## 固定源码构建与目标机验证入口

WinUIEdit 改为固定源码构建，提交为 `d9f60dbca65c2af54ced590d873d9438de32ce77`。构建副本应用 [committed snapshot 补丁](../../patches/winuiedit/README.md)，补丁和 overlay 身份由 `patches/winuiedit/source-lock.json` 校验，不修改研究子模块。当前应用适配器仍使用 M0 的诊断导出，下一轮 UI 接入才会使用新增的 committed 接口；不能因为接口存在而宣称编译快照已经接入。

| 依赖 | 固定版本或身份 | 用途 |
| --- | --- | --- |
| WinUIEdit | 固定源码提交与 source-lock | 原生编辑控件、Scintilla / Lexilla |
| Microsoft.WindowsAppSDK | 1.7.260224002 | WinUI 3 与桌面窗口 |
| Microsoft.Windows.CppWinRT | 2.0.240405.15 | WinRT 投影和代码生成 |
| Microsoft.Windows.SDK.BuildTools | 10.0.28000.1839 | Windows 打包构建工具 |
| Microsoft.Windows.ImplementationLibrary | 1.0.260126.7 | 上游控件实际使用的 WIL |

目标主机需要 PowerShell 7、Visual Studio 2022 v143 C++ 工具、完整 Windows SDK、C++/WinRT 与单项目 MSIX / WinUI 构建工具。ARM64 还需安装对应的 C++ ARM64 工具。框架依赖部署需要匹配架构的 Windows App SDK 1.7 运行时，以及开发人员模式所需的本地注册权限。

在项目根目录执行：

```powershell
pwsh -File ./tools/windows/verify.ps1 -Configuration Debug -Platform x64 -Deploy -Launch
```

脚本先确认真实主机，再通过 vswhere 发现 Visual Studio 2022、通过 Windows Kits 注册项发现 SDK，导入官方 Developer PowerShell 模块建立 MSVC 环境，然后构建共享 Core、列出并执行 CTest、构建并执行便携 committed 状态测试、准备并构建源码控件、构建应用。编译器路径及所需环境以数据配置生成 MSVC 工具链 profile，供 Explorer 启动的应用使用。

可使用 `-MSBuildPath`、`-CMakePath`、`-NinjaPath` 和 `-WindowsSdkVersion` 显式指定工具，`-Parallelism` 控制构建并行度。发现 Ninja 时使用单配置生成器，否则使用 Visual Studio 2022；交叉架构只报告构建，不伪称已执行测试。工程默认 SDK 为 10.0.22621.0，脚本覆盖为实际安装版本；最低 OS 版本为 10.0.17763.0，目标验收 OS 为 10.0.19045 / SKU 161。

输出位于 `out/windows/<Platform>/<Configuration>/`，包括逐阶段日志、真实退出码、Core 测试名称、控件与包 SHA256、MSBuild binlog、MSIX layout、`build-result.json`、`verification-report.json` 和中文 `measurement-template.md`。源副本的目录名包含源码提交和完整 source-lock 身份；已有副本的原文件被修改时拒绝复用并保留改动。

`-Deploy` 注册生成的 AppxManifest；`-Launch` 同时请求部署与启动。Windows 10 Appx 模块通过 PowerShell 7 的 Windows PowerShell 兼容宿主调用。项目使用正常 WinMD 项目引用生成组件注册；没有临时 ActivatableClass 绕过。`activationRequested` 只表示启动请求，`uiValidationCompleted` 始终保持 false。10 个 IME、撤销、多标签、Core 取消、UTF-8、DPI、虚拟桌面、UU 和大文件场景默认 `not_run` / `passed: null`，只有完成目标机操作并附证据后才能记录通过。

也可仅执行 `build.ps1` 构建。`verify.ps1` 不跳过 Core 测试。任何原生命令失败都保留错误与报告。在非 Windows 主机执行时，脚本写入失败报告及未执行模板，并以失败退出，不进行 Windows 构建或注册。

## 目标机验证

| 场景 | 应记录的证据 |
| --- | --- |
| Debug / Release 构建和部署 | OS 版本、架构、SDK、MSBuild、NuGet 版本、binlog、依赖注册结果 |
| C++ 编辑和语法高亮 | 中文注释、模板、括号、缩进、复制粘贴、撤销 / 重做、语言切换 |
| UTF-8 字节往返 | 无 BOM / BOM、LF / CRLF、emoji、组合 Unicode、NUL 字节；保存前后字节比较 |
| 快照版本 | 编辑增加 revision；光标移动、滚动、语法切换不增加；caret / anchor 指向 UTF-8 边界 |
| 中文输入法 | 拼音组合、候选窗口、候选选择、Esc 取消、组合中切换焦点、撤销与快照内容 |
| 文件事务 | Save As 取消、保存失败、未保存关闭、打开失败、磁盘无空间、已有文件被占用 |
| 滚动和大文件 | 1 MiB / 10 MiB / 20 MiB 文件，持续输入、快速滚动、选择和窗口缩放 |
| 多显示器与桌面 | 100% / 150% / 200% DPI、跨屏、虚拟桌面切换、睡眠唤醒 |
| UU 远程 | 与现有 CP Editor 在同机、同网络、同文件下比较输入延迟、滚动和恢复后的状态 |

性能结果需报告测量方法、文件内容、首次 / 后续启动、工作集和采样时间。这里尚未设定或验证“稳定 60 FPS”“内存小于某值”等产品承诺。

## 当前验证范围

macOS 上执行了 XML / JSON 解析、项目静态引用检查、XAML 事件和资源键检查、源码 API 交叉核对、PowerShell 语法解析、实际源码准备与篡改拒绝、非 Windows guard、便携状态模型 ASan/UBSan 以及 `git diff --check`。这些检查不能发现全部 MIDL、XAML、C++/WinRT、NuGet 或 MSIX 运行问题。Windows 编译、UI、IME、DPI、UU 远程和性能验证尚未执行。
