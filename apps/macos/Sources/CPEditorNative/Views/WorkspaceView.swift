import AppKit
import SwiftUI
import NativeEditorModel
import NativeCoreBridge

struct WorkspaceView: View {
    @Environment(\.openSettings) private var openSettings
    @State private var workspace: Workspace
    @State private var splitRatio: CGFloat = 0.57

    init(workspace: Workspace) {
        _workspace = State(initialValue: workspace)
    }

    private var canMutateWorkspace: Bool {
        !workspace.isBusy && workspace.persistentChangesAllowed
    }

    private var companionToolbarTitle: String {
        let pending = workspace.companion.pendingCount
        if pending > 0 {
            return String(format: L10n.string("open_companion_count_format"), pending)
        }
        return L10n.string("open_companion_short")
    }

    private var companionToolbarHelp: String {
        let pending = workspace.companion.pendingCount
        if pending > 0 {
            return String(format: L10n.string("help_companion_pending_format"), pending)
        }
        return L10n.string("help_companion_inbox")
    }

    var body: some View {
        VStack(spacing: 0) {
            DocumentTabBar(workspace: workspace)

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

            WorkspaceSplitContainer(
                layoutMode: workspace.layoutMode,
                splitRatio: $splitRatio
            ) {
                leftEditorPane
            } right: {
                rightRunPane
            }
        }
        .navigationTitle(workspace.activeSession.map(workspace.title(for:)) ?? L10n.string("app_name"))
        .navigationSubtitle(workspace.activeSession.map(workspace.windowSubtitle(for:)) ?? "")
        .toolbar {
            ToolbarItemGroup(placement: .navigation) {
                Button { workspace.createDocument() } label: {
                    Label(L10n.string("new_file"), systemImage: "doc.badge.plus")
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
            }

            ToolbarItemGroup(placement: .primaryAction) {
                Menu {
                    ForEach(workspace.runner.profiles) { profile in
                        Button {
                            workspace.selectToolchain(profile.id)
                        } label: {
                            if profile.id == workspace.runner.selectedProfileID {
                                Label(profile.displayName, systemImage: "checkmark")
                            } else {
                                Text(profile.displayName)
                            }
                        }
                    }
                    Divider()
                    Button(L10n.string("toolchain_settings")) {
                        workspace.runner.beginProfileEdit()
                        if workspace.runner.configurationDraft != nil { openSettings() }
                    }
                    Button(L10n.string("import_toolchain_profile")) {
                        workspace.importToolchain()
                    }
                } label: {
                    HStack(spacing: 4) {
                        Image(systemName: "hammer")
                        Text(workspace.runner.selectedProfile?.displayName ?? L10n.string("toolchain"))
                            .lineLimit(1)
                    }
                }
                .help(L10n.string("help_toolchain_menu"))
                .disabled(!workspace.canConfigureRunner)

                SettingsLink {
                    Label(L10n.string("settings_title"), systemImage: "gearshape")
                }
                .labelStyle(.iconOnly)
                .help(L10n.string("settings_title"))

                if workspace.runner.isRunning {
                    Button { workspace.runner.cancel() } label: {
                        Label(L10n.string("stop_short"), systemImage: "stop.fill")
                    }
                    .help(L10n.string("help_cancel_run"))
                } else {
                    Button { workspace.runActiveDocument() } label: {
                        Label(L10n.string("run_short"), systemImage: "play.fill")
                    }
                    .buttonStyle(.borderedProminent)
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

                Picker(L10n.string("layout_mode"), selection: $workspace.layoutMode) {
                    Image(systemName: "doc.text")
                        .help(L10n.string("layout_editor_only"))
                        .accessibilityLabel(L10n.string("layout_editor_only"))
                        .tag(WorkspaceLayoutMode.editorOnly)
                    Image(systemName: "rectangle.split.2x1")
                        .help(L10n.string("layout_split"))
                        .accessibilityLabel(L10n.string("layout_split"))
                        .tag(WorkspaceLayoutMode.split)
                    Image(systemName: "checklist")
                        .help(L10n.string("layout_io_only"))
                        .accessibilityLabel(L10n.string("layout_io_only"))
                        .tag(WorkspaceLayoutMode.ioOnly)
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .fixedSize()
                .help(L10n.string("help_layout_mode"))
                .disabled(!workspace.canChangeLayout)
            }
        }
        .sheet(isPresented: Binding(
            get: { workspace.companion.isPresented },
            set: { workspace.companion.isPresented = $0 }
        )) {
            CompanionInboxView(controller: workspace.companion)
        }
        .focusedSceneValue(\.editorWorkspace, workspace)
        .task { await workspace.bootstrap() }
        .background(WindowCloseGuard(workspace: workspace).frame(width: 0, height: 0))
        .alert(L10n.string("error_title"), isPresented: Binding(
            get: { workspace.errorMessage != nil || workspace.runner.errorMessage != nil },
            set: { if !$0 { dismissError() } }
        )) {
            Button(L10n.string("ok"), role: .cancel) {}
        } message: {
            Text(workspace.errorMessage ?? workspace.runner.errorMessage ?? "")
        }
        .frame(minWidth: 740, minHeight: 480)
    }

    private func dismissError() {
        if workspace.errorMessage != nil { workspace.errorMessage = nil }
        else { workspace.runner.errorMessage = nil }
    }

    private var leftEditorPane: some View {
        VStack(spacing: 0) {
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
                                Label(L10n.string("new_file"), systemImage: "doc.badge.plus")
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
                EditorStatusView(
                    session: session,
                    languageName: workspace.languageDisplayName(for: session),
                    companionLimits: workspace.companionLimitsSummary(for: session)
                )
            }
        }
        .background(Color(nsColor: .textBackgroundColor))
    }

    @ViewBuilder
    private var rightRunPane: some View {
        if let session = workspace.activeSession {
            RunPanelView(session: session, runner: workspace.runner, canConfigureRunner: workspace.canConfigureRunner)
                .disabled(workspace.isLoading || workspace.isClosing)
        } else {
            ContentUnavailableView(
                L10n.string("no_document"),
                systemImage: "checklist",
                description: Text(L10n.string("no_tests_right_pane_hint"))
            )
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(Color(nsColor: .windowBackgroundColor))
        }
    }
}

private struct WorkspaceSplitContainer<Left: View, Right: View>: View {
    let layoutMode: WorkspaceLayoutMode
    @Binding var splitRatio: CGFloat
    private let left: Left
    private let right: Right

    init(
        layoutMode: WorkspaceLayoutMode,
        splitRatio: Binding<CGFloat>,
        @ViewBuilder left: () -> Left,
        @ViewBuilder right: () -> Right
    ) {
        self.layoutMode = layoutMode
        self._splitRatio = splitRatio
        self.left = left()
        self.right = right()
    }

    var body: some View {
        GeometryReader { proxy in
            let totalWidth = max(0, proxy.size.width)
            let totalHeight = max(0, proxy.size.height)
            let dividerWidth: CGFloat = 1
            let available = max(0, totalWidth - dividerWidth)
            let minLeft = min(300, available * 0.45)
            let minRight = min(320, available * 0.45)
            let clampedLeft = min(max(available * splitRatio, minLeft), max(minLeft, available - minRight))
            let rightWidth = max(0, available - clampedLeft)

            ZStack(alignment: .topLeading) {
                left
                    .frame(width: layoutMode == .editorOnly ? totalWidth : max(300, clampedLeft), height: totalHeight)
                    .opacity(layoutMode == .ioOnly ? 0 : 1)
                    .allowsHitTesting(layoutMode != .ioOnly)
                    .accessibilityHidden(layoutMode == .ioOnly)

                if layoutMode != .editorOnly {
                    HStack(spacing: 0) {
                        if layoutMode == .split {
                            Color.clear
                                .frame(width: clampedLeft)
                                .allowsHitTesting(false)
                            SplitResizeHandle(
                                splitRatio: $splitRatio,
                                totalWidth: available,
                                minLeft: minLeft,
                                minRight: minRight
                            )
                        }
                        right
                            .frame(width: layoutMode == .ioOnly ? totalWidth : rightWidth, height: totalHeight)
                            .background(Color(nsColor: .windowBackgroundColor))
                    }
                }
            }
            .frame(width: totalWidth, height: totalHeight, alignment: .topLeading)
        }
    }
}

private struct SplitResizeHandle: View {
    @Binding var splitRatio: CGFloat
    let totalWidth: CGFloat
    let minLeft: CGFloat
    let minRight: CGFloat

    @State private var dragStartRatio: CGFloat?
    @State private var pushedResizeCursor = false

    var body: some View {
        Rectangle()
            .fill(Color(nsColor: .separatorColor))
            .frame(width: 1)
            .frame(maxHeight: .infinity)
            .overlay {
                Color.clear
                    .frame(width: 9)
                    .contentShape(Rectangle())
            }
            .onHover { hovering in
                if hovering && !pushedResizeCursor {
                    NSCursor.resizeLeftRight.push()
                    pushedResizeCursor = true
                } else if !hovering && pushedResizeCursor {
                    NSCursor.pop()
                    pushedResizeCursor = false
                }
            }
            .onDisappear {
                if pushedResizeCursor {
                    NSCursor.pop()
                    pushedResizeCursor = false
                }
            }
            .gesture(
                DragGesture(minimumDistance: 1, coordinateSpace: .global)
                    .onChanged { value in
                        guard totalWidth > minLeft + minRight else { return }
                        if dragStartRatio == nil {
                            dragStartRatio = splitRatio
                        }
                        let baseLeft = (dragStartRatio ?? splitRatio) * totalWidth
                        let proposedLeft = min(max(baseLeft + value.translation.width, minLeft), totalWidth - minRight)
                        splitRatio = proposedLeft / totalWidth
                    }
                    .onEnded { _ in
                        dragStartRatio = nil
                    }
            )
    }
}

private struct DocumentTabBar: View {
    @Bindable var workspace: Workspace

    private var canMutateWorkspace: Bool {
        !workspace.isBusy && workspace.persistentChangesAllowed
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 0) {
                ScrollViewReader { proxy in
                    ScrollView(.horizontal, showsIndicators: false) {
                        HStack(spacing: 4) {
                            ForEach(workspace.sessions, id: \.document.id) { session in
                                DocumentTabItem(
                                    workspace: workspace,
                                    session: session,
                                    isSelected: workspace.selection == session.document.id
                                )
                                .id(session.document.id)
                            }
                        }
                        .padding(.horizontal, 8)
                        .padding(.vertical, 4)
                    }
                    .onChange(of: workspace.selection) { _, newID in
                        guard let newID else { return }
                        withAnimation(.easeOut(duration: 0.15)) {
                            proxy.scrollTo(newID)
                        }
                    }
                }

                Divider()
                    .frame(height: 16)
                    .padding(.horizontal, 4)

                Button {
                    workspace.createDocument()
                } label: {
                    Image(systemName: "plus")
                        .font(.caption.weight(.semibold))
                        .frame(width: 24, height: 24)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.borderless)
                .disabled(!canMutateWorkspace)
                .help(L10n.string("help_new_document"))
                .padding(.trailing, 8)
            }
            .frame(height: 34)
            .background(.bar)
            .disabled(workspace.isLoading || workspace.isClosing || workspace.closingDocumentID != nil || workspace.decidingCloseID != nil)

            Divider()
        }
    }
}

