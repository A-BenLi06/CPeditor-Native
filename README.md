# CPEditor-Native

Architecture and source research for a native competitive-programming editor: **WinUI 3 + C++/WinRT on Windows, SwiftUI + AppKit on macOS, and a shared Qt-free C++20 competition engine**.

This repository currently contains the architecture specification and a complete upstream CP Editor reference checkout, including its nested submodules. Native applications and the new engine have not been implemented or benchmarked.

## Start here

| Document | Purpose |
| --- | --- |
| [Architecture](docs/architecture.md) | Component boundaries, platform integration, dependency direction, and proposed repository layout |
| [Core contracts](docs/core-contracts.md) | Source snapshots, job lifecycle, process limits, verdicts, bridge ownership, and persistence |
| [Upstream audit](docs/upstream-audit.md) | Source-backed migration map and compatibility differences |
| [Editor evaluation](docs/editor-evaluation.md) | Verified dependencies, licenses, editor tradeoffs, and prototype gates |
| [Implementation plan](docs/implementation-plan.md) | Ordered milestones, acceptance evidence, and unresolved decisions |
| [Walkthrough](walkthrough.md) | Timestamped changes, reasoning, and validation |

## Upstream source

[CP Editor](https://github.com/cpeditor/cpeditor) is cloned at `references/cpeditor` as a Git submodule. The source audit's exact baseline is recorded in [the upstream audit](docs/upstream-audit.md); the parent Git commit pins the reference tree. It is a research reference, outside the future native application's build graph.

From the repository root, these commands work in PowerShell 7:

```powershell
git submodule update --init --recursive
git -C references/cpeditor rev-parse HEAD
git -C references/cpeditor show 7.0.2:CMakeLists.txt
git submodule status --recursive
```

For a fresh checkout, clone this repository with `git clone --recurse-submodules <repository-url>`. No remote has been configured for this new local repository. Do not use `git submodule update --remote` for reproducible research: updating the reference requires a reviewed gitlink change and walkthrough entry.

The current upstream snapshot declares version 7.3.0 and Qt 6. The supplied evaluation describes tag 7.0.2 and Qt 5. Both are considered, with the distinction preserved in the audit.

## Initial decisions

- Prototype WinUIEdit first on the specified Windows 10 22H2 machine, including the original UU remote/virtual-desktop reproduction.
- Compare CodeEditorView and STTextView on macOS; keep CodeEditSourceEditor as a third candidate. Use CodeEdit and BriskEdit as architecture references.
- Extract competition semantics and rebuild process, persistence, and protocol adapters. Keep editor buffers, IME, undo, and rendering within each native application.
- Use a stable C ABI for the Swift bridge; the Windows C++ shell calls the same engine through its C++ API.
- Keep configuration and localization extensible. C++ is the first tested language profile, rather than a fixed language model.

CP Editor source headers grant GPL version 3 or later. Reference projects retain their own licenses. Native implementation reuse and distribution must follow the provenance and licensing decisions in the audit; this architecture-only change does not copy their implementation into a new Core.
