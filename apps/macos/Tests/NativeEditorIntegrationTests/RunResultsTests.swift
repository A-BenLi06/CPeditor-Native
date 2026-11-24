import AppKit
import Foundation
import Observation
import Testing
import NativeEditorModel
import NativeCoreBridge
@testable import CPEditorNative

@MainActor @Suite(.serialized) struct RunResultsTests {
    private final class ObservationSignal: @unchecked Sendable {
        private let lock = NSLock()
        private var changed = false
        func signal() { lock.withLock { changed = true } }
        var wasSignalled: Bool { lock.withLock { changed } }
    }
    private let source = "#include <iostream>\nint main(){std::cout << \"A result\\n\";}\n"
    private let waitingSource = "int main(){for(;;){}}\n"

    private func session(id: String, source: String, testcaseID: String) throws -> NativeEditorSession {
        let document = SourceDocument(id: RawIdentifier(id), text: source, editorKind: .stTextView, languageID: "cpp")
        let session = try NativeEditorSession(document: document, profile: .load(id: "cpp"))
        session.testCases = [TestCaseInput(id: testcaseID, expected: "A result\n")]
        return session
    }

    private func allowTimeForCancellation(_ runner: RunController) throws {
        let profile = try #require(runner.selectedProfile)
        let updated = try profile.updating(buildExecutable: profile.buildExecutable, buildArguments: profile.buildArguments,
                                           buildWallTimeoutMS: 30_000, runWallTimeoutMS: 30_000)
        try runner.applyProfile(updated) { _, _ in }
    }

    /// 等待实际 Core Running 事件，不用固定延时猜测编译或启动是否完成。
    private func reachedRunning(_ runner: RunController, documentID: RawIdentifier) async -> Bool {
        let clock = ContinuousClock()
        let deadline = clock.now.advanced(by: .seconds(10))
        while runner.isRunning && runner.state(for: documentID) != .running && clock.now < deadline {
            await Task.yield()
        }
        return runner.isRunning && runner.state(for: documentID) == .running
    }

    @Test func runResultTracksInputsAndOnlyAppliesCurrentOutput() async throws {
        _ = NSApplication.shared
        let session = try session(id: "current-result", source: source, testcaseID: "sample")
        let runner = RunController()
        defer { session.dispose() }
        let test = TestCaseInput(id: "sample", expected: "different\n")
        session.testCases = [test]
        runner.runAll(session)
        await runner.waitForCurrentRun()
        let result = try #require(runner.result(for: session.document.id))
        let output = try #require(result.tests.first?.process.completeTextOutput)
        #expect(result.tests.first?.verdict == "WA")
        #expect(runner.resultIsCurrent(for: session))

        session.testCases[0].input = WireBytes("changed input\n")
        #expect(!runner.resultIsCurrent(for: session))
        runner.useOutputAsExpected(output, for: session, testcaseID: test.id)
        #expect(session.testCases[0].expected == test.expected)
        session.testCases = [test]

        session.testCases[0].expected = nil
        #expect(!runner.resultIsCurrent(for: session))
        session.testCases = [test]
        session.testCases[0].enabled = false
        #expect(!runner.resultIsCurrent(for: session))
        session.testCases = [test, TestCaseInput()]
        #expect(!runner.resultIsCurrent(for: session))
        session.testCases = [test]

        let comparison = runner.comparison
        runner.comparison = .byte
        #expect(!runner.resultIsCurrent(for: session))
        runner.comparison = comparison

        let checker = """
        {"schema_version":1,"command":{"executable":"/usr/bin/true","arguments":[]}}
        """
        try runner.applyChecker(checker, enabled: false)
        #expect(runner.resultIsCurrent(for: session))
        runner.externalCheckerEnabled = true
        #expect(!runner.resultIsCurrent(for: session))
        runner.externalCheckerEnabled = false

        let profile = try #require(runner.selectedProfile)
        let alternative = try #require(runner.profiles.first { $0.id != profile.id })
        runner.selectedProfileID = alternative.id
        #expect(runner.resultIsCurrent(for: session))
        session.toolchainProfileID = alternative.id
        #expect(!runner.resultIsCurrent(for: session))
        session.toolchainProfileID = profile.id
        runner.selectedProfileID = profile.id
        try allowTimeForCancellation(runner)
        #expect(!runner.resultIsCurrent(for: session))
        try runner.applyProfile(profile) { _, _ in }
        #expect(runner.resultIsCurrent(for: session))

        runner.useOutputAsExpected(WireBytes("an older run's output"), for: session, testcaseID: test.id)
        #expect(session.testCases[0].expected == test.expected)
        runner.useOutputAsExpected(output, for: session, testcaseID: test.id)
        #expect(session.testCases[0].expected == output)
        #expect(!runner.resultIsCurrent(for: session))

        runner.runAll(session)
        await runner.waitForCurrentRun()
        #expect(runner.result(for: session.document.id)?.tests.first?.verdict == "AC")
        #expect(runner.resultIsCurrent(for: session))
        try session.document.markSaved(at: URL(fileURLWithPath: "/tmp/answer.cpp"), snapshot: session.document.snapshot())
        #expect(!runner.resultIsCurrent(for: session))
        await runner.shutdown()
    }

