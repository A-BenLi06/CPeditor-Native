import AppKit
import Foundation
import Testing
import NativeEditorModel
import NativeCoreBridge
@testable import CPEditorNative

@MainActor @Suite(.serialized) struct ToolchainDiscoveryTests {
    @Test func discoversVersionedGNUCompilerAndIgnoresAppleAlias() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let config = ToolchainDiscovery(schemaVersion: 1, searchDirectories: [directory.path], executablePattern: try ToolchainDiscovery.load().executablePattern, profileResources: [])
        for name in ["g++", "g++-9", "g++-15", "g++-20.txt"] {
            let url = directory.appendingPathComponent(name)
            try Data("fixture".utf8).write(to: url)
            try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: url.path)
        }
        #expect(try config.findCompiler(path: "")?.lastPathComponent == "g++-15")
        try FileManager.default.removeItem(at: directory.appendingPathComponent("g++-15"))
        try FileManager.default.removeItem(at: directory.appendingPathComponent("g++-9"))
        #expect(try config.findCompiler(path: "") == nil)
    }

    @Test func restoredCatalogIncludesNewProfilesWithoutChangingUserSelection() throws {
        let runner = RunController()
        let clang = try #require(runner.profiles.first { $0.id == RawIdentifier("macos-clang-cpp17") })
        let recovered = try runner.recoverToolchains([clang], selected: clang.id)
        #expect(recovered.selected == clang.id)
        #expect(recovered.profiles.contains { $0.id == RawIdentifier("macos-clang-cpp20") })
        for discovered in runner.bundledProfiles { #expect(recovered.profiles.contains { $0.id == discovered.id }) }
    }

    @Test func missingGNUCompilerBlocksDefaultInsteadOfUsingAppleAlias() throws {
        _ = NSApplication.shared
        let bundled = try ToolchainDiscovery.load()
        let missing = ToolchainDiscovery(schemaVersion: 1, searchDirectories: [], executablePattern: bundled.executablePattern,
                                         profileResources: bundled.profileResources)
        let catalog = try missing.discover(path: "")
        let runner = RunController(discoverToolchains: { catalog })
        let session = try NativeEditorSession(document: SourceDocument(text: "int main() {}", editorKind: .stTextView, languageID: "cpp"), profile: .load(id: "cpp"))
        defer { session.dispose() }
        #expect(runner.selectedProfileID == RawIdentifier("macos-gcc-cpp17"))
        #expect(runner.selectedProfile != nil)
        #expect(!runner.canRun(session))
        #expect(runner.toolchainNotice(for: session) == L10n.string("gnu_cpp_required"))
        runner.runAll(session)
        #expect(!runner.isRunning)
        #expect(runner.result == nil)
        #expect(runner.errorMessage == L10n.string("gnu_cpp_required"))
        let profile = try #require(runner.selectedProfile)
        let edited = try profile.updating(buildExecutable: profile.buildExecutable, buildArguments: profile.buildArguments + ["-Wall"],
                                         buildWallTimeoutMS: 40_000, runWallTimeoutMS: 3_000)
        try runner.applyProfile(edited) { _, _ in }
        #expect(!runner.canRun(session))
        #expect(runner.toolchainNotice(for: session) == L10n.string("gnu_cpp_required"))
        let recovered = try runner.recoverToolchains(runner.profiles, selected: runner.selectedProfileID)
        #expect(recovered.selected == RawIdentifier("macos-gcc-cpp17"))
        #expect(recovered.profiles.contains { $0.id == recovered.selected })
    }

    @Test func installingGNUCompilerResolvesPreviouslyUnavailableSavedTemplate() throws {
        let runner = RunController()
        let templates = try ToolchainDiscovery.load().discover().templates
        let selected = RawIdentifier("macos-gcc-cpp17")
        let restored = try runner.recoverToolchains(templates, selected: selected)
        #expect(restored.profiles.first { $0.id == selected } == runner.profiles.first { $0.id == selected })
        let template = try #require(templates.first { $0.id == selected })
        let edited = try template.updating(buildExecutable: template.buildExecutable, buildArguments: template.buildArguments + ["-Wall"],
                                          buildWallTimeoutMS: 40_000, runWallTimeoutMS: 3_000)
        let custom = try runner.recoverToolchains([edited], selected: selected)
        let resolved = try #require(custom.profiles.first { $0.id == selected })
        #expect(resolved.buildExecutable == runner.selectedProfile?.buildExecutable)
        #expect(resolved.buildArguments == edited.buildArguments)
        #expect(resolved.buildWallTimeoutMS == 40_000)
    }

    @Test func installedGNUCompilerBuildsBitsHeaderThroughCore() async throws {
        let compiler = try ToolchainDiscovery.load().findCompiler()
        if ProcessInfo.processInfo.environment["CPN_REQUIRE_GNU_CPP"] == "1" { try #require(compiler != nil) }
        guard compiler != nil else { return }
        _ = NSApplication.shared
        let runner = RunController()
        #expect(runner.selectedProfileID == RawIdentifier("macos-gcc-cpp17"))
        let source = "#include <bits/stdc++.h>\nint main(){std::vector<int> v{1,2}; std::cout << std::accumulate(v.begin(),v.end(),0) << '\\n';}\n"
        let session = try NativeEditorSession(document: SourceDocument(text: source, editorKind: .stTextView, languageID: "cpp"), profile: .load(id: "cpp"))
        defer { session.dispose() }
        session.testCases = [TestCaseInput(expected: "3\n")]
        runner.runAll(session)
        await runner.waitForCurrentRun()
        let result = try #require(runner.result)
        #expect(result.buildOutcome == "Succeeded", Comment(rawValue: result.build?.stderr.text ?? result.error))
        #expect(result.tests.first?.verdict == "AC")
        await runner.shutdown()
    }

    @Test func recoveryFollowsRemovedManagedCompilerButKeepsExplicitPaths() throws {
        _ = NSApplication.shared
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let managed = root.appendingPathComponent("compilers")
        try FileManager.default.createDirectory(at: managed, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let bundled = try ToolchainDiscovery.load()
        let discovery = ToolchainDiscovery(schemaVersion: 1, searchDirectories: [managed.path], executablePattern: bundled.executablePattern,
                                          profileResources: bundled.profileResources)
        func install(_ name: String) throws -> URL {
            let url = managed.appendingPathComponent(name)
            try Data("fixture".utf8).write(to: url)
            try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: url.path)
            return url
        }
        let old = try install("g++-14")
        let initial = RunController(discoverToolchains: { try discovery.discover(path: "") })
        let original = try #require(initial.selectedProfile)
        let saved = try original.updating(buildExecutable: old.path, buildArguments: original.buildArguments + ["-Wall"],
                                         buildWallTimeoutMS: 45_000, runWallTimeoutMS: 4_000)
        let newer = try install("g++-15")
        let beforeRemoval = RunController(discoverToolchains: { try discovery.discover(path: "") })
        #expect(try beforeRemoval.recoverToolchains([saved], selected: saved.id).profiles.first { $0.id == saved.id } == saved)
        try FileManager.default.removeItem(at: old)
        let restarted = RunController(discoverToolchains: { try discovery.discover(path: "") })
        let restored = try restarted.recoverToolchains([saved], selected: saved.id)
        let resolved = try #require(restored.profiles.first { $0.id == saved.id })
        #expect(resolved.buildExecutable == newer.path)
        #expect(resolved.buildArguments == saved.buildArguments)
        #expect(resolved.buildWallTimeoutMS == 45_000)
        #expect(resolved.runWallTimeoutMS == 4_000)

        let explicit = try saved.updating(buildExecutable: root.appendingPathComponent("g++-14").path, buildArguments: saved.buildArguments,
                                         buildWallTimeoutMS: 45_000, runWallTimeoutMS: 4_000)
        #expect(try restarted.recoverToolchains([explicit], selected: explicit.id).profiles.first { $0.id == explicit.id } == explicit)
        try FileManager.default.removeItem(at: newer)
        let uninstalled = RunController(discoverToolchains: { try discovery.discover(path: "") })
        let unavailable = try uninstalled.recoverToolchains([saved], selected: saved.id)
        uninstalled.profiles = unavailable.profiles
        let session = try NativeEditorSession(document: SourceDocument(text: "", editorKind: .stTextView, languageID: "cpp"), profile: .load(id: "cpp"))
        defer { session.dispose() }
        #expect(!uninstalled.canRun(session))
        #expect(uninstalled.toolchainNotice(for: session) == L10n.string("gnu_cpp_required"))
    }
}
