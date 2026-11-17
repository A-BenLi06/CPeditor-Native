import AppKit
import Darwin
import Foundation
import Testing
import NativeEditorModel
import NativeCoreBridge
@testable import CPEditorNative

@MainActor @Suite(.serialized) struct CompanionNativeTests {
    private func folder() throws -> URL {
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        return folder
    }
    private func payload(in directory: URL, name: String = "Sum", interactive: Bool = false) throws -> URL {
        let file = directory.appendingPathComponent(UUID().uuidString + ".json")
        let source = "{\"name\":\"\(name)\",\"group\":\"Test\",\"url\":\"https://example.org/\(name)\",\"memoryLimit\":0,\"timeLimit\":0,\"interactive\":\(interactive),\"tests\":[{\"input\":\"7 8\\r\\n\\u0000\",\"output\":\"\"}],\"future\":18446744073709551616}"
        try Data(source.utf8).write(to: file)
        return file
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

    @Test func fileImportApplySaveAsAndRestartKeepStableInboxAndDeletionReceipt() async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let file = directory.appendingPathComponent("recovery.json")
        let workspace = Workspace(arguments: ["--editor", "sttextview"], recoveryURL: file)
        await workspace.bootstrap()
        let source = try payload(in: directory)
        await workspace.companion.accept(file: source)
        let item = try #require(workspace.companion.items.first)
        #expect(item.state == .pending)
        await workspace.companion.apply(importID: item.importID)
        #expect(workspace.companion.items[0].state == .applied)
        let imported = try #require(workspace.sessions.first { $0.document.id == item.documentID })
        #expect(imported.document.url == nil)
        #expect(imported.testCases[0].input.data == Data([55, 32, 56, 13, 10, 0]))
        #expect(imported.testCases[0].expected?.data == Data())
        let count = workspace.sessions.count
        await workspace.companion.accept(file: source)
        await workspace.companion.apply(importID: item.importID)
        #expect(workspace.sessions.count == count)
        let localSource = directory.appendingPathComponent("answer.cpp")
        #expect(await workspace.saveSource(imported, to: localSource, selectedReplacement: true))
        let sourceBefore = try Data(contentsOf: localSource)
        #expect(!(await workspace.recovery!.saveCopy(to: localSource)))
        #expect(try Data(contentsOf: localSource) == sourceBefore)
        let inbox = try #require(workspace.companion.paths)
        let recovery = try #require(workspace.recovery)
        let copy = directory.appendingPathComponent("another-recovery.json")
        #expect(await recovery.saveCopy(to: copy))
        #expect(workspace.companion.paths == inbox)
        #expect(!(await workspace.saveSource(imported, to: URL(fileURLWithPath: inbox.inboxFile), selectedReplacement: true)))
        #expect(!(await recovery.saveCopy(to: URL(fileURLWithPath: inbox.backupFile))))
        workspace.sessions.removeAll { $0 === imported }; workspace.selection = workspace.sessions.first?.document.id; imported.dispose()
        #expect(await recovery.flush())
        await dispose(workspace)
        let reopened = Workspace(recoveryURL: copy)
        await reopened.bootstrap()
        #expect(reopened.companion.paths == inbox)
        #expect(reopened.companion.items[0].state == .applied)
        let afterDeletion = reopened.sessions.count
        await reopened.companion.apply(importID: item.importID)
        #expect(reopened.sessions.count == afterDeletion)
        #expect(!reopened.sessions.contains { $0.document.id == item.documentID })
        await dispose(reopened)
    }

