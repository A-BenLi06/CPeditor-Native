import AppKit
import SwiftUI
import NativeEditorModel
import NativeCoreBridge

struct WorkspaceView: View {
    @Environment(\.openSettings) private var openSettings
    @Environment(\.colorScheme) private var colorScheme
    @State private var workspace: Workspace

    init(workspace: Workspace) {
        _workspace = State(initialValue: workspace)
    }

    private var canMutateWorkspace: Bool {
        !workspace.isBusy && workspace.persistentChangesAllowed
    }

    private var inspectorPresentedBinding: Binding<Bool> {
        Binding(
            get: { WorkspaceInspectorMetrics.isInspectorPresented(for: workspace.layoutMode) },
            set: { presented in
                guard workspace.canChangeLayout else { return }
                workspace.layoutMode = WorkspaceInspectorMetrics.layoutModeAfterInspectorToggle(
                    isPresented: presented,
                    current: workspace.layoutMode
                )
            }
        )
    }

    private var inspectorToggleButton: some View {
        let isInspectorOpen = WorkspaceInspectorMetrics.isInspectorPresented(for: workspace.layoutMode)
        return Button {
            guard workspace.canChangeLayout else { return }
            workspace.layoutMode = WorkspaceInspectorMetrics.layoutModeAfterInspectorToggle(
                isPresented: !isInspectorOpen,
                current: workspace.layoutMode
            )
        } label: {
            Label(
                L10n.string(isInspectorOpen ? "hide_run_panel" : "show_run_panel"),
                systemImage: "sidebar.trailing"
            )
        }
        .help(L10n.string("help_toggle_run_panel"))
        .disabled(!workspace.canChangeLayout)
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
                    .buttonBorderShape(.roundedRectangle(radius: 5))
                    .disabled(workspace.isBusy)
                }
            }

            ZStack {
                leftEditorPane
                    .opacity(workspace.layoutMode == .ioOnly ? 0 : 1)
                    .allowsHitTesting(workspace.layoutMode != .ioOnly)
                    .accessibilityHidden(workspace.layoutMode == .ioOnly)

                if workspace.layoutMode == .ioOnly {
                    rightRunPane(inInspector: false)
                }
            }
        }
        .navigationTitle(workspace.activeSession.map(workspace.title(for:)) ?? L10n.string("app_name"))
        .navigationSubtitle(workspace.activeSession.map(workspace.windowSubtitle(for:)) ?? "")
        .toolbar {
            ToolbarItemGroup(placement: .navigation) {
                Button { workspace.runner.cancel() } label: {
                    Label(L10n.string("stop_short"), systemImage: "stop.fill")
                }
                .help(L10n.string("help_cancel_run"))
                .disabled(!workspace.runner.isRunning)

                if workspace.runner.supportsCompile(for: workspace.activeSession) {
                    Button { workspace.compileActiveDocument() } label: {
                        Label(L10n.string("compile_short"), systemImage: "hammer")
                    }
                    .help(L10n.string("help_compile"))
                    .disabled(workspace.isLoading || workspace.isClosing || !workspace.runner.canCompile(workspace.activeSession))

                    Button { workspace.runOnlyActiveDocument() } label: {
                        Label(L10n.string("run_without_compiling"), systemImage: "play.rectangle")
                    }
                    .help(workspace.runner.runOnlyHelpText(for: workspace.activeSession))
                    .disabled(workspace.isLoading || workspace.isClosing || !workspace.runner.canRunOnly(workspace.activeSession))

                    Button { workspace.runActiveDocument() } label: {
                        Label(L10n.string("compile_and_run_short"), systemImage: "play.fill")
                    }
                    .help(L10n.string("help_compile_and_run"))
                    .disabled(workspace.isLoading || workspace.isClosing || !workspace.runner.canRun(workspace.activeSession))
                } else {
                    Button { workspace.runActiveDocument() } label: {
                        Label(L10n.string("run_short"), systemImage: "play.fill")
                    }
                    .help(L10n.string("help_run_interpreted"))
                    .disabled(workspace.isLoading || workspace.isClosing || !workspace.runner.canRun(workspace.activeSession))
                }
            }

            ToolbarItem(placement: .principal) {
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
                    HStack(spacing: 5) {
                        Image(systemName: "wrench.and.screwdriver")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        Text(workspace.runner.selectedProfile?.displayName ?? L10n.string("toolchain"))
                            .lineLimit(1)
                    }
                }
                .help(L10n.string("help_toolchain_menu"))
                .disabled(!workspace.canConfigureRunner)
            }

            ToolbarItemGroup(placement: .primaryAction) {
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

            ToolbarSpacer(.fixed, placement: .primaryAction)

            ToolbarItemGroup(placement: .primaryAction) {
                Button { workspace.companion.isPresented = true } label: {
                    if workspace.companion.pendingCount > 0 {
                        Label("\(workspace.companion.pendingCount)", systemImage: "tray.full.fill")
                            .labelStyle(.titleAndIcon)
                    } else {
                        Label(L10n.string("open_companion_short"), systemImage: "tray.and.arrow.down")
                            .labelStyle(.iconOnly)
                    }
                }
                .accessibilityLabel(companionToolbarTitle)
                .help(companionToolbarHelp)
                .disabled(workspace.isLoading || workspace.isClosing)

                SettingsLink {
                    Label(L10n.string("settings_title"), systemImage: "gearshape")
                }
                .labelStyle(.iconOnly)
                .help(L10n.string("settings_title"))
            }

            if workspace.layoutMode != .split {
                ToolbarSpacer(.fixed, placement: .primaryAction)

                ToolbarItem(placement: .primaryAction) {
                    inspectorToggleButton
                }
            }
        }
        .inspector(isPresented: inspectorPresentedBinding) {
            rightRunPane(inInspector: true)
                .inspectorColumnWidth(
                    min: WorkspaceInspectorMetrics.minWidth,
                    ideal: WorkspaceInspectorMetrics.idealWidth,
                    max: WorkspaceInspectorMetrics.maxWidth
                )
                .toolbar {
                    if workspace.layoutMode == .split {
                        ToolbarSpacer(.flexible, placement: .primaryAction)
                        ToolbarItem(placement: .primaryAction) {
                            inspectorToggleButton
                        }
                    }
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
            DocumentTabBar(workspace: workspace)
            Divider().opacity(0.45)

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
                        .controlSize(.regular)
                        .buttonBorderShape(.roundedRectangle(radius: 6))
                    }
                }
                if workspace.isLoading {
                    ProgressView(L10n.string("loading_recovery"))
                        .padding(16)
                        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 10))
                }
            }

            if let session = workspace.activeSession {
                Divider().opacity(0.45)
                EditorStatusView(
                    session: session,
                    languageName: workspace.languageDisplayName(for: session),
                    companionLimits: workspace.companionLimitsSummary(for: session)
                )
            }
        }
        .background(
            workspace.activeSession?.document.editorKind == .codeEditorView
                ? CodeEditorSurface.canvasBackgroundColor(for: colorScheme)
                : Color(nsColor: .textBackgroundColor)
        )
    }

    @ViewBuilder
    private func rightRunPane(inInspector: Bool) -> some View {
        if let session = workspace.activeSession {
            RunPanelView(
                session: session,
                runner: workspace.runner,
                canConfigureRunner: workspace.canConfigureRunner,
                isInspector: inInspector
            )
            .disabled(workspace.isLoading || workspace.isClosing)
        } else {
            ContentUnavailableView(
                L10n.string("no_document"),
                systemImage: "checklist",
                description: Text(L10n.string("no_tests_right_pane_hint"))
            )
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(inInspector ? Color.clear : Color(nsColor: .windowBackgroundColor))
        }
    }
}

