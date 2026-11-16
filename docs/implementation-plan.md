# Implementation plan and acceptance

Status: planned work. The only completed work is source acquisition and architecture research recorded in `walkthrough.md`.

## Milestones

| Milestone | Deliverable | Exit evidence |
| --- | --- | --- |
| M0: baseline and editor risk | Minimal WinUI 3/WinUIEdit host, macOS CodeEditorView/STTextView comparison, exact upstream/reference locks | Builds/launches on target hosts; common editor fixtures and original Windows reproduction matrix pass; actual latency/memory observations recorded |
| M1: portable engine | CMake C++20 domain/services/ports, process hosts, checker policies, C ABI and headless harness | Standalone Core builds without Qt or UI dependencies on Windows/macOS; process, race, Unicode, checker and ABI tests pass |
| M2: Windows vertical slice | Document tabs, captured-source build/run, sample results, cancellation and versioned recovery | Open/edit unsaved C++ source → configure toolchain → Run All → compare → cancel → restart/recover; stable source/test IDs and correct cleanup |
| M3: interoperability | Companion import, sample filename profiles, legacy session/settings importer, testlib/custom checkers | Real protocol/config fixtures import without loss; malformed input cannot replace current work; custom checker errors classify correctly |
| M4: macOS vertical slice | Chosen editor, Swift C ABI client, native commands/panels/preferences, same engine workflows | Same headless fixture outcomes as Windows; native editing/undo/IME and session recovery pass on selected minimum OS |
| M5: language and advanced services | clangd diagnostics/completion, formatter, terminal capability, stress and interactive services, optional judge adapters | Feature-specific protocol/cancellation/revision tests; no unverified mode is exposed as working |
| M6: release | Dependency/provenance review, performance budgets, MSIX and signed/notarized macOS artifacts | Clean install/update/recovery on supported hosts; native input regression suite; measured budgets and distribution/runtime checks |

M0 freezes the editor adapter contract and rejects unsuitable candidates before full UI work. Windows M0 leads; the macOS comparison can run independently. After M1 contracts stabilize, the two shells can proceed in parallel. LSP/formatting can be moved earlier when needed for a useful vertical slice, while keeping their revision gates intact. No calendar estimate substitutes for these exit gates.

## First implementation slice

1. Add a Windows C++/WinRT WinUI 3 host with pinned WinUIEdit and discoverable Visual Studio/MSBuild tooling through PowerShell 7. Keep native background capabilities compatible with Windows 10.
2. Open the same C++ fixture as CP Editor. Implement open/save, committed edit/revision reporting, selection, scrolling and focus lifecycle; capture text snapshots without a compilation engine yet.
3. Record Chinese IME, Unicode, mixed-DPI, pointer and UU/virtual-desktop results alongside an empty native host and ordinary native text control. Include the user's affected Qt 5 build; compare the current Qt 6 upstream build when available.
4. Implement the same fixture suite in CodeEditorView and STTextView hosts. Include a real C++ lexical configuration where required; measure integration work, not merely blank-editor rendering.
5. Choose candidates and pin their transitive dependencies only after the build, license and editor gates have evidence. Add the engine vertical slice after these gates.

The prototypes are experiment hosts, without product copy describing development requirements. If the original input problem persists in empty/native-control hosts, investigate the system/remote/input path and record it. Do not claim native reconstruction has solved it or insert sleeps/suppress pointer events to disguise it.

## Verification matrix

| Area | Required cases |
| --- | --- |
| Process execution | Missing tool, launch failure, space/CJK paths, explicit argv/quoting, large stdin plus simultaneous stderr/stdout, no implicit shell |
| Lifetimes and limits | Cancel at spawn/build/run/check; natural exit versus timeout/cancel; retained-output caps including final drain; inherited pipes; process-tree/group cleanup; close/shutdown while running |
| Outcome correctness | Empty input; absent versus empty answer; CE versus infrastructure errors; TLE/RE/OLE; explicit memory capability; checker timeout/crash/start failure and unsupported scoring |
| Comparison compatibility | CRLF/CR/LF, terminal empty lines, Unicode whitespace, leading/internal spaces, NUL/invalid UTF-8 artifacts, testlib path order and code mapping |
| Document consistency | Type during compilation; formatting/completion on stale revisions; delete/reorder samples while running; undo back to savepoint; IME composition during diagnostics |
| Recovery/import | Unsaved documents; external disk changes; damaged/unknown-version JSON; atomic-save failure; preserved recovery; actual QSettings aliases/QVariant exports; unsupported migration values; safe filenames |
| Companion | Real extension payloads, batch/deduplication, valid empty samples, optional/unknown fields, malformed/oversized JSON, port conflicts, loopback-only bind, unsupported interactive/file modes |
| Native editing | CJK, emoji and combining marks; correct UTF-8/UTF-16/LSP conversion; accessibility, selection/focus lifecycle, scroll/DPI/Spaces/desktop switching and disposal |
| Bridge | ABI version mismatch, allocator ownership, invalid handle/cancel, bounded event draining, shutdown while event task is active; no UI-thread process work |

