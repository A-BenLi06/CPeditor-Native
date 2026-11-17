import SwiftUI
import NativeEditorModel
import NativeCoreBridge

struct WorkspaceView: View {
    @State private var workspace: Workspace

    init(workspace: Workspace = Workspace(arguments: Array(CommandLine.arguments.dropFirst()), automaticRecovery: true)) {
        _workspace = State(initialValue: workspace)
    }

    private var canMutateWorkspace: Bool {
        !workspace.isBusy && workspace.persistentChangesAllowed
    }

    private var companionToolbarTitle: String {
        let pending = workspace.companion.pendingCount
        if pending > 0 {
            return String(format: L10n.string("companion_pending_badge_format"), pending)
        }
        return L10n.string("companion_title")
    }

    private var companionToolbarHelp: String {
        let pending = workspace.companion.pendingCount
        if pending > 0 {
            return String(format: L10n.string("help_companion_pending_format"), pending)
        }
        return L10n.string("help_companion_inbox")
    }

    var body: some View {
        NavigationSplitView {
            List(selection: $workspace.selection) {
                ForEach(workspace.sessions, id: \.document.id) { session in
                    DocumentSidebarRow(workspace: workspace, session: session)
                        .tag(session.document.id)
                }
            }
            .listStyle(.sidebar)
            .navigationTitle(L10n.string("documents"))
            .disabled(workspace.isLoading || workspace.isClosing || workspace.closingDocumentID != nil || workspace.decidingCloseID != nil)
            .safeAreaInset(edge: .bottom, spacing: 0) {
                SidebarFooterBar(workspace: workspace)
            }
            .navigationSplitViewColumnWidth(min: 185, ideal: 225, max: 320)
        } detail: {
            VSplitView {
                VStack(spacing: 0) {
                    if let notice = workspace.preferenceNotice {
                        NoticeBannerView(kind: .info, message: notice)
                    }
                    if let recovery = workspace.recovery, let notice = recovery.notice {
                        NoticeBannerView(kind: recovery.isBlocked ? .warning : .info, message: notice) {
                            HStack(spacing: 6) {
                                Button(L10n.string("save_recovery_as")) { Task { await recovery.saveElsewhere() } }
                                Button(L10n.string("load_recovery_backup")) { Task { await recovery.loadBackup() } }
                                    .disabled(!workspace.persistentChangesAllowed)
                                Button(L10n.string("retry_recovery")) { Task { await recovery.retry() } }
                            }
                            .controlSize(.small)
                            .disabled(workspace.isBusy)
                        }
                    }
                    ZStack {
                        NativeEditorDeck(
                            sessions: workspace.editorDeckSessions,
                            selectedID: workspace.selection,
                            wrapText: workspace.wrapText,
                            showMinimap: workspace.showMinimap
                        )
                        if workspace.activeSession == nil && !workspace.isLoading {
                            ContentUnavailableView {
                                Label(L10n.string("no_document"), systemImage: "chevron.left.forwardslash.chevron.right")
                            } description: {
                                Text(L10n.string("open_or_create"))
                            } actions: {
                                HStack(spacing: 8) {
                                    Button {
                                        workspace.createDocument()
                                    } label: {
                                        Label(L10n.string("new_document"), systemImage: "doc.badge.plus")
                                    }
                                    .buttonStyle(.borderedProminent)
                                    .disabled(!canMutateWorkspace)

                                    Button {
                                        Task { await workspace.openPanel() }
                                    } label: {
                                        Label(L10n.string("open_document"), systemImage: "folder")
                                    }
                                    .disabled(!canMutateWorkspace)

                                    Button {
                                        workspace.companion.isPresented = true
                                    } label: {
                                        Label(L10n.string("companion_title"), systemImage: "tray.and.arrow.down")
                                    }
                                    .disabled(workspace.isLoading || workspace.isClosing)
                                }
                            }
                        }
                        if workspace.isLoading {
                            ProgressView(L10n.string("loading_recovery"))
                                .padding(16)
                                .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 10))
                        }
                    }
                    if let session = workspace.activeSession {
                        Divider()
                        EditorStatusView(session: session, languageName: workspace.languageDisplayName(for: session))
                    }
                }
                .frame(minHeight: 190)

                if workspace.isRunPanelVisible, let session = workspace.activeSession {
                    RunPanelView(session: session, runner: workspace.runner)
                        .disabled(workspace.isLoading || workspace.isClosing)
                }
            }
            .navigationTitle(workspace.activeSession.map(workspace.title(for:)) ?? L10n.string("app_name"))
            .navigationSubtitle(workspace.activeSession.map(workspace.windowSubtitle(for:)) ?? "")
        }
        .toolbar {
            ToolbarItemGroup(placement: .primaryAction) {
                Button { workspace.createDocument() } label: {
                    Label(L10n.string("new_document"), systemImage: "doc.badge.plus")
                }
                .help(L10n.string("help_new_document"))
                .disabled(!canMutateWorkspace)

                Button { Task { await workspace.openPanel() } } label: {
                    Label(L10n.string("open_document"), systemImage: "folder")
                }
                .help(L10n.string("help_open_document"))
                .disabled(!canMutateWorkspace)

                Button { Task { await workspace.save() } } label: {
                    Label(L10n.string("save"), systemImage: "square.and.arrow.down")
                }
                .help(L10n.string("help_save_document"))
                .disabled(workspace.activeSession?.canMutatePersistence != true || !canMutateWorkspace)

                if workspace.runner.isRunning {
                    Button { workspace.runner.cancel() } label: {
                        Label(L10n.string("cancel_run"), systemImage: "stop.fill")
                    }
                    .help(L10n.string("help_cancel_run"))
                } else {
                    Button { workspace.runActiveDocument() } label: {
                        Label(L10n.string("run_all"), systemImage: "play.fill")
                    }
                    .help(L10n.string("help_run_all"))
                    .disabled(workspace.isLoading || workspace.isClosing || !workspace.runner.canRun(workspace.activeSession))
                }

                Button { workspace.companion.isPresented = true } label: {
                    Label(
                        companionToolbarTitle,
                        systemImage: workspace.companion.pendingCount > 0 ? "tray.full.fill" : "tray.and.arrow.down"
                    )
                }
                .help(companionToolbarHelp)
                .disabled(workspace.isLoading || workspace.isClosing)

                Toggle(isOn: $workspace.wrapText) {
                    Label(L10n.string("wrap_lines"), systemImage: "text.alignleft")
                }
                .help(L10n.string("help_wrap_lines"))
                .disabled(workspace.isLoading || workspace.isClosing || !workspace.persistentChangesAllowed)

                Toggle(isOn: $workspace.showMinimap) {
                    Label(L10n.string("show_minimap"), systemImage: "sidebar.right")
                }
                .help(L10n.string("help_show_minimap"))
                .disabled(workspace.activeSession?.document.editorKind != .codeEditorView || workspace.isLoading || workspace.isClosing || !workspace.persistentChangesAllowed)

                Toggle(isOn: $workspace.isRunPanelVisible) {
                    Label(L10n.string("toggle_run_panel"), systemImage: "rectangle.bottomthird.inset.filled")
                }
                .help(L10n.string("help_toggle_run_panel"))
                .disabled(workspace.isLoading || workspace.isClosing || workspace.activeSession == nil)
            }
        }
        .sheet(isPresented: Binding(
            get: { workspace.companion.isPresented },
            set: { workspace.companion.isPresented = $0 }
        )) {
            CompanionInboxView(controller: workspace.companion)
        }
        .sheet(item: Binding(
            get: { workspace.runner.configurationDraft },
            set: { workspace.runner.configurationDraft = $0 }
        )) { draft in
            ConfigurationJSONSheet(draft: draft) { jsonText in
                switch draft.kind {
                case .toolchain: try workspace.runner.applyProfile(jsonText)
                case .checker: try workspace.runner.applyChecker(jsonText)
                }
            }
        }
        .focusedSceneValue(\.editorWorkspace, workspace)
        .task { await workspace.bootstrap() }
        .background(WindowCloseGuard(workspace: workspace).frame(width: 0, height: 0))
        .alert(L10n.string("error_title"), isPresented: Binding(
            get: { workspace.errorMessage != nil },
            set: { if !$0 { workspace.errorMessage = nil } }
        )) {
            Button(L10n.string("ok"), role: .cancel) { workspace.errorMessage = nil }
        } message: {
            Text(workspace.errorMessage ?? "")
        }
        .frame(minWidth: 720, minHeight: 460)
    }
}