internal enum WorkspaceInspectorMetrics {
    static let minWidth: CGFloat = 320
    static let idealWidth: CGFloat = 510
    static let maxWidth: CGFloat = 700

    static func isInspectorPresented(for layoutMode: WorkspaceLayoutMode) -> Bool {
        layoutMode == .split
    }

    static func layoutModeAfterInspectorToggle(
        isPresented: Bool,
        current: WorkspaceLayoutMode
    ) -> WorkspaceLayoutMode {
        if isPresented {
            return .split
        }
        return current == .split ? .editorOnly : current
    }
}

internal enum DocumentTabBarMetrics {
    static let barHeight: CGFloat = 38
    static let pillTrackHeight: CGFloat = 30
    static let tabCapsuleHeight: CGFloat = 26
    static let horizontalInset: CGFloat = 2
    static let minimumTabWidth: CGFloat = 120
    static let dragHysteresis: CGFloat = 8
    static let trackCoordinateSpace = "documentTabBarTrack"

    static func tabWidth(forViewportWidth viewportWidth: CGFloat, tabCount: Int) -> CGFloat {
        guard tabCount > 0, viewportWidth.isFinite else { return minimumTabWidth }
        let usableWidth = max(0, viewportWidth - 2 * horizontalInset)
        let evenWidth = floor(usableWidth / CGFloat(tabCount))
        return max(minimumTabWidth, evenWidth)
    }

