import AppKit
import Darwin
import Foundation
import Testing
import SwiftUI
import NativeEditorModel
import NativeCoreBridge
@testable import CPEditorNative

@MainActor @Suite(.serialized) struct RevisionPersistenceTests {
    private func folder() throws -> URL {
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        return folder
    }
    private func seed(_ directory: URL, workspaceRevision: UInt64, documentRevision: UInt64 = 7,
                      editor: EditorKind = .stTextView, original: URL? = nil, extraPreferences: [NativePreferences] = [],
                      text: String = "// 中文🙂e\u{301}\r\n\0\n") async throws -> (URL, WorkspaceSessionRecord) {
        let recovery = directory.appendingPathComponent("workspace.json")
        let digest = SourceDigest.sha256(Data(text.utf8))
        let point = original.map { _ in SourceSavePoint(revision: documentRevision, sourceDigest: digest, diskDigest: digest) }
        let document = SourceDocument(id: RawIdentifier("revision/document"), text: text, url: original, editorKind: editor, languageID: "cpp", revision: documentRevision, savePoint: point)
        let config = try RecoveryConfiguration.load()
        let own = NativePreferences(namespaceID: config.documentNamespace, schemaVersion: 1,
            objectJSON: String(decoding: try JSONEncoder().encode(DocumentNativePreferences(editor: editor, selectionLocation: 0, selectionLength: 0)), as: UTF8.self))
        let opaque = NativePreferences(namespaceID: "future/revision-test", schemaVersion: 99, objectJSON: "{ \"large\": 18446744073709551616, \"exact\": \"e\\u0301\" }")
        let record = SessionDocumentRecord(snapshot: document.snapshot(), originalPath: original?.path, languageID: "cpp", toolchainProfileID: nil,
            savePoint: document.savePoint, tests: [TestCaseInput(id: "stable/sample", input: "中\0\r\n", expected: "")], editorPreferences: [own, opaque])
        let session = WorkspaceSessionRecord(workspaceID: RawIdentifier("revision/workspace"), revision: workspaceRevision,
            selectedDocumentID: record.documentID, documents: [record], editorPreferences: [opaque] + extraPreferences)
        let core = CoreCoordinator()
        let saved = try await core.saveSession(session, paths: config.paths(for: recovery), expectedDiskDigest: nil, lockTimeoutMS: 0)
        #expect(saved.status == .saved); await core.shutdown()
        return (recovery, session)
    }
    private func dispose(_ workspace: Workspace) async {
        await workspace.companion.shutdown(); await workspace.recovery?.shutdown(); await workspace.runner.shutdown()
        workspace.sessions.forEach { $0.dispose() }
    }
    private func waitUntil(_ predicate: () -> Bool) async {
        let deadline = ContinuousClock.now + .seconds(3)
        while !predicate(), ContinuousClock.now < deadline { await Task.yield() }
        #expect(predicate())
    }
    private func bytes(_ session: WorkspaceSessionRecord) throws -> Data {
        let encoder = JSONEncoder(); encoder.outputFormatting = [.sortedKeys]
        return try encoder.encode(session)
    }

