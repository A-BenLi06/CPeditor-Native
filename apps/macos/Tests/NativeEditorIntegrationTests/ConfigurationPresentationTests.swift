import AppKit
import Observation
import SwiftUI
import Testing
import NativeCoreBridge
@testable import CPEditorNative

@MainActor @Observable private final class PresentationFixture {
    var draft: ConfigurationJSONDraft?
    private(set) var applicationAttempts: [String] = []
    let runner = RunController()

    func present(_ kind: ConfigurationJSONDraft.Kind) throws {
        let text = switch kind {
        case .checker: try runner.checkerDraft()
        case .toolchain: try runner.selectedProfile!.jsonText()
        }
        draft = ConfigurationJSONDraft(kind: kind, jsonText: text)
    }

    func apply(_ text: String, kind: ConfigurationJSONDraft.Kind) throws {
        applicationAttempts.append(text)
        switch kind {
        case .checker: try runner.applyChecker(text)
        case .toolchain: try runner.applyProfile(text)
        }
    }
}

private struct PresentationHost: View {
    @Bindable var fixture: PresentationFixture

    var body: some View {
        Color.clear.frame(width: 800, height: 600)
            .sheet(item: $fixture.draft) { draft in
                ConfigurationJSONSheet(draft: draft) { try fixture.apply($0, kind: draft.kind) }
            }
    }
}

@MainActor @Suite(.serialized) struct ConfigurationPresentationTests {
    private func textView(in view: NSView) -> NSTextView? {
        if let text = view as? NSTextView, text.isEditable { return text }
        return view.subviews.compactMap { textView(in: $0) }.first
    }

    private func servicePresentation(until predicate: () -> Bool) -> Bool {
        let deadline = Date().addingTimeInterval(3)
        repeat {
            if predicate() { return true }
            RunLoop.current.run(mode: .default, before: min(deadline, Date().addingTimeInterval(0.01)))
        } while Date() < deadline
        return predicate()
    }

    private func host(_ fixture: PresentationFixture) -> NSWindow {
        _ = NSApplication.shared
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 800, height: 600), styleMask: [.titled], backing: .buffered, defer: false)
        window.contentView = NSHostingView(rootView: PresentationHost(fixture: fixture))
        // 仅创建测试宿主的后台窗口，不激活或操作前台验收应用。
        window.orderBack(nil)
        window.contentView?.layoutSubtreeIfNeeded()
        return window
    }

    private func sendShortcut(_ characters: String, keyCode: UInt16, to window: NSWindow) -> Bool {
        let event = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: [], timestamp: 0,
            windowNumber: window.windowNumber, context: nil, characters: characters,
            charactersIgnoringModifiers: characters, isARepeat: false, keyCode: keyCode)!
        return window.performKeyEquivalent(with: event)
    }

    @Test(arguments: [ConfigurationJSONDraft.Kind.checker, .toolchain], [false, true])
    func firstPresentationUsesPayloadAndCancelReopensFreshDraft(kind: ConfigurationJSONDraft.Kind, configured: Bool) async throws {
        let fixture = PresentationFixture()
        if configured {
            try fixture.runner.applyChecker("{ \"schema_version\": 1, \"command\": { \"executable\": \"/usr/bin/true\", \"arguments\": [] } }", enabled: false)
        }
        let window = host(fixture)
        defer {
            if let sheet = window.attachedSheet { window.endSheet(sheet) }
            window.orderOut(nil); window.contentView = nil
        }
        try fixture.present(kind)
        let expected = try #require(fixture.draft?.jsonText)
        #expect(servicePresentation { window.attachedSheet?.contentView.flatMap { textView(in: $0) } != nil })
        let sheet = try #require(window.attachedSheet)
        let editor = try #require(sheet.contentView.flatMap { textView(in: $0) })
        #expect(editor.string.utf8.elementsEqual(expected.utf8))
        window.makeFirstResponder(nil)
        sheet.makeFirstResponder(editor)
        editor.selectAll(nil)
        editor.insertText("discarded draft", replacementRange: .init(location: NSNotFound, length: 0))
        #expect(sendShortcut("\u{1b}", keyCode: 53, to: sheet))
        #expect(servicePresentation { window.attachedSheet == nil && fixture.draft == nil })
        #expect(fixture.applicationAttempts.isEmpty)
        #expect(!fixture.runner.externalCheckerEnabled)
        try fixture.present(kind)
        #expect(servicePresentation { window.attachedSheet?.contentView.flatMap { textView(in: $0) } != nil })
        let reopened = try #require(window.attachedSheet?.contentView.flatMap { textView(in: $0) })
        #expect(reopened.string.utf8.elementsEqual(expected.utf8))
        await fixture.runner.shutdown()
    }

    @Test func nativeApplyKeepsInvalidDraftThenAppliesEditedConfiguration() async throws {
        let fixture = PresentationFixture()
        let original = "{ \"schema_version\": 1, \"command\": { \"executable\": \"/usr/bin/true\", \"arguments\": [] } }"
        try fixture.runner.applyChecker(original, enabled: false)
        let window = host(fixture)
        defer {
            if let sheet = window.attachedSheet { window.endSheet(sheet) }
            window.orderOut(nil); window.contentView = nil
        }
        try fixture.present(.checker)
        #expect(servicePresentation { window.attachedSheet?.contentView.flatMap { textView(in: $0) } != nil })
        let sheet = try #require(window.attachedSheet)
        let editor = try #require(sheet.contentView.flatMap { textView(in: $0) })
        sheet.makeFirstResponder(editor)
        editor.selectAll(nil)
        editor.insertText("{\"schema_version\":1.0}", replacementRange: .init(location: NSNotFound, length: 0))
        #expect(sendShortcut("\r", keyCode: 36, to: sheet))
        #expect(servicePresentation { fixture.applicationAttempts == ["{\"schema_version\":1.0}"] })
        sheet.contentView?.layoutSubtreeIfNeeded()
        #expect(editor.string == "{\"schema_version\":1.0}")
        #expect(window.attachedSheet === sheet)
        #expect(fixture.runner.externalChecker?.originalJSON == original)
        #expect(!fixture.runner.externalCheckerEnabled)
        let replacement = "{\"schema_version\":1,\"command\":{\"executable\":\"/usr/bin/false\",\"arguments\":[]}}"
        editor.selectAll(nil)
        editor.insertText(replacement, replacementRange: .init(location: NSNotFound, length: 0))
        #expect(sendShortcut("\r", keyCode: 36, to: sheet))
        #expect(servicePresentation { window.attachedSheet == nil && fixture.draft == nil })
        #expect(fixture.applicationAttempts == ["{\"schema_version\":1.0}", replacement])
        #expect(fixture.runner.externalChecker?.originalJSON == replacement)
        #expect(fixture.runner.externalCheckerEnabled)
        await fixture.runner.shutdown()
    }
}
