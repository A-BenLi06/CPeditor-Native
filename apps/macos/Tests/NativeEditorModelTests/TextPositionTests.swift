import Foundation
import Observation
import Testing
@testable import NativeEditorModel

@MainActor struct TextPositionTests {
    private final class ChangeSignal: @unchecked Sendable {
        private let lock = NSLock()
        private var changed = false
        func signal() { lock.withLock { changed = true } }
        var value: Bool { lock.withLock { changed } }
    }

    private func reference(_ text: String, _ offset: Int) -> TextPosition {
        let prefix = text.utf16.prefix(max(0, offset))
        return TextPosition(line: prefix.filter { $0 == 10 }.count + 1,
                            column: prefix.reversed().prefix { $0 != 10 }.count + 1)
    }

    @Test func everyUTF16OffsetPreservesLFAndColumnSemantics() {
        let fixtures = ["", "\n", "\r\n\r\n", "a\rb\u{2028}c\u{2029}d", "中🙂e\u{301}\r\n\0\treturn 0;\n", "\n\nlast",
                        String(repeating: "a", count: 4096) + "\n🙂\n"]
        for text in fixtures {
            let index = TextLineIndex(text)
            for offset in -2...(text.utf16.count + 2) {
                #expect(index.position(atUTF16Offset: offset) == reference(text, offset))
            }
            #expect(index.position(atUTF16Offset: Int.min) == reference(text, 0))
            #expect(index.position(atUTF16Offset: Int.max) == reference(text, text.utf16.count))
        }
    }

    @Test func sparseBitSegmentsMatchEveryBoundaryAndDenseEmptyLines() {
        let fixtures = [String(repeating: "\n", count: 257),
                        String(repeating: "a", count: 63) + "\n" + String(repeating: "b", count: 191) + "\n\n",
                        String(repeating: "a", count: 64) + "\n" + String(repeating: "b", count: 62) + "\n",
                        "\n" + String(repeating: "中🙂", count: 100) + "\n" + String(repeating: "c", count: 90)]
        for text in fixtures {
            let index = TextLineIndex(text)
            for offset in -1...(text.utf16.count + 1) {
                #expect(index.position(atUTF16Offset: offset) == reference(text, offset))
            }
        }
    }

    @Test func positionTracksExactCommittedTextAndDocumentIdentity() throws {
        let document = SourceDocument(id: RawIdentifier("same-id"), text: "é\nold", languageID: "cpp", revision: .max - 1)
        let other = SourceDocument(id: document.id, text: "other\n\ntext", languageID: "cpp", revision: document.revision)
        #expect(document.position(atUTF16Offset: 1).column == 2)
        #expect(other.position(atUTF16Offset: Int.max).line == 3)
        let saved = document.snapshot()
        try document.commit("e\u{301}\nnew\n")
        #expect(document.revision == .max)
        #expect(document.position(atUTF16Offset: 2) == TextPosition(line: 1, column: 3))
        #expect(document.position(atUTF16Offset: Int.max) == TextPosition(line: 3, column: 1))
        #expect(document.position(atUTF16Offset: 3) == TextPosition(line: 2, column: 1))
        #expect(other.position(atUTF16Offset: Int.max) == TextPosition(line: 3, column: 5))
        #expect(throws: DocumentError.revisionExhausted) { try document.commit(saved.text) }
        #expect(document.position(atUTF16Offset: Int.max).line == 3)
    }

    @Test func cachedPositionRetainsRevisionObservationWithoutReadSideEffects() throws {
        let document = SourceDocument(text: "old\n", languageID: "cpp")
        let signal = ChangeSignal()
        let before = document.snapshot()
        withObservationTracking {
            #expect(document.position(atUTF16Offset: 4) == TextPosition(line: 2, column: 1))
        } onChange: { signal.signal() }
        for offset in 0...10 { _ = document.position(atUTF16Offset: offset) }
        #expect(!signal.value)
        #expect(document.snapshot() == before)
        try document.commit("new\n\n")
        #expect(signal.value)
        #expect(document.position(atUTF16Offset: Int.max) == TextPosition(line: 3, column: 1))
    }
}