    static func needsHorizontalScroll(forViewportWidth viewportWidth: CGFloat, tabCount: Int) -> Bool {
        guard tabCount > 0, viewportWidth.isFinite else { return false }
        let usableWidth = max(0, viewportWidth - 2 * horizontalInset)
        return CGFloat(tabCount) * minimumTabWidth > usableWidth
    }

    static func clampedDragTranslation(
        rawTranslationX: CGFloat,
        startIndex: Int,
        tabWidth: CGFloat,
        tabCount: Int
    ) -> CGFloat {
        guard tabCount > 1,
              startIndex >= 0,
              startIndex < tabCount,
              tabWidth > 0,
              tabWidth.isFinite,
              rawTranslationX.isFinite
        else { return 0 }
        let minOffset = -CGFloat(startIndex) * tabWidth
        let maxOffset = CGFloat(tabCount - 1 - startIndex) * tabWidth
        return min(max(rawTranslationX, minOffset), maxOffset)
    }

    static func draggedTabTargetIndex(
        startIndex: Int,
        translationX: CGFloat,
        tabWidth: CGFloat,
        tabCount: Int,
        currentTargetIndex: Int,
        hysteresis: CGFloat = dragHysteresis
    ) -> Int {
        guard tabCount > 1,
              startIndex >= 0,
              startIndex < tabCount,
              tabWidth > 0,
              tabWidth.isFinite
        else { return 0 }
        let clampedCurrent = min(max(0, currentTargetIndex), tabCount - 1)
        let clampedX = clampedDragTranslation(
            rawTranslationX: translationX,
            startIndex: startIndex,
            tabWidth: tabWidth,
            tabCount: tabCount
        )
        let center = (CGFloat(startIndex) + 0.5) * tabWidth + clampedX
        let rawIndex = min(max(0, Int(floor(center / tabWidth))), tabCount - 1)
        let safeHysteresis = hysteresis.isFinite ? max(0, hysteresis) : 0
        let band = min(safeHysteresis, tabWidth * 0.2)
        if rawIndex > clampedCurrent {
            return center >= CGFloat(clampedCurrent + 1) * tabWidth + band ? rawIndex : clampedCurrent
        }
        if rawIndex < clampedCurrent {
            return center <= CGFloat(clampedCurrent) * tabWidth - band ? rawIndex : clampedCurrent
        }
        return clampedCurrent
    }

    static func tabDisplacementOffset(
        forIndex index: Int,
        dragStartIndex: Int,
        dragTargetIndex: Int,
        tabWidth: CGFloat
    ) -> CGFloat {
        guard tabWidth > 0, tabWidth.isFinite, index != dragStartIndex else { return 0 }
        if dragTargetIndex > dragStartIndex && index > dragStartIndex && index <= dragTargetIndex {
            return -tabWidth
        }
        if dragTargetIndex < dragStartIndex && index >= dragTargetIndex && index < dragStartIndex {
            return tabWidth
        }
        return 0
    }

