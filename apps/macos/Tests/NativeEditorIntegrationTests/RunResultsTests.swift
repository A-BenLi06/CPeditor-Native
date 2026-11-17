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

    /// 等待实际 Core Running 事件，不用固定延时猜测编译或启动是否完成。
    private func reachedRunning(_ runner: RunController, documentID: RawIdentifier) async -> Bool {
        let clock = ContinuousClock()
        let deadline = clock.now.advanced(by: .seconds(10))
        while runner.isRunning && runner.state(for: documentID) != .running && clock.now < deadline {
            await Task.yield()
        }
        return runner.isRunning && runner.state(for: documentID) == .running
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
}
