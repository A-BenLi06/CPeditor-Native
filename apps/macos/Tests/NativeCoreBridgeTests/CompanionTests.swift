import Foundation
import Testing
import NativeEditorModel
@testable import NativeCoreBridge

@Suite(.serialized) struct CompanionBridgeTests {
    private func folder() throws -> URL {
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        return folder
    }
    private let original = "{ \"name\":\"中文🙂e\\u0301\", \"group\":\"Group\", \"url\":\"https://example.org\", \"memoryLimit\":0, \"timeLimit\":0, \"tests\":[{\"input\":\"\\u0000\\r\\n\",\"output\":\"\"}], \"future\":18446744073709551616 }"

    @Test func fourABIsRetainRawPayloadAndRequireActuallySavedReceipt() async throws {
        let directory = try folder(); defer { try? FileManager.default.removeItem(at: directory) }
        let paths = CompanionPaths(inboxFile: directory.appendingPathComponent("inbox.json").path,
            backupFile: directory.appendingPathComponent("old-inbox.json").path, lockFile: directory.appendingPathComponent("inbox.lock").path)
        let context = CompanionContext(workspaceID: RawIdentifier("native/中文"), paths: paths)
        let core = CoreCoordinator(maximumBuffers: 1)
        let accepted = try await core.companionAccept(context, originalJSON: original, lockTimeoutMS: 0)
        #expect(accepted.status == "Accepted"); #expect(accepted.inboxReplaced)
        let page = try await core.companionPage(context, maxItems: 1, maxPageBytes: 1_000_000)
        let item = try #require(page.items.first)
        #expect(Data(item.originalJSON.utf8) == Data(original.utf8))
        #expect(item.testIDs.count == 1); #expect(!page.hasMore)
        let base = WorkspaceSessionRecord(workspaceID: context.workspaceID, revision: 0, selectedDocumentID: nil, documents: [], editorPreferences: [])
        let defaults = CompanionDefaults(sourceText: "// local template\n", languageID: "cpp", toolchainProfileID: nil, documentPreferences: [])
        let prepared = try await core.companionPrepare(context, importID: item.importID, session: base, defaults: defaults)
        #expect(prepared.status == "Prepared")
        let candidate = try #require(prepared.session)
        #expect(candidate.documents[0].tests[0].input.data == Data([0, 13, 10]))
        #expect(candidate.documents[0].tests[0].expected?.data == Data())
        #expect(try CompanionProvenance.ordinaryStdioAllowed(documentID: item.documentID, preferences: candidate.documents[0].editorPreferences))
        let sessions = SessionPaths(recoveryFile: directory.appendingPathComponent("recovery.json").path,
            backupFile: directory.appendingPathComponent("old-recovery.json").path, lockFile: directory.appendingPathComponent("recovery.lock").path)
        let before = try await core.companionMarkApplied(context, importID: item.importID, sessionPaths: sessions, lockTimeoutMS: 0)
        #expect(before.status == "SessionUnavailable"); #expect(!before.inboxReplaced)
        let saved = try await core.saveSession(candidate, paths: sessions, expectedDiskDigest: nil, lockTimeoutMS: 0)
        #expect(saved.status == .saved)
        let marked = try await core.companionMarkApplied(context, importID: item.importID, sessionPaths: sessions, lockTimeoutMS: 0)
        #expect(marked.status == "Applied")
        let changed = try await core.companionPage(context, maxItems: 1, maxPageBytes: 1_000_000)
        #expect(changed.items[0].state == .applied)
        #expect(changed.items[0].sequence == item.sequence)
        let deleted = WorkspaceSessionRecord(workspaceID: candidate.workspaceID, revision: 2, selectedDocumentID: nil, documents: [], editorPreferences: candidate.editorPreferences)
        let replay = try await core.companionPrepare(context, importID: item.importID, session: deleted, defaults: defaults)
        #expect(replay.status == "AlreadyApplied"); #expect(replay.session?.documents.isEmpty == true)
        await core.shutdown()
    }

    @Test func provenanceRejectsInvalidOwnFieldsAndFutureVersionsWithoutTouchingOriginals() throws {
        let id = RawIdentifier(String(repeating: "a", count: 64))
        let receipt = "\"inbox_id\":\"opaque/中文\",\"import_id\":\"\(id.rawValue)\",\"problem_id\":\"\(id.rawValue)\",\"document_id\":\"\(id.rawValue)\",\"payload_digest\":\"\(id.rawValue)\",\"test_ids\":[]"
        func preference(_ text: String, version: UInt64 = 1) -> NativePreferences { NativePreferences(namespaceID: CompanionProvenance.documentNamespace, schemaVersion: version, objectJSON: text) }
        let valid = preference("{\(receipt),\"ordinary_stdio_compatible\":true}")
        #expect(try CompanionProvenance.ordinaryStdioAllowed(documentID: id, preferences: [valid]))
        #expect(try CompanionProvenance.ordinaryStdioAllowed(documentID: id, preferences: []))
        for text in ["{\(receipt),\"ordinary_stdio_compatible\":true,\"unknown\":1}",
                     "{\(receipt),\"ordinary_stdio_compatible\":true,\"ordinary_stdio_compatible\":false}",
                     "{\(receipt),\"ordinary_stdio_compatible\":1.0}",
                     "{\(receipt),\"ordinary_stdio_compatible\":null}"] {
            #expect(throws: (any Error).self) { try CompanionProvenance.ordinaryStdioAllowed(documentID: id, preferences: [preference(text)]) }
        }
        #expect(throws: (any Error).self) { try CompanionProvenance.ordinaryStdioAllowed(documentID: RawIdentifier("wrong"), preferences: [valid]) }
        let future = preference("{\"future\":18446744073709551616}", version: 99)
        #expect(throws: (any Error).self) { try CompanionProvenance.ordinaryStdioAllowed(documentID: id, preferences: [valid, future]) }
        #expect(future.objectJSON == "{\"future\":18446744073709551616}")
    }
}
