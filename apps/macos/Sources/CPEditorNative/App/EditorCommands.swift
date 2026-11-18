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

    private var canEditTests: Bool {
        workspace != nil
            && workspace?.activeSession?.canMutatePersistence == true
            && workspace?.runner.isRunning == false
            && workspace?.isLoading == false
            && workspace?.isClosing == false
    }

    var body: some Commands {
        CommandGroup(replacing: .newItem) {
            Button(L10n.string("new_file")) { workspace?.createDocument() }
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
            Button(L10n.string("save_all")) { Task { await workspace?.saveAllDocuments() } }
                .keyboardShortcut("s", modifiers: [.command, .option])
                .disabled(!canMutateWorkspace || workspace?.sessions.contains(where: { $0.document.isDirty && $0.canMutatePersistence }) != true)
        }

        CommandGroup(after: .saveItem) {
            Button(L10n.string("reveal_in_finder")) { workspace?.revealInFinder() }
                .disabled(workspace?.activeSession?.document.url == nil)
            Divider()
            Button(L10n.string("close_tab")) { Task { await workspace?.closeActiveDocument() } }
                .keyboardShortcut("w")
                .disabled(workspace?.activeSession == nil || !canMutateWorkspace)
            Button(L10n.string("close_saved_tabs")) { Task { await workspace?.closeSavedDocuments() } }
                .keyboardShortcut("w", modifiers: [.command, .option])
                .disabled(workspace?.sessions.isEmpty != false || !canMutateWorkspace)
            Divider()
            Menu(L10n.string("workspace_recovery_menu")) {
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
                Divider()
                Button(L10n.string("copy_current_buffer")) { workspace?.openLiveBufferCopy() }
                    .disabled(workspace?.activeSession?.captureFailure == nil || !canMutateWorkspace)
            }
        }

        CommandGroup(after: .toolbar) {
            Button(L10n.string("layout_editor_only")) { workspace?.layoutMode = .editorOnly }
                .keyboardShortcut("1", modifiers: [.command, .control])
                .disabled(workspace == nil || workspace?.isLoading == true || workspace?.isClosing == true)
            Button(L10n.string("layout_split")) { workspace?.layoutMode = .split }
                .keyboardShortcut("2", modifiers: [.command, .control])
                .disabled(workspace == nil || workspace?.isLoading == true || workspace?.isClosing == true)
            Button(L10n.string("layout_io_only")) { workspace?.layoutMode = .ioOnly }
                .keyboardShortcut("3", modifiers: [.command, .control])
                .disabled(workspace == nil || workspace?.isLoading == true || workspace?.isClosing == true)
            Button(workspace?.isRunPanelVisible == false ? L10n.string("show_run_panel") : L10n.string("hide_run_panel")) {
                workspace?.isRunPanelVisible.toggle()
            }
            .keyboardShortcut("y", modifiers: [.command, .shift])
            .disabled(workspace == nil || workspace?.isLoading == true || workspace?.isClosing == true)
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
        }

        CommandMenu(L10n.string("run_menu")) {
            Button(L10n.string("compile_and_run")) { workspace?.runActiveDocument() }
                .keyboardShortcut("r")
                .disabled(workspace == nil || workspace?.isLoading == true || workspace?.isClosing == true || workspace?.runner.canRun(workspace?.activeSession) != true)
            Button(L10n.string("stop_short")) { workspace?.runner.cancel() }
                .keyboardShortcut(".", modifiers: [.command])
                .disabled(workspace?.runner.isRunning != true)
            Divider()
            Button(L10n.string("add_test")) { workspace?.addTestToActiveDocument() }
                .keyboardShortcut("t", modifiers: [.command, .shift])
                .disabled(!canEditTests)
            Button(L10n.string("import_tests")) { workspace?.importTestsIntoActiveDocument() }
                .disabled(!canEditTests)
            Button(L10n.string("enable_all_tests")) { workspace?.setAllTestsEnabled(true) }
                .disabled(!canEditTests || workspace?.activeSession?.testCases.isEmpty != false)
            Button(L10n.string("disable_all_tests")) { workspace?.setAllTestsEnabled(false) }
                .disabled(!canEditTests || workspace?.activeSession?.testCases.isEmpty != false)
            Divider()
            Button(L10n.string("toolchain_settings")) { workspace?.runner.beginProfileEdit() }
                .disabled(workspace == nil || workspace?.runner.isRunning != false || workspace?.runner.persistentChangesAllowed != true || workspace?.isLoading != false || workspace?.isClosing != false)
            Button(L10n.string("import_toolchain_profile")) { workspace?.runner.loadProfile() }
                .disabled(workspace == nil || workspace?.runner.isRunning != false || workspace?.runner.persistentChangesAllowed != true || workspace?.isLoading != false || workspace?.isClosing != false)
            Button(L10n.string("checker_settings")) { workspace?.runner.beginCheckerEdit() }
                .disabled(workspace == nil || workspace?.runner.checkerPreferencesAreOpaque != false || workspace?.runner.checkerPreferenceStorageAvailable != true || workspace?.runner.isRunning != false || workspace?.runner.persistentChangesAllowed != true || workspace?.isLoading != false || workspace?.isClosing != false)
        }

        CommandMenu(L10n.string("companion_title")) {
            Button(L10n.string("companion_show")) { workspace?.companion.isPresented = true }
                .keyboardShortcut("i", modifiers: [.command, .shift])
                .disabled(workspace?.isLoading != false || workspace?.isClosing != false)
            Button(L10n.string("companion_import_json")) { Task { await workspace?.companion.importPanel() } }
                .disabled(workspace?.companion.paths == nil || !canMutateWorkspace)
        }
    }
}
