# Repository instructions

- Primary target: Windows 10 22H2 Professional for Workstations. Use PowerShell 7 for Windows development scripts. macOS is the second native application target.
- Check the actual execution host before running platform commands. Do not report Windows verification from a macOS host.
- Default documentation and application language is English. Put user-visible strings in native localization resources.
- Keep languages, toolchains, limits, filename patterns, and integration configuration data-driven. Define protocol fields once in versioned contracts rather than duplicating string keys throughout the application.
- Keep project/development requirements in repository documentation, outside product content.
- Implement correct behavior; do not hide defects with timing hacks or suppressed input.
- After each completed change set, append its changes, reasoning, and validation to root `walkthrough.md` with a machine-generated ISO 8601 timestamp including the UTC offset, then make a Git commit.
- Treat `references/cpeditor` as an upstream research submodule. Do not edit its implementation as part of the native application. Explicitly record a new source revision when updating it.
- Research and architecture documents describe proposed behavior until implementation and validation provide evidence.
