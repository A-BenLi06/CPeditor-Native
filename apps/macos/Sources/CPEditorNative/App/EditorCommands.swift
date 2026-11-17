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

    private var canMutateWorkspace: Bool {
        workspace != nil && workspace?.isBusy == false && workspace?.persistentChangesAllowed == true
    }

    private var canMutateActiveDocument: Bool {
        canMutateWorkspace && workspace?.activeSession?.canMutatePersistence == true
    }

    var body: some Commands {
        CommandGroup(replacing: .newItem) {
            Button(L10n.string("new_document")) { workspace?.createDocument() }
                .keyboardShortcut("n")
                .disabled(!canMutateWorkspace)
            Button(L10n.string("open_document")) { Task { await workspace?.openPanel() } }
                .keyboardShortcut("o")
                .disabled(!canMutateWorkspace)
        }
        CommandGroup(replacing: .saveItem) {
            Button(L10n.string("save")) { Task { await workspace?.save() } }
                .keyboardShortcut("s")
                .disabled(!canMutateActiveDocument)
            Button(L10n.string("save_as")) { Task { await workspace?.save(chooseURL: true) } }
                .keyboardShortcut("s", modifiers: [.command, .shift])
                .disabled(!canMutateActiveDocument)
        }
        CommandGroup(after: .saveItem) {
            Button(L10n.string("reveal_in_finder")) { workspace?.revealInFinder() }
                .disabled(workspace?.activeSession?.document.url == nil)
            Divider()
            Button(L10n.string("close_document")) { Task { await workspace?.closeActiveDocument() } }
                .keyboardShortcut("w")
                .disabled(workspace?.activeSession == nil || !canMutateWorkspace)
        }
        CommandMenu(L10n.string("editor_menu")) {
            ForEach(EditorKind.allCases, id: \.self) { editor in
                Button(String(format: L10n.string("new_with_editor"), L10n.editorName(editor))) {
                    workspace?.createDocument(editor: editor)
                }
                .disabled(!canMutateWorkspace)
            }
            Divider()
            Button(L10n.string("open_copy_alternative")) { workspace?.openCopyInAlternativeEditor() }
                .disabled(workspace?.activeSession == nil || !canMutateWorkspace)
            Button(L10n.string("copy_current_buffer")) { workspace?.openLiveBufferCopy() }
                .disabled(workspace?.activeSession?.captureFailure == nil || !canMutateWorkspace)
            Divider()
            Toggle(L10n.string("wrap_lines"), isOn: Binding(
                get: { workspace?.wrapText ?? false },
                set: { workspace?.wrapText = $0 }
            ))
            .keyboardShortcut("l", modifiers: [.command, .option])
            .disabled(workspace == nil || workspace?.isLoading == true || workspace?.isClosing == true || workspace?.persistentChangesAllowed != true)
            Toggle(L10n.string("show_minimap"), isOn: Binding(
                get: { workspace?.showMinimap ?? false },
                set: { workspace?.showMinimap = $0 }
            ))
            .keyboardShortcut("m", modifiers: [.command, .shift])
            .disabled(workspace?.activeSession?.document.editorKind != .codeEditorView || workspace?.isLoading == true || workspace?.isClosing == true || workspace?.persistentChangesAllowed != true)
            Divider()
            Button(workspace?.isRunPanelVisible == false ? L10n.string("show_run_panel") : L10n.string("hide_run_panel")) {
                workspace?.isRunPanelVisible.toggle()
            }
            .keyboardShortcut("y", modifiers: [.command, .shift])
            .disabled(workspace == nil || workspace?.isLoading == true || workspace?.isClosing == true)
        }
        CommandMenu(L10n.string("run_menu")) {
            Button(L10n.string("run_all")) { workspace?.runActiveDocument() }
                .keyboardShortcut("r")
                .disabled(workspace == nil || workspace?.isLoading == true || workspace?.isClosing == true || workspace?.runner.canRun(workspace?.activeSession) != true)
            Button(L10n.string("cancel_run")) { workspace?.runner.cancel() }
                .keyboardShortcut(".", modifiers: [.command])
                .disabled(workspace?.runner.isRunning != true)
            Divider()
            Button(L10n.string("add_test")) { workspace?.addTestToActiveDocument() }
                .keyboardShortcut("t", modifiers: [.command, .shift])
                .disabled(workspace?.activeSession?.canMutatePersistence != true || workspace?.runner.isRunning != false || workspace?.isLoading != false || workspace?.isClosing != false)
            Button(L10n.string("import_tests")) { workspace?.importTestsIntoActiveDocument() }
                .disabled(workspace?.activeSession?.canMutatePersistence != true || workspace?.runner.isRunning != false || workspace?.isLoading != false || workspace?.isClosing != false)
            Divider()
            Button(L10n.string("load_toolchain")) { workspace?.runner.loadProfile() }
                .disabled(workspace == nil || workspace?.runner.isRunning != false || workspace?.runner.persistentChangesAllowed != true || workspace?.isLoading != false || workspace?.isClosing != false)
            Button(L10n.string("edit_toolchain")) { workspace?.runner.beginProfileEdit() }
                .disabled(workspace == nil || workspace?.runner.isRunning != false || workspace?.runner.persistentChangesAllowed != true || workspace?.isLoading != false || workspace?.isClosing != false)
            Button(L10n.string("edit_checker")) { workspace?.runner.beginCheckerEdit() }
                .disabled(workspace == nil || workspace?.runner.checkerPreferencesAreOpaque != false || workspace?.runner.checkerPreferenceStorageAvailable != true || workspace?.runner.isRunning != false || workspace?.runner.persistentChangesAllowed != true || workspace?.isLoading != false || workspace?.isClosing != false)
        }
        CommandMenu(L10n.string("companion_title")) {
            Button(L10n.string("companion_show")) { workspace?.companion.isPresented = true }
                .keyboardShortcut("i", modifiers: [.command, .shift])
                .disabled(workspace?.isLoading != false || workspace?.isClosing != false)
            Button(L10n.string("companion_import_json")) { Task { await workspace?.companion.importPanel() } }
                .disabled(workspace?.companion.paths == nil || !canMutateWorkspace)
        }
        CommandMenu(L10n.string("recovery_menu")) {
            Button(L10n.string("save_recovery_as")) { Task { await workspace?.recovery?.saveElsewhere() } }
                .disabled(workspace?.recovery == nil || (workspace?.isBusy ?? true))
            Button(L10n.string("load_recovery_backup")) { Task { await workspace?.recovery?.loadBackup() } }
                .disabled(workspace?.recovery == nil || !canMutateWorkspace)
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
