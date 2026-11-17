# 实施状态与证据

更新时间：2026-11-18T02:39:54+08:00。目标仍为完成设计与实施，以下仅是当前进度，不代表全部里程碑已经通过。

| 阶段 | 状态 | 当前证据与剩余工作 |
| --- | --- | --- |
| 设计与源码研究 | 已完成当前基线 | 中文设计文档、固定 CP Editor 子模块与源码引用；实现中如需调整会继续更新 |
| M0 原生编辑器 | 进行中 | 两端宿主已编写；macOS 统一 Xcode 27／macOS 26.0 构建目标与 Mach-O 产物核验、状态栏 `TextLineIndex` 稀疏位集行列索引、`STTextView` 浅色／深色外观同步与 SwiftUI 工作区 UI/UX 优化已集成，80 项测试、局部基准、界面快照核验、应用打包启动和严格签名通过；Windows 固定源码补丁、PowerShell 入口与报告已提供；5225776 与 6fc3606 的 Server 2022 x64 Debug／Release 已通过 Core、Runner／Session、WinUIEdit及完整宿主编译／产物断言；a4d08e3 新增Companion六组桥接已在两配置CI执行通过；57f4fe7 的 Server2022 x64 Debug／Release 已通过新增 Companion 与完整宿主构建。目标 Windows、最低 macOS 26 实机、系统 IME／远程及整应用性能仍待验证 |
| M1 可移植 Core | 进行中 | Qt-free C++20 领域、原生进程、文件事务、会话恢复、外部 checker、批量服务、JobRegistry、Companion 纯解码器与 ABI 1 已实现；macOS 普通和完整严格警告 ASan/UBSan 17/17 入口通过（含监听生命周期 ABI），CLI 3 AC。Windows Server 2022 x64 Debug／Release 在 dbecc57 均已通过 Core 14/14（含持久收件箱）及正式桥接测试，取消竞态／补丁换行修复已复验；501ad0d／4c18ee2 的 Windows Core 15/15 已通过；594a9cf 的 Windows Core 16/16、Runner／Session 桥接与 WinUIEdit 编译／产物断言均已通过，6fc3606 的 Windows Core 17/17、Runner／Session及完整宿主也已通过；目标机UI仍待验收 |
| M2–M4 原生功能链路/互操作 | 未完成 | macOS C ABI actor 与原生测试面板已实现；13 项 Swift／真实 Core 测试和 CUA 导入样例→3 AC 通过。macOS 会话恢复、协调保存与关闭已接入；41 项 Swift／真实 Core 测试和 CUA 重启未保存正文／样例、冲突保护／另存及恢复后 AC 通过；外部 checker 配置、独立诊断、原始输出 hex 和重启配置恢复已通过真实 UI 验证，首次配置窗口的数据流缺陷已修复。Windows 多标签、独立样例、committed 快照及 C ABI Run All／Cancel 已接入源码；在 macOS 的普通／ASan+UBSan／TSan 真实 Core 桥接测试通过。Windows 会话 worker／完整候选模型／源文件事务／自动恢复与关闭屏障已接入源码，含最终 MAX 事务及同步 TextChanging；独立普通／ASan+UBSan 13组与作者TSan13组通过，原生恢复链路仍待实际Windows宿主及目标机验收；Companion 有界持久收件箱／去重更新／原子会话导入回执已通过普通及 ASan/UBSan 全部 Core 验收；四个导入 C ABI 已集成且通过全部 Core 验收；有界回环 HTTP 服务已完成独立审查，根普通／完整严格警告 ASan+UBSan 16/16 与 TSan HTTP 1/1 通过；监听生命周期 ABI 已实现，普通／完整 ASan+UBSan 17/17 及独立44项故障检查通过；Windows Companion面板、显式监听与文件导入已接入源码，根普通／完整ASan+UBSan六组与作者TSan六组通过，两处恢复基线／首owner预算问题经独立真实Core探针复验；新增Companion六组已在Windows两配置CI通过；57f4fe7 两配置 CI 已通过完整宿主，目标机 UI 仍待验收。macOS Companion 文件收件箱、持久绑定、回执后确认及来源运行门禁已接入，70项 Swift／真实 Core 测试通过，前台 Pending→Applied→AC／不兼容禁用／重启防重放通过；MAX 精确恢复、最后提交／关闭回滚与源写入完成回执、跨标签结果归属均有自动及独立探针。macOS HTTP／真实扩展和高级功能继续验收 |
| M5 高级服务 | 未完成 | 外部 checker 已接入 Core／C ABI／CLI 和 macOS 原生配置／独立诊断面板；Windows checker 配置、LSP、格式化、终端、压力/交互判题和提交适配器尚未完成 |
| M6 发布 | 未完成 | 未完成目标系统性能预算、安装/更新/签名/公证与完整依赖审查 |

当前可运行的检查：在仓库根目录执行 `cmake --preset dev`、`cmake --build --preset dev` 和 `ctest --preset dev`。这些覆盖当前领域、真实进程与判题服务、并发生命周期和 C ABI，不证明完整产品／目标平台验收完成。准确请求格式见 [当前 Core API](core-api.md)。

实际执行主机：macOS 27.0.1 arm64，Xcode 27.2，Swift 6.4.2，Apple Clang 21，CMake 4.4.3。Windows 10 22H2/PowerShell 7 目标验证待执行。

## 本轮可复核证据

- `out/macos/build-verification.json` 与 `out/macos/cursor-position-perf/benchmark.json`：Xcode 27.2／SDK 27.2／macOS 26.0 下 15 个 Core 对象与最终可执行文件的 `LC_BUILD_VERSION` 核验、工具链环境 17/17 Core CTest、80 项 Swift 测试、状态栏稀疏位集索引局部基准，以及 `out/macos/layout-audit/` 浅色／深色真实界面渲染核验。
- `out/macos/session-resume-verification/verification.json`：前轮 70 项 Swift 测试、打包启动／严格签名、收件箱前台截图／AX与原字节／回执核对。当前 macOS 27.0.1 arm64，不替代最低系统／Intel／系统 IME 或整应用性能预算。
- `out/macos/session-resume-review/verification.json`：原生源写入 B 与文档最后编辑 C/MAX 的原失败及修复复验；同路径／另存实际保存点准确，失败无伪回执。
- `out/macos/run-results-review/verification.json`：跨标签、晚事件、同 ID 新会话、只读 MAX 运行和 shutdown 清理的独立真实 Core 探针。
- [Windows CI 44009461078](https://github.com/A-BenLi06/CPeditor-Native/actions/runs/44009461078)：57f4fe7，Server 2022 Datacenter 20348／PowerShell 7.6.10，x64 Debug／Release 各21阶段、Core17/17、Runner／Session／Companion及完整宿主通过。`out/windows/ci/44009461078/ci-readonly-audit/verification.json` 保留报告与 SHA；20个人工场景全 not_run／passed:null，无部署或启动。
