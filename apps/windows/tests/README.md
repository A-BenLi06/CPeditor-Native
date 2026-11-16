# Windows 运行与会话桥接便携测试

`CoreRunnerTests.cpp` 编译应用实际使用的 `CoreWire.cpp`、`CoreRunner.cpp`、`RunnerConfiguration.cpp` 和 `SaveBaseline.h`，链接完整共享 Core 归档。测试 executable 也充当编译失败、执行失败、回显与可取消长任务的子进程 fixture，不依赖系统 C++ / Python 编译器。

Windows 入口使用 `wmain`，路径从 UTF-16 转为 UTF-8 契约；子进程 fixture 将标准输入输出设为二进制模式。当前测试实际运行于 macOS ARM64 的中文工作区路径，Windows Unicode 路径仍待真实目标机执行。

## 覆盖范围

- 保存正文基线：B 保存时当前 C、undo A 仍 dirty、undo B clean、revision 缓存、失败不替换基线和 NUL。这是实际 adapter 使用的纯字节模型测试，不证明 Windows 文件事务或 Scintilla 保存点行为。
- 实际 Core C ABI：文档 ID、`2^53 + 1` / `UINT64_MAX` revision、NUL 回显、空期望 AC / WA、Unchecked、禁用样例。
- 以 `max_jobs=2` 连续运行五次，验证 terminal drain 后释放 job，而不是依赖放大 job 限额。
- 无效源 admission failure、真实子进程编译失败与 RE。
- marker 确认真实子进程启动后取消，排队取消与重复取消。
- 两个外部线程并发 close、关闭期间取消并保留 terminal、关闭后拒绝新请求。
- 回调在协调线程重入 close 时明确拒绝，随后仍可接受并完成第二个任务；回调抛异常时 delivery failure 可观察。

## 当前执行证据

普通、ASan/UBSan 与 TSan 的严格编译和真实运行均通过。日志位于项目根目录下：

| 模式 | 日志 | 运行结果 |
| --- | --- | --- |
| 普通 | `out/windows/research/core-adapter-tests.log` | 退出码 0 |
| ASan / UBSan | `out/windows/research/core-adapter-tests-asan.log` | 退出码 0，无 sanitizer 诊断 |
| TSan | `out/windows/research/core-adapter-tests-tsan.log` | 退出码 0，无 sanitizer 诊断 |

机器生成的主机、时间戳、命令和归档身份记录为 `out/windows/research/core-adapter-validation.json`。`detect_leaks=1` 在当前 Apple ASan 运行时被拒绝，退出码 134；原始输出保留为 `core-adapter-tests-asan-leak-unsupported.log`。后续 ASan 使用 `detect_leaks=0`，因此没有泄漏检测结论。

## macOS 复现命令

在项目根目录执行普通版本：

```sh
cmake -S . -B out/windows/research/core-bridge -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build out/windows/research/core-bridge --parallel 4
clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror \
  -DCPN_PORTABLE_ADAPTER_TEST \
  -Icore/include -Ithird_party/json/include -Iapps/windows/CPEditorNativeWindows \
  apps/windows/CPEditorNativeWindows/CoreWire.cpp \
  apps/windows/CPEditorNativeWindows/CoreRunner.cpp \
  apps/windows/CPEditorNativeWindows/RunnerConfiguration.cpp \
  apps/windows/tests/CoreRunnerTests.cpp \
  out/windows/research/core-bridge/libcpn_core.a \
  -o out/windows/research/core-adapter-tests
out/windows/research/core-adapter-tests apps/windows/CPEditorNativeWindows/runner-settings.json
```

此处 `BUILD_TESTING=OFF` 仅避免重复构建 Core 独立 CTest 套件，当前 executable 自身执行正式桥接测试。完整 Core CTest 由 Windows `verify.ps1` 单独列出并执行。

ASan / UBSan 必须同时 instrument 完整 Core 归档和桥接 executable：

```sh
cmake -S . -B out/windows/research/core-bridge-asan -DBUILD_TESTING=OFF \
  -DCMAKE_BUILD_TYPE=Debug '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer'
cmake --build out/windows/research/core-bridge-asan --parallel 4
```

