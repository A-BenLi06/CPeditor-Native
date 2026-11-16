import SwiftUI
import NativeEditorModel

struct WorkspaceKey: FocusedValueKey { typealias Value = Workspace }
extension FocusedValues {
    var editorWorkspace: Workspace? {
        get { self[WorkspaceKey.self] }
        set { self[WorkspaceKey.self] = newValue }
    }
}

struct EditorCommands: Commands {
    @FocusedValue(\.editorWorkspace) private var workspace

    var body: some Commands {
        CommandGroup(replacing: .newItem) {
            Button(L10n.string("new_document")) { workspace?.createDocument() }
                .keyboardShortcut("n")
            Button(L10n.string("open_document")) { workspace?.openPanel() }
                .keyboardShortcut("o")
        }
        CommandGroup(replacing: .saveItem) {
            Button(L10n.string("save")) { workspace?.save() }
                .keyboardShortcut("s").disabled(workspace?.activeSession == nil)
            Button(L10n.string("save_as")) { workspace?.save(chooseURL: true) }
                .keyboardShortcut("s", modifiers: [.command, .shift])
                .disabled(workspace?.activeSession == nil)
        }
        CommandGroup(after: .saveItem) {
            Button(L10n.string("close_document")) { workspace?.closeActiveDocument() }
                .keyboardShortcut("w").disabled(workspace?.activeSession == nil)
        }
        CommandMenu(L10n.string("editor_menu")) {
            ForEach(EditorKind.allCases, id: \.self) { editor in
                Button(String(format: L10n.string("new_with_editor"), L10n.editorName(editor))) {
                    workspace?.createDocument(editor: editor)
                }
            }
            Divider()
            Button(L10n.string("open_copy_alternative")) { workspace?.openCopyInAlternativeEditor() }
                .disabled(workspace?.activeSession == nil)
        }
    }
}