    @Test(arguments: EditorKind.allCases) func maxWorkspaceRestoresExactRecordReadOnlyAndClosesWithoutAnotherRevision(editor: EditorKind) async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let (file, record) = try await seed(directory, workspaceRevision: .max, editor: editor)
        let originalBytes = try Data(contentsOf: file)
        let workspace = Workspace(recoveryURL: file); await workspace.bootstrap()
        let recovery = try #require(workspace.recovery); let native = try #require(workspace.activeSession)
        #expect(!recovery.isBlocked); #expect(workspace.persistence.isFrozen)
        #expect(native.document.text == record.documents[0].sourceText)
        #expect(native.testCases == record.documents[0].tests)
        #expect(!native.interactionEnabled)
        let before = try workspace.sessionRecord(revision: 0, configuration: .load())
        #expect(try bytes(before) == bytes(record))
        #expect(throws: DocumentError.revisionExhausted) { try native.document.commit("// forbidden source") }
        let tests = native.testCases; native.testCases.append(TestCaseInput()); #expect(native.testCases == tests)
        let profiles = workspace.runner.profiles; workspace.runner.profiles = []; #expect(workspace.runner.profiles == profiles)
        let wrap = workspace.wrapText; workspace.wrapText.toggle(); #expect(workspace.wrapText == wrap)
        #expect(workspace.createDocument(text: "// cannot spend another revision") == nil)
        #expect(await recovery.flush())
        #expect(try Data(contentsOf: file) == originalBytes)
        #expect(await workspace.prepareToClose())
        #expect(try Data(contentsOf: file) == originalBytes)
        native.dispose()
    }

    @Test func maxDocumentRestoresSourceAndMetadataReadOnlyWithinMutableWorkspace() async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let (file, record) = try await seed(directory, workspaceRevision: 4, documentRevision: .max)
        let workspace = Workspace(recoveryURL: file); await workspace.bootstrap()
        let session = try #require(workspace.activeSession)
        #expect(workspace.persistence.canMutate); #expect(!session.canMutatePersistence)
        #expect(session.document.revision == .max); #expect(session.testCases == record.documents[0].tests)
        #expect(throws: DocumentError.revisionExhausted) { try session.document.commit("// impossible") }
        let preferences = session.preservedPreferences
        session.preservedPreferences.append(NativePreferences(namespaceID: "new", schemaVersion: 1, objectJSON: "{}"))
        #expect(session.preservedPreferences == preferences)
        #expect(await workspace.recovery!.flush())
        let persisted = try workspace.sessionRecord(revision: 5, configuration: .load())
        let encoder = JSONEncoder(); encoder.outputFormatting = [.sortedKeys]
        #expect(try encoder.encode(persisted.documents[0]) == encoder.encode(record.documents[0]))
        #expect(!session.container.allowsEditingCommands)
        await dispose(workspace)
    }

    @Test func finalSaveFreezesAllPersistentInputsAndPreReplacementFailureUnfreezesForNewText() async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let (file, _) = try await seed(directory, workspaceRevision: .max - 1)
        let workspace = Workspace(recoveryURL: file); await workspace.bootstrap()
        let recovery = try #require(workspace.recovery); let session = try #require(workspace.activeSession); let native = try #require(session.stView)
        #expect(await recovery.flush())
        let descriptor = Darwin.open(recovery.paths.lockFile, O_RDWR)
        #expect(descriptor >= 0); defer { _ = flock(descriptor, LOCK_UN); _ = Darwin.close(descriptor) }
        #expect(flock(descriptor, LOCK_EX | LOCK_NB) == 0)
        native.insertText("// B final attempt\n", replacementRange: .init(location: NSNotFound, length: 0)); session.capture()
        let saving = Task { await recovery.flush() }
        await waitUntil { recovery.isSaving }
        #expect(workspace.persistence.isFinalizing); #expect(!native.isEditable)
        let tests = session.testCases; session.testCases.append(TestCaseInput()); #expect(session.testCases == tests)
        #expect(throws: DocumentError.revisionExhausted) { try session.document.commit("// C must not be accepted during final snapshot") }
        try Data("{external invalid".utf8).write(to: file)
        _ = flock(descriptor, LOCK_UN)
        #expect(!(await saving.value))
        #expect(workspace.persistence.canMutate); #expect(native.isEditable)
        native.insertText("// C after failed replacement\n", replacementRange: .init(location: NSNotFound, length: 0)); session.capture()
        #expect(session.document.text.contains("C after failed replacement"))
        let copy = directory.appendingPathComponent("recovered.json")
        #expect(await recovery.saveCopy(to: copy))
        #expect(await recovery.flush())
        #expect(workspace.persistence.isFrozen)
        let core = CoreCoordinator(); let loaded = try await core.loadSession(at: copy.path)
        #expect(loaded.session?.revision == .max)
        #expect(loaded.session?.documents[0].sourceText == session.document.text)
        #expect(await recovery.flush())
        await core.shutdown(); await dispose(workspace)
    }

    @Test func finalSnapshotWaitsForActualSourceWriterAndKeepsLaterNativeText() async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let source = directory.appendingPathComponent("answer.cpp")
        let (file, original) = try await seed(directory, workspaceRevision: .max - 1, original: source)
        try Data(original.documents[0].sourceText.utf8).write(to: source)
        let workspace = Workspace(recoveryURL: file); await workspace.bootstrap()
        let recovery = try #require(workspace.recovery); let session = try #require(workspace.activeSession); let native = try #require(session.stView)
        #expect(await recovery.flush())
        let ready = DispatchSemaphore(value: 0); let release = DispatchSemaphore(value: 0)
        defer { release.signal() }
        let blocker = Task.detached {
            let coordinator = NSFileCoordinator(); var failure: NSError?
            coordinator.coordinate(writingItemAt: source, options: .forReplacing, error: &failure) { _ in ready.signal(); release.wait() }
            return failure?.localizedDescription
        }
        await withCheckedContinuation { continuation in DispatchQueue.global().async { ready.wait(); continuation.resume() } }
        let setupLease = try workspace.persistence.beginOperation()
        native.insertText("// B source snapshot\n", replacementRange: .init(location: NSNotFound, length: 0)); session.capture()
        let sourceSnapshot = session.document.snapshot()
        let writing = Task { await workspace.saveSource(session, to: source, selectedReplacement: true) }
        await waitUntil { workspace.savingDocumentIDs.contains(session.document.id) }
        workspace.persistence.endOperation(setupLease)
        #expect(!(await recovery.flush()))
        #expect(workspace.persistence.canMutate); #expect(native.isEditable)
        native.insertText("// C later native edit\n", replacementRange: .init(location: NSNotFound, length: 0)); session.capture()
        release.signal(); #expect(await blocker.value == nil); #expect(await writing.value)
        #expect(await recovery.flush())
        #expect(workspace.persistence.isFrozen)
        #expect(try Data(contentsOf: source) == sourceSnapshot.utf8)
        let core = CoreCoordinator(); let loaded = try await core.loadSession(at: file.path)
        #expect(loaded.session?.revision == .max)
        #expect(loaded.session?.documents[0].sourceText.contains("C later native edit") == true)
        #expect(loaded.session?.documents[0].savePoint?.sourceDigest == sourceSnapshot.sourceDigest)
        await core.shutdown(); await dispose(workspace)
    }

    @Test func finalRevisionWaitsForCompositionWithoutForcingItsCommit() async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let (file, _) = try await seed(directory, workspaceRevision: .max - 1)
        let workspace = Workspace(recoveryURL: file); await workspace.bootstrap()
        let recovery = try #require(workspace.recovery); let session = try #require(workspace.activeSession); let native = try #require(session.stView)
        #expect(await recovery.flush())
        let operation = try workspace.persistence.beginOperation()
        native.insertText("// B before composition\n", replacementRange: .init(location: NSNotFound, length: 0)); session.capture()
        let committed = session.document.snapshot()
        native.setMarkedText("临时拼音", selectedRange: .init(location: 4, length: 0), replacementRange: .init(location: NSNotFound, length: 0))
        session.capture(); workspace.persistence.endOperation(operation)
        #expect(!(await recovery.flush()))
        #expect(native.hasMarkedText()); #expect(session.document.snapshot() == committed)
        #expect(workspace.persistence.canMutate); #expect(native.isEditable)
        native.insertText("中文", replacementRange: .init(location: NSNotFound, length: 0)); session.capture()
        #expect(!native.hasMarkedText())
        #expect(await recovery.flush()); #expect(workspace.persistence.isFrozen)
        let core = CoreCoordinator(); let loaded = try await core.loadSession(at: file.path)
        #expect(loaded.session?.revision == .max)
        #expect(loaded.session?.documents[0].sourceText == session.document.text)
        #expect(loaded.session?.documents[0].sourceText.contains("临时拼音") == false)
        await core.shutdown(); await dispose(workspace)
    }

    @Test(arguments: [false, true]) func finalDocumentEditKeepsCompletedSourceWriteAndExactRecoverySavePoint(saveAs: Bool) async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let source = directory.appendingPathComponent("answer.cpp")
        let (file, original) = try await seed(directory, workspaceRevision: 4, documentRevision: .max - 2, original: source)
        let originalBytes = Data(original.documents[0].sourceText.utf8)
        try originalBytes.write(to: source)
        let destination = saveAs ? directory.appendingPathComponent("saved-as.cpp") : source
        if saveAs { try originalBytes.write(to: destination) }
        let workspace = Workspace(recoveryURL: file); await workspace.bootstrap()
        let session = try #require(workspace.activeSession); let native = try #require(session.stView)
        let recovery = try #require(workspace.recovery); #expect(await recovery.flush())
        let ready = DispatchSemaphore(value: 0); let release = DispatchSemaphore(value: 0)
        defer { release.signal() }
        let blocker = Task.detached {
            let coordinator = NSFileCoordinator(); var failure: NSError?
            coordinator.coordinate(writingItemAt: destination, options: .forReplacing, error: &failure) { _ in ready.signal(); release.wait() }
            return failure?.localizedDescription
        }
        await withCheckedContinuation { continuation in DispatchQueue.global().async { ready.wait(); continuation.resume() } }
        native.insertText("// B source snapshot\n", replacementRange: .init(location: NSNotFound, length: 0)); session.capture()
        let snapshot = session.document.snapshot(); #expect(snapshot.revision == UInt64.max - 1)
        let writing = Task { await workspace.saveSource(session, to: destination, selectedReplacement: saveAs) }
        await waitUntil { workspace.savingDocumentIDs.contains(session.document.id) }
        native.insertText("// C final edit\n", replacementRange: .init(location: NSNotFound, length: 0)); session.capture()
        #expect(session.document.revision == .max); #expect(!native.isEditable)
        release.signal(); #expect(await blocker.value == nil); #expect(await writing.value)
        #expect(try Data(contentsOf: destination) == snapshot.utf8)
        if saveAs { #expect(try Data(contentsOf: source) == originalBytes) }
        #expect(session.document.text.contains("C final edit")); #expect(session.document.isDirty)
        #expect(session.document.url == destination)
        #expect(session.document.savePoint == SourceSavePoint(revision: snapshot.revision, sourceDigest: snapshot.sourceDigest, diskDigest: snapshot.sourceDigest))
        #expect(await recovery.flush())
        let core = CoreCoordinator(); let loaded = try await core.loadSession(at: file.path)
        let restored = try #require(loaded.session?.documents.first)
        #expect(restored.revision == .max); #expect(restored.sourceText == session.document.text)
        #expect(restored.savePoint == session.document.savePoint); #expect(restored.originalPath == destination.path)
        let forbidden = directory.appendingPathComponent("after-max.cpp")
        #expect(!(await workspace.saveSource(session, to: forbidden)))
        #expect(!FileManager.default.fileExists(atPath: forbidden.path))
        await core.shutdown(); await dispose(workspace)
    }

    @Test func codeEditorCaptureReachingFinalDocumentRevisionRejectsNewSourceWrite() async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let source = directory.appendingPathComponent("answer.cpp")
        let (file, original) = try await seed(directory, workspaceRevision: 4, documentRevision: .max - 1, editor: .codeEditorView, original: source)
        let originalBytes = Data(original.documents[0].sourceText.utf8); try originalBytes.write(to: source)
        let workspace = Workspace(recoveryURL: file); await workspace.bootstrap()
        let session = try #require(workspace.activeSession); let recovery = try #require(workspace.recovery)
        #expect(await recovery.flush())
        func findText(_ view: NSView) -> NSTextView? {
            if let text = view as? NSTextView { return text }
            return view.subviews.lazy.compactMap(findText).first
        }
        let native = try #require(findText(session.container))
        let beforePoint = session.document.savePoint
        native.insertText("// C deferred final edit\n", replacementRange: .init(location: NSNotFound, length: 0))
        #expect(session.document.revision == UInt64.max - 1)
        let destination = directory.appendingPathComponent("must-not-write.cpp")
        #expect(!(await workspace.saveSource(session, to: destination)))
        #expect(session.document.revision == .max); #expect(!native.isEditable)
        #expect(session.document.text.contains("C deferred final edit"))
        #expect(session.document.url == source); #expect(session.document.savePoint == beforePoint)
        #expect(!FileManager.default.fileExists(atPath: destination.path))
        #expect(try Data(contentsOf: source) == originalBytes)
        #expect(workspace.errorMessage == L10n.string("revision_exhausted"))
        #expect(await recovery.flush())
        await dispose(workspace)
    }

    @Test func failedSourceWriteDoesNotRecordACompletionAndReleasesItsLease() async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let source = directory.appendingPathComponent("answer.cpp")
        let (file, original) = try await seed(directory, workspaceRevision: 4, original: source)
        let originalBytes = Data(original.documents[0].sourceText.utf8); try originalBytes.write(to: source)
        let workspace = Workspace(recoveryURL: file); await workspace.bootstrap()
        let session = try #require(workspace.activeSession); let native = try #require(session.stView)
        let beforePoint = session.document.savePoint
        native.insertText("// B before failed write\n", replacementRange: .init(location: NSNotFound, length: 0)); session.capture()
        #expect(!(await workspace.saveSource(session, to: file, selectedReplacement: true)))
        #expect(session.document.savePoint == beforePoint); #expect(session.document.url == source)
        #expect(try Data(contentsOf: source) == originalBytes)
        #expect(await workspace.saveSource(session, to: source))
        #expect(try Data(contentsOf: source) == Data(session.document.text.utf8))
        #expect(session.document.savePoint?.revision == session.document.revision)
        await dispose(workspace)
    }

    @Test(arguments: [false, true]) func lastRevisionImportSavesThenMarksAndReadonlySavedReceiptCanBeConfirmed(manualPendingReceipt: Bool) async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let config = try CompanionConfiguration.load()
        let locations = config.paths(for: directory.appendingPathComponent("workspace.json"))
        let binding = CompanionStorageBinding(workspaceID: RawIdentifier("revision/workspace"), paths: locations)
        let preference = NativePreferences(namespaceID: config.storageNamespace, schemaVersion: 1,
            objectJSON: String(decoding: try JSONEncoder().encode(binding), as: UTF8.self))
        let (file, _) = try await seed(directory, workspaceRevision: .max - 1, extraPreferences: [preference])
        let core = CoreCoordinator(); let context = CompanionContext(workspaceID: binding.workspaceID, paths: locations)
        let payload = "{\"name\":\"Final import\",\"group\":\"Test\",\"url\":\"https://example.org/final\",\"timeLimit\":0,\"memoryLimit\":0,\"tests\":[{\"input\":\"1\",\"output\":\"1\"}]}"
        let accepted = try await core.companionAccept(context, originalJSON: payload, lockTimeoutMS: 0); #expect(accepted.status == "Accepted")
        let workspace = Workspace(recoveryURL: file); await workspace.bootstrap()
        let recovery = try #require(workspace.recovery); let item = try #require(workspace.companion.items.first)
        #expect(await recovery.flush())
        if manualPendingReceipt {
            let base = try recovery.snapshotForCompanion()
            let preparation = try await core.companionPrepare(context, importID: item.importID, session: base,
                defaults: workspace.companionDefaults(source: "", configuration: recovery.configuration))
            try workspace.appendCompanionCandidate(#require(preparation.session), base: base, receipt: #require(preparation.receipt), configuration: recovery.configuration)
            #expect(await recovery.flush())
            let pending = try await core.companionPage(context, maxItems: 8, maxPageBytes: 1_000_000)
            #expect(pending.items[0].state == .pending); #expect(workspace.persistence.isFrozen)
        }
        await workspace.companion.apply(importID: item.importID)
        #expect(workspace.companion.items[0].state == .applied)
        #expect(workspace.persistence.isFrozen)
        let saved = try await core.loadSession(at: file.path)
        #expect(saved.session?.revision == .max)
        #expect(saved.session?.documents.filter { $0.documentID == item.documentID }.count == 1)
        let savedBytes = try Data(contentsOf: file)
        await workspace.companion.apply(importID: item.importID)
        #expect(workspace.companion.items[0].state == .applied)
        #expect(try Data(contentsOf: file) == savedBytes)
        await core.shutdown(); await dispose(workspace)
    }

    @Test func firstInboxBindingWithOnlyOneRevisionLeftIsRejectedBeforeAnyNewImport() async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let (file, _) = try await seed(directory, workspaceRevision: .max - 1)
        let originalBytes = try Data(contentsOf: file)
        let workspace = Workspace(recoveryURL: file); await workspace.bootstrap()
        let config = try CompanionConfiguration.load(); let paths = try #require(workspace.companion.paths)
        let payload = directory.appendingPathComponent("payload.json")
        try Data("{\"name\":\"Rejected\",\"group\":\"Test\",\"url\":\"https://example.org/rejected\",\"timeLimit\":0,\"memoryLimit\":0,\"tests\":[]}".utf8).write(to: payload)
        await workspace.companion.accept(file: payload)
        #expect(!FileManager.default.fileExists(atPath: paths.inboxFile))
        #expect(try workspace.companionStoragePreference(namespace: config.storageNamespace) == nil)
        #expect(workspace.companion.notice == L10n.string("companion_revision_budget"))
        #expect(try Data(contentsOf: file) == originalBytes)
        #expect(workspace.persistence.canMutate)
        await dispose(workspace)
    }

    @Test(arguments: EditorKind.allCases) func closingLastCleanDocumentCommitsMaxEmptyRecordAndReopensWithoutGhostTab(editor: EditorKind) async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let (file, _) = try await seed(directory, workspaceRevision: .max - 1, editor: editor, text: "")
        let workspace = Workspace(recoveryURL: file); await workspace.bootstrap()
        let original = try #require(workspace.activeSession)
        #expect(!original.document.isDirty)
        await workspace.closeActiveDocument()
        #expect(workspace.sessions.isEmpty); #expect(workspace.editorDeckSessions.isEmpty)
        #expect(workspace.selection == nil); #expect(workspace.retainedCloseSession == nil)
        #expect(workspace.persistence.isFrozen); #expect(!workspace.isBusy)
        let core = CoreCoordinator(); let loaded = try await core.loadSession(at: file.path)
        #expect(loaded.session?.revision == .max); #expect(loaded.session?.documents.isEmpty == true)
        #expect(await workspace.recovery!.flush())
        await dispose(workspace)
        let reopened = Workspace(recoveryURL: file); await reopened.bootstrap()
        #expect(reopened.sessions.isEmpty); #expect(reopened.editorDeckSessions.isEmpty)
        #expect(reopened.persistence.isFrozen); #expect(reopened.activeSession == nil)
        #expect(await reopened.recovery!.flush())
        await core.shutdown(); await dispose(reopened)
    }

    @Test(arguments: EditorKind.allCases) func failedFinalDocumentRemovalRollsBackSameMountedBufferAndRedoHistory(editor: EditorKind) async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let source = directory.appendingPathComponent("answer.cpp")
        let (file, seedRecord) = try await seed(directory, workspaceRevision: .max - 1, editor: editor, original: source)
        try Data(seedRecord.documents[0].sourceText.utf8).write(to: source)
        let workspace = Workspace(recoveryURL: file); await workspace.bootstrap()
        let session = try #require(workspace.activeSession); let recovery = try #require(workspace.recovery)
        let window = NSWindow(contentRect: .init(x: 0, y: 0, width: 800, height: 600), styleMask: [.titled], backing: .buffered, defer: false)
        let host = NSHostingView(rootView: WorkspaceView(workspace: workspace)); window.contentView = host
        window.layoutIfNeeded(); host.layoutSubtreeIfNeeded()
        defer { window.contentView = nil; window.orderOut(nil) }
        func findText(_ view: NSView) -> NSTextView? {
            if let text = view as? NSTextView { return text }
            return view.subviews.lazy.compactMap(findText).first
        }
        let native: NSView = editor == .stTextView ? try #require(session.stView) : try #require(findText(session.container))
        #expect(await recovery.flush())
        let descriptor = Darwin.open(recovery.paths.lockFile, O_RDWR)
        #expect(descriptor >= 0); defer { _ = flock(descriptor, LOCK_UN); _ = Darwin.close(descriptor) }
        #expect(flock(descriptor, LOCK_EX | LOCK_NB) == 0)
        let setup = try workspace.persistence.beginOperation()
        session.container.history.groupsByEvent = false; session.container.history.beginUndoGrouping()
        if let st = session.stView { st.insertText("// temporary user edit\n", replacementRange: .init(location: NSNotFound, length: 0)) }
        else { (native as! NSTextView).insertText("// temporary user edit\n", replacementRange: .init(location: NSNotFound, length: 0)) }
        session.container.history.endUndoGrouping(); session.capture()
        session.container.history.undo(); session.capture()
        #expect(!session.document.isDirty); #expect(session.container.history.canRedo)
        let expectedText = session.document.text; let oldSelection = workspace.selection
        let invalid = Data("{external invalid".utf8)
        let failureInjection = Task {
            await waitUntil { recovery.isSaving }
            #expect(workspace.sessions.isEmpty); #expect(workspace.retainedCloseSession === session)
            #expect(workspace.editorDeckSessions.contains { $0 === session })
            #expect(session.container.superview != nil)
            try invalid.write(to: file)
            _ = flock(descriptor, LOCK_UN)
        }
        workspace.persistence.endOperation(setup)
        await workspace.closeActiveDocument()
        try await failureInjection.value
        #expect(workspace.sessions.count == 1); #expect(workspace.activeSession === session)
        #expect(workspace.selection == oldSelection); #expect(workspace.retainedCloseSession == nil)
        #expect(workspace.persistence.canMutate); #expect(!workspace.isBusy)
        #expect(workspace.errorMessage != nil); #expect(session.document.text == expectedText)
        #expect(session.container.history.canRedo); #expect(session.container.allowsEditingCommands)
        let restoredNative: NSView = editor == .stTextView ? try #require(session.stView) : try #require(findText(session.container))
        #expect(restoredNative === native)
        #expect(try Data(contentsOf: file) == invalid)
        await dispose(workspace)
    }

    @Test func unchangedMaxDetectsExternalDiskChangesWithoutReplacingThem() async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let (file, _) = try await seed(directory, workspaceRevision: .max)
        let workspace = Workspace(recoveryURL: file); await workspace.bootstrap()
        let external = Data("{outside invalid".utf8); try external.write(to: file)
        #expect(!(await workspace.recovery!.flush()))
        #expect(try Data(contentsOf: file) == external)
        #expect(workspace.persistence.isFrozen)
        await dispose(workspace)
    }
}
