import AppKit
import SwiftUI
import Foundation
import Testing
import NativeEditorModel
import NativeCoreBridge
@testable import CPEditorNative

@MainActor @Suite(.serialized) struct NativeCheckerTests {
    private func configuration(mode: String = "exit", exit: String = "7") throws -> String {
        let macos = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        let library = ProcessInfo.processInfo.environment["CPN_CORE_LIBRARY_DIR"] ?? macos.appendingPathComponent("../../out/core/dev").standardized.path
        let helper = URL(fileURLWithPath: library).appendingPathComponent("cpn_checker_helper")
        let object: [String: Any] = ["schema_version": 1, "command": ["executable": helper.path,
            "arguments": [mode, "${input}", "${actual}", "${expected}", exit]]]
        return String(decoding: try JSONSerialization.data(withJSONObject: object, options: [.prettyPrinted, .sortedKeys]), as: UTF8.self)
    }
    private func folder() throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    @Test func runnerUsesConfiguredCheckerAndKeepsSolverStatusIndependent() async throws {
        _ = NSApplication.shared
        let source = SourceDocument(text: "int main(){return 0;}\n", editorKind: .stTextView, languageID: "cpp", saved: false)
        let session = try NativeEditorSession(document: source, profile: .load(id: "cpp"))
        defer { session.dispose() }
        session.testCases = [TestCaseInput(id: "no-answer", expected: nil), TestCaseInput(id: "empty-answer", expected: "")]
        let runner = RunController()
        runner.externalCheckerEnabled = true
        runner.runAll(session)
        #expect(!runner.isRunning)
        #expect(runner.errorMessage == L10n.string("checker_configuration_required"))
        let original = try configuration()
        try runner.applyChecker(original)
        let previous = runner.externalChecker
        #expect(throws: CheckerConfigurationError.self) { try runner.applyChecker("{\"schema_version\":1.0}") }
        #expect(throws: CheckerConfigurationError.self) { try runner.applyChecker(original + String(repeating: " ", count: 65536)) }
        #expect(runner.externalChecker == previous)
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 900, height: 500), styleMask: [.titled], backing: .buffered, defer: false)
        let host = NSHostingView(rootView: RunPanelView(session: session, runner: runner, canConfigureRunner: true))
        window.contentView = host
        defer { window.contentView = nil; window.orderOut(nil) }
        runner.runAll(session)
        await runner.waitForCurrentRun()
        host.layoutSubtreeIfNeeded()
        let result = try #require(runner.result)
        #expect(result.tests[0].verdict == "Unchecked")
        #expect(result.tests[0].checker == nil)
        #expect(result.tests[1].verdict == "CheckerError")
        #expect(result.tests[1].process.exitCode == 0)
        #expect(result.tests[1].checker?.process.exitCode == 7)
        #expect(result.tests[1].checker?.process.stdout.data == Data("0.5\n".utf8))
        #expect(runner.externalChecker?.originalJSON == original)
        try runner.applyChecker(configuration(mode: "compare", exit: ""))
        runner.runAll(session); await runner.waitForCurrentRun()
        #expect(runner.result?.tests[1].verdict == "AC")
        #expect(runner.result?.tests[1].checker?.verdict == "Accepted")
        #expect(runner.result?.tests[1].checker?.processAttempted == true)
        await runner.shutdown()
    }

    @Test func independentCheckerPreferencesRestoreExactConfigurationAndEnabledState() async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let file = directory.appendingPathComponent("checker-workspace.json")
        let workspace = Workspace(recoveryURL: file)
        await workspace.bootstrap()
        let original = try configuration(mode: "compare", exit: "")
        try workspace.runner.applyChecker(original)
        #expect(await workspace.recovery?.flush() == true)
        await workspace.recovery?.shutdown(); await workspace.runner.shutdown()
        workspace.sessions.forEach { $0.dispose() }
        let restored = Workspace(recoveryURL: file)
        await restored.bootstrap()
        #expect(restored.runner.externalCheckerEnabled)
        #expect(restored.runner.externalChecker?.originalJSON.utf8.elementsEqual(original.utf8) == true)
        #expect(!restored.runner.checkerPreferencesAreOpaque)
        restored.runner.externalCheckerEnabled = false
        #expect(await restored.recovery?.flush() == true)
        await restored.recovery?.shutdown(); await restored.runner.shutdown()
        restored.sessions.forEach { $0.dispose() }
        let disabled = Workspace(recoveryURL: file)
        await disabled.bootstrap()
        #expect(!disabled.runner.externalCheckerEnabled)
        #expect(disabled.runner.externalChecker?.originalJSON.utf8.elementsEqual(original.utf8) == true)
        await disabled.recovery?.shutdown(); await disabled.runner.shutdown()
        disabled.sessions.forEach { $0.dispose() }
    }

    @Test func futureCheckerPreferencesRemainOpaqueAndMalformedCurrentCandidateIsAtomic() async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let workspace = Workspace()
        let config = try RecoveryConfiguration.load()
        let source = SourceDocument(id: RawIdentifier("future/opaque"), text: "// future checker\n", languageID: "cpp")
        let document = SessionDocumentRecord(snapshot: source.snapshot(), originalPath: nil, languageID: "cpp", toolchainProfileID: nil,
            savePoint: nil, tests: [TestCaseInput(id: "future-test")], editorPreferences: [])
        let raw = "{ \"enabled\": true, \"future\": 18446744073709551616 }"
        let future = NativePreferences(namespaceID: config.checkerNamespace, schemaVersion: 99, objectJSON: raw)
        let older = CheckerNativePreferences(enabled: true, configurationJSON: "invalid but opaque in the presence of a future owner")
        let compatible = NativePreferences(namespaceID: config.checkerNamespace, schemaVersion: 1,
            objectJSON: String(decoding: try JSONEncoder().encode(older), as: UTF8.self))
        let candidate = WorkspaceSessionRecord(workspaceID: RawIdentifier("future/workspace"), revision: 1,
            selectedDocumentID: source.id, documents: [document], editorPreferences: [compatible, future])
        try workspace.applyRecoveryCandidate(candidate, configuration: config)
        #expect(workspace.runner.checkerPreferencesAreOpaque)
        #expect(!workspace.runner.externalCheckerEnabled)
        #expect(workspace.runner.externalChecker == nil)
        #expect(workspace.preferenceNotice != nil)
        #expect(throws: WireError.unsupportedSchema) { try workspace.runner.applyChecker(configuration()) }
        let record = try workspace.sessionRecord(revision: 2, configuration: config)
        #expect(record.editorPreferences.filter { $0.namespaceID == config.checkerNamespace } == [compatible, future])
        let coordinator = CoreCoordinator()
        let paths = config.paths(for: directory.appendingPathComponent("future.json"))
        let saved = try await coordinator.saveSession(record, paths: paths, expectedDiskDigest: nil, lockTimeoutMS: 0)
        #expect(saved.status == .saved)
        let loaded = try await coordinator.loadSession(at: paths.recoveryFile)
        #expect(loaded.session?.editorPreferences.filter { $0.namespaceID == config.checkerNamespace } == [compatible, future])
        await coordinator.shutdown()
        let previous = workspace.activeSession
        let malformed = CheckerNativePreferences(enabled: true, configurationJSON: "{\"schema_version\":1.0}")
        let preference = NativePreferences(namespaceID: config.checkerNamespace, schemaVersion: 1,
            objectJSON: String(decoding: try JSONEncoder().encode(malformed), as: UTF8.self))
        let invalid = WorkspaceSessionRecord(workspaceID: RawIdentifier("invalid"), revision: 3,
            selectedDocumentID: source.id, documents: [document], editorPreferences: [preference])
        #expect(throws: CheckerConfigurationError.self) { try workspace.applyRecoveryCandidate(invalid, configuration: config) }
        #expect(workspace.activeSession === previous)
        #expect(workspace.runner.checkerPreferencesAreOpaque)
        await workspace.runner.shutdown(); workspace.sessions.forEach { $0.dispose() }
    }

    @Test(arguments: [false, true]) func fullOwnerPreferencesPreserveForeignDataAndRejectNewChecker(hasWorkspacePreference: Bool) async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let workspace = Workspace()
        let config = try RecoveryConfiguration.load()
        let initial = try workspace.sessionRecord(revision: 1, configuration: config)
        let own = try #require(initial.editorPreferences.first { $0.namespaceID == config.workspaceNamespace })
        let foreignCount = config.maxPreferencesPerOwner - (hasWorkspacePreference ? 1 : 0)
        let foreign = (0..<foreignCount).map { NativePreferences(namespaceID: "foreign.\($0)", schemaVersion: 99, objectJSON: "{ \"index\": \($0) }") }
        let preferences = foreign + (hasWorkspacePreference ? [own] : [])
        let candidate = WorkspaceSessionRecord(workspaceID: RawIdentifier("full/owner"), revision: 2,
            selectedDocumentID: initial.selectedDocumentID, documents: initial.documents, editorPreferences: preferences)
        try workspace.applyRecoveryCandidate(candidate, configuration: config)
        #expect(!workspace.runner.checkerPreferenceStorageAvailable)
        #expect(throws: CheckerConfigurationError.self) { try workspace.runner.applyChecker(configuration()) }
        #expect(workspace.runner.externalChecker == nil)
        #expect(!workspace.runner.externalCheckerEnabled)
        workspace.createDocument(text: "// source preserved\n", editor: .stTextView)
        let session = try #require(workspace.activeSession)
        let native = try #require(session.stView)
        native.textSelection = NSRange(location: session.document.text.utf16.count, length: 0)
        native.insertText("// latest 中文\n", replacementRange: .init(location: NSNotFound, length: 0)); session.capture()
        session.testCases.append(TestCaseInput(id: "new-sample", input: "1 2", expected: "3"))
        let record = try workspace.sessionRecord(revision: 3, configuration: config)
        #expect(record.editorPreferences.count == config.maxPreferencesPerOwner)
        #expect(record.editorPreferences.filter { $0.namespaceID.hasPrefix("foreign.") } == foreign)
        #expect(record.editorPreferences.filter { $0.namespaceID == config.checkerNamespace }.isEmpty)
        let coordinator = CoreCoordinator()
        let paths = config.paths(for: directory.appendingPathComponent("full.json"))
        let saved = try await coordinator.saveSession(record, paths: paths, expectedDiskDigest: nil, lockTimeoutMS: 0)
        #expect(saved.status == .saved)
        let loaded = try await coordinator.loadSession(at: paths.recoveryFile)
        #expect(loaded.session?.editorPreferences.filter { $0.namespaceID.hasPrefix("foreign.") } == foreign)
        #expect(loaded.session?.documents.last?.sourceText.hasSuffix("// latest 中文\n") == true)
        #expect(loaded.session?.documents.last?.tests.count == 2)
        await coordinator.shutdown(); await workspace.runner.shutdown()
        workspace.sessions.forEach { $0.dispose() }
    }
}
