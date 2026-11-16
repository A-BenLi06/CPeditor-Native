# 实施任务登记

登记时间：2026-11-17T00:21:45+08:00。

| 所有者 | 独占范围 | 工作 | 状态 |
| --- | --- | --- | --- |
| 根 agent | 共享 Core、CMake、tests/core、共享文档与提交；已交由其他所有者的文件除外 | Core 契约／服务集成与统一验收 | 会话已提交，外部 checker 接入正在验收 |
| process_backend | core/include/cpn/external_checker.hpp、core/src/services/external_checker.cpp、tests/core/external_checker_tests.cpp、tests/checker、tools/native/cpn_cli.cpp | 外部 checker 和 CLI 入口 | 服务已通过审查，CLI 正在接入 |
| windows_poc | apps/windows、tools/windows、patches/winuiedit | 固定源码编辑器、验证入口和 Core UI | 脚本／补丁当前阶段已完成，Core UI 草稿尚未接入 |
| macos_poc | apps/macos、script、.codex/environments | 会话恢复、协调保存和关闭流程 | 原生会话接入正在实施和验证 |

根 agent 维护子模块、构建集成和 walkthrough，并在完整变更集通过检查后提交。审查期间暂停对应范围写入；不得将草稿或未执行的目标机验收纳入完成声明。
