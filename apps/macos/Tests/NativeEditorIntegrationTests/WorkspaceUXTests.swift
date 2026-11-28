import AppKit
import SwiftUI
import Testing
import NativeEditorModel
import NativeCoreBridge
@testable import CPEditorNative

@MainActor @Suite(.serialized) struct WorkspaceUXTests {
    private func folder() throws -> URL {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        return directory
    }

    private func dispose(_ workspace: Workspace) async {
        await workspace.companion.shutdown()
        await workspace.recovery?.shutdown()
        await workspace.runner.shutdown()
        workspace.sessions.forEach { $0.dispose() }
    }

    private func editableTextView(in view: NSView) -> NSTextView? {
        if let text = view as? NSTextView, text.isEditable { return text }
        for child in view.subviews {
            if let found = editableTextView(in: child) { return found }
        }
        return nil
    }

    @Test func titlesAndSubtitlesReflectCompanionMetadataAndHideInternalEditorNames() async throws {
        _ = NSApplication.shared
        let directory = try folder()
        defer { try? FileManager.default.removeItem(at: directory) }
        let recoveryFile = directory.appendingPathComponent("ux-recovery.json")
        let workspace = Workspace(arguments: ["--editor", "sttextview"], recoveryURL: recoveryFile)
        await workspace.bootstrap()

        let first = try #require(workspace.activeSession)
        #expect(workspace.title(for: first) == "Untitled")
        #expect(!workspace.sidebarSubtitle(for: first).contains("STTextView"))
        #expect(!workspace.windowSubtitle(for: first).contains("STTextView"))

        let second = try #require(workspace.createDocument(editor: .codeEditorView))
        #expect(workspace.title(for: first) == "Untitled")
        #expect(workspace.title(for: second) == "Untitled 2")
        #expect(!workspace.windowSubtitle(for: second).contains("CodeEditorView"))

        let payloadFile = directory.appendingPathComponent("problem.json")
        let payloadJSON = """
        {"name":"A. Two Pointers","group":"Codeforces Round 999","url":"https://example.org/problem/A","memoryLimit":256,"timeLimit":1500,"interactive":false,"tests":[{"input":"3 4\\n","output":"7\\n"}]}
        """
        try Data(payloadJSON.utf8).write(to: payloadFile)

        await workspace.companion.accept(file: payloadFile)
        #expect(workspace.companion.pendingCount == 1)
        #expect(workspace.companion.appliedCount == 0)

        let inboxItem = try #require(workspace.companion.items.first)
        await workspace.companion.apply(importID: inboxItem.importID)
        #expect(workspace.companion.pendingCount == 0)
        #expect(workspace.companion.appliedCount == 1)

        let imported = try #require(workspace.sessions.first { $0.document.id == inboxItem.documentID })
        #expect(workspace.title(for: imported) == "A. Two Pointers")
        #expect(workspace.sidebarSubtitle(for: imported).contains("Codeforces Round 999"))
        #expect(workspace.windowSubtitle(for: imported).contains("1500 ms"))
        #expect(workspace.companionLimitsSummary(for: imported)?.contains("1500 ms") == true)
        #expect(await workspace.recovery?.flush() == true)
        await dispose(workspace)

        let reopened = Workspace(recoveryURL: recoveryFile)
        await reopened.bootstrap()
        let reopenedImported = try #require(reopened.sessions.first { $0.document.id == inboxItem.documentID })
        #expect(reopened.title(for: reopenedImported) == "A. Two Pointers")

        let savedPath = directory.appendingPathComponent("solution.cpp")
        #expect(await reopened.saveSource(reopenedImported, to: savedPath, selectedReplacement: true))
        #expect(reopened.title(for: reopenedImported) == "solution.cpp")
        #expect(reopened.windowSubtitle(for: reopenedImported).contains("A. Two Pointers"))
        await dispose(reopened)
    }