    @Test func immediateCancellationIsBoundToTheSubmittedRun() async throws {
        _ = NSApplication.shared
        let session = try session(id: "cancel-before-submit", source: source, testcaseID: "sample")
        let runner = RunController()
        defer { session.dispose() }
        runner.runAll(session)
        runner.cancel()
        await runner.waitForCurrentRun()
        #expect(runner.state(for: session.document.id) == .cancelled)
        #expect(runner.result(for: session.document.id)?.tests.first?.verdict == "Cancelled")
        runner.runAll(session)
        await runner.waitForCurrentRun()
        #expect(runner.result(for: session.document.id)?.tests.first?.verdict == "AC")
        await runner.shutdown()
    }

    private func folder() throws -> URL {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        return directory
    }

    @Test func eachDocumentRetainsItsOwnResultWhenAnotherRunIsCancelled() async throws {
        _ = NSApplication.shared
        let first = try session(id: "run-owner-A", source: source, testcaseID: "sample-A")
        let second = try session(id: "run-owner-B", source: waitingSource, testcaseID: "sample-B")
        let runner = RunController()
        try allowTimeForCancellation(runner)
        defer {
            first.dispose(); second.dispose()
            Task { await runner.shutdown() }
        }
        runner.runAll(first)
        await runner.waitForCurrentRun()
        let original = try #require(runner.result(for: first.document.id))
        #expect(original.documentID == first.document.id)
        #expect(original.tests.first?.testcaseID == "sample-A")
        #expect(original.tests.first?.verdict == "AC")
        #expect(original.tests.first?.process.stdout.text == "A result\n")
        #expect(runner.state(for: first.document.id) == .completed)
        #expect(runner.result(for: second.document.id) == nil)
        #expect(runner.state(for: second.document.id) == nil)

        runner.runAll(second)
        let unrelatedUpdate = ObservationSignal()
        withObservationTracking {
            _ = runner.result(for: first.document.id)
            _ = runner.state(for: first.document.id)
        } onChange: {
            unrelatedUpdate.signal()
        }
        #expect(await reachedRunning(runner, documentID: second.document.id))
        #expect(runner.result(for: first.document.id) == original)
        #expect(runner.state(for: first.document.id) == .completed)
        #expect(runner.result(for: second.document.id) == nil)
        runner.cancel()
        await runner.waitForCurrentRun()
        let cancelled = try #require(runner.result(for: second.document.id))
        #expect(cancelled.documentID == second.document.id)
        #expect(cancelled.tests.first?.testcaseID == "sample-B")
        #expect(cancelled.tests.first?.verdict == "Cancelled")
        #expect(runner.state(for: second.document.id) == .cancelled)
        #expect(runner.result(for: first.document.id) == original)
        #expect(!unrelatedUpdate.wasSignalled)
        #expect(runner.retainedDocumentRunCount == 2)
        await runner.shutdown()
        #expect(runner.retainedDocumentRunCount == 0)
        #expect(runner.result == nil)
        #expect(runner.submitted == nil)
    }

