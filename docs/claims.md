# 实施任务登记

登记时间：2026-11-17T12:37:34+08:00。

| 所有者 | 独占范围 | 工作 | 状态 |
| --- | --- | --- | --- |
| 根 agent | 共享 Core、CMake、tests/core、共享文档与提交；已交由其他所有者的文件除外 | Core 契约／服务集成与统一验收 | 会话／外部 checker／严格 ABI 已提交；macOS checker 原生验收与共享收件箱集成已通过；Windows CI 回归持续验收 |
| process_backend | core/include/cpn/external_checker.hpp、core/src/services/external_checker.cpp、tests/core/external_checker_tests.cpp、tests/checker、tools/native/cpn_cli.cpp | 外部 checker 和 CLI 入口 | 服务和 CLI 已提交并通过验收 |
| windows_poc | apps/windows、tools/windows | 固定源码编辑器、验证入口和 Core UI | Core UI 与可靠保存基线已提交；Windows 会话恢复／源文件事务／最终版本与同步样例事务已通过作者13组普通／ASan／TSan及独立复审，文件已释放；脚本属性／产物断言已修复，原生验收待CI及目标机 |
| macos_poc | apps/macos、script、.codex/environments | 会话恢复、协调保存和关闭流程 | 70 项测试／Companion 文件收件箱前台导入、回执、来源门禁与重启通过；MAX恢复／关闭回滚、源写入完成凭证及跨标签结果独立复核通过，文件已释放 |
| companion_design | 新增 companion.hpp／companion.cpp／companion_tests.cpp、contracts/fixtures/companion、docs/companion.md | 共享纯 Companion 解码器 | 纯解码器、收件箱、HTTP及生命周期 ABI 已集成并通过完整 Core 验收；原生接入继续独立验收 |
| companion_inbox | 新增 companion_inbox.hpp／companion_inbox.cpp／companion_inbox_tests.cpp 及独立夹具 | 有界持久收件箱、去重／更新与原子会话导入回执 | 实现／历史回归审查及完整 Core 验收通过，服务文件已释放；四个 C ABI 已实现并普通／ASan+UBSan 集成通过；HTTP与生命周期 ABI已实现，原生导入继续验收 |
| companion_abi | api.h、api.cpp、companion_api_tests.cpp | 四个共享收件箱 C ABI 与预留写响应 | 实现／作者独立验收和根 15/15 集成通过；独立 910 个分页边界普通／ASan+UBSan 审查通过，文件已释放 |
| windows_companion | apps/windows；root维护tools/windows | 原生Inbox、显式监听及回执确认 | 作者停止写入；根普通／完整ASan+UBSan六组、作者TSan六组通过；独立原两P2探针修复复验通过；PowerShell报告与20场景11项检查通过。57f4fe7 新WinUI面板完整宿主两配置CI通过，目标机UI仍待执行，文件已释放 |
| listener_abi | api.h、api.cpp、companion_listener_api_tests.cpp 与副作用边界修复 | 每引擎有界监听生命周期与有限写回应 | 根普通／完整严格警告 ASan+UBSan 17/17通过；独立44项按真实绑定／发布／inode替换的分配故障检查通过，SHA一致；原失败证据保留，文件已释放 |
| companion_http | 新 companion_listener.hpp／.cpp／companion_listener_tests.cpp | 有界可取消回环 HTTP 与持久接纳 | 服务已停止写入并集成；root／独立审查修复自身 worker 重入 start 死锁；完整普通／ASan+UBSan 16/16 及 TSan HTTP 1/1 通过，文件已释放 |

本轮 session-resume-review 独占 SourceDocument／saveSource／revision 测试的源写入凭证修复已完成并释放；根 agent 统一完成文档、提交和推送。

根 agent 维护子模块、构建集成和 walkthrough，并在完整变更集通过检查后提交。审查期间暂停对应范围写入；不得将草稿或未执行的目标机验收纳入完成声明。
