import AppKit
import SwiftUI
import STTextView
import NativeEditorModel
import Observation
import NativeCoreBridge

/// 每个文档一个实例，标签切换只移动其容器，不重建原生 buffer。
@MainActor @Observable final class NativeEditorSession: NSObject, @preconcurrency STTextViewDelegate {
    let document: SourceDocument
    let container = EditorContainer()
    private(set) var isComposing = false
    private(set) var captureFailure: DocumentError?
    private(set) var selection = NSRange(location: 0, length: 0)
    private(set) var committedSelection = NSRange(location: 0, length: 0)
    private(set) var interactionEnabled = true
    var testCases = [TestCaseInput()]
    var toolchainProfileID: RawIdentifier?
    var preservedPreferences: [NativePreferences] = []
    @ObservationIgnored private var codeState: CodeEditorState?
    @ObservationIgnored private var codeHost: NSHostingView<CodeEditorSurface>?
    @ObservationIgnored private weak var codeView: NSTextView?
    @ObservationIgnored private(set) var stView: STTextView?
    @ObservationIgnored private var observers: [NSObjectProtocol] = []
    @ObservationIgnored private var pendingCapture: Task<Void, Never>?
    @ObservationIgnored private let highlighter: LexicalHighlighter

    init(document: SourceDocument, profile: LanguageProfile) throws {
        self.document = document
        highlighter = try LexicalHighlighter(profile: profile)
        super.init()

        switch document.editorKind {
        case .codeEditorView:
            let state = try CodeEditorState(text: document.text, profile: profile)
            codeState = state
            let host = NSHostingView(rootView: CodeEditorSurface(state: state))
            codeHost = host
            container.install(host)
            for name in [NSText.didBeginEditingNotification, NSText.didChangeNotification,
                         NSTextView.didChangeSelectionNotification] {
                observers.append(NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) { [weak self] notification in
                    guard let view = notification.object as? NSTextView else { return }
                    MainActor.assumeIsolated {
                        guard let self, view.isDescendant(of: self.container), view.isEditable || self.codeView === view else { return }
                        self.codeView = view
                        // AppKit 在 setMarkedText 的内部替换步骤发通知，此时 marked range
                        // 可能尚未建立。等当前原生事件调用栈完成后再观察输入事务结果。
                        self.pendingCapture?.cancel()
                        self.pendingCapture = Task { @MainActor [weak self] in
                            guard !Task.isCancelled, let self else { return }
                            self.capture()
                            self.pendingCapture = nil
                        }
                    }
                })
            }
        case .stTextView:
            let scroll = STTextView.scrollableTextView()
            guard let view = scroll.documentView as? STTextView else { throw CocoaError(.coderInvalidValue) }
            stView = view
            view.font = NSFont.monospacedSystemFont(ofSize: NSFont.systemFontSize, weight: .regular)
            view.showsLineNumbers = true
            view.highlightSelectedLine = true
            view.isEditable = true
            view.allowsUndo = true
            view.text = document.text
            view.textDelegate = self
            container.install(scroll)
            highlight()
        }
        container.history.removeAllActions()
    }

    func dispose() {
        pendingCapture?.cancel()
        pendingCapture = nil
        observers.forEach(NotificationCenter.default.removeObserver)
        observers.removeAll()
        stView?.textDelegate = nil
        container.history.removeAllActions()
    }

    @discardableResult func capture() -> Bool {
        if let view = stView {
            isComposing = view.hasMarkedText()
            selection = view.selectedRange()
            guard !isComposing else { return false }
            committedSelection = selection
            return commit(view.text ?? "")
        }
        if let view = codeView {
            isComposing = view.hasMarkedText()
            selection = view.selectedRange()
            guard !isComposing else { return false }
            committedSelection = selection
            return commit(view.string)
        }
        return false
    }

    func restoreSelection(_ range: NSRange) throws {
        _ = try TextCoordinateIndex(document.text).utf8Range(for: range)
        selection = range
        committedSelection = range
        codeState?.position.selections = [range]
        codeView?.setSelectedRange(range)
        stView?.textSelection = range
    }

    func setInteractionEnabled(_ enabled: Bool) {
        interactionEnabled = enabled
        container.allowsEditingCommands = enabled
        let editable = enabled && document.revision < UInt64.max
        stView?.isEditable = editable
        if codeView == nil {
            func find(_ view: NSView) -> NSTextView? {
                if let text = view as? NSTextView, text.isEditable { return text }
                return view.subviews.lazy.compactMap(find).first
            }
            codeView = find(container)
        }
        codeView?.isEditable = editable
    }

    func currentNativeText() throws -> String {
        if let view = stView {
            guard !view.hasMarkedText() else { throw DocumentError.invalidRange }
            return view.text ?? ""
        }
        if let view = codeView {
            guard !view.hasMarkedText() else { throw DocumentError.invalidRange }
            return view.string
        }
        return document.text
    }

    /// 在一次性应用恢复候选之前，构造并检查组件真正持有的原生 buffer。
    func validateRestoredBuffer() throws {
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 640, height: 400),
                              styleMask: [.titled], backing: .buffered, defer: false)
        window.contentView = container
        defer { window.contentView = nil; window.orderOut(nil) }
        window.layoutIfNeeded()
        container.layoutSubtreeIfNeeded()
        if document.editorKind == .codeEditorView {
            func find(_ view: NSView) -> NSTextView? {
                if let text = view as? NSTextView, text.isEditable { return text }
                return view.subviews.lazy.compactMap(find).first
            }
            guard let view = find(container), view.string.utf8.elementsEqual(document.text.utf8) else {
                throw DocumentError.invalidUTF8
            }
            codeView = view
        } else if !(stView?.text ?? "").utf8.elementsEqual(document.text.utf8) {
            throw DocumentError.invalidUTF8
        }
    }

    private func commit(_ text: String) -> Bool {
        do {
            let changed = try document.commit(text)
            captureFailure = nil
            if document.revision == UInt64.max { stView?.isEditable = false; codeView?.isEditable = false }
            return changed
        } catch { captureFailure = error as? DocumentError; return false }
    }

    func update(wrapText: Bool, showMinimap: Bool) {
        codeState?.wrapText = wrapText
        codeState?.showMinimap = showMinimap
        stView?.isHorizontallyResizable = !wrapText
    }

    func restoreFocus() -> Bool {
        guard let window = container.window else { return false }
        if let stView { return window.makeFirstResponder(stView) }
        if let codeView { return window.makeFirstResponder(codeView) }
        return false
    }

    func undoManager(for textView: STTextView) -> UndoManager? { container.history }

    func textViewDidChangeText(_ notification: Notification) {
        capture()
        if !isComposing { highlight() }
    }

    func textViewDidChangeSelection(_ notification: Notification) { capture() }

    private func highlight() {
        guard let view = stView, !view.hasMarkedText() else { return }
        let text = view.text ?? ""
        let range = NSRange(location: 0, length: text.utf16.count)
        view.addRenderingAttributes([.foregroundColor: NSColor.textColor], range: range)
        for token in highlighter.tokens(in: text) {
            let color: NSColor
            switch token.kind {
            case .keyword: color = .systemPurple
            case .string: color = .systemRed
            case .number: color = .systemBlue
            case .comment: color = .secondaryLabelColor
            case .directive: color = .systemOrange
            }
            view.addRenderingAttributes([.foregroundColor: color], range: token.range)
        }
    }
}