    @Test(arguments: EditorKind.allCases) func layoutModesAndRunningPreserveMountedEditorAndUndoHistory(editor: EditorKind) async throws {
        _ = NSApplication.shared
        let workspace = Workspace(arguments: ["--editor", editor.rawValue])
        let session = try #require(workspace.activeSession)
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 1180, height: 740),
            styleMask: [.titled, .resizable],
            backing: .buffered,
            defer: false
        )
        let host = NSHostingView(rootView: WorkspaceView(workspace: workspace))
        window.contentView = host
        window.orderBack(nil)
        host.layoutSubtreeIfNeeded()
        defer {
            window.orderOut(nil)
            window.contentView = nil
            Task { await workspace.runner.shutdown() }
            workspace.sessions.forEach { $0.dispose() }
        }

        let initialNative: NSView = editor == .stTextView
            ? try #require(session.stView)
            : try #require(editableTextView(in: session.container))
        window.makeFirstResponder(initialNative)
        let source = "#include <iostream>\nint main(){int a,b;if(std::cin>>a>>b)std::cout<<a+b<<'\\n';}\n"
        session.container.history.groupsByEvent = false
        session.container.history.beginUndoGrouping()
        if let stView = session.stView {
            stView.insertText(source, replacementRange: NSRange(location: 0, length: 0))
        } else if let textView = initialNative as? NSTextView {
            textView.insertText(source, replacementRange: NSRange(location: 0, length: 0))
        }
        session.container.history.endUndoGrouping()
        await Task.yield()
        session.capture()
        #expect(session.document.text == source)
        #expect(session.container.history.canUndo)

        for mode in [WorkspaceLayoutMode.editorOnly, .ioOnly, .split] {
            workspace.layoutMode = mode
            await Task.yield()
            host.layoutSubtreeIfNeeded()
            if editor == .stTextView {
                #expect(session.stView === initialNative)
            } else {
                #expect(editableTextView(in: session.container) === initialNative)
            }
            #expect(session.container.history.canUndo)
        }

        workspace.layoutMode = .editorOnly
        session.testCases = [TestCaseInput(id: "ux-sample", input: "10 20\n", expected: "30\n")]
        workspace.runActiveDocument()
        #expect(workspace.layoutMode == .split)
        await workspace.runner.waitForCurrentRun()
        host.layoutSubtreeIfNeeded()

        let result = try #require(workspace.runner.result(for: session.document.id))
        #expect(VerdictDescriptor.summary(for: result).label == "AC")
        if editor == .stTextView {
            #expect(session.stView === initialNative)
        } else {
            #expect(editableTextView(in: session.container) === initialNative)
        }

        session.container.history.undo()
        await Task.yield()
        session.capture()
        #expect(session.document.text.isEmpty)
        await workspace.runner.shutdown()
    }

    @Test func closingBackgroundAndSavedTabsPreserveUnsavedDocuments() async throws {
        _ = NSApplication.shared
        let workspace = Workspace(arguments: ["--editor", "sttextview"])
        let first = try #require(workspace.activeSession)
        let dirtyMiddle = try #require(workspace.createDocument(text: "// unsaved", editor: .stTextView))
        let cleanTrailing = try #require(workspace.createDocument(editor: .stTextView))
        workspace.selection = dirtyMiddle.document.id

        await workspace.closeDocument(cleanTrailing)
        #expect(!workspace.sessions.contains { $0 === cleanTrailing })
        #expect(workspace.selection == dirtyMiddle.document.id)

        await workspace.closeSavedDocuments()
        #expect(workspace.sessions.count == 1)
        #expect(workspace.sessions.first === dirtyMiddle)
        #expect(!workspace.sessions.contains { $0 === first })

        await workspace.runner.shutdown()
        workspace.sessions.forEach { $0.dispose() }
    }

    @Test func toolchainDisplayNamesAndVisualFormUpdatesPreserveUnknownFields() throws {
        let runner = RunController()
        #expect(runner.profiles.count >= 2)
        let cpp17 = try #require(runner.profiles.first { $0.id == RawIdentifier("macos-clang-cpp17") })
        let cpp20 = try #require(runner.profiles.first { $0.id == RawIdentifier("macos-clang-cpp20") })
        #expect(cpp17.displayName == "C++17 (Clang)")
        #expect(cpp20.displayName == "C++20 (Clang)")
        if let gcc17 = runner.profiles.first(where: { $0.id == RawIdentifier("macos-gcc-cpp17") }) {
            #expect(gcc17.displayName == "C++17 (GCC)")
        }
        if let gcc20 = runner.profiles.first(where: { $0.id == RawIdentifier("macos-gcc-cpp20") }) {
            #expect(gcc20.displayName == "C++20 (GCC)")
        }

        let rawWithExtra = """
        {"schema_version":1,"id":"custom-cpp","language_id":"cpp","source_filename":"main.cpp","artifact_name":"prog","build":{"executable":"/usr/bin/clang++","arguments":["-std=c++20","${source}","-o","${artifact}"]},"run":{"executable":"${artifact}","arguments":[]},"build_limits":{"wall_timeout_ms":30000},"run_limits":{"wall_timeout_ms":2000},"variables":[{"name":"custom_var","value":"42"}]}
        """
        let decoded = try JSONDecoder().decode(ToolchainProfile.self, from: Data(rawWithExtra.utf8))
        let updated = try decoded.updating(
            buildExecutable: "/usr/bin/g++",
            buildArguments: ["-std=c++23", "-O2", "${source}", "-o", "${artifact}"],
            buildWallTimeoutMS: 15000,
            runWallTimeoutMS: 3500
        )
        #expect(updated.buildExecutable == "/usr/bin/g++")
        #expect(updated.buildWallTimeoutMS == 15000)
        #expect(updated.runWallTimeoutMS == 3500)
        #expect(updated.displayName.contains("GCC"))
        #expect(updated.payload["variables"]?.array?.count == 1)

        let baseChecker = try CheckerConfiguration(jsonText: try runner.checkerDraft())
        let updatedCheckerText = try baseChecker.updatingJSON(
            executable: "/usr/bin/true",
            arguments: ["${input}", "${actual}", "${expected}"],
            wallTimeoutMS: 4500,
            acceptedExitCodes: [0, 3],
            wrongAnswerExitCodes: [1]
        )
        let parsedChecker = try CheckerConfiguration(jsonText: updatedCheckerText)
        let values = parsedChecker.formValues()
        #expect(values.executable == "/usr/bin/true")
        #expect(values.wallTimeoutMS == 4500)
        #expect(values.acceptedExitCodes == [0, 3])
        #expect(values.wrongAnswerExitCodes == [1])
        #expect(updatedCheckerText.contains("spawn_timeout_ms"))
        Task { await runner.shutdown() }
    }

    @Test func verdictSummaryPrioritizesBuildFailuresAndSevereVerdicts() throws {
        func makeResult(buildOutcome: String = "Succeeded", includeBuild: Bool = false, verdicts: [String]) throws -> BatchResult {
            let testsJSON = verdicts.enumerated().map { index, verdict in
                """
                {"testcase_id":"t-\(index)","verdict":"\(verdict)","detail":"","process":{"termination":"Exited","elapsed_ms":12,"stdout":{"encoding":"base64","data":""},"stderr":{"encoding":"base64","data":""},"detail":"","exit_code":0,"stdout_truncated":false,"stderr_truncated":false,"native_error":0,"stdout_total_bytes":0,"stderr_total_bytes":0}}
                """
            }.joined(separator: ",")
            let buildField = includeBuild
                ? ",\"build\":{\"termination\":\"Exited\",\"elapsed_ms\":48,\"stdout\":{\"encoding\":\"base64\",\"data\":\"\"},\"stderr\":{\"encoding\":\"base64\",\"data\":\"\"},\"detail\":\"\",\"exit_code\":0,\"stdout_truncated\":false,\"stderr_truncated\":false,\"native_error\":0,\"stdout_total_bytes\":0,\"stderr_total_bytes\":0}"
                : ""
            let payload = """
            {"schema_version":1,"document_id":"doc","revision":1,"profile_id":"macos-clang-cpp17","source_digest":"00","build_outcome":"\(buildOutcome)","error":""\(buildField),"tests":[\(testsJSON)]}
            """
            return try JSONDecoder().decode(BatchResult.self, from: Data(payload.utf8))
        }

        #expect(VerdictDescriptor.summary(for: try makeResult(buildOutcome: "CompilationFailed", verdicts: [])).label == "CE")
        #expect(VerdictDescriptor.summary(for: try makeResult(buildOutcome: "Cancelled", verdicts: [])).label == "Cancelled")
        #expect(VerdictDescriptor.summary(for: try makeResult(buildOutcome: "Succeeded", includeBuild: true, verdicts: [])).label == L10n.string("verdict_compiled"))
        #expect(VerdictDescriptor.summary(for: try makeResult(buildOutcome: "NotRequired", verdicts: ["AC", "AC"])).label == "AC")
        #expect(VerdictDescriptor.summary(for: try makeResult(buildOutcome: "NotRequired", verdicts: ["AC", "WA"])).label == "WA")
        #expect(VerdictDescriptor.summary(for: try makeResult(verdicts: ["AC", "AC"])).label == "AC")
        #expect(VerdictDescriptor.summary(for: try makeResult(verdicts: ["AC", "WA"])).label == "WA")
        #expect(VerdictDescriptor.summary(for: try makeResult(verdicts: ["WA", "TLE"])).label == "TLE")
        #expect(VerdictDescriptor.summary(for: try makeResult(verdicts: ["TLE", "RE"])).label == "RE")
        #expect(VerdictDescriptor.summary(for: try makeResult(verdicts: ["RE", "CheckerError"])).label == "CheckerError")
    }

    @Test func testCaseGridOrderAndHorizontalScrollMetrics() {
        #expect(TestCaseGridMetrics.columnOrder == [.input, .output, .expected])

        for viewport in [CGFloat(325), CGFloat(507)] {
            let narrow = TestCaseGridMetrics(viewportWidth: viewport)
            #expect(narrow.columnWidth == 220)
            #expect(narrow.contentWidth == 662)
            #expect(narrow.needsHorizontalScroll)
        }

        let exactThreshold = TestCaseGridMetrics(viewportWidth: 662)
        #expect(exactThreshold.columnWidth == 220)
        #expect(exactThreshold.contentWidth == 662)
        #expect(!exactThreshold.needsHorizontalScroll)

        let ioOnly = TestCaseGridMetrics(viewportWidth: 1214)
        #expect(ioOnly.columnWidth == 404)
        #expect(ioOnly.contentWidth == 1214)
        #expect(!ioOnly.needsHorizontalScroll)
    }

    @Test func tabBarMetricsAndSeparatorVisibility() {
        #expect(DocumentTabBarMetrics.barHeight == 38)
        #expect(DocumentTabBarMetrics.pillTrackHeight == 30)
        #expect(DocumentTabBarMetrics.tabCapsuleHeight == 26)
        #expect(DocumentTabBarMetrics.horizontalInset == 2)
        #expect(DocumentTabBarMetrics.minimumTabWidth == 120)
        #expect(DocumentTabBarMetrics.dragHysteresis == 8)
        #expect(DocumentTabBarMetrics.tabCapsuleHeight < DocumentTabBarMetrics.pillTrackHeight)
        #expect(DocumentTabBarMetrics.pillTrackHeight < DocumentTabBarMetrics.barHeight)

        // 无标签或无效宽度时回退到最小宽度且不触发横向滚动。
        #expect(DocumentTabBarMetrics.tabWidth(forViewportWidth: 800, tabCount: 0) == 120)
        #expect(DocumentTabBarMetrics.tabWidth(forViewportWidth: .nan, tabCount: 3) == 120)
        #expect(!DocumentTabBarMetrics.needsHorizontalScroll(forViewportWidth: 800, tabCount: 0))
        #expect(!DocumentTabBarMetrics.needsHorizontalScroll(forViewportWidth: .infinity, tabCount: 3))

        // 宽屏等宽自适应：viewport 904 -> usable 900；1 个占 900，3 个各 300，5 个各 180。
        #expect(DocumentTabBarMetrics.tabWidth(forViewportWidth: 904, tabCount: 1) == 900)
        #expect(DocumentTabBarMetrics.tabWidth(forViewportWidth: 904, tabCount: 3) == 300)
        #expect(DocumentTabBarMetrics.tabWidth(forViewportWidth: 904, tabCount: 5) == 180)
        #expect(!DocumentTabBarMetrics.needsHorizontalScroll(forViewportWidth: 904, tabCount: 1))
        #expect(!DocumentTabBarMetrics.needsHorizontalScroll(forViewportWidth: 904, tabCount: 3))
        #expect(!DocumentTabBarMetrics.needsHorizontalScroll(forViewportWidth: 904, tabCount: 5))

        // 分屏默认宽度与挤压窄宽边界：usable 660 (viewport 664) 时 5 个各 132 不滚动；usable 360 (viewport 364) 时 3 个恰好 120 不滚动，少 1 pt 即触发滚动。
        #expect(DocumentTabBarMetrics.tabWidth(forViewportWidth: 664, tabCount: 5) == 132)
        #expect(!DocumentTabBarMetrics.needsHorizontalScroll(forViewportWidth: 664, tabCount: 5))
        #expect(DocumentTabBarMetrics.tabWidth(forViewportWidth: 364, tabCount: 3) == 120)
        #expect(!DocumentTabBarMetrics.needsHorizontalScroll(forViewportWidth: 364, tabCount: 3))
        #expect(DocumentTabBarMetrics.tabWidth(forViewportWidth: 363, tabCount: 3) == 120)
        #expect(DocumentTabBarMetrics.needsHorizontalScroll(forViewportWidth: 363, tabCount: 3))

        // Test Inspector 展开挤压与多标签溢出时钳制在 120 并开启横向滚动。
        #expect(DocumentTabBarMetrics.tabWidth(forViewportWidth: 400, tabCount: 5) == 120)
        #expect(DocumentTabBarMetrics.needsHorizontalScroll(forViewportWidth: 400, tabCount: 5))
        #expect(DocumentTabBarMetrics.tabWidth(forViewportWidth: 904, tabCount: 10) == 120)
        #expect(DocumentTabBarMetrics.needsHorizontalScroll(forViewportWidth: 904, tabCount: 10))

        // Inspector 宽度约束与布局模式往返映射。
        #expect(WorkspaceInspectorMetrics.minWidth == 320)
        #expect(WorkspaceInspectorMetrics.idealWidth == 510)
        #expect(WorkspaceInspectorMetrics.maxWidth == 700)
        #expect(!WorkspaceInspectorMetrics.isInspectorPresented(for: .editorOnly))
        #expect(WorkspaceInspectorMetrics.isInspectorPresented(for: .split))
        #expect(!WorkspaceInspectorMetrics.isInspectorPresented(for: .ioOnly))
        #expect(WorkspaceInspectorMetrics.layoutModeAfterInspectorToggle(isPresented: false, current: .split) == .editorOnly)
        #expect(WorkspaceInspectorMetrics.layoutModeAfterInspectorToggle(isPresented: true, current: .editorOnly) == .split)
        #expect(WorkspaceInspectorMetrics.layoutModeAfterInspectorToggle(isPresented: false, current: .ioOnly) == .ioOnly)
        #expect(WorkspaceInspectorMetrics.layoutModeAfterInspectorToggle(isPresented: true, current: .ioOnly) == .split)
        for mode in [WorkspaceLayoutMode.split, .editorOnly, .ioOnly] {
            let nextPresented = !WorkspaceInspectorMetrics.isInspectorPresented(for: mode)
            let toggled = WorkspaceInspectorMetrics.layoutModeAfterInspectorToggle(isPresented: nextPresented, current: mode)
            let expected: WorkspaceLayoutMode = (mode == .split) ? .editorOnly : .split
            #expect(toggled == expected)
        }

        let ids = [RawIdentifier("doc-1"), RawIdentifier("doc-2"), RawIdentifier("doc-3"), RawIdentifier("doc-4")]
        // 选中 doc-2 时，doc-1|doc-2 与 doc-2|doc-3 隐藏分隔线，仅 doc-3|doc-4 显示。
        #expect(!DocumentTabBarMetrics.shouldShowTrailingSeparator(at: 0, documentIDs: ids, selectedID: ids[1], hoveredID: nil))
        #expect(!DocumentTabBarMetrics.shouldShowTrailingSeparator(at: 1, documentIDs: ids, selectedID: ids[1], hoveredID: nil))
        #expect(DocumentTabBarMetrics.shouldShowTrailingSeparator(at: 2, documentIDs: ids, selectedID: ids[1], hoveredID: nil))
        #expect(!DocumentTabBarMetrics.shouldShowTrailingSeparator(at: 3, documentIDs: ids, selectedID: ids[1], hoveredID: nil))
        #expect(!DocumentTabBarMetrics.shouldShowTrailingSeparator(at: -1, documentIDs: ids, selectedID: ids[1], hoveredID: nil))

        // 悬停相邻标签时隐藏分隔线；拖动重排期间隐藏全部内部分隔线。
        #expect(!DocumentTabBarMetrics.shouldShowTrailingSeparator(at: 2, documentIDs: ids, selectedID: ids[0], hoveredID: ids[3]))
        #expect(!DocumentTabBarMetrics.shouldShowTrailingSeparator(at: 2, documentIDs: ids, selectedID: ids[0], hoveredID: ids[2]))
        #expect(DocumentTabBarMetrics.shouldShowTrailingSeparator(at: 1, documentIDs: ids, selectedID: ids[0], hoveredID: ids[3]))
        #expect(!DocumentTabBarMetrics.shouldShowTrailingSeparator(at: 1, documentIDs: ids, selectedID: ids[0], hoveredID: nil, draggingID: ids[0]))

        // 拖动排序位移钳制、迟滞防抖与邻接让位偏移。
        #expect(DocumentTabBarMetrics.clampedDragTranslation(rawTranslationX: -50, startIndex: 0, tabWidth: 120, tabCount: 4) == 0)
        #expect(DocumentTabBarMetrics.clampedDragTranslation(rawTranslationX: 500, startIndex: 1, tabWidth: 120, tabCount: 4) == 240)
        // center = 60 + 65 = 125（刚过 120 边界但未跨过 +8 pt 迟滞），仍保持在 0；到达 128（translation 68）进入 1。
        #expect(DocumentTabBarMetrics.draggedTabTargetIndex(startIndex: 0, translationX: 65, tabWidth: 120, tabCount: 4, currentTargetIndex: 0) == 0)
        #expect(DocumentTabBarMetrics.draggedTabTargetIndex(startIndex: 0, translationX: 68, tabWidth: 120, tabCount: 4, currentTargetIndex: 0) == 1)
        // 回退同样受迟滞保护：center = 115（translation 55）保持在 1；center = 112（translation 52）退回 0。
        #expect(DocumentTabBarMetrics.draggedTabTargetIndex(startIndex: 0, translationX: 55, tabWidth: 120, tabCount: 4, currentTargetIndex: 1) == 1)
        #expect(DocumentTabBarMetrics.draggedTabTargetIndex(startIndex: 0, translationX: 52, tabWidth: 120, tabCount: 4, currentTargetIndex: 1) == 0)

        #expect(DocumentTabBarMetrics.tabDisplacementOffset(forIndex: 0, dragStartIndex: 0, dragTargetIndex: 2, tabWidth: 120) == 0)
        #expect(DocumentTabBarMetrics.tabDisplacementOffset(forIndex: 1, dragStartIndex: 0, dragTargetIndex: 2, tabWidth: 120) == -120)
        #expect(DocumentTabBarMetrics.tabDisplacementOffset(forIndex: 2, dragStartIndex: 0, dragTargetIndex: 2, tabWidth: 120) == -120)
        #expect(DocumentTabBarMetrics.tabDisplacementOffset(forIndex: 3, dragStartIndex: 0, dragTargetIndex: 2, tabWidth: 120) == 0)
        #expect(DocumentTabBarMetrics.tabDisplacementOffset(forIndex: 0, dragStartIndex: 2, dragTargetIndex: 0, tabWidth: 120) == 120)
        #expect(DocumentTabBarMetrics.tabDisplacementOffset(forIndex: 1, dragStartIndex: 2, dragTargetIndex: 0, tabWidth: 120) == 120)
        #expect(DocumentTabBarMetrics.tabDisplacementOffset(forIndex: 2, dragStartIndex: 2, dragTargetIndex: 0, tabWidth: 120) == 0)

        // 窄标签下仅保留短判题徽章全文（AC/WA/CE），较长标签在 >=170 pt 时才展示全文。
        #expect(DocumentTabBarMetrics.showsCompactVerdictText(label: "AC", tabWidth: 120))
        #expect(DocumentTabBarMetrics.showsCompactVerdictText(label: "WA", tabWidth: 120))
        #expect(DocumentTabBarMetrics.showsCompactVerdictText(label: "CE", tabWidth: 120))
        #expect(!DocumentTabBarMetrics.showsCompactVerdictText(label: "Unchecked", tabWidth: 130))
        #expect(DocumentTabBarMetrics.showsCompactVerdictText(label: "Unchecked", tabWidth: 170))
    }

    @Test func reorderingTabsPersistsOrderAndPreservesEditorsAndSelection() async throws {
        _ = NSApplication.shared
        let directory = try folder()
        defer { try? FileManager.default.removeItem(at: directory) }
        let recoveryFile = directory.appendingPathComponent("reorder-recovery.json")
        let workspace = Workspace(arguments: ["--editor", "sttextview"], recoveryURL: recoveryFile)
        await workspace.bootstrap()

        let docA = try #require(workspace.activeSession)
        let docB = try #require(workspace.createDocument(editor: .stTextView))
        let docC = try #require(workspace.createDocument(editor: .stTextView))
        workspace.selection = docB.document.id

        let stViewB = try #require(docB.stView)
        docB.container.history.groupsByEvent = false
        docB.container.history.beginUndoGrouping()
        stViewB.insertText("// keep undoable buffer", replacementRange: NSRange(location: 0, length: 0))
        docB.container.history.endUndoGrouping()
        await Task.yield()
        docB.capture()

        #expect(workspace.moveDocument(id: docA.document.id, toIndex: 2))
        #expect(workspace.sessions.map(\.document.id) == [docB.document.id, docC.document.id, docA.document.id])
        #expect(workspace.selection == docB.document.id)
        #expect(docB.document.text == "// keep undoable buffer")
        #expect(docB.container.history.canUndo)

        #expect(!workspace.moveDocument(id: docA.document.id, toIndex: 99))
        #expect(!workspace.moveDocument(id: RawIdentifier("missing"), toIndex: 1))
        #expect(workspace.moveDocument(id: docC.document.id, toIndex: -5))
        #expect(workspace.sessions.map(\.document.id) == [docC.document.id, docB.document.id, docA.document.id])
        #expect(!workspace.moveDocument(id: docC.document.id, toIndex: 0))

        #expect(await workspace.recovery?.flush() == true)
        await dispose(workspace)

        let reopened = Workspace(recoveryURL: recoveryFile)
        await reopened.bootstrap()
        #expect(reopened.sessions.map(\.document.id) == [docC.document.id, docB.document.id, docA.document.id])
        #expect(reopened.selection == docB.document.id)

        reopened.persistence.freezeRestored()
        #expect(!reopened.moveDocument(id: docA.document.id, toIndex: 0))
        #expect(reopened.sessions.map(\.document.id) == [docC.document.id, docB.document.id, docA.document.id])
        await dispose(reopened)
    }

    @Test func renderLayoutAuditSnapshotsWhenRequested() async throws {
        guard let outputPath = ProcessInfo.processInfo.environment["CPN_LAYOUT_AUDIT_DIR"], !outputPath.isEmpty else { return }
        _ = NSApplication.shared
        let auditDir = URL(fileURLWithPath: outputPath)
        try FileManager.default.createDirectory(at: auditDir, withIntermediateDirectories: true)

        func snapshot<Content: View>(
            _ rootView: Content,
            size: NSSize,
            appearanceName: NSAppearance.Name = .aqua,
            to filename: String
        ) throws {
            let window = NSWindow(
                contentRect: NSRect(origin: .zero, size: size),
                styleMask: [.titled, .closable, .resizable],
                backing: .buffered,
                defer: false
            )
            window.appearance = NSAppearance(named: appearanceName)
            let host = NSHostingView(rootView: rootView.frame(width: size.width, height: size.height))
            host.sizingOptions = []
            host.frame = NSRect(origin: .zero, size: size)
            window.contentView = host
            window.orderBack(nil)
            defer {
                window.orderOut(nil)
                window.contentView = nil
            }
            window.displayIfNeeded()
            host.layoutSubtreeIfNeeded()
            RunLoop.current.run(until: Date().addingTimeInterval(0.08))
            host.layoutSubtreeIfNeeded()
            guard let rep = host.bitmapImageRepForCachingDisplay(in: host.bounds) else {
                throw CocoaError(.coderInvalidValue)
            }
            host.cacheDisplay(in: host.bounds, to: rep)
            guard let png = rep.representation(using: .png, properties: [:]) else {
                throw CocoaError(.coderInvalidValue)
            }
            try png.write(to: auditDir.appendingPathComponent(filename))
        }

        let tempDir = try folder()
        defer { try? FileManager.default.removeItem(at: tempDir) }
        let recoveryFile = tempDir.appendingPathComponent("audit-recovery.json")
        let workspace = Workspace(arguments: ["--editor", "codeeditorview"], recoveryURL: recoveryFile)
        await workspace.bootstrap()

        let sumJSON = """
        {"name":"A. Two Pointers","group":"Codeforces Round 999","url":"https://codeforces.com/contest/999/problem/A","memoryLimit":256,"timeLimit":1500,"interactive":false,"tests":[{"input":"7 8\\n","output":"15\\n"},{"input":"100 250\\n","output":"351\\n"}]}
        """
        let interactiveJSON = """
        {"name":"C. Guess the Number","group":"Interactive Finals","url":"https://codeforces.com/contest/999/problem/C","memoryLimit":512,"timeLimit":2000,"interactive":true,"tests":[]}
        """
        let sumFile = tempDir.appendingPathComponent("sum.json")
        let interactiveFile = tempDir.appendingPathComponent("interactive.json")
        try Data(sumJSON.utf8).write(to: sumFile)
        try Data(interactiveJSON.utf8).write(to: interactiveFile)

        await workspace.companion.accept(file: sumFile)
        await workspace.companion.accept(file: interactiveFile)
        let firstItem = try #require(workspace.companion.items.first)
        let secondItem = try #require(workspace.companion.items.last)
        await workspace.companion.apply(importID: firstItem.importID)
        await workspace.companion.apply(importID: secondItem.importID)

        let pendingJSON = """
        {"name":"D. Dynamic Tree Queries","group":"Codeforces Round 999","url":"https://codeforces.com/contest/999/problem/D","memoryLimit":512,"timeLimit":3000,"interactive":false,"tests":[{"input":"5 3\\n1 2\\n2 3\\n3 4\\n4 5\\n","output":"42\\n17\\n99\\n"}]}
        """
        let pendingFile = tempDir.appendingPathComponent("pending.json")
        try Data(pendingJSON.utf8).write(to: pendingFile)
        await workspace.companion.accept(file: pendingFile)

        let sumSession = try #require(workspace.sessions.first { $0.document.id == firstItem.documentID })
        let interactiveSession = try #require(workspace.sessions.first { $0.document.id == secondItem.documentID })
        workspace.selection = sumSession.document.id

        func populateCodeEditor(_ session: NativeEditorSession, source: String) async throws {
            let warmupHost = NSHostingView(rootView: WorkspaceView(workspace: workspace).frame(width: 1240, height: 780))
            let warmupWindow = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1240, height: 780), styleMask: [.titled], backing: .buffered, defer: false)
            warmupWindow.contentView = warmupHost
            warmupWindow.orderBack(nil)
            warmupHost.layoutSubtreeIfNeeded()
            try await Task.sleep(for: .milliseconds(50))
            warmupHost.layoutSubtreeIfNeeded()
            let textView = try #require(editableTextView(in: session.container))
            textView.insertText(source, replacementRange: NSRange(location: 0, length: 0))
            await Task.yield()
            session.capture()
            warmupWindow.orderOut(nil)
            warmupWindow.contentView = nil
        }

        // 渲染前先向默认 CodeEditorView 注入完整 C++ 代码并执行一次真实编译判题。
        try await populateCodeEditor(
            sumSession,
            source: "#include <iostream>\n\nint main() {\n    long long a, b;\n    if (std::cin >> a >> b) {\n        std::cout << (a + b) << '\\n';\n    }\n    return 0;\n}\n"
        )

        workspace.runner.runAll(sumSession)
        await workspace.runner.waitForCurrentRun()

        try snapshot(WorkspaceView(workspace: workspace), size: NSSize(width: 1240, height: 780), appearanceName: .aqua, to: "01-cev-split-ac-wa-light.png")
        try snapshot(WorkspaceView(workspace: workspace), size: NSSize(width: 1240, height: 780), appearanceName: .darkAqua, to: "02-cev-split-ac-wa-dark.png")
        try snapshot(WorkspaceView(workspace: workspace), size: NSSize(width: 780, height: 520), appearanceName: .aqua, to: "04-cev-compact-780x520.png")

        workspace.layoutMode = .ioOnly
        try snapshot(WorkspaceView(workspace: workspace), size: NSSize(width: 1240, height: 780), appearanceName: .aqua, to: "05-cev-io-only-light.png")
        workspace.layoutMode = .split

        // 同时验证 STTextView 深色模式及 CE 编译错误横幅展开态。
        let brokenDoc = try #require(workspace.createDocument(text: "", editor: .stTextView))
        brokenDoc.stView?.insertText("int main() { broken_identifier; }\n", replacementRange: NSRange(location: 0, length: 0))
        brokenDoc.capture()
        brokenDoc.testCases = [TestCaseInput(id: "ce-case", input: "1\n", expected: "1\n")]
        workspace.runner.runAll(brokenDoc)
        await workspace.runner.waitForCurrentRun()
        try snapshot(WorkspaceView(workspace: workspace), size: NSSize(width: 1240, height: 780), appearanceName: .darkAqua, to: "03-st-split-ce-dark.png")

        workspace.selection = interactiveSession.document.id
        try snapshot(WorkspaceView(workspace: workspace), size: NSSize(width: 1240, height: 780), appearanceName: .aqua, to: "06-workspace-incompatible-empty.png")

        workspace.companion.selection = workspace.companion.items.last?.id
        try snapshot(CompanionInboxView(controller: workspace.companion), size: NSSize(width: 900, height: 620), appearanceName: .aqua, to: "07-companion-inbox-light.png")

        let toolchainDraft = ConfigurationJSONDraft(kind: .toolchain, jsonText: try workspace.runner.selectedProfile!.jsonText())
        try snapshot(ConfigurationSheet(draft: toolchainDraft) { _ in }, size: NSSize(width: 640, height: 480), appearanceName: .aqua, to: "08-toolchain-config-visual-light.png")

        let checkerDraft = ConfigurationJSONDraft(kind: .checker, jsonText: try workspace.runner.checkerDraft())
        try snapshot(ConfigurationSheet(draft: checkerDraft) { _ in }, size: NSSize(width: 640, height: 500), appearanceName: .aqua, to: "09-checker-config-visual-light.png")

        // 验证带标准错误输出和 Unchecked 样例的扁平诊断渲染。
        let stderrDoc = try #require(workspace.createDocument(text: "", editor: .codeEditorView))
        try await populateCodeEditor(
            stderrDoc,
            source: "#include <iostream>\n\nint main() {\n    long long x;\n    if (std::cin >> x) {\n        std::cerr << \"debug: x=\" << x << '\\n';\n        std::cout << (x * 2) << '\\n';\n    }\n    return 0;\n}\n"
        )
        stderrDoc.testCases = [
            TestCaseInput(id: "stderr-ac", input: "21\n", expected: "42\n"),
            TestCaseInput(id: "stderr-unchecked", input: "9\n", expected: nil),
        ]
        workspace.runner.runAll(stderrDoc)
        await workspace.runner.waitForCurrentRun()
        try snapshot(WorkspaceView(workspace: workspace), size: NSSize(width: 1240, height: 780), appearanceName: .aqua, to: "10-cev-unchecked-stderr-light.png")

        func windowSnapshot(
            _ rootView: WorkspaceView,
            size: NSSize,
            appearanceName: NSAppearance.Name,
            to filename: String
        ) throws {
            let controller = NSHostingController(rootView: rootView)
            let window = NSWindow(contentViewController: controller)
            window.setContentSize(size)
            window.styleMask = [.titled, .closable, .miniaturizable, .resizable]
            window.toolbarStyle = .unified
            window.appearance = NSAppearance(named: appearanceName)
            window.orderBack(nil)
            defer {
                window.orderOut(nil)
                window.contentViewController = nil
            }
            window.layoutIfNeeded()
            RunLoop.current.run(until: Date().addingTimeInterval(0.15))
            window.layoutIfNeeded()
            guard let frameView = window.contentView?.superview else {
                throw CocoaError(.coderInvalidValue)
            }
            frameView.layoutSubtreeIfNeeded()
            guard let rep = frameView.bitmapImageRepForCachingDisplay(in: frameView.bounds) else {
                throw CocoaError(.coderInvalidValue)
            }
            frameView.cacheDisplay(in: frameView.bounds, to: rep)
            guard let png = rep.representation(using: .png, properties: [:]) else {
                throw CocoaError(.coderInvalidValue)
            }
            try png.write(to: auditDir.appendingPathComponent(filename))
        }

        workspace.selection = sumSession.document.id
        workspace.runner.compile(sumSession)
        await workspace.runner.waitForCurrentRun()
        try snapshot(WorkspaceView(workspace: workspace), size: NSSize(width: 1240, height: 780), appearanceName: .aqua, to: "13-cev-compile-only-light.png")

        workspace.runner.runAll(sumSession)
        await workspace.runner.waitForCurrentRun()
        try windowSnapshot(WorkspaceView(workspace: workspace), size: NSSize(width: 1240, height: 780), appearanceName: .aqua, to: "11-window-toolbar-light.png")
        try windowSnapshot(WorkspaceView(workspace: workspace), size: NSSize(width: 1240, height: 780), appearanceName: .darkAqua, to: "12-window-toolbar-dark.png")

        await dispose(workspace)
    }

    @Test func compileErrorInspectorRendersVisibleLeftAndRightContent() async throws {
        let tempDir = try folder()
        defer { try? FileManager.default.removeItem(at: tempDir) }
        let recoveryFile = tempDir.appendingPathComponent("ce-render-recovery.json")
        let workspace = Workspace(arguments: ["--editor", "codeeditorview"], recoveryURL: recoveryFile)
        await workspace.bootstrap()

        let doc = try #require(workspace.createDocument(text: "", editor: .stTextView))
        doc.stView?.insertText("int main() { broken_identifier; }\n", replacementRange: NSRange(location: 0, length: 0))
        doc.capture()
        doc.testCases = [TestCaseInput(id: "ce-case", input: "1\n", expected: "1\n")]

        workspace.runner.runAll(doc)
        await workspace.runner.waitForCurrentRun()

        let result = try #require(workspace.runner.result(for: doc.document.id))
        #expect(result.buildOutcome != "Succeeded")
        #expect(workspace.layoutMode == .split)

        let size = NSSize(width: 1240, height: 780)
        let window = NSWindow(
            contentRect: NSRect(origin: .zero, size: size),
            styleMask: [.titled, .closable, .resizable],
            backing: .buffered,
            defer: false
        )
        window.appearance = NSAppearance(named: .darkAqua)
        let host = NSHostingView(rootView: WorkspaceView(workspace: workspace).frame(width: size.width, height: size.height))
        host.sizingOptions = []
        host.frame = NSRect(origin: .zero, size: size)
        window.contentView = host
        window.orderBack(nil)
        defer {
            window.orderOut(nil)
            window.contentView = nil
        }
        window.displayIfNeeded()
        host.layoutSubtreeIfNeeded()
        try await Task.sleep(for: .milliseconds(120))
        host.layoutSubtreeIfNeeded()

        let rep = try #require(host.bitmapImageRepForCachingDisplay(in: host.bounds))
        host.cacheDisplay(in: host.bounds, to: rep)

        func contrastingSampleCount(xRange: ClosedRange<Double>, yRange: ClosedRange<Double>, baselineX: Double, baselineY: Double) -> Int {
            let bx = min(max(Int(Double(rep.pixelsWide) * baselineX), 0), max(0, rep.pixelsWide - 1))
            let by = min(max(Int(Double(rep.pixelsHigh) * baselineY), 0), max(0, rep.pixelsHigh - 1))
            guard let base = rep.colorAt(x: bx, y: by)?.usingColorSpace(.sRGB) else { return 0 }
            let startX = Int(Double(rep.pixelsWide) * xRange.lowerBound)
            let endX = Int(Double(rep.pixelsWide) * xRange.upperBound)
            let startY = Int(Double(rep.pixelsHigh) * yRange.lowerBound)
            let endY = Int(Double(rep.pixelsHigh) * yRange.upperBound)
            let stepX = max(1, (endX - startX) / 80)
            let stepY = max(1, (endY - startY) / 80)
            var count = 0
            for y in stride(from: startY, to: endY, by: stepY) {
                for x in stride(from: startX, to: endX, by: stepX) {
                    guard let c = rep.colorAt(x: x, y: y)?.usingColorSpace(.sRGB) else { continue }
                    let diff = max(
                        abs(c.redComponent - base.redComponent),
                        max(abs(c.greenComponent - base.greenComponent), abs(c.blueComponent - base.blueComponent))
                    )
                    if diff > 0.04 {
                        count += 1
                    }
                }
            }
            return count
        }

        let leftContrastCount = contrastingSampleCount(xRange: 0.04...0.52, yRange: 0.06...0.95, baselineX: 0.50, baselineY: 0.90)
        let rightContrastCount = contrastingSampleCount(xRange: 0.64...0.96, yRange: 0.06...0.95, baselineX: 0.98, baselineY: 0.90)
        #expect(leftContrastCount >= 150)
        #expect(rightContrastCount >= 150)

        await dispose(workspace)
    }
}
