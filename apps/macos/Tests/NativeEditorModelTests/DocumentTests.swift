import Foundation
import Testing
@testable import NativeEditorModel

@MainActor struct DocumentTests {
    @Test func stableIdentityRevisionAndSavePoint() throws {
        let doc = SourceDocument(text: "int main() {}\n", languageID: "cpp")
        let original = doc.snapshot()
        #expect(!(try doc.commit(original.text)))
        #expect(doc.revision == 0)
        try doc.commit("int main() { return 0; }\n")
        let saved = doc.snapshot()
        #expect(saved.documentID == original.documentID)
        #expect(saved.revision == 1)
        #expect(doc.isDirty)
        try doc.commit("int main() { return 1; }\n")
        try doc.markSaved(at: URL(fileURLWithPath: "/tmp/main.cpp"), snapshot: saved)
        #expect(doc.isDirty)
        try doc.commit(saved.text)
        #expect(!doc.isDirty)
        #expect(doc.revision == 3)
    }

    @Test func rejectsStaleAndSplitCharacterEdits() throws {
        let doc = SourceDocument(text: "中🙂e\u{301}\r\n", languageID: "cpp")
        #expect(try doc.replacing(NSRange(location: 1, length: 2), with: "文", expectedRevision: 0) == "中文e\u{301}\r\n")
        #expect(throws: DocumentError.invalidRange) {
            try doc.replacing(NSRange(location: 2, length: 1), with: "", expectedRevision: 0)
        }
        #expect(throws: DocumentError.invalidRange) {
            try doc.replacing(NSRange(location: 3, length: 1), with: "", expectedRevision: 0)
        }
        try doc.commit("changed")
        #expect(throws: DocumentError.staleRevision) {
            try doc.replacing(NSRange(location: 0, length: 1), with: "", expectedRevision: 0)
        }
    }

    @Test func sourceSaveCompletionRequiresItsLiveLeaseAndConsumesItOnce() throws {
        let document = SourceDocument(text: "B", languageID: "cpp", revision: .max - 1)
        let other = SourceDocument(text: "B", languageID: "cpp")
        let snapshot = document.snapshot()
        let lease = try document.beginSourceSave(snapshot: snapshot)
        let destination = URL(fileURLWithPath: "/tmp/lease-answer.cpp")
        #expect(throws: DocumentError.staleRevision) { try document.beginSourceSave(snapshot: snapshot) }
        #expect(throws: DocumentError.staleRevision) { try other.completeSourceSave(lease, at: destination, diskDigest: snapshot.sourceDigest) }
        try document.commit("C")
        document.setPersistenceReadOnly(true)
        try document.completeSourceSave(lease, at: destination, diskDigest: snapshot.sourceDigest)
        #expect(document.text == "C"); #expect(document.isDirty)
        #expect(document.savePoint?.revision == UInt64.max - 1)
        #expect(document.savePoint?.sourceDigest == snapshot.sourceDigest)
        #expect(document.url == destination)
        #expect(throws: DocumentError.staleRevision) { try document.completeSourceSave(lease, at: destination, diskDigest: snapshot.sourceDigest) }
        #expect(throws: DocumentError.revisionExhausted) { try document.beginSourceSave(snapshot: document.snapshot()) }

        let cancelled = try other.beginSourceSave(snapshot: other.snapshot())
        other.cancelSourceSave(cancelled)
        #expect(throws: DocumentError.staleRevision) { try other.completeSourceSave(cancelled, at: destination, diskDigest: snapshot.sourceDigest) }
        let retry = try other.beginSourceSave(snapshot: other.snapshot())
        other.cancelSourceSave(retry)
        #expect(other.url == nil); #expect(other.savePoint == nil)
    }

    @Test func scalarUTF16ToUTF8Mapping() throws {
        let index = try TextCoordinateIndex("中🙂e\u{301}\t\r\n")
        #expect(try index.utf8Range(for: NSRange(location: 1, length: 2)) == 3..<7)
        #expect(try index.utf8Range(for: NSRange(location: 3, length: 2)) == 7..<10)
        #expect(throws: DocumentError.invalidRange) { try index.utf8Range(for: NSRange(location: 2, length: 0)) }
        #expect(throws: DocumentError.invalidRange) { try index.utf8Range(for: NSRange(location: Int.max, length: 2)) }
    }

    @Test func atomicUnicodeFileRoundTrip() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let url = directory.appendingPathComponent("算法 🙂.cpp")
        let text = "// 中文🙂e\u{301}\r\n\treturn 0;\n"
        let document = SourceDocument(text: text, languageID: "cpp")
        try SourceFile.write(document.snapshot(), to: url)
        #expect(try SourceFile.readUTF8(url) == text)
        try Data([0xff, 0xfe]).write(to: url)
        #expect(throws: DocumentError.invalidUTF8) { try SourceFile.readUTF8(url) }
    }

    @Test func cppProfileHighlightsCommentsStringsAndDirectives() throws {
        let profile = try LanguageProfile.load(id: "cpp")
        let lexer = try LexicalHighlighter(profile: profile)
        let text = "#include <iostream>\n// int 42\nint main() { std::cout << \"中文 42\"; /* return */ return 0; }"
        let tokens = lexer.tokens(in: text)
        #expect(tokens.contains { $0.kind == .directive && (text as NSString).substring(with: $0.range) == "#include" })
        #expect(tokens.contains { $0.kind == .string && (text as NSString).substring(with: $0.range) == "\"中文 42\"" })
        #expect(tokens.filter { $0.kind == .number }.count == 1)
        #expect(tokens.filter { $0.kind == .comment }.count == 2)
    }

    @Test func canonicalEquivalentTextAndIDsRemainByteDistinct() throws {
        let composed = "é"
        let decomposed = "e\u{301}"
        #expect(composed == decomposed)
        let document = SourceDocument(id: RawIdentifier("opaque/" + composed), text: composed, languageID: "cpp")
        let before = document.snapshot()
        #expect(try document.commit(decomposed))
        #expect(document.revision == 1)
        #expect(document.snapshot() != before)
        #expect(document.isDirty)
        #expect(Set([RawIdentifier(composed), RawIdentifier(decomposed)]).count == 2)
        let exhausted = SourceDocument(text: "old", languageID: "cpp", revision: .max)
        #expect(throws: DocumentError.revisionExhausted) { try exhausted.commit("new") }
        #expect(exhausted.revision == UInt64.max)
        #expect(exhausted.text == "old")
    }

    @Test func coordinatedBOMCRLFNULRoundTripAndDiskConflict() async throws {
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: folder) }
        let source = folder.appendingPathComponent("原始 🙂.cpp")
        let copy = folder.appendingPathComponent("副本.cpp")
        let data = Data("\u{feff}// 中文🙂e\u{301}\r\n\0\treturn 0;\r\n".utf8)
        try data.write(to: source)
        let loaded = try await Task.detached { try CoordinatedSourceFile.read(source, maximumBytes: data.count) }.value
        #expect(Data(loaded.text.utf8) == data)
        #expect(loaded.diskDigest == SourceDigest.sha256(data))
        let document = SourceDocument(text: loaded.text, languageID: "cpp")
        let snapshot = document.snapshot()
        let digest = try await Task.detached {
            try CoordinatedSourceFile.write(snapshot, to: copy, expectedDiskDigest: nil)
        }.value
        #expect(try Data(contentsOf: copy) == data)
        #expect(digest == loaded.diskDigest)
        let external = Data("externally changed\n".utf8)
        try external.write(to: source)
        do {
            _ = try await Task.detached { try CoordinatedSourceFile.write(snapshot, to: source, expectedDiskDigest: loaded.diskDigest) }.value
            Issue.record("Expected a source conflict")
        } catch { #expect(error as? DocumentError == .diskConflict) }
        #expect(try Data(contentsOf: source) == external)
        do {
            _ = try await Task.detached { try CoordinatedSourceFile.read(copy, maximumBytes: data.count - 1) }.value
            Issue.record("Expected the configured source size bound")
        } catch { #expect((error as NSError).code == CocoaError.fileReadTooLarge.rawValue) }
        let alias = folder.appendingPathComponent("recovery-alias.json")
        try FileManager.default.linkItem(at: copy, to: alias)
        do {
            _ = try await Task.detached {
                try CoordinatedSourceFile.write(snapshot, to: alias, expectedDiskDigest: digest, reservedPaths: [copy.path])
            }.value
            Issue.record("Expected a reserved recovery alias to be rejected")
        } catch { #expect(error as? DocumentError == .reservedRecoveryPath) }
        #expect(try Data(contentsOf: copy) == data)
    }
}
