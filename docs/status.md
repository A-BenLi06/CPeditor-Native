# 实施状态与证据

更新时间：2026-11-26T22:34:21+08:00。目标仍为完成设计与实施，以下仅是当前进度，不代表全部里程碑已经通过。

| 阶段 | 状态 | 当前证据与剩余工作 |
| --- | --- | --- |
| 设计与源码研究 | 已完成当前基线 | 中文设计文档、固定 CP Editor 子模块与源码引用；实现中如需调整会继续更新 |
| M0 原生编辑器 | 进行中 | macOS 端（27.0.1 arm64 / Xcode 27.2）已完整接入并核验 Release Swift 111 项、Core 17/17 与 6 场景真机截图（`docs/screenshots/`）。Windows 10 22H2 专业工作站版（19045 / SKU 161 x64，VS 2026 Build Tools v145）已完成 WinUI 3 主界面对齐：`MenuBar`、Fluent 紧凑工具栏、左编右测可拖拽分栏与三视图模式、`Compile`/`Run Without Compiling`/`Compile & Run` 分离、Output 先行横向三栏测试区、运行时编译器自动发现与 Visual/JSON 双模配置、原生设置面板（Light/Dark/System、换行/缩进/配对开关）及 `EditorAssist`；Debug x64 全流程构建、MSIX 部署激活（`deploymentCompleted: true`、`activationRequested: true`、实测 `WinUIEditor.dll`/`WinUIEdit.dll` 加载与 `workspace.json` 持久化写入）及 Release x64 全量构建（0 警告 0 错误）均已通过。交互式桌面人工视觉／中文 IME 候选／DPI／UU 远程与整应用性能仍待验收 |
| M1 可移植 Core | 进行中 | Qt-free C++20 领域、原生进程、文件事务、会话恢复、外部 checker、批量服务（含 `compile_only` / `run_only` 模式）、JobRegistry、Companion 纯解码器与 ABI 1 已实现；macOS（普通、Release、ASan/UBSan）与 Windows 10 22H2（Debug/Release x64）均通过 Core CTest **17/17**（将 MSVC Debug 检查迭代器下 `companion_api` 超时上调至 90s）与 CLI/桥接验证 |
| M2–M4 原生功能链路/互操作 | 进行中 | macOS C ABI actor、原生测试面板、编译/运行分离、会话恢复、协调保存与关闭、外部 checker、Companion 收件箱、MAX 精确恢复与跨标签结果归属已接入并核验。Windows 端多标签、committed 快照、C ABI Compile/Run/Cancel、artifact 复用、Output 先行测试区、外部 checker、原生设置、会话恢复及 Companion 收件箱/回环监听已接入并通过 `core-adapter-tests`、`session-tests`、`companion-tests`、`committed-text-tests` 与真机 Debug 启动；交互式窗口功能实测与 IME 闭环待执行 |
| M5 高级服务 | 未完成 | 外部 checker 已接入 Core／C ABI／CLI、macOS 与 Windows 原生配置/诊断面板；LSP、格式化、终端、压力/交互判题和提交适配器尚未完成 |
| M6 发布 | 预览已更新（`v0.1.0`），正式验收未完成 | 已按 `0.1.x` 版本迭代规则基于 `main` 发布 macOS arm64 `v0.1.0` Pre-release（含 6 张实机截图与 SHA-256）；Windows 本轮完成了 Debug/Release 构建、MSIX loose layout 生成及本地部署启动，暂未生成 `v0.1.1` 分发包，交互式人工验收、签名分发与最低系统实机覆盖仍待完成 |

当前可运行的检查：在仓库根目录执行 `cmake --preset dev`、`cmake --build --preset dev` 和 `ctest --preset dev`；在 Windows PowerShell 7 执行 `pwsh -File ./tools/windows/verify.ps1 -Configuration Debug -Platform x64 -Deploy -Launch` 或 `-Configuration Release`。这些覆盖当前领域、真实进程与判题服务、并发生命周期、C ABI 及宿主构建与部署，不证明完整交互式 UI／IME／性能验收完成。准确请求格式见 [当前 Core API](core-api.md)。

macOS 执行主机：macOS 27.0.1 arm64，Xcode 27.2，Swift 6.4.2，Apple Clang 21，CMake 4.4.3，Homebrew GCC 15.2.0_1（`/opt/homebrew/bin/g++-15`）。Windows 执行主机：Windows 10 22H2 专业工作站版（19045 / SKU 161 x64），PowerShell 7.7.0，VS 2026 Build Tools（18.6.3 / v145 / MSVC `19.51.36246.0` / Windows SDK `10.0.26100.0`），已在补齐 WinUI C++ 工具组件后完整通过 Debug 部署启动与 Release 构建；因命令运行环境位于隔离桌面 `WinSta0\exebox-*`，无法跨桌面截取 `WinSta0\Default` 交互窗口截图，交互界面、中文 IME 与高 DPI 验收仍需在交互桌面手动完成。

## 本轮可复核证据

