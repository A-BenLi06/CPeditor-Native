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

    @Test func titlesAndSubtitlesReflectCompanionMetadataNumberedUntitledAndSavedPaths() async throws {
        _ = NSApplication.shared
        let directory = try folder()
        defer { try? FileManager.default.removeItem(at: directory) }
        let recoveryFile = directory.appendingPathComponent("ux-recovery.json")
        let workspace = Workspace(arguments: ["--editor", "sttextview"], recoveryURL: recoveryFile)
        await workspace.bootstrap()

        let first = try #require(workspace.activeSession)
        #expect(workspace.title(for: first) == "Untitled")

        let second = try #require(workspace.createDocument(editor: .sttextviewFallback))
        #expect(workspace.title(for: first) == "Untitled")
        #expect(workspace.title(for: second) == "Untitled 2")

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

    @Test func togglingRunPanelAndRunningPreservesMountedEditorAndUndoHistory() async throws {
        _ = NSApplication.shared
        let workspace = Workspace(arguments: ["--editor", "codeeditorview"])
        let session = try #require(workspace.activeSession)
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 1080, height: 720),
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

        let initialView = try #require(editableTextView(in: session.container))
        window.makeFirstResponder(initialView)
        let source = "#include <iostream>\nint main(){int a,b;if(std::cin>>a>>b)std::cout<<a+b<<'\\n';}\n"
        session.container.history.groupsByEvent = false
        session.container.history.beginUndoGrouping()
        initialView.insertText(source, replacementRange: NSRange(location: 0, length: 0))
        session.container.history.endUndoGrouping()
        await Task.yield()
        session.capture()
        #expect(session.document.text == source)
        #expect(session.container.history.canUndo)

        // 折叠底部测试面板不得卸载原生编辑器或清空撤销栈。
        workspace.isRunPanelVisible = false
        await Task.yield()
        host.layoutSubtreeIfNeeded()
        #expect(editableTextView(in: session.container) === initialView)
        #expect(session.container.history.canUndo)

        session.testCases = [TestCaseInput(id: "ux-sample", input: "10 20\n", expected: "30\n")]
        workspace.runActiveDocument()
        #expect(workspace.isRunPanelVisible)
        await workspace.runner.waitForCurrentRun()
        host.layoutSubtreeIfNeeded()

        let result = try #require(workspace.runner.result(for: session.document.id))
        #expect(VerdictDescriptor.summary(for: result).label == "AC")
        #expect(editableTextView(in: session.container) === initialView)

        session.container.history.undo()
        await Task.yield()
        session.capture()
        #expect(session.document.text.isEmpty)
        await workspace.runner.shutdown()
    }

    @Test func closingBackgroundDocumentKeepsActiveSelectionIntact() async throws {
        _ = NSApplication.shared
        let workspace = Workspace(arguments: ["--editor", "sttextview"])
        let first = try #require(workspace.activeSession)
        let middle = try #require(workspace.createDocument(text: "// clean", editor: .stTextView))
        // 空文本初始 saved=true；这里显式使用干净的新文档测试后台关闭。
        let cleanMiddle = try #require(workspace.createDocument(editor: .stTextView))
        let active = try #require(workspace.createDocument(editor: .stTextView))
        _ = middle
        workspace.selection = active.document.id

        await workspace.closeDocument(cleanMiddle)
        #expect(!workspace.sessions.contains { $0 === cleanMiddle })
        #expect(workspace.selection == active.document.id)
        #expect(workspace.sessions.contains { $0 === first })

        await workspace.runner.shutdown()
        workspace.sessions.forEach { $0.dispose() }
    }

    @Test func verdictSummaryPrioritizesBuildFailuresAndSevereVerdicts() throws {
        func makeResult(buildOutcome: String = "Succeeded", verdicts: [String]) throws -> BatchResult {
            let testsJSON = verdicts.enumerated().map { index, verdict in
                """
                {"testcase_id":"t-\(index)","verdict":"\(verdict)","detail":"","process":{"termination":"Exited","elapsed_ms":12,"stdout":{"encoding":"base64","data":""},"stderr":{"encoding":"base64","data":""},"detail":"","exit_code":0,"stdout_truncated":false,"stderr_truncated":false,"native_error":0,"stdout_total_bytes":0,"stderr_total_bytes":0}}
                """
            }.joined(separator: ",")
            let payload = """
            {"schema_version":1,"document_id":"doc","revision":1,"profile_id":"macos-clang-cpp17","source_digest":"00","build_outcome":"\(buildOutcome)","error":"","tests":[\(testsJSON)]}
            """
            return try JSONDecoder().decode(BatchResult.self, from: Data(payload.utf8))
        }

        #expect(VerdictDescriptor.summary(for: try makeResult(buildOutcome: "CompilationFailed", verdicts: [])).label == "CE")
        #expect(VerdictDescriptor.summary(for: try makeResult(buildOutcome: "Cancelled", verdicts: [])).label == "Cancelled")
        #expect(VerdictDescriptor.summary(for: try makeResult(verdicts: ["AC", "AC"])).label == "AC")
        #expect(VerdictDescriptor.summary(for: try makeResult(verdicts: ["AC", "WA"])).label == "WA")
        #expect(VerdictDescriptor.summary(for: try makeResult(verdicts: ["WA", "TLE"])).label == "TLE")
        #expect(VerdictDescriptor.summary(for: try makeResult(verdicts: ["TLE", "RE"])).label == "RE")
        #expect(VerdictDescriptor.summary(for: try makeResult(verdicts: ["RE", "CheckerError"])).label == "CheckerError")
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
            RunLoop.current.run(until: Date().addingTimeInterval(0.05))
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
        let workspace = Workspace(arguments: ["--editor", "sttextview"], recoveryURL: recoveryFile)
        await workspace.bootstrap()

        let sumJSON = """
        {"name":"A. Two Pointers","group":"Codeforces Round 999","url":"https://codeforces.com/contest/999/problem/A","memoryLimit":256,"timeLimit":1500,"interactive":false,"tests":[{"input":"7 8\\n","output":"15\\n"},{"input":"100 250\\n","output":"350\\n"}]}
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
        sumSession.stView?.insertText(
            "#include <iostream>\n\nint main() {\n    long long a, b;\n    if (std::cin >> a >> b) {\n        std::cout << (a + b) << '\\n';\n    }\n    return 0;\n}\n",
            replacementRange: NSRange(location: 0, length: 0)
        )
        sumSession.capture()
        workspace.runner.runAll(sumSession)
        await workspace.runner.waitForCurrentRun()

        try snapshot(WorkspaceView(workspace: workspace), size: NSSize(width: 1180, height: 760), appearanceName: .aqua, to: "02-workspace-ac-light.png")
        try snapshot(WorkspaceView(workspace: workspace), size: NSSize(width: 1180, height: 760), appearanceName: .darkAqua, to: "03-workspace-ac-dark.png")

        workspace.selection = interactiveSession.document.id
        try snapshot(WorkspaceView(workspace: workspace), size: NSSize(width: 1180, height: 760), appearanceName: .aqua, to: "04-workspace-incompatible-empty.png")

        workspace.companion.selection = workspace.companion.items.last?.id
        try snapshot(CompanionInboxView(controller: workspace.companion), size: NSSize(width: 900, height: 620), appearanceName: .aqua, to: "05-companion-inbox-light.png")

        let draft = ConfigurationJSONDraft(kind: .checker, jsonText: try workspace.runner.checkerDraft())
        try snapshot(ConfigurationJSONSheet(draft: draft) { _ in }, size: NSSize(width: 700, height: 540), appearanceName: .aqua, to: "06-configuration-sheet-light.png")

        await dispose(workspace)
    }
}

private extension EditorKind {
    static var sttextviewFallback: Self { .stTextView }
}