    @Test func closingRunningDocumentDoesNotRecreateItsResultAfterCancellation() async throws {
        _ = NSApplication.shared
        let directory = try folder()
        defer { try? FileManager.default.removeItem(at: directory) }
        let file = directory.appendingPathComponent("waiting.cpp")
        try Data(waitingSource.utf8).write(to: file)
        let workspace = Workspace(arguments: ["--editor", "sttextview"])
        defer {
            workspace.sessions.forEach { $0.dispose() }
            Task { await workspace.runner.shutdown() }
        }
        await workspace.open(file)
        let closing = try #require(workspace.activeSession)
        closing.testCases = [TestCaseInput(id: "closing-test", expected: nil)]
        workspace.runner.runAll(closing)
        #expect(await reachedRunning(workspace.runner, documentID: closing.document.id))
        await workspace.closeActiveDocument()
        #expect(!workspace.sessions.contains { $0 === closing })
        #expect(workspace.runner.retainedDocumentRunCount == 0)
        await workspace.runner.waitForCurrentRun()
        #expect(!workspace.runner.isRunning)
        #expect(workspace.runner.result(for: closing.document.id) == nil)
        #expect(workspace.runner.state(for: closing.document.id) == nil)
        #expect(workspace.runner.retainedDocumentRunCount == 0)
        #expect(try Data(contentsOf: file) == Data(waitingSource.utf8))
        await workspace.runner.shutdown()
    }

    @Test func recoveredDocumentWithSameIDDoesNotAdoptThePreviousSessionRun() async throws {
        _ = NSApplication.shared
        let workspace = Workspace(arguments: ["--editor", "sttextview"])
        let config = try RecoveryConfiguration.load()
        defer {
            workspace.sessions.forEach { $0.dispose() }
            Task { await workspace.runner.shutdown() }
        }
        let previous = try #require(workspace.createDocument(text: waitingSource, editor: .stTextView))
        previous.testCases = [TestCaseInput(id: "previous-test", expected: nil)]
        workspace.runner.runAll(previous)
        #expect(await reachedRunning(workspace.runner, documentID: previous.document.id))

        let replacementSource = SourceDocument(id: previous.document.id, text: source, editorKind: .stTextView, languageID: "cpp")
        let replacement = SessionDocumentRecord(snapshot: replacementSource.snapshot(), originalPath: nil,
            languageID: "cpp", toolchainProfileID: nil, savePoint: nil,
            tests: [TestCaseInput(id: "replacement-test", expected: "A result\n")], editorPreferences: [])
        let record = WorkspaceSessionRecord(workspaceID: workspace.workspaceID, revision: 1,
            selectedDocumentID: replacement.documentID, documents: [replacement], editorPreferences: [])
        try workspace.applyRecoveryCandidate(record, configuration: config)
        let reopened = try #require(workspace.activeSession)
        #expect(reopened !== previous)
        #expect(reopened.document.id == previous.document.id)
        #expect(workspace.runner.retainedDocumentRunCount == 0)
        await workspace.runner.waitForCurrentRun()
        #expect(workspace.runner.result(for: reopened.document.id) == nil)
        #expect(workspace.runner.state(for: reopened.document.id) == nil)
        #expect(workspace.runner.retainedDocumentRunCount == 0)

        workspace.runner.runAll(reopened)
        await workspace.runner.waitForCurrentRun()
        let result = try #require(workspace.runner.result(for: reopened.document.id))
        #expect(result.tests.first?.testcaseID == "replacement-test")
        #expect(result.tests.first?.verdict == "AC")
        #expect(result.matches(reopened.document.snapshot()))
        #expect(workspace.runner.retainedDocumentRunCount == 1)
        await workspace.runner.shutdown()
        #expect(workspace.runner.retainedDocumentRunCount == 0)
    }

