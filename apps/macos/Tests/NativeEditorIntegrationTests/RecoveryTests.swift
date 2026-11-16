import AppKit
import Darwin
import Foundation
import Testing
import NativeEditorModel
import NativeCoreBridge
@testable import CPEditorNative

@MainActor @Suite(.serialized) struct RecoveryTests {
    private func folder() throws -> URL {
        let value = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: value, withIntermediateDirectories: true)
        return value
    }
    private func preference(_ editor: EditorKind, range: NSRange = .init(location: 0, length: 0)) throws -> NativePreferences {
        let config = try RecoveryConfiguration.load()
        let encoder = JSONEncoder(); encoder.outputFormatting = [.sortedKeys]
        let value = DocumentNativePreferences(editor: editor, selectionLocation: range.location, selectionLength: range.length)
        return NativePreferences(namespaceID: config.documentNamespace, schemaVersion: 1,
                                 objectJSON: String(decoding: try encoder.encode(value), as: UTF8.self))
    }
    private func record(_ id: String, text: String, revision: UInt64 = 4) throws -> SessionDocumentRecord {
        let source = SourceDocument(id: RawIdentifier(id), text: text, editorKind: .stTextView, languageID: "cpp", saved: false, revision: revision)
        return SessionDocumentRecord(snapshot: source.snapshot(), originalPath: nil, languageID: "cpp", toolchainProfileID: nil,
            savePoint: nil, tests: [TestCaseInput(id: "sample/nonUUID", input: "1 2\n", expected: "3\n")],
            editorPreferences: [try preference(.stTextView)])
    }

    @Test func recoveryRoundTripRetainsOpaqueIdentitySavePointProfilesAndExactBuffer() async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let original = directory.appendingPathComponent("原始.cpp")
        let file = directory.appendingPathComponent("workspace.json")
        let originalText = "\u{feff}// 中文🙂e\u{301}\r\n\0\r\n"
        try Data(originalText.utf8).write(to: original)
        let first = Workspace(recoveryURL: file)
        await first.bootstrap()
        let document = SourceDocument(id: RawIdentifier("document/非UUID e\u{301}"), text: originalText, url: original,
            editorKind: .stTextView, languageID: "cpp", revision: 3,
            savePoint: SourceSavePoint(revision: 3, sourceDigest: SourceDigest.sha256(Data(originalText.utf8)), diskDigest: SourceDigest.sha256(Data(originalText.utf8))))
        let session = try NativeEditorSession(document: document, profile: .load(id: "cpp"))
        first.sessions.forEach { $0.dispose() }; first.sessions = [session]; first.selection = document.id
        let opaque = NativePreferences(namespaceID: "future.native", schemaVersion: 99,
            objectJSON: "{ \"opaque\": 18446744073709551616, \"anchor\": [1,2] }")
        session.preservedPreferences = [opaque]
        session.testCases = [TestCaseInput(id: "test/中文", input: "1 2\r\n", expected: "3\r\n")]
        var fields = try #require(first.runner.selectedProfile).payload
        if case .object(var object) = fields { object[ToolchainProfile.Field.id.rawValue] = .string("toolchain/非UUID"); fields = .object(object) }
        let custom = try ToolchainProfile(payload: fields)
        try first.runner.applyProfile(custom.jsonText())
        session.toolchainProfileID = custom.id
        first.wrapText = true
        let native = try #require(session.stView)
        native.textSelection = NSRange(location: originalText.utf16.count, length: 0)
        session.container.history.groupsByEvent = false
        session.container.history.beginUndoGrouping()
        native.insertText("// unsaved 中文\r\n", replacementRange: NSRange(location: NSNotFound, length: 0))
        session.container.history.endUndoGrouping()
        session.capture()
        let snapshot = document.snapshot()
        let selection = session.committedSelection
        #expect(document.isDirty)
        let recovery = try #require(first.recovery)
        #expect(await recovery.flush())
        await recovery.shutdown(); await first.runner.shutdown()
        first.sessions.forEach { $0.dispose() }
        // 恢复正文独立于磁盘源文件；外部的新正文不得覆盖未保存内容。
        try Data("external disk edit\n".utf8).write(to: original)
        let second = Workspace(recoveryURL: file)
        await second.bootstrap()
        let restored = try #require(second.activeSession)
        #expect(restored.document.id == document.id)
        #expect(restored.document.revision == snapshot.revision)
        #expect(Data(restored.document.text.utf8) == snapshot.utf8)
        #expect(restored.document.isDirty)
        #expect(restored.document.savePoint == document.savePoint)
        #expect(restored.committedSelection == selection)
        #expect(restored.testCases == session.testCases)
        #expect(restored.preservedPreferences == [opaque])
        #expect(restored.toolchainProfileID == custom.id)
        #expect(second.runner.selectedProfileID == custom.id)
        #expect(second.wrapText)
        #expect(!restored.container.history.canUndo)
        let restoredNative = try #require(restored.stView)
        restored.container.history.groupsByEvent = false
        restored.container.history.beginUndoGrouping()
        restoredNative.insertText("// later", replacementRange: NSRange(location: NSNotFound, length: 0))
        restored.container.history.endUndoGrouping()
        restored.capture()
        restored.container.history.undo(); restored.capture()
        #expect(restored.document.snapshot().utf8 == snapshot.utf8)
        #expect(await second.recovery?.flush() == true)
        await second.recovery?.shutdown(); await second.runner.shutdown()
        second.sessions.forEach { $0.dispose() }
    }

    @Test func failedNativeCandidateAndMaxRevisionLeaveWholeWorkspaceUntouched() throws {
        _ = NSApplication.shared
        let workspace = Workspace()
        let before = try #require(workspace.activeSession)
        let originalID = workspace.workspaceID
        let documents = [try record("é", text: "// first\n"), try record("e\u{301}", text: "// second\n")]
        let candidate = WorkspaceSessionRecord(workspaceID: RawIdentifier("new/workspace"), revision: 8,
            selectedDocumentID: documents[0].documentID, documents: documents, editorPreferences: [])
        var created = 0
        #expect(throws: CocoaError.self) {
            try workspace.applyRecoveryCandidate(candidate, configuration: .load()) { document, profile in
                created += 1
                if created == 2 { throw CocoaError(.coderInvalidValue) }
                return try NativeEditorSession(document: document, profile: profile)
            }
        }
        #expect(created == 2)
        #expect(workspace.activeSession === before)
        #expect(workspace.workspaceID == originalID)
        let exhausted = try record("exhausted", text: "// original", revision: .max)
        let invalid = WorkspaceSessionRecord(workspaceID: RawIdentifier("new"), revision: 1,
            selectedDocumentID: exhausted.documentID, documents: [exhausted], editorPreferences: [])
        #expect(throws: Error.self) { try workspace.applyRecoveryCandidate(invalid, configuration: .load()) }
        #expect(workspace.activeSession === before)
        workspace.sessions.forEach { $0.dispose() }
    }

    @Test func compositionPersistsOnlyCommittedTextAndSelectionAndBlocksClose() async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let workspace = Workspace(recoveryURL: directory.appendingPathComponent("workspace.json"))
        await workspace.bootstrap()
        workspace.createDocument(text: "// ", editor: .stTextView)
        let session = try #require(workspace.activeSession)
        let native = try #require(session.stView)
        native.textSelection = NSRange(location: 3, length: 0); session.capture()
        let previous = session.committedSelection
        native.setMarkedText("zhong", selectedRange: NSRange(location: 5, length: 0), replacementRange: .init(location: NSNotFound, length: 0))
        session.capture()
        #expect(session.isComposing)
        let records = try workspace.sessionRecord(revision: 1, configuration: .load())
        let record = try #require(records.documents.first { $0.documentID == session.document.id })
        #expect(record.sourceText == "// ")
        let preference = try #require(record.editorPreferences.first)
        let prefs = try JSONDecoder().decode(DocumentNativePreferences.self, from: Data(preference.objectJSON.utf8))
        #expect(prefs.selectionLocation == previous.location)
        #expect(prefs.selectionLength == previous.length)
        #expect(await workspace.recovery?.flush() == true)
        #expect(!(await workspace.prepareToClose()))
        #expect(!workspace.isClosing)
        #expect(!workspace.runner.isClosed)
        native.insertText("中文", replacementRange: .init(location: NSNotFound, length: 0)); session.capture()
        #expect(await workspace.prepareToClose())
        workspace.sessions.forEach { $0.dispose() }
    }

    @Test func sourceConflictRejectsOverwriteAndSaveAsPreservesExactBytes() async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let original = directory.appendingPathComponent("source.cpp")
        let copy = directory.appendingPathComponent("copy.cpp")
        let text = "\u{feff}// 中文🙂\r\n\0\n"
        try Data(text.utf8).write(to: original)
        let workspace = Workspace(arguments: ["--editor", "sttextview"])
        await workspace.open(original)
        let session = try #require(workspace.activeSession)
        let native = try #require(session.stView)
        native.textSelection = NSRange(location: text.utf16.count, length: 0)
        session.container.history.groupsByEvent = false; session.container.history.beginUndoGrouping()
        native.insertText("// modified\r\n", replacementRange: .init(location: NSNotFound, length: 0))
        session.container.history.endUndoGrouping(); session.capture()
        let edited = session.document.snapshot()
        let external = Data("external\n".utf8); try external.write(to: original)
        #expect(!(await workspace.saveSource(session, to: original)))
        #expect(try Data(contentsOf: original) == external)
        #expect(session.document.snapshot() == edited)
        #expect(session.document.isDirty)
        #expect(await workspace.saveSource(session, to: copy, selectedReplacement: true))
        #expect(try Data(contentsOf: copy) == edited.utf8)
        #expect(try Data(contentsOf: original) == external)
        #expect(!session.document.isDirty)
        session.container.history.beginUndoGrouping()
        native.insertText("// another", replacementRange: .init(location: NSNotFound, length: 0))
        session.container.history.endUndoGrouping(); session.capture()
        #expect(session.document.isDirty)
        session.container.history.undo(); session.capture()
        #expect(!session.document.isDirty)
        await workspace.runner.shutdown(); workspace.sessions.forEach { $0.dispose() }
    }

    @Test func corruptRecoveryBackupRestoreAndExportPreserveOriginalFile() async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let file = directory.appendingPathComponent("workspace.json")
        let config = try RecoveryConfiguration.load(); let paths = config.paths(for: file)
        let first = try record("opaque/doc", text: "// previous 中文\n")
        let previous = WorkspaceSessionRecord(workspaceID: RawIdentifier("opaque/workspace"), revision: 1,
            selectedDocumentID: first.documentID, documents: [first], editorPreferences: [])
        let coordinator = CoreCoordinator()
        let saved = try await coordinator.saveSession(previous, paths: paths, expectedDiskDigest: nil, lockTimeoutMS: 0)
        let next = WorkspaceSessionRecord(workspaceID: previous.workspaceID, revision: 2,
            selectedDocumentID: first.documentID, documents: [first], editorPreferences: [])
        _ = try await coordinator.saveSession(next, paths: paths, expectedDiskDigest: saved.diskDigest, lockTimeoutMS: 0)
        await coordinator.shutdown()
        let corrupt = Data("{corrupt-original".utf8); try corrupt.write(to: file)
        let workspace = Workspace(recoveryURL: file)
        await workspace.bootstrap()
        let recovery = try #require(workspace.recovery)
        #expect(recovery.isBlocked)
        #expect(!(await recovery.flush()))
        #expect(try Data(contentsOf: file) == corrupt)
        await recovery.loadBackup()
        #expect(workspace.activeSession?.document.id == first.documentID)
        #expect(workspace.activeSession?.document.text == first.sourceText)
        #expect(recovery.isBlocked)
        let newFile = directory.appendingPathComponent("separate.json")
        #expect(await recovery.saveCopy(to: newFile))
        #expect(!recovery.isBlocked)
        #expect(try Data(contentsOf: file) == corrupt)
        let checking = CoreCoordinator()
        let exported = try await checking.loadSession(at: newFile.path)
        #expect(exported.session?.documents.first?.sourceText == first.sourceText)
        await checking.shutdown(); await recovery.shutdown(); await workspace.runner.shutdown()
        workspace.sessions.forEach { $0.dispose() }
    }

    @Test func exhaustedNativeRevisionReportsBufferDivergenceAndCopiesActualText() throws {
        _ = NSApplication.shared
        let workspace = Workspace()
        let document = SourceDocument(text: "// old", editorKind: .stTextView, languageID: "cpp", revision: .max)
        let session = try NativeEditorSession(document: document, profile: .load(id: "cpp"))
        workspace.sessions.forEach { $0.dispose() }; workspace.sessions = [session]; workspace.selection = document.id
        let native = try #require(session.stView)
        session.capture()
        #expect(!native.isEditable)
        native.text = "// programmatic current buffer"
        session.capture()
        #expect(session.captureFailure == .revisionExhausted)
        #expect(document.text == "// old")
        #expect(throws: DocumentError.revisionExhausted) { try workspace.sessionRecord(revision: 0, configuration: .load()) }
        workspace.openLiveBufferCopy()
        let copy = try #require(workspace.activeSession)
        #expect(copy.document.id != document.id)
        #expect(copy.document.text == "// programmatic current buffer")
        #expect(copy.document.revision == 0)
        workspace.sessions.forEach { $0.dispose() }
    }

    @Test(arguments: EditorKind.allCases) func bothEditorsRestoreExactBytesAndOpaquePreferences(editor: EditorKind) throws {
        _ = NSApplication.shared
        let workspace = Workspace()
        let text = "\u{feff}// 中文🙂e\u{301}\r\n\0\n"
        let source = SourceDocument(id: RawIdentifier("opaque/" + editor.rawValue), text: text, languageID: "cpp", revision: 17)
        let range = NSRange(location: 4, length: 2)
        let config = try RecoveryConfiguration.load()
        let raw = "{ \"integer\": 18446744073709551616, \"é\": 1 }"
        let futureDocument = NativePreferences(namespaceID: "future." + config.documentNamespace, schemaVersion: 99, objectJSON: raw)
        let futureWorkspace = NativePreferences(namespaceID: "future." + config.workspaceNamespace, schemaVersion: 99, objectJSON: raw)
        let document = SessionDocumentRecord(snapshot: source.snapshot(), originalPath: nil, languageID: "cpp", toolchainProfileID: nil,
            savePoint: nil, tests: [], editorPreferences: [try preference(editor, range: range), futureDocument])
        let candidate = WorkspaceSessionRecord(workspaceID: RawIdentifier("opaque/workspace"), revision: 11,
            selectedDocumentID: source.id, documents: [document], editorPreferences: [futureWorkspace])
        try workspace.applyRecoveryCandidate(candidate, configuration: config)
        let restored = try #require(workspace.activeSession)
        try restored.validateRestoredBuffer()
        restored.capture()
        #expect(Data(try restored.currentNativeText().utf8) == Data(text.utf8))
        #expect(restored.document.revision == 17)
        #expect(restored.committedSelection == range)
        let rebuilt = try workspace.sessionRecord(revision: 12, configuration: config)
        #expect(rebuilt.editorPreferences.contains(futureWorkspace))
        #expect(rebuilt.documents[0].editorPreferences.contains(futureDocument))
        #expect(!restored.container.history.canUndo)
        workspace.sessions.forEach { $0.dispose() }
    }

    @Test func recoveryExportInProgressPreventsCloseAndFlushWaitsForExport() async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let workspace = Workspace(recoveryURL: directory.appendingPathComponent("workspace.json"))
        await workspace.bootstrap()
        let recovery = try #require(workspace.recovery)
        #expect(await recovery.flush())
        let copy = directory.appendingPathComponent("copy.json")
        let lockPath = recovery.configuration.paths(for: copy).lockFile
        let descriptor = Darwin.open(lockPath, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR)
        #expect(descriptor >= 0)
        defer { _ = flock(descriptor, LOCK_UN); _ = Darwin.close(descriptor) }
        #expect(flock(descriptor, LOCK_EX | LOCK_NB) == 0)
        let export = Task { await recovery.saveCopy(to: copy) }
        let deadline = ContinuousClock.now + .seconds(2)
        while !recovery.isSaving, ContinuousClock.now < deadline { await Task.yield() }
        #expect(recovery.isChangingLocation)
        #expect(workspace.isBusy)
        #expect(!(await workspace.prepareToClose()))
        #expect(!workspace.runner.isClosed)
        var flushFinished = false
        let flush = Task { let success = await recovery.flush(); flushFinished = true; return success }
        for _ in 0..<10 { await Task.yield() }
        #expect(!flushFinished)
        _ = flock(descriptor, LOCK_UN)
        #expect(await export.value)
        #expect(await flush.value)
        #expect(FileManager.default.fileExists(atPath: copy.path))
        #expect(recovery.paths.recoveryFile == copy.path)
        await recovery.shutdown(); await workspace.runner.shutdown()
        workspace.sessions.forEach { $0.dispose() }
    }

    @Test func futureOwnNamespacesRemainOpaqueWhileSourceAndTestsStillSave() async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let workspace = Workspace()
        let config = try RecoveryConfiguration.load()
        let raw = "{ \"future\": 18446744073709551616, \"editor\": {\"mode\":\"future\"} }"
        let documentPreference = NativePreferences(namespaceID: config.documentNamespace, schemaVersion: 99, objectJSON: raw)
        let workspacePreference = NativePreferences(namespaceID: config.workspaceNamespace, schemaVersion: 99, objectJSON: raw)
        let source = SourceDocument(id: RawIdentifier("opaque/future"), text: "// future 中文\n", languageID: "cpp", revision: 3)
        let document = SessionDocumentRecord(snapshot: source.snapshot(), originalPath: nil, languageID: "cpp", toolchainProfileID: nil,
            savePoint: nil, tests: [TestCaseInput(id: "future/sample", input: "", expected: nil)], editorPreferences: [documentPreference])
        let candidate = WorkspaceSessionRecord(workspaceID: RawIdentifier("opaque/workspace"), revision: 11,
            selectedDocumentID: source.id, documents: [document], editorPreferences: [workspacePreference])
        try workspace.applyRecoveryCandidate(candidate, configuration: config)
        #expect(workspace.preferenceNotice != nil)
        let session = try #require(workspace.activeSession)
        try session.validateRestoredBuffer()
        func find(_ view: NSView) -> NSTextView? {
            if let text = view as? NSTextView, text.isEditable { return text }
            return view.subviews.lazy.compactMap(find).first
        }
        let native = try #require(find(session.container))
        native.setSelectedRange(NSRange(location: source.text.utf16.count, length: 0))
        native.insertText("// current change\n", replacementRange: .init(location: NSNotFound, length: 0))
        await Task.yield(); session.capture()
        session.testCases.append(TestCaseInput(id: "current/sample", input: "1 2", expected: "3"))
        let updated = try workspace.sessionRecord(revision: 12, configuration: config)
        #expect(updated.editorPreferences.filter { $0.namespaceID == config.workspaceNamespace } == [workspacePreference])
        #expect(updated.documents[0].editorPreferences == [documentPreference])
        let coordinator = CoreCoordinator()
        let paths = config.paths(for: directory.appendingPathComponent("future.json"))
        let saved = try await coordinator.saveSession(updated, paths: paths, expectedDiskDigest: nil, lockTimeoutMS: 0)
        #expect(saved.status == .saved)
        let reloaded = try await coordinator.loadSession(at: paths.recoveryFile)
        let restored = try #require(reloaded.session)
        #expect(restored.editorPreferences.filter { $0.namespaceID == config.workspaceNamespace } == [workspacePreference])
        #expect(restored.documents[0].editorPreferences == [documentPreference])
        #expect(restored.documents[0].sourceText.hasSuffix("// current change\n"))
        #expect(restored.documents[0].tests.count == 2)
        await coordinator.shutdown(); await workspace.runner.shutdown()
        workspace.sessions.forEach { $0.dispose() }
    }

    @Test func closeSaveRechecksEditsMadeWhileSourceWriterIsCoordinated() async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let source = directory.appendingPathComponent("source.cpp")
        try Data("// original\n".utf8).write(to: source)
        let workspace = Workspace(arguments: ["--editor", "sttextview"])
        await workspace.open(source)
        let session = try #require(workspace.activeSession)
        let native = try #require(session.stView)
        native.textSelection = NSRange(location: session.document.text.utf16.count, length: 0)
        native.insertText("// saved snapshot\n", replacementRange: .init(location: NSNotFound, length: 0)); session.capture()
        let savedSnapshot = session.document.snapshot()
        let ready = DispatchSemaphore(value: 0)
        let release = DispatchSemaphore(value: 0)
        defer { release.signal() }
        let blocker = Task.detached {
            let coordinator = NSFileCoordinator()
            var failure: NSError?
            coordinator.coordinate(writingItemAt: source, options: .forReplacing, error: &failure) { _ in
                ready.signal(); release.wait()
            }
            return failure?.localizedDescription
        }
        await withCheckedContinuation { continuation in
            DispatchQueue.global().async { ready.wait(); continuation.resume() }
        }
        let closingSave = Task { await workspace.saveForClose(session) }
        let deadline = ContinuousClock.now + .seconds(2)
        while !workspace.savingDocumentIDs.contains(session.document.id), ContinuousClock.now < deadline { await Task.yield() }
        #expect(workspace.savingDocumentIDs.contains(session.document.id))
        native.insertText("// latest edit while saving\n", replacementRange: .init(location: NSNotFound, length: 0)); session.capture()
        release.signal()
        #expect(await blocker.value == nil)
        #expect(!(await closingSave.value))
        #expect(try Data(contentsOf: source) == savedSnapshot.utf8)
        #expect(session.document.text.hasSuffix("// latest edit while saving\n"))
        #expect(session.document.isDirty)
        #expect(workspace.sessions.contains { $0 === session })
        await workspace.runner.shutdown(); workspace.sessions.forEach { $0.dispose() }
    }
}