    static func hoverCapsuleBackgroundColor(for colorScheme: ColorScheme) -> Color {
        colorScheme == .dark
            ? Color.white.opacity(0.08)
            : Color.black.opacity(0.055)
    }

    static func pillTrackFillColor(for colorScheme: ColorScheme) -> Color {
        colorScheme == .dark
            ? Color.white.opacity(0.075)
            : Color.black.opacity(0.055)
    }

    static func pillTrackBorderColor(for colorScheme: ColorScheme) -> Color {
        colorScheme == .dark
            ? Color.white.opacity(0.07)
            : Color.black.opacity(0.06)
    }

    static func shouldShowTrailingSeparator(
        at index: Int,
        documentIDs: [RawIdentifier],
        selectedID: RawIdentifier?,
        hoveredID: RawIdentifier?,
        draggingID: RawIdentifier? = nil
    ) -> Bool {
        guard draggingID == nil, index >= 0, index + 1 < documentIDs.count else { return false }
        let current = documentIDs[index]
        let next = documentIDs[index + 1]
        return current != selectedID
            && next != selectedID
            && current != hoveredID
            && next != hoveredID
    }

    static func showsCompactVerdictText(label: String, tabWidth: CGFloat) -> Bool {
        label.count <= 4 || tabWidth >= 170
    }
}

private struct DocumentTabBar: View {
    @Environment(\.colorScheme) private var colorScheme
    @Bindable var workspace: Workspace
    @State private var hoveredTabID: RawIdentifier?
    @State private var draggingTabID: RawIdentifier?
    @State private var dragStartIndex: Int?
    @State private var dragTargetIndex: Int?
    @State private var dragTranslation: CGFloat = 0
    @State private var isSettlingDrag = false
    @State private var pendingCommit: (id: RawIdentifier, startIndex: Int, destinationIndex: Int)?

    private var canMutateWorkspace: Bool {
        !workspace.isBusy && workspace.persistentChangesAllowed
    }

    private var canReorderTabs: Bool {
        canMutateWorkspace && workspace.sessions.count > 1
    }

    private func resetDragState() {
        draggingTabID = nil
        dragStartIndex = nil
        dragTargetIndex = nil
        dragTranslation = 0
        isSettlingDrag = false
        pendingCommit = nil
    }

    private func commitPendingDragIfNeeded() {
        guard let pending = pendingCommit else {
            resetDragState()
            return
        }
        pendingCommit = nil
        var transaction = Transaction(animation: nil)
        transaction.disablesAnimations = true
        withTransaction(transaction) {
            if pending.destinationIndex != pending.startIndex {
                workspace.moveDocument(id: pending.id, toIndex: pending.destinationIndex)
            }
            draggingTabID = nil
            dragStartIndex = nil
            dragTargetIndex = nil
            dragTranslation = 0
            isSettlingDrag = false
        }
    }

    private func handleDragChanged(
        id: RawIdentifier,
        rawTranslationX: CGFloat,
        tabWidth: CGFloat,
        scrollable: Bool,
        proxy: ScrollViewProxy
    ) {
        guard canReorderTabs else { return }
        if isSettlingDrag {
            commitPendingDragIfNeeded()
        }
        if draggingTabID == nil {
            guard let liveIndex = workspace.sessions.firstIndex(where: { $0.document.id == id }) else { return }
            draggingTabID = id
            dragStartIndex = liveIndex
            dragTargetIndex = liveIndex
            workspace.selection = id
        }
        guard draggingTabID == id, let start = dragStartIndex else { return }
        let tabCount = workspace.sessions.count
        let clamped = DocumentTabBarMetrics.clampedDragTranslation(
            rawTranslationX: rawTranslationX,
            startIndex: start,
            tabWidth: tabWidth,
            tabCount: tabCount
        )
        dragTranslation = clamped
        let nextTarget = DocumentTabBarMetrics.draggedTabTargetIndex(
            startIndex: start,
            translationX: clamped,
            tabWidth: tabWidth,
            tabCount: tabCount,
            currentTargetIndex: dragTargetIndex ?? start
        )
        if nextTarget != dragTargetIndex {
            dragTargetIndex = nextTarget
            if scrollable, workspace.sessions.indices.contains(nextTarget) {
                let targetSlotID = workspace.sessions[nextTarget].document.id
                withAnimation(.easeOut(duration: 0.15)) {
                    proxy.scrollTo(targetSlotID)
                }
            }
        }
    }

