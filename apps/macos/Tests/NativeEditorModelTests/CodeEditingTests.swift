import Foundation
import Testing
@testable import NativeEditorModel

struct CodeEditingTests {
    private func rules() throws -> CodeEditingRules { try CodeEditingRules(profile: .load(id: "cpp")) }

    @Test func wrapsUnicodeSelectionAndKeepsItSelected() throws {
        let text = "🙂变量"
        let edit = try #require(try rules().edit(.insert("("), text: text, selection: NSRange(location: 0, length: text.utf16.count)))
        #expect(edit.replacement == "(🙂变量)")
        #expect(edit.selection == NSRange(location: 1, length: text.utf16.count))
    }

    @Test func pairsSkipAndDelete() throws {
        let rules = try rules()
        for (open, close) in [("(", ")"), ("[", "]"), ("{", "}"), ("\"", "\""), ("'", "'")] {
            #expect(rules.edit(.insert(open), text: "", selection: NSRange(location: 0, length: 0))?.replacement == open + close)
            let skip = rules.edit(.insert(close), text: open + close, selection: NSRange(location: 1, length: 0))
            #expect(skip?.replacement == "")
            #expect(skip?.selection.location == 2)
            #expect(rules.edit(.backspace, text: open + close, selection: NSRange(location: 1, length: 0))?.range == NSRange(location: 0, length: 2))
        }
    }

    @Test func commentsStringsEscapesSeparatorsAndPasteAreUntouched() throws {
        let rules = try rules()
        for text in ["// hi ", "/* hi ", "\"hello ", "R\"tag(raw \" text "] {
            #expect(rules.edit(.insert("("), text: text, selection: NSRange(location: text.utf16.count, length: 0)) == nil)
        }
        #expect(rules.edit(.insert("'"), text: "100", selection: NSRange(location: 3, length: 0)) == nil)
        #expect(rules.edit(.insert("\""), text: "\"a\\\"", selection: NSRange(location: 3, length: 0)) == nil)
        #expect(rules.edit(.insert("(pasted)"), text: "", selection: NSRange(location: 0, length: 0)) == nil)
        let afterRaw = "R\"tag(raw \" text)tag\"; "
        #expect(rules.edit(.insert("("), text: afterRaw, selection: NSRange(location: afterRaw.utf16.count, length: 0))?.replacement == "()")
    }

    @Test func newlineAndBlockIndentationPreserveCRLF() throws {
        let rules = try rules()
        let text = "// 🙂\r\n    if (true) {}"
        let location = text.utf16.count - 1
        let edit = try #require(rules.edit(.newline, text: text, selection: NSRange(location: location, length: 0)))
        #expect(edit.replacement == "\r\n        \r\n    ")
        #expect(edit.selection.location == location + 10)
        let block = "one\r\ntwo\r\nnext"
        let indent = try #require(rules.edit(.indent, text: block, selection: NSRange(location: 0, length: 10)))
        #expect(indent.replacement == "    one\r\n    two\r\n")
        let indented = (block as NSString).replacingCharacters(in: indent.range, with: indent.replacement)
        let outdent = try #require(rules.edit(.outdent, text: indented, selection: indent.selection))
        #expect((indented as NSString).replacingCharacters(in: outdent.range, with: outdent.replacement) == block)
    }

    @Test func closingCharactersOnlySkipTheirMatchingOpeningContext() throws {
        let rules = try rules()
        #expect(rules.edit(.insert(")"), text: ")", selection: NSRange(location: 0, length: 0)) == nil)
        #expect(rules.edit(.insert(")"), text: "([)", selection: NSRange(location: 2, length: 0)) == nil)
        #expect(rules.edit(.insert("\""), text: "\"word\"", selection: NSRange(location: 0, length: 0))?.replacement == "\"\"")
        #expect(rules.edit(.backspace, text: "\"\"\"\"", selection: NSRange(location: 2, length: 0)) == nil)
    }

    @Test func hexSeparatorsAndIncompleteStringsDoNotDisableLaterCodePairing() throws {
        let rules = try rules()
        for text in ["auto n = 0xAB'CD; f", "auto s = \"unfinished\ncall", "auto c = '\ncall"] {
            #expect(rules.edit(.insert("("), text: text, selection: NSRange(location: text.utf16.count, length: 0))?.replacement == "()")
        }
        let continued = "\"text\\\ninside "
        #expect(rules.edit(.insert("("), text: continued, selection: NSRange(location: continued.utf16.count, length: 0)) == nil)
    }

    @Test func indentingSingleLineSelectionPreservesCodeAndUndoableRange() throws {
        let edit = try #require(try rules().edit(.indent, text: "  value", selection: NSRange(location: 2, length: 5)))
        #expect(edit.range == NSRange(location: 0, length: 7))
        #expect(edit.replacement == "      value")
        #expect(edit.selection == NSRange(location: 6, length: 5))
    }

    @Test func newlineUsesLocalLineEndingIncludingCRFiles() throws {
        let rules = try rules()
        for (text, location, expected) in [("one\rtwo", 2, "\r"), ("one\r\ntwo\nthree", 8, "\n")] {
            #expect(rules.edit(.newline, text: text, selection: NSRange(location: location, length: 0))?.replacement == expected)
        }
    }

    @Test func outdentMapsSelectionEndingInsideRemovedWhitespace() throws {
        let edit = try #require(try rules().edit(.outdent, text: "first\n    second", selection: NSRange(location: 0, length: 7)))
        #expect(edit.replacement == "first\nsecond")
        #expect(edit.selection == NSRange(location: 0, length: 6))
    }
}
