# Walkthrough

## 2026-11-16T20:11:27+08:00 — Initial source acquisition and native architecture

### Changes

- Initialized the empty CPEditor-Native directory as a Git repository on `main`.
- Cloned `https://github.com/cpeditor/cpeditor` as `references/cpeditor`, pinned at `77e563b4719fcc9d2bde5b2f651ceb25c4a48e2c`, and initialized all six nested upstream submodules. The 7.0.2 tag/history is available for the supplied evaluation baseline.
- Added root repository instructions and build/IDE-output exclusions. No original CP Editor implementation was modified.
- Added README and five English design documents covering architecture, Core contracts, source migration, editor evaluation and implementation gates.

### Reasoning

The current upstream snapshot declares 7.3.0/C++14/Qt 6, while the supplied assessment describes 7.0.2/Qt 5. Its Core, test model, settings, sessions and extensions still contain Qt/UI dependencies. The design therefore preserves useful competition semantics and explicit import compatibility while implementing a Qt-free C++20 service layer with native process/file/protocol adapters. WinUI 3/C++/WinRT and SwiftUI/AppKit own their native document/editor/window behavior. Windows calls the engine's C++ facade; Swift uses a stable C ABI with explicit lifetime and event ownership.

Editor selection remains gated by actual prototypes. Source research corrected WinUIEdit's MIT description, STTextView's current GPL/commercial license, and CodeEditorView's missing C++ lexical configuration. CodeEdit/BriskEdit remain architecture references; CPH-NG remains a workflow reference. Dependency/provenance claims refer to immutable source commits rather than floating README descriptions.

Core rules distinguish empty input, absent versus empty expected output, checker errors and solution verdicts. They preserve CP Editor newline/Unicode-whitespace comparison semantics and introduce explicit source revisions, stable testcase IDs, raw-output byte limits, process cancellation and safe recovery. Read-only review corrected the state diagram's checker prerequisite, background-tab undo retention and completed-event backpressure.

### Validation and scope

- Confirmed the fetched CP Editor revision, 7.0.2 baseline and all nested submodule checkouts; upstream worktree is clean.
- Inspected source/manifests/licenses using local Git history and immutable GitHub source URLs; independent read-only reviews covered Core migration and both native editor paths.
- Design documents distinguish observed facts, proposed contracts and unexecuted acceptance gates.
- Final repository/document integrity checks are recorded below before commit.
- Execution host is macOS with zsh; PowerShell 7 is unavailable on this host. Windows 10 22H2 Professional for Workstations/PowerShell 7 remain the primary target requirements. No native/editor build, original application build, runtime benchmark, legacy export-fixture generation or deployment was performed. Those belong to the documented implementation milestones.

### 2026-11-16T20:11:53+08:00 — Final integrity results

- Passed UTF-8 Markdown integrity checks for eight documents, fifteen local links and thirty-two immutable CP Editor source path/line citations.
- Recursive submodule status confirms the exact pinned reference and all six nested modules are initialized; source diff is empty.
- Core consistency review findings were resolved; final macOS/editor/bridge review reported no material findings.
- Whitespace checks passed; the completed architecture/source-acquisition change is ready for its initial Git commit.
