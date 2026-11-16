# Windows Core 桥接便携测试

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

## Windows 入口

```powershell
pwsh -File ./tools/windows/verify.ps1 -Configuration Debug -Platform x64
```

脚本从已发现的 MSVC 环境构建正式桥接 executable；Debug 使用 `/MDd`，Release 使用 `/MD`，与共享 Core CRT 匹配。在相同架构主机执行后才将 `bridgeTestsExecuted` / `bridgeTestsPassed` 记为 true。WinUI 应用、IME、文件事务、DPI、UU 和性能测试属于独立人工验收，便携桥接通过不会将这些场景自动标为通过。
