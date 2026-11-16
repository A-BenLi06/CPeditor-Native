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
            Button(L10n.string("new_document")) { workspace?.createDocument() }.disabled(workspace?.isBusy ?? true)
                .keyboardShortcut("n")
            Button(L10n.string("open_document")) { Task { await workspace?.openPanel() } }.disabled(workspace?.isBusy ?? true)
                .keyboardShortcut("o")
        }
        CommandGroup(replacing: .saveItem) {
            Button(L10n.string("save")) { Task { await workspace?.save() } }
                .keyboardShortcut("s").disabled(workspace?.activeSession == nil || (workspace?.isBusy ?? true))
            Button(L10n.string("save_as")) { Task { await workspace?.save(chooseURL: true) } }
                .keyboardShortcut("s", modifiers: [.command, .shift])
                .disabled(workspace?.activeSession == nil || (workspace?.isBusy ?? true))
        }
        CommandGroup(after: .saveItem) {
            Button(L10n.string("close_document")) { Task { await workspace?.closeActiveDocument() } }
                .keyboardShortcut("w").disabled(workspace?.activeSession == nil || (workspace?.isBusy ?? true))
        }
        CommandMenu(L10n.string("editor_menu")) {
            ForEach(EditorKind.allCases, id: \.self) { editor in
                Button(String(format: L10n.string("new_with_editor"), L10n.editorName(editor))) {
                    workspace?.createDocument(editor: editor)
                }.disabled(workspace?.isBusy ?? true)
            }
            Divider()
            Button(L10n.string("open_copy_alternative")) { workspace?.openCopyInAlternativeEditor() }
                .disabled(workspace?.activeSession == nil || (workspace?.isBusy ?? true))
            Button(L10n.string("copy_current_buffer")) { workspace?.openLiveBufferCopy() }
                .disabled(workspace?.activeSession?.captureFailure == nil || (workspace?.isBusy ?? true))
        }
        CommandMenu(L10n.string("recovery_menu")) {
            Button(L10n.string("save_recovery_as")) { Task { await workspace?.recovery?.saveElsewhere() } }
                .disabled(workspace?.recovery == nil || (workspace?.isBusy ?? true))
            Button(L10n.string("load_recovery_backup")) { Task { await workspace?.recovery?.loadBackup() } }
                .disabled(workspace?.recovery == nil || (workspace?.isBusy ?? true))
            Button(L10n.string("retry_recovery")) { Task { await workspace?.recovery?.retry() } }
                .disabled(workspace?.recovery?.isBlocked != true || (workspace?.isBusy ?? true))
            Button(L10n.string("cancel_recovery_save")) { workspace?.recovery?.cancelSave() }
                .disabled(workspace?.recovery?.isSaving != true)
            Divider()
            Button(L10n.string("show_recovery_file")) { workspace?.recovery?.revealFile() }
                .disabled(workspace?.recovery == nil)
        }
    }
}