    private func handleDragEnded(id: RawIdentifier, tabWidth: CGFloat) {
        guard draggingTabID == id, let start = dragStartIndex else { return }
        let finalIndex = dragTargetIndex ?? start
        let finalTranslation = CGFloat(finalIndex - start) * tabWidth
        isSettlingDrag = true
        pendingCommit = (id: id, startIndex: start, destinationIndex: finalIndex)
        withAnimation(.snappy(duration: 0.18, extraBounce: 0)) {
            dragTranslation = finalTranslation
        } completion: {
            guard pendingCommit?.id == id else { return }
            commitPendingDragIfNeeded()
        }
    }

    var body: some View {
        let documentIDs = workspace.sessions.map(\.document.id)
        let separatorColor = Color(nsColor: .separatorColor).opacity(colorScheme == .dark ? 0.72 : 0.6)

        GlassEffectContainer(spacing: 4) {
            HStack(spacing: 6) {
                GeometryReader { geometry in
                    let viewportWidth = geometry.size.width
                    let tabCount = workspace.sessions.count
                    let resolvedTabWidth = DocumentTabBarMetrics.tabWidth(
                        forViewportWidth: viewportWidth,
                        tabCount: tabCount
                    )
                    let scrollable = DocumentTabBarMetrics.needsHorizontalScroll(
                        forViewportWidth: viewportWidth,
                        tabCount: tabCount
                    )

                    ScrollViewReader { proxy in
                        ScrollView(.horizontal, showsIndicators: false) {
                            HStack(spacing: 0) {
                                ForEach(Array(workspace.sessions.enumerated()), id: \.element.document.id) { index, session in
                                    let id = session.document.id
                                    let isDragging = draggingTabID == id
                                    let showsSeparator = DocumentTabBarMetrics.shouldShowTrailingSeparator(
                                        at: index,
                                        documentIDs: documentIDs,
                                        selectedID: workspace.selection,
                                        hoveredID: hoveredTabID,
                                        draggingID: draggingTabID
                                    )
                                    let offsetX: CGFloat = if isDragging {
                                        dragTranslation
                                    } else if let start = dragStartIndex, let target = dragTargetIndex {
                                        DocumentTabBarMetrics.tabDisplacementOffset(
                                            forIndex: index,
                                            dragStartIndex: start,
                                            dragTargetIndex: target,
                                            tabWidth: resolvedTabWidth
                                        )
                                    } else {
                                        0
                                    }

                                    DocumentTabItem(
                                        workspace: workspace,
                                        session: session,
                                        tabWidth: resolvedTabWidth,
                                        isSelected: workspace.selection == id,
                                        isHovered: hoveredTabID == id,
                                        isDragging: isDragging,
                                        canReorder: canReorderTabs,
                                        showsTrailingSeparator: showsSeparator,
                                        separatorColor: separatorColor,
                                        onHoverChanged: { hovering in
                                            if hovering {
                                                hoveredTabID = id
                                            } else if hoveredTabID == id {
                                                hoveredTabID = nil
                                            }
                                        },
                                        onDragChanged: { translationX in
                                            handleDragChanged(
                                                id: id,
                                                rawTranslationX: translationX,
                                                tabWidth: resolvedTabWidth,
                                                scrollable: scrollable,
                                                proxy: proxy
                                            )
                                        },
                                        onDragEnded: {
                                            handleDragEnded(id: id, tabWidth: resolvedTabWidth)
                                        }
                                    )
                                    .offset(x: offsetX)
                                    .zIndex(isDragging ? 10 : (workspace.selection == id ? 2 : 0))
                                    .animation(
                                        isDragging ? nil : .snappy(duration: 0.22, extraBounce: 0),
                                        value: dragTargetIndex
                                    )
                                    .id(id)
                                }
                            }
                            .coordinateSpace(name: DocumentTabBarMetrics.trackCoordinateSpace)
                            .padding(.horizontal, DocumentTabBarMetrics.horizontalInset)
                            .frame(
                                minWidth: viewportWidth,
                                minHeight: DocumentTabBarMetrics.pillTrackHeight,
                                maxHeight: DocumentTabBarMetrics.pillTrackHeight,
                                alignment: .leading
                            )
                            .accessibilityAddTraits(.isTabBar)
                        }
                        .scrollDisabled(!scrollable)
                        .onChange(of: workspace.selection) { _, newID in
                            guard let newID else { return }
                            withAnimation(.easeOut(duration: 0.15)) {
                                proxy.scrollTo(newID)
                            }
                        }
                    }
                }
                .frame(height: DocumentTabBarMetrics.pillTrackHeight)
                .background {
                    Capsule(style: .continuous)
                        .fill(DocumentTabBarMetrics.pillTrackFillColor(for: colorScheme))
                }
                .overlay {
                    Capsule(style: .continuous)
                        .strokeBorder(DocumentTabBarMetrics.pillTrackBorderColor(for: colorScheme), lineWidth: 0.75)
                }
                .clipShape(Capsule(style: .continuous))

                PanelIconButton(
                    systemImage: "plus",
                    helpKey: "help_new_document",
                    accessibilityKey: "new_file",
                    size: 26,
                    font: .system(size: 12, weight: .semibold),
                    isDisabled: !canMutateWorkspace
                ) {
                    workspace.createDocument()
                }
                .background(DocumentTabBarMetrics.pillTrackFillColor(for: colorScheme), in: Circle())
            }
            .padding(.horizontal, 10)
            .padding(.top, 2)
            .padding(.bottom, 6)
        }
        .frame(height: DocumentTabBarMetrics.barHeight)
        .background(Color(nsColor: .windowBackgroundColor))
        .onChange(of: documentIDs) { _, updatedIDs in
            if let draggingTabID, !updatedIDs.contains(draggingTabID), pendingCommit == nil {
                resetDragState()
            }
        }
        .onChange(of: canMutateWorkspace) { _, allowed in
            if !allowed, draggingTabID != nil {
                resetDragState()
            }
        }
        .disabled(workspace.isLoading || workspace.isClosing || workspace.closingDocumentID != nil || workspace.decidingCloseID != nil)
    }
}