    @Test func failedDocumentCloseKeepsItsResultUntilActualRemoval() async throws {
        _ = NSApplication.shared
        let directory = try folder()
        defer { try? FileManager.default.removeItem(at: directory) }
        let file = directory.appendingPathComponent("answer.cpp")
        try Data(source.utf8).write(to: file)
        let workspace = Workspace(arguments: ["--editor", "sttextview"], recoveryURL: directory.appendingPathComponent("workspace.json"))
        await workspace.bootstrap()
        defer {
            workspace.sessions.forEach { $0.dispose() }
            Task { await workspace.recovery?.shutdown(); await workspace.runner.shutdown() }
        }
        await workspace.open(file)
        let session = try #require(workspace.activeSession)
        session.testCases = [TestCaseInput(id: "retained-test", expected: "A result\n")]
        #expect(await workspace.recovery?.flush() == true)
        workspace.runner.runAll(session)
        await workspace.runner.waitForCurrentRun()
        let original = try #require(workspace.runner.result(for: session.document.id))
        #expect(original.tests.first?.verdict == "AC")
        let external = Data("externally damaged recovery; must preserve".utf8)
        let recovery = try #require(workspace.recovery)
        try external.write(to: URL(fileURLWithPath: recovery.paths.recoveryFile))
        await workspace.closeActiveDocument()
        #expect(workspace.activeSession === session)
        #expect(workspace.sessions.contains { $0 === session })
        #expect(workspace.runner.result(for: session.document.id) == original)
        #expect(workspace.runner.state(for: session.document.id) == .completed)
        #expect(workspace.runner.retainedDocumentRunCount == 1)
        #expect(try Data(contentsOf: URL(fileURLWithPath: recovery.paths.recoveryFile)) == external)
        await recovery.shutdown(); await workspace.runner.shutdown()
        #expect(workspace.runner.retainedDocumentRunCount == 0)
    }