private struct DocumentSidebarRow: View {
    let workspace: Workspace
    let session: NativeEditorSession
    @State private var isHovering = false

    private var iconName: String {
        if workspace.companionProblemTitle(for: session) != nil {
            return "curlybraces.square"
        }
        if session.document.url != nil {
            return "doc.text.fill"
        }
        return "doc.text"
    }

    private var runState: JobState? {
        workspace.runner.state(for: session.document.id)
    }

    private var runResult: BatchResult? {
        workspace.runner.result(for: session.document.id)
    }

    private var canCloseTab: Bool {
        !workspace.isBusy && workspace.persistentChangesAllowed
    }

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: iconName)
                .symbolRenderingMode(.monochrome)
                .font(.callout)
                .foregroundStyle(.secondary)
                .frame(width: 16)

            VStack(alignment: .leading, spacing: 2) {
                Text(workspace.title(for: session))
                    .font(.body.weight(session.document.isDirty ? .medium : .regular))
                    .lineLimit(1)
                Text(workspace.sidebarSubtitle(for: session))
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }

            Spacer(minLength: 4)

            if let state = runState, !state.isTerminal {
                ProgressView()
                    .controlSize(.mini)
            } else if let result = runResult {
                VerdictBadge(
                    descriptor: .summary(for: result),
                    compact: true,
                    dimmed: !result.matches(session.document.snapshot()),
                    prominentSelection: workspace.selection == session.document.id
                )
            }

            ZStack {
                let isSelected = workspace.selection == session.document.id
                if isHovering && canCloseTab {
                    Button {
                        Task { await workspace.closeDocument(session) }
                    } label: {
                        Image(systemName: "xmark")
                            .font(.system(size: 9, weight: .bold))
                            .foregroundStyle(isSelected ? .white : .secondary)
                            .frame(width: 16, height: 16)
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(.borderless)
                    .help(L10n.string("close_document"))
                } else if !session.canMutatePersistence {
                    Image(systemName: "lock.fill")
                        .font(.system(size: 9))
                        .foregroundStyle(isSelected ? .white.opacity(0.85) : .secondary)
                        .accessibilityLabel(L10n.string("readonly_status"))
                } else if session.document.isDirty {
                    Circle()
                        .fill(isSelected ? Color.white : Color.orange)
                        .frame(width: 7, height: 7)
                        .accessibilityLabel(L10n.string("unsaved_changes"))
                }
            }
            .frame(width: 16, height: 16)
        }
        .padding(.vertical, 2)
        .contentShape(Rectangle())
        .onHover { isHovering = $0 }
        .contextMenu {
            Button(L10n.string("save")) {
                Task { await workspace.save(session) }
            }
            .disabled(!session.canMutatePersistence || workspace.isBusy || !workspace.persistentChangesAllowed)

            Button(L10n.string("save_as")) {
                Task { await workspace.save(session, chooseURL: true) }
            }
            .disabled(!session.canMutatePersistence || workspace.isBusy || !workspace.persistentChangesAllowed)

            Button(L10n.string("reveal_in_finder")) {
                workspace.revealInFinder(session)
            }
            .disabled(session.document.url == nil)

            Divider()

            Button(L10n.string("open_copy_alternative")) {
                workspace.selection = session.document.id
                workspace.openCopyInAlternativeEditor()
            }
            .disabled(workspace.isBusy || !workspace.persistentChangesAllowed)

            Divider()

            Button(L10n.string("close_document"), role: .destructive) {
                Task { await workspace.closeDocument(session) }
            }
            .disabled(!canCloseTab)
        }
    }
}

