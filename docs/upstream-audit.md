# CP Editor source audit and migration map

Research date: 2026-11-16 (UTC+08:00). Findings are observations of fixed source revisions; destinations and improvements are proposed architecture.

## Source acquisition and baselines

| Source | Revision | Use |
| --- | --- | --- |
| Local reference `references/cpeditor`, upstream `master` | `77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c` | Current source migration baseline; declares project version 7.3.0 |
| CP Editor tag `7.0.2` | `8f8cfafae7c6f3ab4923ce0e08cdb60c2b0e0640` | Baseline used by the supplied evaluation; available in the cloned history |

The first row records the fetched commit, rather than a claim about the latest published release. The local clone includes upstream history and all nested submodules. The parent repository tracks a gitlink, not a copy of upstream implementation inside the new Core. Inspect exact revisions with the commands in [README](../README.md).

[Current CMake](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/CMakeLists.txt#L7) declares 7.3.0, C++14, Qt 6 Widgets/Network/Core5Compat, and KF6 syntax highlighting. [7.0.2 CMake](https://github.com/cpeditor/cpeditor/blob/8f8cfafae7c6f3ab4923ce0e08cdb60c2b0e0640/CMakeLists.txt#L7) uses C++14, Qt 5 and KF5. Native performance experiments must distinguish the affected Qt 5 build from the current Qt 6 source.

## Migration inventory

All CP Editor links below refer to the current pinned commit.

| Upstream component | Observed dependency or behavior | Native destination |
| --- | --- | --- |
| [Compiler](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Core/Compiler.cpp#L56) | `QObject`, `QProcess`, Qt settings/files/codecs; language-name branches for C++/Java/Python; interpreted Python bypasses compilation; C++ adds original include directory | `BuildService`, declarative toolchain profiles and native process host; preserve useful include/runtime rules |
| [Runner](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Core/Runner.cpp#L60) | Qt process/timers, redirected temporary input file, command construction, detached terminal paths | `RunService` and native `ProcessHost`; separate terminal/interactive capabilities |
| [Checker](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Core/Checker.hpp#L31) | Includes a Widget header for verdicts; built-in text checks and runtime compilation of bundled testlib/custom checkers | Pure comparison policies, domain verdicts and external-checker service |
| [SessionManager](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Core/SessionManager.cpp#L18) | Includes generated app UI and window classes; restores tabs directly; Qt JSON/timer/file behavior | Versioned `SessionRepository`; native shells restore their own views |
| [SettingsManager](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Settings/SettingsManager.cpp#L148) | User settings are QSettings INI/QVariant; JSON file drives Qt setting/UI generation | Typed config schema, native preference resources and isolated legacy importer |
| [TestCase](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Widgets/TestCase.hpp#L38) / [TestCases](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Widgets/TestCases.cpp#L547) | Test bytes, selection, checker settings and AC/WA/TLE/RE enum reside in QWidget classes; cases are renumbered | Stable-ID test records and independent native presentation models |
| [CompanionServer](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Extensions/CompanionServer.cpp#L56) | Qt/qhttp listener and JSON parsing; accepts HTTP before parsing; carries original JSON plus URL/time/sample values | Bounded loopback protocol service, versioned field mapping and transactional import |
| [LanguageServer](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Extensions/LanguageServer.hpp#L21) | Direct `CodeEditor` and Qt dependencies; one document reference, full-text lint sync, display-language branches | Independent JSON-RPC/LSP service over native process transport; versioned document mirror and editor adapters |
| [CodeFormatter](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Extensions/CodeFormatter.cpp#L31) | Direct editor/cursor handling and QProcess-based formatting | Snapshot formatter service and revision-checked native undo transaction |
| [CFTool](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Extensions/CFTool.cpp#L42) | External process, Codeforces URL parsing, translated log/toast behavior | Optional judge/CLI adapter; process rules shared, notifications native |
| [StressTesting](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Widgets/StressTesting.cpp#L315) | Generator/reference/candidate orchestration inside QWidget, editor/settings and C++/Java/Python decisions | Dedicated cancellable stress service using snapshot/profile/seed data |
| Editor/highlighter, styles, translator, main windows | Qt text/layout, KSyntaxHighlighting, signals, preferences and window orchestration | New native shells/editor adapters and resource-backed localization |

Use the old project as a behavioral oracle and migration source. Do not rename its Qt-dependent classes and describe the result as a portable Core. The target contracts are specified in [Core contracts](core-contracts.md).

## Compatibility rules to retain

- [Strict and trailing-space implementations](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Core/Checker.cpp#L233): Strict converts CRLF and CR to LF. IgnoreTrailingSpaces also trims trailing Unicode whitespace and terminal whitespace-only lines. Preserve these modes with fixtures; byte-exact and ASCII-only comparison are separate policies.
- [Checker mode inventory](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Core/Checker.hpp#L47): `ncmp`, `rcmp4`, `rcmp6`, `rcmp9`, `wcmp`, `nyesno`, plus the two built-in modes and custom checker. Testlib is a separate dependency with its own provenance.
- [External checker invocation](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Core/Checker.cpp#L297): arguments are input path, actual-output path and expected-output path, in that order.
- [Sample filename rules](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Widgets/TestCases.cpp#L595): templates include `${filename}`, `${basename}`, `${0-index}`, `${1-index}`. Do not hardcode every sample to `.in/.out`.
- [Source snapshots/status](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/mainwindow.cpp#L533): saved text and editor text are distinct. Preserve unsaved source and restoration priority.
- [Preserved recovery](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Core/SessionManager.cpp#L113): current master can avoid autosaving over a preserved session, a behavior added after the 7.0.2 baseline.

## Deliberate corrections, without silent compatibility claims

| Observed behavior | Proposed correction |
| --- | --- |
| [Runner timer](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Core/Runner.cpp#L92) starts before the `started` signal; elapsed time begins later | Separate spawn timeout, execution wall deadline and cleanup/drain timeout with monotonic timestamps |
| [Output reads](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Core/Runner.cpp#L180) remove NUL; retained buffers are byte-counted while UI says characters | Preserve raw byte artifacts, explicit caps and display decoding; never compare silently filtered/truncated output |
| [Checker exits](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Core/Checker.cpp#L172) map `_fail` and points/partial outcomes to WA; unknown exits only log | Distinguish checker failure from wrong answer; support scoring only through an explicit adapter |
| [Empty-answer gate](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/mainwindow.cpp#L1567) uses an option to decide whether an empty expected string is checked | Optional expected-output value distinguishes absent from explicitly empty; importer records the selected legacy interpretation |
| Process destruction kills owned QProcess, without Job Object/group/memory-enforcement support in the audited paths | Native process backends implement and test declared capabilities; no inherited sandbox/MLE guarantee |
| [LSP diagnostics](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Extensions/LanguageServer.cpp#L232) are applied to a stored editor reference | Stable document IDs/revisions, typed position conversions and stale-response rejection |

The normal Runner's temporary-file stdin flow is batch execution. Interactive judging needs bidirectional solution/interactor channels, independent limits, EOF handling, idle/overall deadlines and cancellation. Stress testing needs explicit generator/reference/candidate snapshots, reproducible seed/arguments, bounded iterations and persisted counterexamples. Neither should be implemented as an unchecked flag inside the batch runner.

## Session and settings import

[Session JSON](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Core/SessionManager.cpp#L126) has `currentIndex` and `tabs`, without a top-level schema version. [Tab serialization](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/mainwindow.cpp#L477) includes source/saved text, path/language/compile command, problem URL, cursor/anchor, scroll positions, input/expected arrays, checker choices, selected-test flags and splitter values.

`editorCursor`/`editorAnchor` come from `QTextCursor`: treat them as UTF-16 positions and convert to typed source anchors against the imported text. Qt scrollbar/splitter pixels are legacy presentation metadata, without portable meaning. The misleading `testcasesIsShow` field is a legacy name for case selection and needs a documented mapping. Validate array alignment, recover available samples and report malformed/missing values instead of truncating unsaved content.

[Settings key derivation](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Settings/SettingsInfo.hpp#L53) lowercases names, maps `c++` to `cpp`, and spaces to underscores. [SettingsUpdater](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Settings/SettingsUpdater.cpp#L47) also recognizes old aliases. The generator's `settings.json` is not the persisted INI file. Generic INI parsing alone does not guarantee correct Qt QVariant/font/shortcut conversion.

Create fixtures from actual 7.0.2/current exports before implementing the decoder. An optional standalone Qt-linked exporter is acceptable for unsupported legacy values while the native runtime remains Qt-free. Import preserves original files and produces a versioned native session/config plus a report. MVP compatibility is import-only, without an export-to-Qt promise. [FileUtil](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Util/FileUtil.cpp#L74) already uses QSaveFile in safe mode; the new store must retain atomic-save semantics through native implementations.

## CPH-NG and Companion references

CPH-NG baseline: `15bc6465383be7b2133895894ebf785ed3840ad1`. Its [README](https://github.com/langningchen/cph-ng/blob/15bc6465383be7b2133895894ebf785ed3840ad1/README.md) says `main` contains TypeScript while Rust kernel work is on `refactor/rust-kernel`. These are different research baselines.

Useful observed boundaries include the [testcase/result model](https://github.com/langningchen/cph-ng/blob/15bc6465383be7b2133895894ebf785ed3840ad1/packages/vscode-ext/src/domain/entities/testcase.ts), [execution strategy cancellation contract](https://github.com/langningchen/cph-ng/blob/15bc6465383be7b2133895894ebf785ed3840ad1/packages/vscode-ext/src/application/ports/problems/judge/runner/execution/strategies/IExecutionStrategy.ts), [interactive judge](https://github.com/langningchen/cph-ng/blob/15bc6465383be7b2133895894ebf785ed3840ad1/packages/vscode-ext/src/application/useCases/problems/judge/interactiveJudgeService.ts), and [stress use case](https://github.com/langningchen/cph-ng/blob/15bc6465383be7b2133895894ebf785ed3840ad1/packages/vscode-ext/src/application/useCases/webview/problem/stressTest/StartStressTest.ts). Borrow their responsibility separation and workflow ideas; TypeScript/VS Code service dependencies are outside the proposed runtime.

The project [package manifest](https://github.com/langningchen/cph-ng/blob/15bc6465383be7b2133895894ebf785ed3840ad1/package.json) declares `AGPL-3.0-only`, while inspected source headers use GPL version 3 or later wording. Do not infer permission to copy from one label; resolve exact file/package provenance before any implementation reuse. This change uses it as a design reference only.

Competitive Companion protocol baseline: `df90fabb52e8f566ea3382405e22d246af5d6a69`. Its [custom-tool format](https://github.com/jmerle/competitive-companion/blob/df90fabb52e8f566ea3382405e22d246af5d6a69/README.md#custom-tools) specifies HTTP POST `/`, compatible ports, limits in ms/MB, tests and optional interactive/input/output/language/batch fields. Use this as the external contract, rather than assuming CP Editor's subset contains every supported field.

## Licensing and evidence limits

The CP Editor [LICENSE](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/LICENSE) is GPL v3; audited [source headers](https://github.com/cpeditor/cpeditor/blob/77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c/src/Core/Compiler.cpp#L1) permit version 3 or later. Plan a GPL-compatible derived implementation when porting code, retain notices and track exact copied files. Qt, testlib, editor and other transitive dependencies have separate notices. Do not label a derived Core MIT merely because a selected editor is permissive.

This audit reads code and manifests. No original or native application was compiled, no legacy export fixture was generated, and no process/input-performance claim was validated at runtime. The migration map is the implementation starting point, with required verification in [the plan](implementation-plan.md).
