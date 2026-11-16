import Foundation
import Darwin
import Testing
import NativeEditorModel
@testable import NativeCoreBridge

@Suite(.serialized) struct SessionTests {
    private func folder() throws -> URL {
        let value = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: value, withIntermediateDirectories: true)
        return value
    }
    private func paths(_ folder: URL) -> SessionPaths {
        SessionPaths(recoveryFile: folder.appendingPathComponent("session.json").path,
            backupFile: folder.appendingPathComponent("previous.json").path,
            lockFile: folder.appendingPathComponent("session.lock").path)
    }
    @MainActor private func record(revision: UInt64 = 7) -> WorkspaceSessionRecord {
        let source = SourceDocument(id: RawIdentifier("document/中文 e\u{301}"), text: "\u{feff}// 中文🙂\r\n\0\n",
                                    languageID: "cpp", saved: false, revision: revision)
        let opaque = NativePreferences(namespaceID: "future.native.editor", schemaVersion: 99,
            objectJSON: "{ \"opaque\": 18446744073709551616, \"anchor\": [1,2] }")
        let document = SessionDocumentRecord(snapshot: source.snapshot(), originalPath: "/tmp/source-separate.cpp", languageID: "cpp",
            toolchainProfileID: RawIdentifier("toolchain/非UUID"),
            savePoint: SourceSavePoint(revision: 2, sourceDigest: SourceDigest.sha256(Data("old".utf8)), diskDigest: SourceDigest.sha256(Data("old-on-disk".utf8))),
            tests: [TestCaseInput(id: "test/样例", input: "中\0\r\n", expected: nil)], editorPreferences: [opaque])
        return WorkspaceSessionRecord(workspaceID: RawIdentifier("workspace/非UUID"), revision: revision,
            selectedDocumentID: document.documentID, documents: [document], editorPreferences: [opaque])
    }

    @Test func actualABISessionPreservesBytesIDsAndUnknownPreferences() async throws {
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let locations = paths(directory)
        let coordinator = CoreCoordinator(maximumBuffers: 1)
        let missing = try await coordinator.loadSession(at: locations.recoveryFile)
        #expect(missing.status == .missing)
        let original = await record()
        let saved = try await coordinator.saveSession(original, paths: locations, expectedDiskDigest: nil, lockTimeoutMS: 0)
        #expect(saved.status == .saved)
        #expect(saved.recoveryReplaced)
        let loaded = try await coordinator.loadSession(at: locations.recoveryFile)
        #expect(loaded.status == .loaded)
        let session = try #require(loaded.session)
        #expect(session.workspaceID == original.workspaceID)
        #expect(session.documents[0].documentID == original.documents[0].documentID)
        #expect(Data(session.documents[0].sourceText.utf8) == Data(original.documents[0].sourceText.utf8))
        #expect(session.documents[0].tests == original.documents[0].tests)
        #expect(session.documents[0].savePoint == original.documents[0].savePoint)
        #expect(session.documents[0].editorPreferences == original.documents[0].editorPreferences)
        #expect(session.documents[0].toolchainProfileID == original.documents[0].toolchainProfileID)
        let previousBytes = try Data(contentsOf: URL(fileURLWithPath: locations.recoveryFile))
        let updated = await record(revision: 8)
        let second = try await coordinator.saveSession(updated, paths: locations, expectedDiskDigest: saved.diskDigest, lockTimeoutMS: 0)
        #expect(second.status == .saved)
        #expect(try Data(contentsOf: URL(fileURLWithPath: locations.backupFile)) == previousBytes)
        let conflict = try await coordinator.saveSession(original, paths: locations, expectedDiskDigest: saved.diskDigest, lockTimeoutMS: 0)
        #expect(conflict.status == .conflict)
        #expect(!conflict.recoveryReplaced)
        await coordinator.shutdown()
    }

    @Test func invalidAndFutureSessionFilesAreNeverOverwritten() async throws {
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let locations = paths(directory)
        let coordinator = CoreCoordinator(maximumBuffers: 1)
        let candidate = await record()
        for (bytes, status) in [(Data("{broken".utf8), SessionLoadStatus.invalid),
                                (Data("{\"schema_version\":2}".utf8), SessionLoadStatus.unsupportedSchema)] {
            try bytes.write(to: URL(fileURLWithPath: locations.recoveryFile))
            let loaded = try await coordinator.loadSession(at: locations.recoveryFile)
            #expect(loaded.status == status)
            #expect(loaded.session == nil)
            let result = try await coordinator.saveSession(candidate, paths: locations, expectedDiskDigest: loaded.diskDigest, lockTimeoutMS: 0)
            #expect(result.status == .invalidExisting)
            #expect(!result.recoveryReplaced)
            #expect(try Data(contentsOf: URL(fileURLWithPath: locations.recoveryFile)) == bytes)
        }
        await coordinator.shutdown()
    }

    @Test func externalLockCancellationPreservesTokenAndAllowsFreshCoordinator() async throws {
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let locations = paths(directory)
        let coordinator = CoreCoordinator()
        let initial = await record()
        let saved = try await coordinator.saveSession(initial, paths: locations, expectedDiskDigest: nil, lockTimeoutMS: 0)
        let initialBytes = try Data(contentsOf: URL(fileURLWithPath: locations.recoveryFile))
        let descriptor = Darwin.open(locations.lockFile, O_RDWR)
        #expect(descriptor >= 0)
        defer { _ = flock(descriptor, LOCK_UN); _ = Darwin.close(descriptor) }
        #expect(flock(descriptor, LOCK_EX | LOCK_NB) == 0)
        let changed = await record(revision: 8)
        let operation = Task { try await coordinator.saveSession(changed, paths: locations,
            expectedDiskDigest: saved.diskDigest, lockTimeoutMS: 5000) }
        // 外部 flock 已确实持有；此等待仅让测试调用进入 Core 的有界锁等待。
        try await Task.sleep(for: .milliseconds(100))
        coordinator.requestCancellation()
        let cancelled = try await operation.value
        #expect(cancelled.status == .cancelled)
        #expect(!cancelled.recoveryReplaced)
        #expect(try Data(contentsOf: URL(fileURLWithPath: locations.recoveryFile)) == initialBytes)
        await coordinator.shutdown()
        _ = flock(descriptor, LOCK_UN)
        let fresh = CoreCoordinator()
        let retried = try await fresh.saveSession(changed, paths: locations, expectedDiskDigest: saved.diskDigest, lockTimeoutMS: 0)
        #expect(retried.status == .saved)
        await fresh.shutdown()
    }

    @Test func sessionSaveReportsRealIOFailureWithoutReplacingData() async throws {
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let missingParent = directory.appendingPathComponent("does-not-exist", isDirectory: true)
        let coordinator = CoreCoordinator()
        let candidate = await record()
        let result = try await coordinator.saveSession(candidate, paths: paths(missingParent), expectedDiskDigest: nil, lockTimeoutMS: 0)
        #expect(result.status == .ioFailed)
        #expect(!result.recoveryReplaced)
        #expect(!result.detail.isEmpty)
        #expect(!FileManager.default.fileExists(atPath: paths(missingParent).recoveryFile))
        await coordinator.shutdown()
    }

    @Test func cancellationBeforeEngineCreationCannotWriteRecovery() async throws {
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let coordinator = CoreCoordinator()
        coordinator.requestCancellation()
        let candidate = await record()
        do {
            _ = try await coordinator.saveSession(candidate, paths: paths(directory), expectedDiskDigest: nil, lockTimeoutMS: 0)
            Issue.record("A queued cancellation must prevent recovery writes")
        } catch { #expect(error is CoreFailure) }
        #expect(!FileManager.default.fileExists(atPath: paths(directory).recoveryFile))
        await coordinator.shutdown()
    }
}