private struct SidebarFooterBar: View {
    let workspace: Workspace

    private var canMutate: Bool {
        !workspace.isBusy && workspace.persistentChangesAllowed
    }

    private var countLabel: String {
        let count = workspace.sessions.count
        if count == 1 {
            return L10n.string("document_count_single")
        }
        return String(format: L10n.string("document_count_plural"), count)
    }

    var body: some View {
        VStack(spacing: 0) {
            Divider()
            HStack(spacing: 10) {
                Menu {
                    ForEach(EditorKind.allCases, id: \.self) { editor in
                        Button(String(format: L10n.string("new_with_editor"), L10n.editorName(editor))) {
                            workspace.createDocument(editor: editor)
                        }
                    }
                } label: {
                    Image(systemName: "plus")
                } primaryAction: {
                    workspace.createDocument()
                }
                .menuStyle(.borderlessButton)
                .fixedSize()
                .disabled(!canMutate)
                .help(L10n.string("help_new_document"))

                Button {
                    Task { await workspace.openPanel() }
                } label: {
                    Image(systemName: "folder")
                }
                .buttonStyle(.borderless)
                .disabled(!canMutate)
                .help(L10n.string("help_open_document"))

                Spacer(minLength: 4)

                Text(countLabel)
                    .font(.caption2.monospacedDigit())
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 6)
            .background(.bar)
        }
    }
}