private struct DocumentTabItem: View {
    let workspace: Workspace
    let session: NativeEditorSession
    let isSelected: Bool
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
        HStack(spacing: 6) {
            Image(systemName: iconName)
                .font(.caption)
                .foregroundStyle(isSelected ? Color.primary : Color.secondary)

            Text(workspace.title(for: session))
                .font(.callout.weight(isSelected ? .medium : .regular))
                .foregroundStyle(isSelected ? Color.primary : Color.secondary)
                .lineLimit(1)

            if let state = runState, !state.isTerminal {
                ProgressView()
                    .controlSize(.mini)
            } else if let result = runResult {
                let resultIsCurrent = workspace.runner.resultIsCurrent(for: session)
                VerdictBadge(
                    descriptor: .summary(for: result),
                    compact: true,
                    dimmed: !resultIsCurrent
                )
                .help(resultIsCurrent ? "" : L10n.string("run_result_outdated_help"))
            }

            ZStack {
                if isHovering && canCloseTab {
                    Button {
                        Task { await workspace.closeDocument(session) }
                    } label: {
                        Image(systemName: "xmark")
                            .font(.system(size: 9, weight: .bold))
                            .foregroundStyle(.secondary)
                            .frame(width: 14, height: 14)
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(.borderless)
                    .help(L10n.string("close_tab"))
                } else if !session.canMutatePersistence {
                    Image(systemName: "lock.fill")
                        .font(.system(size: 9))
                        .foregroundStyle(.secondary)
                        .accessibilityLabel(L10n.string("readonly_status"))
                } else if session.document.isDirty {
                    Circle()
                        .fill(Color.orange)
                        .frame(width: 6, height: 6)
                        .accessibilityLabel(L10n.string("unsaved_changes"))
                }
            }
            .frame(width: 14, height: 14)
        }
        .padding(.horizontal, 9)
        .padding(.vertical, 4)
        .frame(height: 26)
        .background {
            if isSelected {
                RoundedRectangle(cornerRadius: 6)
                    .fill(Color(nsColor: .controlBackgroundColor))
            } else if isHovering {
                RoundedRectangle(cornerRadius: 6)
                    .fill(.quaternary.opacity(0.6))
            }
        }
        .overlay {
            if isSelected {
                RoundedRectangle(cornerRadius: 6)
                    .strokeBorder(Color(nsColor: .separatorColor).opacity(0.75), lineWidth: 1)
            }
        }
        .contentShape(RoundedRectangle(cornerRadius: 6))
        .onTapGesture {
            workspace.selection = session.document.id
        }
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

            Button(L10n.string("close_tab")) {
                Task { await workspace.closeDocument(session) }
            }
            .disabled(!canCloseTab)

            Button(L10n.string("close_other_tabs")) {
                Task { await workspace.closeOtherDocuments(keeping: session) }
            }
            .disabled(!canCloseTab || workspace.sessions.count <= 1)

            Button(L10n.string("close_saved_tabs")) {
                Task { await workspace.closeSavedDocuments() }
            }
            .disabled(!canCloseTab)
        }
    }
}