private struct DocumentTabItem: View {
    @Environment(\.colorScheme) private var colorScheme
    let workspace: Workspace
    let session: NativeEditorSession
    let tabWidth: CGFloat
    let isSelected: Bool
    let isHovered: Bool
    let isDragging: Bool
    let canReorder: Bool
    let showsTrailingSeparator: Bool
    let separatorColor: Color
    let onHoverChanged: (Bool) -> Void
    let onDragChanged: (CGFloat) -> Void
    let onDragEnded: () -> Void

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

    private var showsCloseButton: Bool {
        isHovered && !isDragging && canCloseTab
    }

    private var showsReadOnlyLock: Bool {
        !showsCloseButton && !session.canMutatePersistence
    }

    private var showsLeadingAccessory: Bool {
        showsCloseButton || showsReadOnlyLock
    }

    private var showsDirtyDot: Bool {
        session.document.isDirty
    }

    private var centerCluster: some View {
        HStack(spacing: 4) {
            Image(systemName: iconName)
                .font(.caption.weight(isSelected ? .semibold : .regular))
                .foregroundStyle(isSelected ? Color.accentColor : Color.secondary.opacity(0.85))
                .fixedSize()

            Text(workspace.title(for: session))
                .font(.callout.weight(isSelected ? .semibold : .regular))
                .foregroundStyle(isSelected ? Color.primary : Color.secondary.opacity(0.85))
                .lineLimit(1)

            if let state = runState, !state.isTerminal {
                ProgressView()
                    .controlSize(.mini)
                    .fixedSize()
            } else if let result = runResult {
                let descriptor = VerdictDescriptor.summary(for: result)
                let resultIsCurrent = workspace.runner.resultIsCurrent(for: session)
                if DocumentTabBarMetrics.showsCompactVerdictText(label: descriptor.label, tabWidth: tabWidth) {
                    VerdictBadge(
                        descriptor: descriptor,
                        compact: true,
                        dimmed: !resultIsCurrent
                    )
                    .fixedSize()
                    .help(resultIsCurrent ? "" : L10n.string("run_result_outdated_help"))
                } else {
                    Image(systemName: descriptor.symbol)
                        .font(.system(size: 10, weight: .semibold))
                        .foregroundStyle(descriptor.tone.color.opacity(resultIsCurrent ? 0.9 : 0.45))
                        .fixedSize()
                        .help(resultIsCurrent ? descriptor.label : "\(descriptor.label) — \(L10n.string("run_result_outdated_help"))")
                }
            }
        }
    }

