# Native editor evaluation

Research date: 2026-11-16 (UTC+08:00). Requirements and licenses below were checked against immutable source snapshots, without building the components. Manifest declarations, README claims and actual runtime validation are distinct.

## Recommendation

Start Windows with WinUIEdit behind a replaceable adapter. Compare CodeEditorView and STTextView on macOS using identical C++ fixtures; retain CodeEditSourceEditor as a third candidate. Use CodeEdit and BriskEdit for application boundaries and lifecycle patterns. Choose production components after the gates below, without forking a full IDE.

Several supplied project descriptions need revision at the inspected commits: WinUIEdit's root notice is custom permissive Scintilla-style wording, rather than MIT; STTextView offers GPLv3/commercial licensing; CodeEditorView is Apache-2.0, uses tokeniser/regex language configurations, and does not ship a C/C++ language configuration.

## Windows source baseline

WinUIEdit `main`: `d9f60dbca65c2af54ced590d873d9438de32ce77`.

| Observation | Evidence and consequence |
| --- | --- |
| C++ native Scintilla WinUI 3/UWP port, with C++/WinRT and C# examples | [README](https://github.com/BreeceW/WinUIEdit/blob/d9f60dbca65c2af54ced590d873d9438de32ce77/README.md), [C++ WinUI 3 demo](https://github.com/BreeceW/WinUIEdit/tree/d9f60dbca65c2af54ced590d873d9438de32ce77/CppDemoWinUI3); appropriate first Windows candidate |
| README says not production-ready and expects breaking APIs | Pin versions and contain APIs in the adapter; do not promise component stability |
| WinUI 3 demo minimum Windows `10.0.17763.0`, target SDK `10.0.22621.0` | [Project](https://github.com/BreeceW/WinUIEdit/blob/d9f60dbca65c2af54ced590d873d9438de32ce77/CppDemoWinUI3/CppDemoWinUI3.vcxproj), [manifest](https://github.com/BreeceW/WinUIEdit/blob/d9f60dbca65c2af54ced590d873d9438de32ce77/CppDemoWinUI3/Package.appxmanifest); Windows 10 22H2 is within declared range; target SDK is not runtime minimum |
| Windows App SDK `1.7.260224002` in the inspected source packages | [packages.config](https://github.com/BreeceW/WinUIEdit/blob/d9f60dbca65c2af54ced590d873d9438de32ce77/WinUIEditor/packages.config); an observed reference version, not a chosen product dependency lock |
| Root custom permissive notice plus Scintilla/Lexilla notices | [Root LICENSE](https://github.com/BreeceW/WinUIEdit/blob/d9f60dbca65c2af54ced590d873d9438de32ce77/LICENSE), [Scintilla](https://github.com/BreeceW/WinUIEdit/blob/d9f60dbca65c2af54ced590d873d9438de32ce77/scintilla/License.txt), [Lexilla](https://github.com/BreeceW/WinUIEdit/blob/d9f60dbca65c2af54ced590d873d9438de32ce77/lexilla/License.txt); preserve exact notices, without assigning an unverified SPDX ID |
| XAML wrapper exposes editor operations through WinRT | [Control IDL](https://github.com/BreeceW/WinUIEdit/blob/d9f60dbca65c2af54ced590d873d9438de32ce77/WinUIEditor/CodeEditorControl.idl), [wrapper IDL](https://github.com/BreeceW/WinUIEdit/blob/d9f60dbca65c2af54ced590d873d9438de32ce77/WinUIEditor/EditorWrapper.idl); keep these types outside shared Core |

The [native platform layer](https://github.com/BreeceW/WinUIEdit/tree/d9f60dbca65c2af54ced590d873d9438de32ce77/scintilla/winui) implements pointer translation, DirectWrite drawing, text-store input and DPI conversion. This is a Windows port, without a macOS backend. Its package manifest includes WebView2 SDK, but the editor's observed hosting path is native Scintilla; the package's presence alone does not establish a Chromium editor runtime.

Concrete inspection risks:

- [EditorBaseControl lifecycle/reactivation](https://github.com/BreeceW/WinUIEdit/blob/d9f60dbca65c2af54ced590d873d9438de32ce77/WinUIEditor/EditorBaseControl.cpp#L30): close/lifecycle and rapid reactivation/Ctrl+A comments require targeted tests.
- [ScintillaWin input implementation](https://github.com/BreeceW/WinUIEdit/blob/d9f60dbca65c2af54ced590d873d9438de32ce77/scintilla/winui/ScintillaWin.cxx#L821): IME undo TODO, screen-coordinate/DPI uncertainties and pointer queuing under text-store locking. These are inspection findings, without a reproduced stall.
- [Automation peer](https://github.com/BreeceW/WinUIEdit/blob/d9f60dbca65c2af54ced590d873d9438de32ce77/WinUIEditor/EditorBaseControlAutomationPeer.cpp): incomplete accessibility/composition behavior needs verification.
- [Demo window](https://github.com/BreeceW/WinUIEdit/blob/d9f60dbca65c2af54ced590d873d9438de32ce77/CppDemoWinUI3/MainWindow.xaml.cpp): Mica and focus workarounds are demo behavior. Product code uses supported native backdrop/focus capabilities on Windows 10.

## macOS source baselines

| Component | Revision | Observed license/requirements | Decision value |
| --- | --- | --- | --- |
| CodeEditorView | `2394a48f919d6570cfbfbd53d047a3eaeb95b59e` | Apache-2.0; manifest Swift tools 5.9, macOS 14+; README pre-release; actual toolchain minimum unverified | Integrated SwiftUI/AppKit editor, messages/minimap/completion hooks; C++ lexical and clangd integration required |
| STTextView | `c7d519318b06ebb7a99a641e4aa8831105d3b7d4` | GPLv3/commercial; macOS 14+; manifest tools 5.9; README requires Xcode 26+ | TextKit 2 editing foundation with plugins and SwiftUI wrapper; highlighting/language-service layers remain integration work |
| CodeEditSourceEditor | `1fa4d3c3ffba007482111466cb9721416f97ae00` | MIT; manifest tools 5.9, macOS 13+; README not production-ready | Broader Tree-sitter editor with AppKit/SwiftUI interface; third comparison candidate |
| CodeEdit | `fa2aebd86373211c78626074b53ab75010767575` | MIT; README development/pre-release | Native workspace/commands/window composition reference; no full IDE fork |
| BriskEdit | `47ea9076469b67436c2d322a90c0f5cadbbfde05` | MIT; macOS 15+, Swift 6, Xcode 26 project configuration | Lightweight application boundaries, delayed services, document revision and large-file policies |

CodeEditorView's [manifest](https://github.com/mchakravarty/CodeEditorView/blob/2394a48f919d6570cfbfbd53d047a3eaeb95b59e/Package.swift), [README](https://github.com/mchakravarty/CodeEditorView/blob/2394a48f919d6570cfbfbd53d047a3eaeb95b59e/README.md), and [LICENSE](https://github.com/mchakravarty/CodeEditorView/blob/2394a48f919d6570cfbfbd53d047a3eaeb95b59e/LICENSE) establish the declaration. Current source uses SwiftUI `@Entry`, so tools-version 5.9 must not be advertised as a verified oldest build toolchain. Its [LanguageConfiguration](https://github.com/mchakravarty/CodeEditorView/blob/2394a48f919d6570cfbfbd53d047a3eaeb95b59e/Sources/LanguageSupport/LanguageConfiguration.swift) provides tokeniser configuration; shipped languages at this snapshot lack C/C++. Its [LanguageService](https://github.com/mchakravarty/CodeEditorView/blob/2394a48f919d6570cfbfbd53d047a3eaeb95b59e/Sources/LanguageSupport/LanguageService.swift#L194) provides synchronization/completion/information hooks, without a bundled clangd server or complete LSP transport. Evaluate a custom C++ configuration and an adapter to the shared language service.

STTextView's [manifest](https://github.com/krzyzanowskim/STTextView/blob/c7d519318b06ebb7a99a641e4aa8831105d3b7d4/Package.swift), [README](https://github.com/krzyzanowskim/STTextView/blob/c7d519318b06ebb7a99a641e4aa8831105d3b7d4/README.md), [license terms](https://github.com/krzyzanowskim/STTextView/blob/c7d519318b06ebb7a99a641e4aa8831105d3b7d4/LICENSE.md#L1), and [SwiftUI AppKit wrapper](https://github.com/krzyzanowskim/STTextView/blob/c7d519318b06ebb7a99a641e4aa8831105d3b7d4/Sources/STTextViewSwiftUIAppKit/TextView.swift) establish a lower-level alternative. Record the exact licensing option before integration. Native foundation quality is subject to the same editing/IME tests.

CodeEditSourceEditor's [manifest](https://github.com/CodeEditApp/CodeEditSourceEditor/blob/1fa4d3c3ffba007482111466cb9721416f97ae00/Package.swift), [README](https://github.com/CodeEditApp/CodeEditSourceEditor/blob/1fa4d3c3ffba007482111466cb9721416f97ae00/README.md), [license](https://github.com/CodeEditApp/CodeEditSourceEditor/blob/1fa4d3c3ffba007482111466cb9721416f97ae00/LICENSE.md), and [SourceEditor wrapper](https://github.com/CodeEditApp/CodeEditSourceEditor/blob/1fa4d3c3ffba007482111466cb9721416f97ae00/Sources/CodeEditSourceEditor/SourceEditor/SourceEditor.swift#L14) support the third candidate. Keep its coordinator callbacks in the adapter and avoid importing a second language-service owner.

[CodeEdit README](https://github.com/CodeEditApp/CodeEdit/blob/fa2aebd86373211c78626074b53ab75010767575/README.md) is useful context for native application composition. BriskEdit's [architecture document](https://github.com/jx-grxf/BriskEdit/blob/47ea9076469b67436c2d322a90c0f5cadbbfde05/ARCHITECTURE.md) still calls its run pipeline planned, while [RunService source](https://github.com/jx-grxf/BriskEdit/blob/47ea9076469b67436c2d322a90c0f5cadbbfde05/Sources/BriskEdit/Services/RunService.swift) implements execution. Use code for current behavior. Its [project configuration](https://github.com/jx-grxf/BriskEdit/blob/47ea9076469b67436c2d322a90c0f5cadbbfde05/project.yml) and README thresholds belong to BriskEdit; do not copy its OS floor or large-file byte policies without measuring this product.

## Common editor adapter requirements

Native editor interfaces share semantic fixtures, while their types remain platform-local. Required operations include open/capture document, revision-checked edits, savepoint/dirty state, typed range reveal, diagnostics, editor commands, focus, appearance, capabilities and disposal. Events include committed text changes and selection/savepoint status.

The engine receives immutable UTF-8 text and typed revisions. Scintilla byte offsets, AppKit UTF-16 offsets and LSP negotiated positions require revision-specific conversion. Buffer ownership, IME, selection, undo, highlighting, layout, accessibility and pointer handling stay native. Editor-specific language/grammar IDs are profile mappings inside the adapter. Large-file capability changes must not corrupt text or invalidate source/check results.

## Prototype gates

All gates below are proposed and unexecuted.

| Gate | Evidence required |
| --- | --- |
| Build/dependency | Restore/build/launch each pinned candidate on the proposed minimum OS and selected compiler; record resolved transitive dependencies and notices |
| C++ workload | Real syntax configuration/grammar, representative snippets, comments/strings/preprocessor, multiline/long-line fixtures; no blank-editor substitute |
| Chinese IME | Compose, convert, cancel, commit, replace selection, multiline input, undo/redo; correct candidate placement after scrolling and display changes; no dropped/duplicate characters |
| Unicode/ranges | CJK, surrogate pairs, combining marks, tabs, CRLF/LF; save/snapshot round trips; diagnostics/edits target correct spans; reject stale ranges and character splits |
| Focus/lifetime | Rapid reactivation/Ctrl+A, tab open/close, menus, Alt+Tab or Spaces/full-screen, minimize/restore, shutdown; no sticky capture or late callbacks |
| DPI/accessibility | Native scale changes, mixed-DPI windows where applicable, selection/completion/context/IME geometry; keyboard and accessibility tools |
| Performance | Ordinary/large/long-line C++ fixtures; cold/warm startup, first edit, scroll/input distribution, memory/CPU, enabled/disabled highlighting/minimap/LSP; baseline-derived budgets |
| Snapshot/async services | Compile during typing, diagnostics during marked text, formatter conflict, undo preservation, close with pending service response |
| Windows original regression | Same host/files/hardware, affected Qt 5 build, empty WinUI host, normal native control, WinUIEdit, current Qt 6 when available; repeat UU disconnect and desktop switching |
| Windows high-rate input | Available 125–8000 Hz device settings; event/dispatch delay and lock/backlog measurements; bounded queues and correct selection/scroll |

macOS uses the same files, clangd results and source-offset fixtures in both initial candidates. A third candidate is justified by concrete missing capability/build/license/latency findings. Editor choice must record integration cost and actual results, including platform/toolchain limits. The Windows system-cursor symptom must be tracked separately from editor rendering; no source inspection proves it solved.

## Supporting references

[Official WinUI Gallery](https://github.com/microsoft/WinUI-Gallery) is the native control/interaction reference. [MonacoEditor.WinUI3](https://github.com/luttje/MonacoEditor.WinUI3) is a WebView2-based contingency outside the initial path. [SwiftEd](https://github.com/glaforge/swifted) can inform future text-buffer experiments; it is not a selected dependency and has not received a source audit in this change. [CP-IDE](https://github.com/j-o-s-e-p-h-h/CP-IDE/tree/fe014bfd040c237f6c3fec8e4a0fed35231d43af) is an ancillary contest-workflow reference, without an approved editor integration. CPH-NG and Competitive Companion source-backed workflow observations are in [the upstream audit](upstream-audit.md).
