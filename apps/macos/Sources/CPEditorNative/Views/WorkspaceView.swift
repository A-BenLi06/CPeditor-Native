import SwiftUI
import NativeEditorModel

struct WorkspaceView: View {
    @State private var workspace: Workspace

    init(workspace: Workspace = Workspace(arguments: Array(CommandLine.arguments.dropFirst()), automaticRecovery: true)) {
        _workspace = State(initialValue: workspace)
    }

    var body: some View {
        NavigationSplitView {
            List(selection: $workspace.selection) {
                ForEach(workspace.sessions, id: \.document.id) { session in
                    HStack {
                        Image(systemName: "doc.text")
                        VStack(alignment: .leading, spacing: 3) {
                            Text(workspace.title(for: session)).lineLimit(1)
                            Text(L10n.editorName(session.document.editorKind))
                                .font(.caption).foregroundStyle(.secondary)
                        }
                        Spacer(minLength: 0)
                        if session.document.isDirty {
                            Image(systemName: "circle.fill").font(.system(size: 6))
                                .accessibilityLabel(L10n.string("unsaved_changes"))
                        }
                    }.tag(session.document.id)
                }
            }
            .navigationTitle(L10n.string("documents"))
            .disabled(workspace.isLoading || workspace.isClosing || workspace.closingDocumentID != nil || workspace.decidingCloseID != nil)
            .navigationSplitViewColumnWidth(min: 180, ideal: 230, max: 320)
        } detail: {
            VSplitView {
              VStack(spacing: 0) {
                if let notice = workspace.preferenceNotice {
                    Text(notice).font(.caption).foregroundStyle(.secondary).padding(10)
                }
                if let recovery = workspace.recovery, let notice = recovery.notice {
                    HStack(alignment: .top) {
                        Image(systemName: "exclamationmark.triangle")
                        Text(notice).textSelection(.enabled)
                        Spacer()
                        Button(L10n.string("save_recovery_as")) { Task { await recovery.saveElsewhere() } }
                        Button(L10n.string("load_recovery_backup")) { Task { await recovery.loadBackup() } }.disabled(!workspace.persistentChangesAllowed)
                        Button(L10n.string("retry_recovery")) { Task { await recovery.retry() } }
                    }.font(.caption).padding(10).background(.quaternary).disabled(workspace.isBusy)
                }
                ZStack {
                    NativeEditorDeck(sessions: workspace.editorDeckSessions, selectedID: workspace.selection,
                                     wrapText: workspace.wrapText, showMinimap: workspace.showMinimap)
                    if workspace.activeSession == nil {
                        ContentUnavailableView(L10n.string("no_document"), systemImage: "doc.text",
                                               description: Text(L10n.string("open_or_create")))
                    }
                    if workspace.isLoading { ProgressView(L10n.string("loading_recovery")) }
                }
                if let session = workspace.activeSession {
                    Divider()
                    EditorStatusView(session: session)
                }
              }.frame(minHeight: 170)
              if let session = workspace.activeSession {
                  RunPanelView(session: session, runner: workspace.runner)
                      .disabled(workspace.isLoading || workspace.isClosing)
              }
            }
            .navigationTitle(workspace.activeSession.map(workspace.title(for:)) ?? L10n.string("app_name"))
        }
        .toolbar {
            ToolbarItemGroup(placement: .primaryAction) {
                Button { Task { await workspace.openPanel() } } label: {
                    Label(L10n.string("open_document"), systemImage: "folder")
                }.disabled(workspace.isBusy || !workspace.persistentChangesAllowed)
                Button { Task { await workspace.save() } } label: {
                    Label(L10n.string("save"), systemImage: "square.and.arrow.down")
                }.disabled(workspace.activeSession?.canMutatePersistence != true || workspace.isBusy || !workspace.persistentChangesAllowed)
                Button { workspace.companion.isPresented = true } label: {
                    Label(L10n.string("companion_title"), systemImage: "tray.and.arrow.down")
                }.disabled(workspace.isLoading || workspace.isClosing)
                Toggle(isOn: $workspace.wrapText) {
                    Label(L10n.string("wrap_lines"), systemImage: "text.alignleft")
                }.disabled(workspace.isLoading || workspace.isClosing || !workspace.persistentChangesAllowed)
                Toggle(isOn: $workspace.showMinimap) {
                    Label(L10n.string("show_minimap"), systemImage: "sidebar.right")
                }.disabled(workspace.activeSession?.document.editorKind != .codeEditorView || workspace.isLoading || workspace.isClosing || !workspace.persistentChangesAllowed)
            }
        }
        .sheet(isPresented: Binding(get: { workspace.companion.isPresented }, set: { workspace.companion.isPresented = $0 })) { CompanionInboxView(controller: workspace.companion) }
        .focusedSceneValue(\.editorWorkspace, workspace)
        .task { await workspace.bootstrap() }
        .background(WindowCloseGuard(workspace: workspace).frame(width: 0, height: 0))
        .alert(L10n.string("error_title"), isPresented: Binding(
            get: { workspace.errorMessage != nil },
            set: { if !$0 { workspace.errorMessage = nil } })) {
                Button(L10n.string("ok"), role: .cancel) { workspace.errorMessage = nil }
            } message: { Text(workspace.errorMessage ?? "") }
        .frame(minWidth: 680, minHeight: 420)
    }
}

struct EditorStatusView: View {
    let session: NativeEditorSession

    private var position: (line: Int, column: Int) {
        let prefix = session.document.text.utf16.prefix(max(0, session.selection.location))
        let line = prefix.filter { $0 == 10 }.count + 1
        let column = prefix.reversed().prefix { $0 != 10 }.count + 1
        return (line, column)
    }

    var body: some View {
        HStack {
            Text(L10n.editorName(session.document.editorKind))
            Spacer()
            if session.isComposing { Text(L10n.string("composing")) }
            Text(String(format: L10n.string("position_format"), position.line, position.column))
            Text(L10n.string("encoding_utf8"))
        }
        .font(.caption).foregroundStyle(.secondary)
        .padding(.horizontal, 12).padding(.vertical, 6)
    }
}