    @Test func saveWhileImportingRetainsExistingNativeEditorNewEditsAndUndoHistory() async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let workspace = Workspace(arguments: ["--editor", "sttextview"], recoveryURL: directory.appendingPathComponent("recovery.json"))
        await workspace.bootstrap()
        let old = try #require(workspace.activeSession); let native = try #require(old.stView)
        native.insertText("// before import\n", replacementRange: .init(location: NSNotFound, length: 0)); old.capture()
        await workspace.companion.accept(file: try payload(in: directory))
        let item = try #require(workspace.companion.items.first); let recovery = try #require(workspace.recovery)
        #expect(await recovery.flush())
        let descriptor = Darwin.open(recovery.paths.lockFile, O_RDWR)
        #expect(descriptor >= 0)
        defer { _ = flock(descriptor, LOCK_UN); _ = Darwin.close(descriptor) }
        #expect(flock(descriptor, LOCK_EX | LOCK_NB) == 0)
        let application = Task { await workspace.companion.apply(importID: item.importID) }
        await waitUntil { recovery.isSaving }
        #expect(workspace.companion.isOperating)
        old.container.history.groupsByEvent = false; old.container.history.beginUndoGrouping()
        native.insertText("// new edit during save 中文🙂\n", replacementRange: .init(location: NSNotFound, length: 0))
        old.container.history.endUndoGrouping(); old.capture()
        old.testCases.append(TestCaseInput(id: "local/new", input: "1", expected: "1"))
        _ = flock(descriptor, LOCK_UN)
        await application.value
        #expect(workspace.sessions.contains { $0 === old })
        #expect(old.document.text.contains("new edit during save")); #expect(old.container.history.canUndo)
        #expect(workspace.companion.items[0].state == .applied)
        let core = CoreCoordinator(); let loaded = try await core.loadSession(at: recovery.paths.recoveryFile)
        let saved = try #require(loaded.session?.documents.first { $0.documentID == old.document.id })
        #expect(saved.sourceText == old.document.text); #expect(saved.tests.contains { $0.id == RawIdentifier("local/new") })
        await core.shutdown(); await dispose(workspace)
    }

    @Test func preparedCandidateMergesOnlyNewDocumentAfterExistingBufferChanges() async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let workspace = Workspace(arguments: ["--editor", "sttextview"], recoveryURL: directory.appendingPathComponent("recovery.json"))
        await workspace.bootstrap()
        let existing = try #require(workspace.activeSession)
        await workspace.companion.accept(file: try payload(in: directory))
        let item = try #require(workspace.companion.items.first)
        let recovery = try #require(workspace.recovery)
        #expect(await recovery.flush())
        let base = try recovery.snapshotForCompanion()
        let core = CoreCoordinator()
        let context = CompanionContext(workspaceID: workspace.workspaceID, paths: try #require(workspace.companion.paths))
        let prepared = try await core.companionPrepare(context, importID: item.importID, session: base,
            defaults: workspace.companionDefaults(source: "", configuration: recovery.configuration))
        let candidate = try #require(prepared.session); let receipt = try #require(prepared.receipt)
        existing.stView?.insertText("// edit after capture and before applying candidate\n", replacementRange: .init(location: NSNotFound, length: 0)); existing.capture()
        existing.testCases.append(TestCaseInput(id: "after-capture", input: "9", expected: "9"))
        let latestText = existing.document.text; let latestTests = existing.testCases
        try workspace.appendCompanionCandidate(candidate, base: base, receipt: receipt, configuration: recovery.configuration)
        #expect(workspace.sessions.first === existing)
        #expect(existing.document.text == latestText); #expect(existing.testCases == latestTests)
        #expect(await recovery.flush())
        let marked = try await core.companionMarkApplied(context, importID: item.importID, sessionPaths: recovery.paths, lockTimeoutMS: 0)
        #expect(marked.status == "Applied")
        let loaded = try await core.loadSession(at: recovery.paths.recoveryFile)
        #expect(loaded.session?.documents.first?.sourceText == latestText)
        await core.shutdown(); await dispose(workspace)
    }

    @Test func failedRecoverySaveLeavesPendingAndNewDocumentCanBeRecoveredElsewhere() async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let workspace = Workspace(arguments: ["--editor", "sttextview"], recoveryURL: directory.appendingPathComponent("recovery.json"))
        await workspace.bootstrap()
        await workspace.companion.accept(file: try payload(in: directory))
        let item = try #require(workspace.companion.items.first); let recovery = try #require(workspace.recovery)
        #expect(await recovery.flush())
        let descriptor = Darwin.open(recovery.paths.lockFile, O_RDWR)
        #expect(descriptor >= 0)
        defer { _ = flock(descriptor, LOCK_UN); _ = Darwin.close(descriptor) }
        #expect(flock(descriptor, LOCK_EX | LOCK_NB) == 0)
        let application = Task { await workspace.companion.apply(importID: item.importID) }
        await waitUntil { recovery.isSaving }
        try Data("{external invalid".utf8).write(to: URL(fileURLWithPath: recovery.paths.recoveryFile))
        _ = flock(descriptor, LOCK_UN)
        await application.value
        #expect(recovery.isBlocked)
        #expect(workspace.companion.items[0].state == .pending)
        #expect(workspace.sessions.contains { $0.document.id == item.documentID })
        let count = workspace.sessions.count
        #expect(await recovery.saveCopy(to: directory.appendingPathComponent("recovered.json")))
        await workspace.companion.apply(importID: item.importID)
        #expect(workspace.companion.items[0].state == .applied)
        #expect(workspace.sessions.count == count)
        await dispose(workspace)
    }

    @Test func updatedPayloadCreatesSeparateDocumentAndPaginationRefreshesChangingHistory() async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let workspace = Workspace(arguments: ["--editor", "sttextview"], recoveryURL: directory.appendingPathComponent("recovery.json"))
        await workspace.bootstrap()
        let source = try payload(in: directory)
        await workspace.companion.accept(file: source)
        let first = try #require(workspace.companion.items.first)
        await workspace.companion.apply(importID: first.importID)
        let original = try #require(workspace.sessions.first { $0.document.id == first.documentID })
        original.stView?.insertText("// user's edited answer\n", replacementRange: .init(location: NSNotFound, length: 0)); original.capture()
        original.testCases.append(TestCaseInput(id: "user/sample", input: "42", expected: "42"))
        let edited = original.document.text; let tests = original.testCases
        let updatedText = try String(contentsOf: source, encoding: .utf8).replacingOccurrences(of: "7 8", with: "20 30")
        try Data(updatedText.utf8).write(to: source)
        await workspace.companion.accept(file: source)
        let second = try #require(workspace.companion.items.last)
        #expect(second.previousImportID == first.importID); #expect(second.documentID != first.documentID)
        await workspace.companion.apply(importID: second.importID)
        #expect(original.document.text == edited); #expect(original.testCases == tests)
        #expect(workspace.sessions.filter { $0.document.id == second.documentID }.count == 1)
        for index in 0..<9 { await workspace.companion.accept(file: try payload(in: directory, name: "Next\(index)")) }
        #expect(workspace.companion.hasMore); #expect(workspace.companion.items.count == 8)
        await workspace.companion.loadMore()
        #expect(!workspace.companion.hasMore); #expect(workspace.companion.items.count == 11)
        #expect(Set(workspace.companion.items.map(\.importID)).count == 11)
        await dispose(workspace)
    }

    @Test func fullAndFutureStoragePreferencesArePreservedWithoutAcceptingIntoNewInbox() async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let config = try CompanionConfiguration.load()
        for future in [false, true] {
            let file = directory.appendingPathComponent(future ? "future.json" : "full.json")
            let workspaceID = RawIdentifier("test/workspace/" + String(future))
            let paths = config.paths(for: file)
            let binding = CompanionStorageBinding(workspaceID: workspaceID, paths: paths)
            let encoded = String(decoding: try JSONEncoder().encode(binding), as: UTF8.self)
            let preferences = future ? [NativePreferences(namespaceID: config.storageNamespace, schemaVersion: 1, objectJSON: encoded),
                NativePreferences(namespaceID: config.storageNamespace, schemaVersion: 99, objectJSON: "{\"future\":18446744073709551616}")]
                : (0..<16).map { NativePreferences(namespaceID: "foreign/\($0)", schemaVersion: 99, objectJSON: "{ \"raw\": 18446744073709551616 }") }
            let candidate = WorkspaceSessionRecord(workspaceID: workspaceID, revision: 1, selectedDocumentID: nil, documents: [], editorPreferences: preferences)
            let core = CoreCoordinator(); let recovery = try RecoveryConfiguration.load().paths(for: file)
            let saved = try await core.saveSession(candidate, paths: recovery, expectedDiskDigest: nil, lockTimeoutMS: 0)
            #expect(saved.status == .saved); await core.shutdown()
            let workspace = Workspace(recoveryURL: file); await workspace.bootstrap()
            await workspace.companion.accept(file: try payload(in: directory))
            #expect(!FileManager.default.fileExists(atPath: paths.inboxFile))
            let record = try workspace.sessionRecord(revision: 2, configuration: .load())
            #expect(record.editorPreferences.filter { preference in preferences.contains(preference) } == preferences)
            if future { #expect(workspace.companion.paths == nil) }
            #expect(await workspace.recovery!.flush())
            await dispose(workspace)
        }
    }

    @Test func backupIntroducingDifferentInboxBindingIsRejectedAndPendingHistoryRemainsIntact() async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let workspace = Workspace(recoveryURL: directory.appendingPathComponent("current.json")); await workspace.bootstrap()
        let recovery = try #require(workspace.recovery); #expect(await recovery.flush())
        let fallback = try #require(workspace.companion.paths)
        let config = try CompanionConfiguration.load()
        let base = try workspace.sessionRecord(revision: 1, configuration: .load())
        let alternate = config.paths(for: directory.appendingPathComponent("legacy.json"))
        let binding = CompanionStorageBinding(workspaceID: workspace.workspaceID, paths: alternate)
        let preference = NativePreferences(namespaceID: config.storageNamespace, schemaVersion: 1,
            objectJSON: String(decoding: try JSONEncoder().encode(binding), as: UTF8.self))
        let candidate = WorkspaceSessionRecord(workspaceID: base.workspaceID, revision: 2, selectedDocumentID: base.selectedDocumentID,
            documents: base.documents, editorPreferences: base.editorPreferences + [preference])
        let core = CoreCoordinator()
        let generated = directory.appendingPathComponent("generated.json")
        let saved = try await core.saveSession(candidate, paths: RecoveryConfiguration.load().paths(for: generated), expectedDiskDigest: nil, lockTimeoutMS: 0)
        #expect(saved.status == .saved)
        try Data(contentsOf: generated).write(to: URL(fileURLWithPath: recovery.paths.backupFile))
        let context = CompanionContext(workspaceID: workspace.workspaceID, paths: alternate)
        let accepted = try await core.companionAccept(context, originalJSON: String(contentsOf: payload(in: directory), encoding: .utf8), lockTimeoutMS: 0)
        #expect(accepted.status == "Accepted")
        let inboxBytes = try Data(contentsOf: URL(fileURLWithPath: alternate.inboxFile))
        let oldEditor = workspace.activeSession
        await recovery.loadBackup()
        #expect(workspace.activeSession === oldEditor)
        #expect(try workspace.companionStoragePreference(namespace: config.storageNamespace) == nil)
        #expect(workspace.companion.paths == fallback)
        #expect(recovery.notice == L10n.string("companion_backup_binding"))
        #expect(try Data(contentsOf: URL(fileURLWithPath: alternate.inboxFile)) == inboxBytes)
        let page = try await core.companionPage(context, maxItems: 8, maxPageBytes: 1_000_000)
        #expect(page.items.count == 1); #expect(page.items[0].state == .pending)
        await core.shutdown(); await dispose(workspace)
    }

    @Test func backupWorkspaceIdentityMustMatchBothLiveWorkspaceAndExistingInboxOwner() async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let workspace = Workspace(recoveryURL: directory.appendingPathComponent("current.json")); await workspace.bootstrap()
        await workspace.companion.accept(file: try payload(in: directory))
        let recovery = try #require(workspace.recovery); #expect(await recovery.flush())
        let config = try CompanionConfiguration.load()
        let loadedBinding = try workspace.companionStoragePreference(namespace: config.storageNamespace)
        let currentBinding = try #require(loadedBinding)
        let originalID = workspace.workspaceID; let oldEditor = workspace.activeSession
        let base = try workspace.sessionRecord(revision: 2, configuration: .load())
        let candidate = WorkspaceSessionRecord(workspaceID: RawIdentifier("different/workspace"), revision: 2,
            selectedDocumentID: base.selectedDocumentID, documents: base.documents, editorPreferences: base.editorPreferences)
        #expect(!workspace.companionBindingMatches(candidate))
        let core = CoreCoordinator(); let generated = directory.appendingPathComponent("generated.json")
        let saved = try await core.saveSession(candidate, paths: RecoveryConfiguration.load().paths(for: generated), expectedDiskDigest: nil, lockTimeoutMS: 0)
        #expect(saved.status == .saved)
        try Data(contentsOf: generated).write(to: URL(fileURLWithPath: recovery.paths.backupFile))
        let inboxBefore = try Data(contentsOf: URL(fileURLWithPath: currentBinding.paths.inboxFile))
        await recovery.loadBackup()
        #expect(workspace.workspaceID == originalID); #expect(workspace.activeSession === oldEditor)
        #expect(workspace.companion.paths == currentBinding.paths)
        #expect(recovery.notice == L10n.string("companion_backup_binding"))
        #expect(try Data(contentsOf: URL(fileURLWithPath: currentBinding.paths.inboxFile)) == inboxBefore)
        let page = try await core.companionPage(CompanionContext(workspaceID: workspace.workspaceID, paths: currentBinding.paths), maxItems: 8, maxPageBytes: 1_000_000)
        #expect(page.status == .loaded); #expect(page.items.count == 1); #expect(page.items[0].state == .pending)
        await core.shutdown(); await dispose(workspace)
    }

    @Test func incompatibleAndFutureProvenanceRemainBlockedAfterRestart() async throws {
        _ = NSApplication.shared
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let file = directory.appendingPathComponent("recovery.json")
        let workspace = Workspace(arguments: ["--editor", "sttextview"], recoveryURL: file)
        await workspace.bootstrap()
        await workspace.companion.accept(file: try payload(in: directory, interactive: true))
        let item = try #require(workspace.companion.items.first)
        await workspace.companion.apply(importID: item.importID)
        let session = try #require(workspace.activeSession)
        #expect(session.companionRunNotice != nil)
        workspace.runner.runAll(session); #expect(!workspace.runner.isRunning)
        session.preservedPreferences.append(NativePreferences(namespaceID: CompanionProvenance.documentNamespace, schemaVersion: 99, objectJSON: "{\"future\":18446744073709551616}"))
        #expect(await workspace.recovery!.flush())
        await dispose(workspace)
        let reopened = Workspace(recoveryURL: file); await reopened.bootstrap()
        let restored = try #require(reopened.activeSession)
        #expect(restored.preservedPreferences.filter { $0.namespaceID == CompanionProvenance.documentNamespace }.count == 2)
        #expect(restored.companionRunNotice != nil)
        reopened.runner.runAll(restored); #expect(!reopened.runner.isRunning)
        await dispose(reopened)
    }
}