struct EditorStatusView: View {
    let session: NativeEditorSession
    var languageName: String = ""

    private var saveStateLabel: String {
        if !session.canMutatePersistence {
            return L10n.string("readonly_status")
        }
        return session.document.isDirty ? L10n.string("edited_status") : L10n.string("saved_status")
    }

    var body: some View {
        let position = session.document.position(atUTF16Offset: session.selection.location)
        HStack(spacing: 10) {
            Text(L10n.editorName(session.document.editorKind))
                .fontWeight(.medium)
            if !languageName.isEmpty {
                Divider().frame(height: 11)
                Text(languageName)
            }
            Divider().frame(height: 11)
            HStack(spacing: 4) {
                if !session.canMutatePersistence {
                    Image(systemName: "lock.fill")
                        .font(.system(size: 9))
                } else if session.document.isDirty {
                    Circle()
                        .fill(Color.orange)
                        .frame(width: 6, height: 6)
                }
                Text(saveStateLabel)
            }
            Text(String(format: L10n.string("revision_badge_format"), session.document.revision))
                .monospacedDigit()
                .foregroundStyle(.tertiary)

            Spacer(minLength: 8)

            if session.isComposing {
                Text(L10n.string("composing"))
                    .fontWeight(.medium)
                    .foregroundStyle(Color.accentColor)
                    .padding(.horizontal, 6)
                    .padding(.vertical, 1)
                    .background(Color.accentColor.opacity(0.12), in: Capsule())
            }
            if session.selection.length > 0 {
                Text(String(format: L10n.string("position_selection_format"), position.line, position.column, session.selection.length))
                    .monospacedDigit()
            } else {
                Text(String(format: L10n.string("position_format"), position.line, position.column))
                    .monospacedDigit()
            }
            Divider().frame(height: 11)
            Text(L10n.string("encoding_utf8"))
        }
        .font(.caption)
        .foregroundStyle(.secondary)
        .padding(.horizontal, 12)
        .padding(.vertical, 5)
        .background(.bar)
    }
}
