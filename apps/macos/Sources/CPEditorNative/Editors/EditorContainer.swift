import AppKit

@MainActor final class EditorContainer: NSView, NSMenuItemValidation {
    let history = UndoManager()
    override var undoManager: UndoManager? { history }

    // NSTextView 注册撤销，但默认 undo:/redo: 的命令接收者通常由文档／窗口
    // 提供。每文档容器拥有历史，也作为原生 responder 链中的命令接收者。
    @IBAction func undo(_ sender: Any?) { history.undo() }
    @IBAction func redo(_ sender: Any?) { history.redo() }

    func validateMenuItem(_ menuItem: NSMenuItem) -> Bool {
        switch menuItem.action {
        case #selector(undo(_:)): return history.canUndo
        case #selector(redo(_:)): return history.canRedo
        default: return false
        }
    }

    func install(_ view: NSView) {
        view.translatesAutoresizingMaskIntoConstraints = false
        addSubview(view)
        NSLayoutConstraint.activate([
            view.leadingAnchor.constraint(equalTo: leadingAnchor),
            view.trailingAnchor.constraint(equalTo: trailingAnchor),
            view.topAnchor.constraint(equalTo: topAnchor),
            view.bottomAnchor.constraint(equalTo: bottomAnchor)
        ])
    }
}