- **Windows 10 22H2 WinUI 3 真机 Debug 部署启动与 Release 全量验证**：在 `Windows 10.0.19045` SKU `161` x64 主机完成：
  - `pwsh -File ./tools/windows/verify.ps1 -Configuration Debug -Platform x64 -Deploy -Launch`：Core CTest **17/17**、四组便携/桥接测试（`committed-text-tests`、`core-adapter-tests`、`session-tests`、`companion-tests`）、`editor-build` 与 `app-build` 均 0 警告 0 错误通过；修复了 `.vcxproj` `<None DeploymentContent>` 打包、`build.ps1` 从 `.build.appxrecipe` 生成 MSIX `layout\` 并自动安装 `Microsoft.VCLibs.140.00.Debug` / `Debug.UWPDesktop`、以及 `AppWindow::Closing` 改用显式 `event_token` 解决 `weak_ref<IAppWindow>` 启动崩溃的问题；实测启动进程无崩溃（`Responding=True`，加载 `WinUIEditor.dll`/`WinUIEdit.dll`/`Microsoft.UI.Xaml.dll`），并在 `LocalState\Recovery\workspace.json` 验证发现本机 MSVC 与 `C:\Python314\python.exe` 工具链及默认工作区恢复落盘。
  - `pwsh -File ./tools/windows/verify.ps1 -Configuration Release -Platform x64`：`automatedPassed: true`，Core 17/17、便携测试、`editor-build` 与 `app-build` 全量 0 警告 0 错误通过，`.build.appxrecipe` 包含全部 10 份 JSON 配置与 `Toolchains\cpp-msvc.json`。

- **v0.1.0 重建与实机运行核验**：在隔离目录 `out/releases/v0.1.0/` 完成 Release 构建与验证：Core Release CTest 17/17（4.09s）、`CPN_REQUIRE_GNU_CPP=1` SwiftPM Release 18 套件 111/111（14.103s）、`verify_macos_build.py` 15 个 Core 对象与可执行文件（`arm64`、`minos 26.0`、`sdk 27.2`、仅系统动态库）、打包前后 `codesign --verify --strict` 通过；解压后以 `--recovery-file` 启动实机 `CPEditorNative.app`，完成 GNU `g++-15` `bits/stdc++.h` 运行与 CE 诊断等 6 张真机窗口截图（`docs/screenshots/`），详情见 [v0.1.0 发布说明](releases/v0.1.0.md)。
- [macOS CI 45548967251](https://github.com/A-BenLi06/CPeditor-Native/actions/runs/45548967251) 验证提交 `8ddde4f`，Swift、Core 与应用包构建及严格签名检查全部通过。
- [Windows CI 45437684539](https://github.com/A-BenLi06/CPeditor-Native/actions/runs/45437684539) 验证提交 `75e7b52`，Server 2022 x64 Debug／Release 完整 Core、桥接测试与宿主构建通过。
- [macOS CI 45160293245](https://github.com/A-BenLi06/CPeditor-Native/actions/runs/45160293245) 验证 `84bdd71`，Swift 108/108、Core 17/17、应用包构建和严格签名检查全部通过。

- `v0.1.0` 之后的回归新增 7 项测试，复现并修复选区缩进丢字、引号边界误删、闭括号无对应上下文时误跳过、十六进制分隔符／未闭合字符串影响后续输入、局部换行格式和 G++ 路径升级失效。详细场景见 [发布后回归](regressions/macos-post-v0.1.0.md)。
- 本机 Debug 完整 Swift 108/108、Core 17/17；应用构建、严格临时签名及真实窗口的 GNU G++ AC → 单行选区缩进 → 撤销 → 复跑 AC 通过。更早的阶段性结果如下，不能与本轮测试数相加。

- macOS 判题结果过期、运行取消归属、工具链验证与工作区恢复容量已修复。macOS 27.0.1 arm64、Xcode 27.2 本机 Swift 91/91、Core CTest 17/17，临时签名检查、隔离工作区保存和前台样例复跑通过。
- [macOS CI 44327079021](https://github.com/A-BenLi06/CPeditor-Native/actions/runs/44327079021) 对 `e199f0a` 用 `xcode-27` ARM64 runner、Xcode 27.1／SDK 27.0 完成 Swift 91/91、Core CTest 17/17、应用包构建及临时签名核验。最低系统 macOS 26 实机、整应用性能与发布验收仍待补齐。

- `out/macos/build-verification.json` 与 `out/macos/cursor-position-perf/benchmark.json`：Xcode 27.2／SDK 27.2／macOS 26.0 下 15 个 Core 对象与最终可执行文件的 `LC_BUILD_VERSION` 核验、工具链环境 17/17 Core CTest、81 项 Swift 测试（15 套件）、状态栏稀疏位集索引局部基准、`out/macos/layout-audit/` 9 组浅色／深色／紧凑窗口真实界面渲染核验，以及 `./script/build_and_run.sh --verify` + `codesign --verify --strict` 打包启动核验。
- `out/macos/session-resume-verification/verification.json`：前轮 70 项 Swift 测试、打包启动／严格签名、收件箱前台截图／AX与原字节／回执核对。当前 macOS 27.0.1 arm64，不替代最低系统／Intel／系统 IME 或整应用性能预算。
- `out/macos/session-resume-review/verification.json`：原生源写入 B 与文档最后编辑 C/MAX 的原失败及修复复验；同路径／另存实际保存点准确，失败无伪回执。
- `out/macos/run-results-review/verification.json`：跨标签、晚事件、同 ID 新会话、只读 MAX 运行和 shutdown 清理的独立真实 Core 探针。
- [Windows CI 44009461078](https://github.com/A-BenLi06/CPeditor-Native/actions/runs/44009461078)：57f4fe7，Server 2022 Datacenter 20348／PowerShell 7.6.10，x64 Debug／Release 各21阶段、Core17/17、Runner／Session／Companion及完整宿主通过。`out/windows/ci/44009461078/ci-readonly-audit/verification.json` 保留报告与 SHA；20个人工场景全 not_run／passed:null，无部署或启动。
