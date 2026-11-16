# 实施任务登记

登记时间：2026-11-16T23:18:49+08:00。

| 所有者 | 独占范围 | 工作 | 状态 |
| --- | --- | --- | --- |
| 根 agent | core/include、core/src/domain、core/src/services、tests/core、CMake、共享文档与提交 | 领域规则、头文件、引擎构建/集成和验收 | 进行中 |
| process_backend | core/include/cpn/session.hpp、core/src/services/session.cpp、tests/core/session_tests.cpp | 进程／批处理修复已完成，新增会话服务 | 会话进行中 |
| windows_poc | apps/windows、tools/windows | M0 原型／JobRegistry 已集成；source composition 契约与 Core 面板 | 下一变更进行中，Windows 验证待执行 |
| macos_poc | apps/macos、script、.codex/environments | M0 已提交；Swift C ABI actor 与测试面板 | Core 原生链路进行中，系统矩阵待验证 |

子模块、根构建及 walkthrough 由根 agent 维护，其他所有者不得并行修改。