struct EditorStatusView: View {
    let session: NativeEditorSession
    var languageName: String = ""
    var companionLimits: String? = nil

    private var saveStateLabel: String {
        if !session.canMutatePersistence {
            return L10n.string("readonly_status")
        }
        return session.document.isDirty ? L10n.string("edited_status") : L10n.string("saved_status")
    }

    var body: some View {
        let position = session.document.position(atUTF16Offset: session.selection.location)
        HStack(spacing: 10) {
            if session.selection.length > 0 {
                Text(String(format: L10n.string("position_selection_format"), position.line, position.column, session.selection.length))
                    .monospacedDigit()
            } else {
                Text(String(format: L10n.string("position_format"), position.line, position.column))
                    .monospacedDigit()
            }
            Divider().frame(height: 11)
            Text(L10n.string("encoding_utf8"))
            if !languageName.isEmpty {
                Divider().frame(height: 11)
                Text(languageName)
                    .fontWeight(.medium)
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

            Spacer(minLength: 8)

            if session.isComposing {
                Text(L10n.string("composing"))
                    .fontWeight(.medium)
                    .foregroundStyle(Color.accentColor)
                    .padding(.horizontal, 6)
                    .padding(.vertical, 1)
                    .background(Color.accentColor.opacity(0.12), in: Capsule())
            }
            if let companionLimits {
                Text(companionLimits)
                    .monospacedDigit()
                    .foregroundStyle(.tertiary)
                    .lineLimit(1)
            }
        }
        .font(.caption)
        .foregroundStyle(.secondary)
        .padding(.horizontal, 12)
        .padding(.vertical, 5)
        .background(.bar)
    }
}
