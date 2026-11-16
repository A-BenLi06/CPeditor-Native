import Foundation
import Testing
@testable import NativeEditorModel

@MainActor struct DocumentTests {
    @Test func stableIdentityRevisionAndSavePoint() throws {
        let doc = SourceDocument(text: "int main() {}\n", languageID: "cpp")
        let original = doc.snapshot()
        #expect(!doc.commit(original.text))
        #expect(doc.revision == 0)
        doc.commit("int main() { return 0; }\n")
        let saved = doc.snapshot()
        #expect(saved.documentID == original.documentID)
        #expect(saved.revision == 1)
        #expect(doc.isDirty)
        doc.commit("int main() { return 1; }\n")
        try doc.markSaved(at: URL(fileURLWithPath: "/tmp/main.cpp"), snapshot: saved)
        #expect(doc.isDirty)
        doc.commit(saved.text)
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
        doc.commit("changed")
        #expect(throws: DocumentError.staleRevision) {
            try doc.replacing(NSRange(location: 0, length: 1), with: "", expectedRevision: 0)
        }
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
}