将普通 `clang++` 命令增加 `-fsanitize=address,undefined -fno-omit-frame-pointer`，链接 `out/windows/research/core-bridge-asan/libcpn_core.a`，输出 `core-adapter-tests-asan`，然后运行：

```sh
ASAN_OPTIONS=detect_leaks=0 out/windows/research/core-adapter-tests-asan \
  apps/windows/CPEditorNativeWindows/runner-settings.json
```

TSan 同样 instrument 两侧：

```sh
cmake -S . -B out/windows/research/core-bridge-tsan -DBUILD_TESTING=OFF \
  -DCMAKE_BUILD_TYPE=Debug '-DCMAKE_CXX_FLAGS=-fsanitize=thread -fno-omit-frame-pointer'
cmake --build out/windows/research/core-bridge-tsan --parallel 4
```

普通 `clang++` 命令增加 `-fsanitize=thread -fno-omit-frame-pointer`，链接 `out/windows/research/core-bridge-tsan/libcpn_core.a`，输出 `core-adapter-tests-tsan`，然后运行：

```sh
TSAN_OPTIONS=halt_on_error=1 out/windows/research/core-adapter-tests-tsan \
  apps/windows/CPEditorNativeWindows/runner-settings.json
```

## 会话与源文件测试

`SessionTests.cpp` 编译应用实际使用的 `SessionWorker.cpp`、`RecoveryModel.cpp`、`SourceStore.cpp`、`RunnerConfiguration.cpp` 和 `SaveBaseline.h`，链接相同的完整 Core 归档。它通过真实 C ABI 进行恢复文件 load/save，不用模型 mock 代替持久化。独立子进程 fixture 真实持有恢复锁，以 marker 确认持锁后才测试等待和取消。

覆盖稳定 ID、`2^53 + 1` revision、正文 CRLF/NUL、样例 NUL/0xFF、空/缺期望输出、opaque 原始偏好、完整本机候选及 UTF-8 字节选区。位置偏好拒绝重复/未知字段、未来版本、相对/NUL 路径和超限；本机工具链与语言引用须有效；最终 `UINT64_MAX` revision 允许恢复，真正需递增的已耗尽候选明确拒绝。当前版本与未来偏好版本共存时整体保留，偏好槽位耗尽不会删除外部条目。

真实持久化覆盖 Missing → Save → Load、预期摘要冲突、上一版备份、损坏原件保留、两个外部线程 close、命令队列 overflow、关闭结算 promise、正在等锁的 save 取消。源文件测试覆盖 BOM/CRLF/NUL 原始摘要、外部修改拒覆、Save As 已有目标拒覆、最终提交前新建目标竞态，以及私有准备文件失败与实际目标替换后持久化未知的区别。恢复保存点测试覆盖仅摘要基线、uint64 revision 缓存、无保存点的空文档仍 dirty 和晚保存切换基线。

便携源码测试不包含 WinUI 的 MainWindow、C++/WinRT、Scintilla 或真正 Windows IME；它不能证明原生自动保存、关闭屏障和晚完成 UI 操作正确。这些行为按 `verification-scenarios.json` 在目标主机验收。

普通、ASan/UBSan 与 TSan 会话测试已在 macOS ARM64 严格重编并链接真实 Core 运行通过。日志分别为 `out/windows/research/session-tests-normal.log`、`session-tests-asan.log` 和 `session-tests-tsan.log`；最终主机、命令与归档身份汇总见 `out/windows/research/session-adapter-validation.json`。Windows 大小写别名锁身份的回归分支需要在 Windows 执行；本机只覆盖 POSIX 大小写不同路径使用不同锁。

普通会话测试复现：

