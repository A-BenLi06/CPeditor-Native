# 实施任务登记

登记时间：2026-11-17T01:05:02+08:00。

| 所有者 | 独占范围 | 工作 | 状态 |
| --- | --- | --- | --- |
| 根 agent | 共享 Core、CMake、tests/core、共享文档与提交；已交由其他所有者的文件除外 | Core 契约／服务集成与统一验收 | 会话／外部 checker／严格 ABI 已提交，原生持续集成验收 |
| process_backend | core/include/cpn/external_checker.hpp、core/src/services/external_checker.cpp、tests/core/external_checker_tests.cpp、tests/checker、tools/native/cpn_cli.cpp | 外部 checker 和 CLI 入口 | 服务和 CLI 已提交并通过验收 |
| windows_poc | apps/windows、tools/windows、patches/winuiedit | 固定源码编辑器、验证入口和 Core UI | Core UI 与可靠保存基线已完成源码接入及便携验收，Windows 原生验收仍待目标机 |
| macos_poc | apps/macos、script、.codex/environments | 会话恢复、协调保存和关闭流程 | 原生会话已提交并通过真实 UI 验证，checker 配置及诊断实施中 |
| companion_design | 新增 companion.hpp／companion.cpp／companion_tests.cpp、contracts/fixtures/companion、docs/companion.md | 共享纯 Companion 解码器 | 源码草稿及独立测试进行中，HTTP／收件箱未接入 |

根 agent 维护子模块、构建集成和 walkthrough，并在完整变更集通过检查后提交。审查期间暂停对应范围写入；不得将草稿或未执行的目标机验收纳入完成声明。