Start with unit tests for pure comparisons, profile/schema resolution, revision maps and lifecycle transitions. Use real small helper processes for the process tests; mocks alone cannot establish pipe drainage, child termination or encoding correctness. Run editor conformance on rendered applications using the native target host. Performance measurements are separate from correctness assertions and recorded with environment metadata.

## Windows regression and performance evidence

Use the same machine, source fixture, editor operation sequence and input hardware for every comparison. Record OS edition/build, Windows App SDK and editor revision, toolchain, display scale/refresh, mouse polling rate, remote software version and run configuration. Compare affected CP Editor, current Qt 6 CP Editor when buildable, empty WinUI host, ordinary native-control host and WinUIEdit host.

Repeat UU remote connection/disconnection and rapid four-finger Windows virtual-desktop switching. Include Alt+Tab, minimize/restore, rapid reactivate → Ctrl+A, mixed-DPI moves and available 125–8000 Hz mouse settings. Track system cursor motion separately from edit/selection dispatch and presentation. Trace input event timestamps, dispatch queues/text-store locks, frame/input-to-paint latency distributions, CPU, working set and cold/warm startup. Native framework choice is not evidence of lower latency or memory.

Measure ordinary, long-line and large-file fixtures with highlighting/minimap/LSP independently toggled. Freeze fixture sizes/content digests and measurement methodology, establish baseline-derived latency/startup/memory budgets before choosing a production candidate, and keep raw measurement artifacts outside source control with a compact result summary. A candidate must have correct text/IME/selection behavior, bounded queues/lifetimes and no repeatable introduced native-host regression. The original defect must be resolved in its reproduction or explicitly remain a release blocker for the intended fix claim.

## Build and delivery policy

- Use CMake targets for the engine and a headless test executable; use MSVC on Windows and Apple Clang on macOS. Select/pin the CMake and compiler minimums during M1 rather than inheriting upstream's C++14 build configuration.
- Keep Windows scripts PowerShell 7 compatible, discover tools (for example with installed VS tooling discovery), avoid drive-specific paths, and check native command exit codes. Document platform prerequisites and exact package resolutions from the verified build.
- Use Windows-specific CI for WinUI restore/build/launch and macOS CI for Swift/C++ ABI and minimum-OS/editor verification. Include a clean Core-only job to catch UI/Qt dependency leakage. Interactive hardware/remote regressions remain explicitly tracked manual target-machine evidence.
- Pin Windows App SDK/editor packages, Swift packages and native third-party revisions. A research reference commit is not an approved runtime lock. Avoid floating branches in shipped dependency definitions.
- Keep runtime compiler discovery/configuration separate from development toolchain discovery. A user may select GCC/Clang/MSVC or an interpreted profile without having the application's build environment.
- Package Windows as signed MSIX and macOS as a Developer ID signed/notarized direct-distribution app initially. Portable Windows and App Store/sandbox options require separate deployment/permission testing.
- Every completed implementation change updates root `walkthrough.md` with a machine-generated timestamp/UTC offset, rationale and validation, then receives a Git commit.

## Decisions remaining

| Decision | Evidence needed | Planned resolution |
| --- | --- | --- |
| Production Windows editor/version | WinUIEdit IME/focus/accessibility/lifecycle and target Windows tests | M0; evaluate direct Scintilla HWND only if required |
| Production macOS editor and OS/Xcode floor | Pinned component builds, actual C++ features, license choice, responsiveness | M0/M4; compare CodeEditorView/STTextView, then CodeEditSourceEditor |
| Serialization/HTTP libraries | Non-Qt support, cancellation behavior, provenance and package lock | M1/M3; inject through ports |
| Legacy complex settings conversion | Real Qt INI/QVariant exports and unsupported-value inventory | M3; optional isolated Qt export utility if decoder coverage is insufficient |
| CPU/memory and descendant guarantees | Native backend resource/process tests and limitations | M1/M6; report unsupported capabilities honestly |
| Large-file and performance budgets | Target-hardware baselines and enabled-feature costs | M0/M6; configurable policy grounded in measurements |
| Distribution license and bundled dependencies | Exact copied-code provenance and resolved license graph | Before implementation reuse/distribution; plan GPL-compatible derivation if upstream code is ported |

No Windows/macOS editor build, native runtime check, latency benchmark, or distribution validation has been performed in this architecture-only change. Those are the next implementation gates.
