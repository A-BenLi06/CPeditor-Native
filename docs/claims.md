# 实施任务登记

登记时间：2026-11-17T02:27:16+08:00。

| 所有者 | 独占范围 | 工作 | 状态 |
| --- | --- | --- | --- |
| 根 agent | 共享 Core、CMake、tests/core、共享文档与提交；已交由其他所有者的文件除外 | Core 契约／服务集成与统一验收 | 会话／外部 checker／严格 ABI 已提交；macOS checker 原生验收与共享收件箱集成已通过；Windows CI 回归持续验收 |
| process_backend | core/include/cpn/external_checker.hpp、core/src/services/external_checker.cpp、tests/core/external_checker_tests.cpp、tests/checker、tools/native/cpn_cli.cpp | 外部 checker 和 CLI 入口 | 服务和 CLI 已提交并通过验收 |
| windows_poc | apps/windows、tools/windows、patches/winuiedit | 固定源码编辑器、验证入口和 Core UI | Core UI 与可靠保存基线已提交；Windows 会话恢复与源文件冲突保护实施中，原生验收待目标机 |
| macos_poc | apps/macos、script、.codex/environments | 会话恢复、协调保存和关闭流程 | checker 41 项测试／真实 CUA／首次配置窗口修复均通过；共享 Inbox 只读审查完成 |
| companion_design | 新增 companion.hpp／companion.cpp／companion_tests.cpp、contracts/fixtures/companion、docs/companion.md | 共享纯 Companion 解码器 | 纯解码器和收件箱已集成并通过完整 Core 验收，HTTP／原生接入未完成 |
| companion_inbox | 新增 companion_inbox.hpp／companion_inbox.cpp／companion_inbox_tests.cpp 及独立夹具 | 有界持久收件箱、去重／更新与原子会话导入回执 | 实现／历史回归审查及完整 Core 验收通过，服务文件已释放；四个 C ABI 已实现并普通／ASan+UBSan 集成通过，HTTP／原生导入未接入 |
| companion_abi | api.h、api.cpp、companion_api_tests.cpp | 四个共享收件箱 C ABI 与预留写响应 | 实现／作者独立验收和根 15/15 集成通过；独立 910 个分页边界普通／ASan+UBSan 审查通过，文件已释放 |
| companion_http | 新 companion_listener.hpp／.cpp／companion_listener_tests.cpp | 有界可取消回环 HTTP 与持久接纳 | 已核实 Boost 1.92.0 stable 来源／SHA，服务与真实 socket 测试实施中 |

根 agent 维护子模块、构建集成和 walkthrough，并在完整变更集通过检查后提交。审查期间暂停对应范围写入；不得将草稿或未执行的目标机验收纳入完成声明。
