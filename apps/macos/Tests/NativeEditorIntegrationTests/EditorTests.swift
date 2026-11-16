import AppKit
import SwiftUI
import Testing
import NativeEditorModel
import STTextView
import NativeCoreBridge
@testable import CPEditorNative

@MainActor @Suite(.serialized) struct EditorTests {
    @Test func runnerCompilesUnsavedNativeBufferAndMarksEditedSourceOld() async throws {
        _ = NSApplication.shared
        let profile = try LanguageProfile.load(id: "cpp")
        let source = "#include <iostream>\nint main(){int a,b;if(std::cin>>a>>b)std::cout<<a+b<<'\\n';}\n"
        let document = SourceDocument(text: source, editorKind: .stTextView, languageID: profile.id, saved: false)
        let session = try NativeEditorSession(document: document, profile: profile)
        defer { session.dispose() }
        session.testCases = [TestCaseInput(input: "1 2\n", expected: "3\n")]
        let runner = RunController()
        #expect(runner.errorMessage == nil)
        runner.runAll(session)
        #expect(runner.isRunning)
        let native = try #require(session.stView)
        native.textSelection = NSRange(location: source.utf16.count, length: 0)
        native.insertText("// changed after Run All\n", replacementRange: NSRange(location: NSNotFound, length: 0))
        session.capture()
        await runner.waitForCurrentRun()
        let result = try #require(runner.result)
        #expect(result.tests.first?.verdict == "AC")
        #expect(!result.matches(document.snapshot()))
        #expect(document.isDirty)
        #expect(document.url == nil)
        #expect(document.text.hasSuffix("// changed after Run All\n"))
        await runner.shutdown()
    }
    private func textView(in view: NSView) -> NSTextView? {
        if let text = view as? NSTextView, text.isEditable { return text }
        for child in view.subviews {
            if let text = textView(in: child) { return text }
        }
        return nil
    }