    @Test func compileAndRunOnlySeparationTracksArtifactsEditsAndCleanup() async throws {
        _ = NSApplication.shared
        let session = try session(id: "split-compile-run", source: source, testcaseID: "sample-1")
        let runner = RunController()
        defer {
            session.dispose()
            Task { await runner.shutdown() }
        }

        // Run Without Compiling before building is rejected cleanly.
        #expect(runner.supportsCompile(for: session))
        #expect(runner.canCompile(session))
        #expect(!runner.hasReusableArtifact(for: session))
        #expect(!runner.canRunOnly(session))
        runner.runOnly(session)
        #expect(!runner.isRunning)
        #expect(runner.errorMessage == L10n.string("compile_before_run"))
        runner.errorMessage = nil

        // Compile succeeds even with zero test cases and produces a reusable artifact.
        session.testCases = []
        runner.compile(session)
        await runner.waitForCurrentRun()
        let compileResult = try #require(runner.result(for: session.document.id))
        #expect(compileResult.buildOutcome == "Succeeded")
        #expect(compileResult.build != nil)
        #expect(compileResult.tests.isEmpty)
        #expect(VerdictDescriptor.summary(for: compileResult).label == L10n.string("verdict_compiled"))
        #expect(runner.resultIsCurrent(for: session))
        #expect(runner.hasReusableArtifact(for: session))
        #expect(runner.isArtifactCurrent(for: session))
        #expect(runner.canRunOnly(session))
        let compiledArtifact = try #require(runner.artifactURL(for: session.document.id))
        #expect(FileManager.default.isExecutableFile(atPath: compiledArtifact.path))

        // Adding/editing test cases and calling Run Only executes without recompiling and stays current.
        session.testCases = [TestCaseInput(id: "run-only-1", input: "", expected: "A result\n")]
        #expect(runner.resultIsCurrent(for: session))
        runner.runOnly(session)
        await runner.waitForCurrentRun()
        let runOnlyResult = try #require(runner.result(for: session.document.id))
        #expect(runOnlyResult.buildOutcome == "NotRequired")
        #expect(runOnlyResult.build == nil)
        #expect(runOnlyResult.tests.first?.verdict == "AC")
        #expect(VerdictDescriptor.summary(for: runOnlyResult).label == "AC")
        #expect(runner.resultIsCurrent(for: session))

        // Editing source keeps Run Only usable against the previous build, marks result outdated, and blocks Use Output.
        let stView = try #require(session.stView)
        let currentLength = (stView.text ?? "").utf16.count
        stView.textSelection = NSRange(location: 0, length: currentLength)
        stView.insertText("#include <iostream>\nint main(){std::cout << \"Updated\\n\";}\n",
                          replacementRange: NSRange(location: 0, length: currentLength))
        session.capture()
        #expect(runner.hasReusableArtifact(for: session))
        #expect(!runner.isArtifactCurrent(for: session))
        #expect(runner.canRunOnly(session))

        session.testCases[0].expected = WireBytes("different\n")
        runner.runOnly(session)
        await runner.waitForCurrentRun()
        let staleRunResult = try #require(runner.result(for: session.document.id))
        #expect(staleRunResult.buildOutcome == "NotRequired")
        #expect(staleRunResult.tests.first?.verdict == "WA")
        #expect(staleRunResult.tests.first?.process.completeTextOutput == WireBytes("A result\n"))
        #expect(!runner.resultIsCurrent(for: session))
        runner.useOutputAsExpected(WireBytes("A result\n"), for: session, testcaseID: session.testCases[0].id)
        #expect(session.testCases[0].expected == WireBytes("different\n"))

        // Compile & Run updates the reusable artifact to the new source; immediate Run Only uses it.
        session.testCases[0].expected = WireBytes("Updated\n")
        runner.runAll(session)
        await runner.waitForCurrentRun()
        let compileAndRunResult = try #require(runner.result(for: session.document.id))
        #expect(compileAndRunResult.buildOutcome == "Succeeded")
        #expect(compileAndRunResult.tests.first?.verdict == "AC")
        #expect(runner.isArtifactCurrent(for: session))
        let refreshedArtifact = try #require(runner.artifactURL(for: session.document.id))
        #expect(refreshedArtifact != compiledArtifact)
        #expect(!FileManager.default.fileExists(atPath: compiledArtifact.path))
        #expect(FileManager.default.isExecutableFile(atPath: refreshedArtifact.path))

        runner.runOnly(session)
        await runner.waitForCurrentRun()
        let postCompileRunOnly = try #require(runner.result(for: session.document.id))
        #expect(postCompileRunOnly.buildOutcome == "NotRequired")
        #expect(postCompileRunOnly.tests.first?.verdict == "AC")
        #expect(runner.resultIsCurrent(for: session))

        // forget() deletes the document's artifact; shutdown() cleans up remaining staging directories.
        runner.forget(documentID: session.document.id)
        #expect(runner.artifactURL(for: session.document.id) == nil)
        #expect(!FileManager.default.fileExists(atPath: refreshedArtifact.path))

        let second = try self.session(id: "split-cleanup", source: source, testcaseID: "sample-2")
        defer { second.dispose() }
        runner.compile(second)
        await runner.waitForCurrentRun()
        let secondArtifact = try #require(runner.artifactURL(for: second.document.id))
        #expect(FileManager.default.fileExists(atPath: secondArtifact.path))
        await runner.shutdown()
        #expect(!FileManager.default.fileExists(atPath: secondArtifact.path))
    }
}