    private var tabRowContent: some View {
        centerCluster
            .padding(.leading, showsLeadingAccessory ? 22 : 8)
            .padding(.trailing, showsDirtyDot ? 16 : 8)
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .center)
            .overlay(alignment: .leading) {
                if showsCloseButton {
                    Button {
                        Task { await workspace.closeDocument(session) }
                    } label: {
                        Image(systemName: "xmark")
                            .font(.system(size: 9, weight: .bold))
                            .foregroundStyle(.secondary)
                            .panelIconHoverChrome(size: 16, cornerRadius: 8, enabled: canCloseTab)
                    }
                    .buttonStyle(.borderless)
                    .help(L10n.string("close_tab"))
                    .padding(.leading, 5)
                } else if showsReadOnlyLock {
                    Image(systemName: "lock.fill")
                        .font(.system(size: 9))
                        .foregroundStyle(.secondary)
                        .accessibilityLabel(L10n.string("readonly_status"))
                        .padding(.leading, 6)
                }
            }
            .overlay(alignment: .trailing) {
                if showsDirtyDot {
                    Circle()
                        .fill(Color.orange)
                        .frame(width: 6, height: 6)
                        .accessibilityLabel(L10n.string("unsaved_changes"))
                        .padding(.trailing, 7)
                }
            }
    }

    var body: some View {
        let dragGesture = DragGesture(
            minimumDistance: 6,
            coordinateSpace: .named(DocumentTabBarMetrics.trackCoordinateSpace)
        )
        .onChanged { value in
            onDragChanged(value.translation.width)
        }
        .onEnded { _ in
            onDragEnded()
        }

        tabRowContent
            .frame(width: max(0, tabWidth - 2), height: DocumentTabBarMetrics.tabCapsuleHeight)
            .background {
                if !isSelected && isHovered {
                    Capsule(style: .continuous)
                        .fill(DocumentTabBarMetrics.hoverCapsuleBackgroundColor(for: colorScheme))
                }
            }
            .glassEffect(
                isSelected ? .regular.interactive() : .identity,
                in: Capsule(style: .continuous)
            )
            .frame(width: tabWidth, height: DocumentTabBarMetrics.pillTrackHeight)
            .overlay(alignment: .trailing) {
                if showsTrailingSeparator {
                    Rectangle()
                        .fill(separatorColor)
                        .frame(width: 1, height: 13)
                }
            }
            .contentShape(Rectangle())
            .accessibilityAddTraits(isSelected ? [.isButton, .isSelected] : [.isButton])
            .onTapGesture {
                workspace.selection = session.document.id
            }
            .onHover(perform: onHoverChanged)
            .gesture(dragGesture, isEnabled: canReorder)
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
        .padding(.vertical, 4)
        .background(Color(nsColor: .windowBackgroundColor))
    }
}
