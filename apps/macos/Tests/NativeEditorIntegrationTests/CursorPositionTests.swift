import AppKit
import Testing
import NativeEditorModel
@testable import CPEditorNative

@Suite(.serialized) @MainActor struct CursorPositionTests {
    private func textView(in view: NSView) -> NSTextView? {
        if let text = view as? NSTextView { return text }
        return view.subviews.lazy.compactMap { textView(in: $0) }.first
    }

    private func expectPosition(_ session: NativeEditorSession) {
        let text = session.document.text
        let prefix = text.utf16.prefix(max(0, session.selection.location))
        let position = session.document.position(atUTF16Offset: session.selection.location)
        #expect(position.line == prefix.filter { $0 == 10 }.count + 1)
        #expect(position.column == prefix.reversed().prefix { $0 != 10 }.count + 1)
    }

    @Test(arguments: EditorKind.allCases) func nativeSelectionAndMarkedTextKeepCommittedPositionSemantics(editor: EditorKind) async throws {
        _ = NSApplication.shared
        let profile = try LanguageProfile.load(id: "cpp")
        let document = SourceDocument(text: "// 中文🙂e\u{301}\r\n\treturn 0;\n", editorKind: editor, languageID: profile.id)
        let session = try NativeEditorSession(document: document, profile: profile)
        defer { session.dispose() }
        let window = NSWindow(contentRect: .init(x: 0, y: 0, width: 640, height: 400), styleMask: [.titled], backing: .buffered, defer: false)
        window.contentView = session.container
        window.layoutIfNeeded(); session.container.layoutSubtreeIfNeeded()
        defer { window.contentView = nil; window.orderOut(nil) }
        let native: NSView = editor == .stTextView ? try #require(session.stView) : try #require(textView(in: session.container))
        window.makeFirstResponder(native)
        let before = document.snapshot()
        for offset in [0, 3, 9, 12, document.text.utf16.count] {
            if let view = session.stView { view.textSelection = .init(location: offset, length: 0) }
            else { (native as! NSTextView).setSelectedRange(.init(location: offset, length: 0)) }
            session.capture(); expectPosition(session)
        }
        #expect(document.snapshot() == before)
        let end = document.text.utf16.count
        if let view = session.stView {
            view.textSelection = .init(location: end, length: 0)
            view.setMarkedText("临时拼音", selectedRange: .init(location: 4, length: 0), replacementRange: .init(location: NSNotFound, length: 0))
        } else {
            let view = native as! NSTextView
            view.setSelectedRange(.init(location: end, length: 0))
            view.setMarkedText("临时拼音", selectedRange: .init(location: 4, length: 0), replacementRange: .init(location: NSNotFound, length: 0))
        }
        await Task.yield(); session.capture()
        #expect(session.isComposing); #expect(document.snapshot() == before)
        expectPosition(session)
        if let view = session.stView { view.insertText("提交🙂\n", replacementRange: .init(location: NSNotFound, length: 0)) }
        else { (native as! NSTextView).insertText("提交🙂\n", replacementRange: .init(location: NSNotFound, length: 0)) }
        await Task.yield(); session.capture()
        #expect(!session.isComposing); #expect(document.revision == before.revision + 1)
        #expect(document.text.hasSuffix("提交🙂\n"))
        expectPosition(session)
        #expect(document.position(atUTF16Offset: Int.max).line == 4)
    }
}