struct NativeEditorView: NSViewRepresentable {
    let session: NativeEditorSession
    let wrapText: Bool
    let showMinimap: Bool

    func makeNSView(context: Context) -> EditorContainer { session.container }
    func updateNSView(_ nsView: EditorContainer, context: Context) {
        session.update(wrapText: wrapText, showMinimap: showMinimap)
    }
}

/// 标签切换保留全部原生视图的 window 关系，避免 NSHostingView 在卸载时
/// 重建内部 CodeEditor representable 及 NSTextView。
struct NativeEditorDeck: NSViewRepresentable {
    let sessions: [NativeEditorSession]
    let selectedID: RawIdentifier?
    let wrapText: Bool
    let showMinimap: Bool

    func makeNSView(context: Context) -> DeckView { DeckView() }
    func updateNSView(_ nsView: DeckView, context: Context) {
        nsView.reconcile(sessions: sessions, selectedID: selectedID,
                         wrapText: wrapText, showMinimap: showMinimap)
    }

    @MainActor final class DeckView: NSView {
        private var selection: RawIdentifier?

        func reconcile(sessions: [NativeEditorSession], selectedID: RawIdentifier?, wrapText: Bool, showMinimap: Bool) {
            let owned = Set(sessions.map { ObjectIdentifier($0.container) })
            for view in subviews where !owned.contains(ObjectIdentifier(view)) { view.removeFromSuperview() }
            for session in sessions {
                let view = session.container
                if view.superview !== self {
                    view.translatesAutoresizingMaskIntoConstraints = false
                    addSubview(view)
                    NSLayoutConstraint.activate([
                        view.leadingAnchor.constraint(equalTo: leadingAnchor),
                        view.trailingAnchor.constraint(equalTo: trailingAnchor),
                        view.topAnchor.constraint(equalTo: topAnchor),
                        view.bottomAnchor.constraint(equalTo: bottomAnchor)
                    ])
                }
                view.isHidden = session.document.id != selectedID
                session.update(wrapText: wrapText, showMinimap: showMinimap)
            }
            if selection != selectedID {
                if let responder = window?.firstResponder as? NSView,
                   sessions.contains(where: { $0.container.isHidden && responder.isDescendant(of: $0.container) }) {
                    if !(sessions.first(where: { $0.document.id == selectedID })?.restoreFocus() ?? false) {
                        window?.makeFirstResponder(nil)
                    }
                }
                selection = selectedID
            }
        }
    }
}