    @Test func codeEditorUndoThroughActualWorkspaceResponderChain() async throws {
        _ = NSApplication.shared
        let workspace = Workspace(arguments: ["--editor", "codeeditorview"])
        let session = try #require(workspace.activeSession)
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 900, height: 600),
                              styleMask: [.titled, .resizable], backing: .buffered, defer: false)
        let host = NSHostingView(rootView: WorkspaceView(workspace: workspace))
        window.contentView = host
        window.makeKeyAndOrderFront(nil)
        host.layoutSubtreeIfNeeded()
        let view = try #require(textView(in: session.container))
        window.makeFirstResponder(view)
        // 与真实 UI 一样走 NSTextView.paste；暂存并恢复系统剪贴板。
        let pasteboard = NSPasteboard.general
        let backup = (pasteboard.pasteboardItems ?? []).map { source in
            let item = NSPasteboardItem()
            for type in source.types { if let data = source.data(forType: type) { item.setData(data, forType: type) } }
            return item
        }
        defer {
            pasteboard.clearContents()
            pasteboard.writeObjects(backup)
            window.orderOut(nil)
            window.contentView = nil
            workspace.sessions.forEach { $0.dispose() }
        }
        pasteboard.clearContents()
        pasteboard.setString("// Native test: 中文 😀 e\u{301}\n", forType: .string)
        view.paste(nil)
        await Task.yield()
        host.layoutSubtreeIfNeeded()
        session.capture()
        #expect(documentText(session) == "// Native test: 中文 😀 e\u{301}\n")
        #expect(view.undoManager === session.container.history)
        #expect(session.container.history.canUndo)
        let undo = NSMenuItem(title: "Undo", action: #selector(EditorContainer.undo(_:)), keyEquivalent: "z")
        let redo = NSMenuItem(title: "Redo", action: #selector(EditorContainer.redo(_:)), keyEquivalent: "Z")
        // XCTest 宿主不是前台应用，NSApp.keyWindow 为 nil。按真实 firstResponder
        // 的公开 nextResponder 链解析菜单接收者，再走原生验证及 tryToPerform 分发。
        let receiver = try #require(commandReceiver(for: undo.action!, startingAt: window.firstResponder))
        #expect(receiver === session.container)
        undo.target = receiver
        redo.target = receiver
        let menu = NSMenu(title: "Edit")
        menu.addItem(undo)
        menu.addItem(redo)
        menu.update()
        #expect(undo.isEnabled)
        #expect(view.tryToPerform(undo.action!, with: undo))
        await Task.yield()
        session.capture()
        #expect(documentText(session).isEmpty)
        menu.update()
        #expect(redo.isEnabled)
        #expect(view.tryToPerform(redo.action!, with: redo))
        await Task.yield()
        session.capture()
        #expect(documentText(session) == "// Native test: 中文 😀 e\u{301}\n")
        workspace.openCopyInAlternativeEditor()
        await Task.yield()
        host.layoutSubtreeIfNeeded()
        workspace.selection = session.document.id
        await Task.yield()
        host.layoutSubtreeIfNeeded()
        let returnedView = try #require(textView(in: session.container))
        #expect(returnedView === view)
        window.makeFirstResponder(returnedView)
        #expect(commandReceiver(for: undo.action!, startingAt: window.firstResponder) === session.container)
        menu.update()
        #expect(undo.isEnabled)
        #expect(returnedView.tryToPerform(undo.action!, with: undo))
        await Task.yield()
        session.capture()
        #expect(documentText(session).isEmpty)
    }

    private func commandReceiver(for action: Selector, startingAt initial: NSResponder?) -> NSResponder? {
        var responder = initial
        while let current = responder {
            if current.responds(to: action) { return current }
            responder = current.nextResponder
        }
        return nil
    }

    private func documentText(_ session: NativeEditorSession) -> String { session.document.text }

    @Test func codeEditorViewRetainsUndoAndExcludesPreedit() async throws {
        _ = NSApplication.shared
        let profile = try LanguageProfile.load(id: "cpp")
        let document = SourceDocument(text: "// ", editorKind: .codeEditorView, languageID: profile.id)
        let session = try NativeEditorSession(document: document, profile: profile)
        defer { session.dispose() }
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 640, height: 400),
                              styleMask: [.titled], backing: .buffered, defer: false)
        window.contentView = session.container
        window.layoutIfNeeded()
        session.container.layoutSubtreeIfNeeded()
        let view = try #require(textView(in: session.container))
        window.makeFirstResponder(view)
        view.setSelectedRange(NSRange(location: 3, length: 0))
        #expect(view.undoManager === session.container.history)
        session.container.history.groupsByEvent = false
        session.container.history.beginUndoGrouping()
        view.setMarkedText("zhong", selectedRange: NSRange(location: 5, length: 0),
                           replacementRange: NSRange(location: NSNotFound, length: 0))
        await Task.yield()
        #expect(view.hasMarkedText())
        #expect(document.text == "// ")
        #expect(document.revision == 0)
        view.insertText("中文🙂", replacementRange: NSRange(location: NSNotFound, length: 0))
        session.container.history.endUndoGrouping()
        await Task.yield()
        #expect(!view.hasMarkedText())
        #expect(document.text == "// 中文🙂")
        #expect(document.revision == 1)
        // 模拟标签切换时移走容器，再装回同一个实例。
        window.contentView = NSView()
        window.contentView = session.container
        window.makeFirstResponder(view)
        session.container.history.undo()
        session.capture()
        #expect(document.text == "// ")
        session.container.history.redo()
        session.capture()
        #expect(document.text == "// 中文🙂")
        window.contentView = nil
    }

    @Test func nativeSTTextViewUnicodeUndoAndCommittedComposition() throws {
        _ = NSApplication.shared
        let profile = try LanguageProfile.load(id: "cpp")
        let document = SourceDocument(text: "// ", editorKind: .stTextView, languageID: profile.id)
        let session = try NativeEditorSession(document: document, profile: profile)
        defer { session.dispose() }
        let view = try #require(session.stView)
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 640, height: 400),
                              styleMask: [.titled], backing: .buffered, defer: false)
        window.contentView = session.container
        window.makeFirstResponder(view)
        session.container.layoutSubtreeIfNeeded()
        view.textSelection = NSRange(location: 3, length: 0)
        session.container.history.groupsByEvent = false
        session.container.history.beginUndoGrouping()
        view.setMarkedText("zhong", selectedRange: NSRange(location: 5, length: 0), replacementRange: .init(location: NSNotFound, length: 0))
        session.capture()
        #expect(view.hasMarkedText())
        #expect(document.text == "// ")
        #expect(document.revision == 0)
        view.insertText("中文🙂", replacementRange: .init(location: NSNotFound, length: 0))
        session.container.history.endUndoGrouping()
        session.capture()
        #expect(!view.hasMarkedText())
        #expect(document.text == "// 中文🙂")
        #expect(document.revision == 1)
        #expect(view.undoManager === session.container.history)
        session.container.history.undo()
        session.capture()
        #expect(document.text == "// ")
        session.container.history.redo()
        session.capture()
        #expect(document.text == "// 中文🙂")
        window.contentView = nil
    }

    @Test func bothFixedComponentsCanCreateSessions() throws {
        _ = NSApplication.shared
        let profile = try LanguageProfile.load(id: "cpp")
        for kind in EditorKind.allCases {
            let text = "#include <iostream>\r\n// 中文🙂e\u{301}\t\nint main() { return 0; }\n"
            let document = SourceDocument(text: text,
                                          editorKind: kind, languageID: profile.id)
            let session = try NativeEditorSession(document: document, profile: profile)
            #expect(session.document.editorKind == kind)
            #expect(session.container.subviews.count == 1)
            let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 640, height: 400),
                                  styleMask: [.titled], backing: .buffered, defer: false)
            window.contentView = session.container
            window.layoutIfNeeded()
            session.container.layoutSubtreeIfNeeded()
            if kind == .codeEditorView {
                let native = try #require(textView(in: session.container))
                #expect(native.string == text)
            } else { #expect(session.stView?.text == text) }
            session.capture()
            #expect(!document.isDirty)
            #expect(document.revision == 0)
            session.update(wrapText: true, showMinimap: false)
            window.contentView = nil
            session.dispose()
        }
    }
}