```sh
clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror \
  -DCPN_PORTABLE_ADAPTER_TEST \
  -Icore/include -Ithird_party/json/include -Iapps/windows/CPEditorNativeWindows \
  apps/windows/CPEditorNativeWindows/SessionWorker.cpp \
  apps/windows/CPEditorNativeWindows/RecoveryModel.cpp \
  apps/windows/CPEditorNativeWindows/SourceStore.cpp \
  apps/windows/CPEditorNativeWindows/RunnerConfiguration.cpp \
  apps/windows/tests/SessionTests.cpp \
  out/windows/research/core-bridge/libcpn_core.a \
  -o out/windows/research/session-tests
out/windows/research/session-tests apps/windows/CPEditorNativeWindows/recovery-settings.json
```

ASan/UBSan 与 TSan 使用前文已经 instrument 的对应 Core 归档，并在这个命令增加相同 sanitizer flags；输出分别为 `session-tests-asan` / `session-tests-tsan`。只有完整归档和 executable 同时 instrument 后的真实执行才能报告 sanitizer 验证通过。当前 Apple ASan 不支持 LeakSanitizer，不将 `detect_leaks=0` 的执行记为泄漏检测。

## Windows 入口

```powershell
pwsh -File ./tools/windows/verify.ps1 -Configuration Debug -Platform x64
```

脚本从已发现的 MSVC 环境分别构建正式运行与会话桥接 executable；Debug 使用 `/MDd`，Release 使用 `/MD`，与共享 Core CRT 匹配。在相同架构主机真实执行后才将 `bridgeTestsExecuted` / `bridgeTestsPassed` 和 `sessionTestsExecuted` / `sessionTestsPassed` 记为 true。`sessionBuildCompleted` 仅代表编译；三个会话结果进入 `build-result.json` / `verification-report.json`，并共同参与自动验证通过条件。WinUI 应用、IME、文件事务、DPI、UU 和性能测试属于独立人工验收，便携桥接通过不会将这些场景自动标为通过。

本次新增第 11 组版本边界回归：workspace / document / test / 三者同时从 MAX-1 最后一次合法变更到 MAX，真实 C ABI 保存、重新加载并完成 Native prepare；保存点 digest 保真；已耗尽的原文重试及另存副本保留身份和字节；未变化关闭仍可检查磁盘 token。`RevisionPolicy` 是 adapter 与 MainWindow 实际使用的同一预算策略，回归验证不会饱和计数，也不允许 native revision 倒退或越界。三套严格重编与真实执行日志为 `session-tests-max-normal.log`、`session-tests-max-asan.log`、`session-tests-max-tsan.log`。Windows 控件的真实 ReadOnly/选择/复制行为仍需目标机测试。

第 12 组确定性事务回归使用应用实际的 `PersistenceGate`，以 promise 控制真实 SourceStore 保存 B 的完成时刻；等待期间构造编辑器 C、样例 C，再确认最后事务无法先越过待完成写入。B 保存点完成并通过 generation 后才冻结完整 C，真实 Core 保存 MAX 并完整恢复源码、样例与 B 的磁盘保存点。回归还覆盖 pending Open、旧 generation 拒绝、损坏目标导致提交前失败解除能力，以及 MAX 副本已落盘但 locator 写入失败时保留两份原件和已消费预算。三套日志为 `session-tests-final-normal.log`、`session-tests-final-asan.log`、`session-tests-final-tsan.log`。这验证事务策略及真实 I/O，不等同于 WinUI 输入与控件事件的运行验证。

第 13 组使用真实窗口共用的 `TestTextChange` 和 `FinalPersistenceLease`，确定性控制“C 已同步记录、界面刷新仍待派发”后才冻结最终候选，并通过真实 C ABI 保存、加载及 Native prepare 校验输入、期望输出与样例 revision。另覆盖 MAX 最后合法样例变更、后续拒绝不溢出、二进制展示不能回写、重复通知不推进版本，以及 gate 取得后模拟界面异常的 RAII 解除、竞争 lease 与幂等释放。三套严格重编和真实运行日志为 `session-tests-sync-normal.log`、`session-tests-sync-asan.log`、`session-tests-sync-tsan.log`，机器证据为 `session-synchronous-validation.json`。这些回归验证实际共享模型和 I/O；WinUI `BeforeTextChanging` / `TextChanging` 的派发时序须由原生输入验收补充。
