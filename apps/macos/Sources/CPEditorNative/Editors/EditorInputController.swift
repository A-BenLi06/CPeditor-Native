import AppKit
import NativeEditorModel
import STTextView

/// One input adapter for both editor engines. Native text APIs own mutations and undo.
@MainActor final class EditorInputController {
    private weak var container: EditorContainer?
    private let rules: CodeEditingRules
    private var monitor: Any?
    var didEdit: (() -> Void)?

    init(container: EditorContainer, profile: LanguageProfile) {
        self.container = container
        rules = CodeEditingRules(profile: profile)
        monitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] event in
            let handled = MainActor.assumeIsolated { self?.handle(event) == true }
            return handled ? nil : event
        }
    }

    func dispose() {
        if let monitor { NSEvent.removeMonitor(monitor) }
        monitor = nil
        didEdit = nil
    }

    func handle(_ event: NSEvent) -> Bool {
        guard let container, container.allowsEditingCommands,
              let view = event.window?.firstResponder as? NSView, view.isDescendant(of: container),
              event.modifierFlags.intersection([.command, .control, .option]).isEmpty else { return false }
        let action: CodeEditingRules.Action
        switch event.characters {
        case "\r", "\n": action = .newline
        case "\t", "\u{19}": action = event.modifierFlags.contains(.shift) ? .outdent : .indent
        case "\u{7f}": action = .backspace
        case .some(let text): action = .insert(text)
        case nil: return false
        }
        return apply(action, to: view)
    }

    /// Marked text and multi-cursor edits stay with the native input system.
    @discardableResult func apply(_ action: CodeEditingRules.Action, to view: NSView) -> Bool {
        guard container?.allowsEditingCommands == true else { return false }
        let text: String, selection: NSRange
        let insert: (String, NSRange) -> Void
        let select: (NSRange) -> Void
        let breakCoalescing: () -> Void
        if let view = view as? NSTextView {
            guard view.isEditable, !view.hasMarkedText(), view.selectedRanges.count == 1 else { return false }
            text = view.string; selection = view.selectedRange()
            insert = { view.insertText($0, replacementRange: $1) }
            select = { view.setSelectedRange($0); view.scrollRangeToVisible($0) }
            breakCoalescing = { view.breakUndoCoalescing() }
        } else if let view = view as? STTextView {
            guard view.isEditable, !view.hasMarkedText(), view.textLayoutManager.textSelections.flatMap(\.textRanges).count == 1 else { return false }
            text = view.text ?? ""; selection = view.selectedRange()
            insert = { view.insertText($0, replacementRange: $1) }
            select = { view.textSelection = $0 }
            breakCoalescing = { view.breakUndoCoalescing() }
        } else { return false }
        guard let edit = rules.edit(action, text: text, selection: selection) else { return false }
        if edit.range.length != 0 || !edit.replacement.isEmpty {
            breakCoalescing()
            view.undoManager?.beginUndoGrouping()
            insert(edit.replacement, edit.range)
            select(edit.selection)
            view.undoManager?.endUndoGrouping()
            breakCoalescing()
        } else { select(edit.selection) }
        didEdit?()
        return true
    }
}
