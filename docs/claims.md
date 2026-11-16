# 实施任务登记

登记时间：2026-11-16T22:27:25+08:00。

| 所有者 | 独占范围 | 工作 | 状态 |
| --- | --- | --- | --- |
| 根 agent | core/include、core/src/domain、core/src/services、tests/core、CMake、共享文档与提交 | 领域规则、头文件、引擎构建/集成和验收 | 进行中 |
| process_backend | core/src/platform、tests/process | 原生进程后端、真实辅助进程与集成测试 | 已分派 |
| windows_poc | apps/windows、tools/windows | WinUI 3/WinUIEdit 编辑器原型与 PowerShell 7 构建脚本 | 已分派 |
| macos_poc | apps/macos、script、.codex/environments | SwiftUI 双编辑器对照原型、构建运行入口与测试 | 已分派 |

子模块、根构建及 walkthrough 由根 agent 维护，其他所有者不得并行修改。
