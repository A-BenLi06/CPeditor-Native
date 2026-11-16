# 实施状态与证据

更新时间：2026-11-17T01:05:02+08:00。目标仍为完成设计与实施，以下仅是当前进度，不代表全部里程碑已经通过。

| 阶段 | 状态 | 当前证据与剩余工作 |
| --- | --- | --- |
| 设计与源码研究 | 已完成当前基线 | 中文设计文档、固定 CP Editor 子模块与源码引用；实现中如需调整会继续更新 |
| M0 原生编辑器 | 进行中 | 两端宿主已编写；macOS 双编辑器与桥接／会话 30 项测试、应用打包启动和真实 CUA 粘贴／菜单撤销通过；Windows 固定源码补丁、PowerShell 入口与报告已提供，便携状态机及源码准备／篡改拒绝检查通过；真实 Windows 构建待执行。目标 Windows、最低 macOS、系统 IME／远程及性能仍待验证 |
| M1 可移植 Core | 进行中 | Qt-free C++20 领域、原生进程、文件事务、会话恢复、外部 checker、批量服务、JobRegistry 与 ABI 1 已实现；macOS 普通和完整严格警告 ASan/UBSan 12/12 入口通过，CLI 3 AC。Windows 编译／运行、后续服务仍待验收 |
| M2–M4 原生功能链路/互操作 | 未完成 | macOS C ABI actor 与原生测试面板已实现；13 项 Swift／真实 Core 测试和 CUA 导入样例→3 AC 通过。macOS 会话恢复、协调保存与关闭已接入；30 项 Swift／真实 Core 测试和 CUA 重启未保存正文／样例、冲突保护／另存及恢复后 AC 通过。Windows 多标签、独立样例、committed 快照及 C ABI Run All／Cancel 已接入源码；在 macOS 的普通／ASan+UBSan／TSan 真实 Core 桥接测试通过。Windows 原生构建、会话恢复与全链路验收、Companion 和高级功能仍未完成 |
| M5 高级服务 | 未完成 | 外部 checker 已接入 Core／C ABI／CLI；原生 checker 配置、LSP、格式化、终端、压力/交互判题和提交适配器尚未完成 |
| M6 发布 | 未完成 | 未完成目标系统性能预算、安装/更新/签名/公证与完整依赖审查 |

当前可运行的检查：在仓库根目录执行 `cmake --preset dev`、`cmake --build --preset dev` 和 `ctest --preset dev`。这些覆盖当前领域、真实进程与判题服务、并发生命周期和 C ABI，不证明完整产品／目标平台验收完成。准确请求格式见 [当前 Core API](core-api.md)。

实际执行主机：macOS 27.0.1 arm64，Xcode 27.2，Swift 6.4.2，Apple Clang 21，CMake 4.4.3。Windows 10 22H2/PowerShell 7 目标验证待执行。
