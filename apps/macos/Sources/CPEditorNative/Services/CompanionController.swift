import AppKit
import Observation
import NativeCoreBridge
import NativeEditorModel
import UniformTypeIdentifiers

struct CompanionConfiguration: Decodable, Sendable {
    let schemaVersion: Int; let storageNamespace: String
    let inboxFilenameFormat: String; let backupFilenameFormat: String; let lockFilenameFormat: String
    let maxImportBytes: Int; let pageItems: UInt64; let pageBytes: UInt64; let previewBytes: Int
    let lockTimeoutMS: UInt64; let sourceTemplate: String; let statusLocalizationKeys: [String: String]
    static func load() throws -> Self {
        guard let url = Bundle.module.url(forResource: "companion", withExtension: "json") else { throw CocoaError(.fileNoSuchFile) }
        let value = try JSONDecoder().decode(Self.self, from: Data(contentsOf: url))
        guard value.schemaVersion == 1, (1...16777216).contains(value.maxImportBytes), (1...256).contains(value.pageItems),
              (1...134217728).contains(value.pageBytes), (1...4096).contains(value.previewBytes), value.lockTimeoutMS <= 60000,
              !value.storageNamespace.isEmpty else { throw WireError.unsupportedSchema }
        return value
    }
    func paths(for file: URL) -> CompanionPaths {
        let parent = file.deletingLastPathComponent(); let stem = file.deletingPathExtension().lastPathComponent
        return CompanionPaths(inboxFile: parent.appendingPathComponent(String(format: inboxFilenameFormat, stem)).path,
                              backupFile: parent.appendingPathComponent(String(format: backupFilenameFormat, stem)).path,
                              lockFile: parent.appendingPathComponent(String(format: lockFilenameFormat, stem)).path)
    }
}

/// 来源预览仅显示经 Core 验证的元数据；原始 JSON 保留在收件箱，不将 URL／文件名变成动作。
struct CompanionPreview: Decodable {
    struct Sample: Decodable { let input: String; let output: String }
    struct Input: Decodable { let type: String; let fileName: String?; let pattern: String? }
    struct Output: Decodable { let type: String; let fileName: String? }
    let name: String; let group: String; let url: String; let memoryLimit: UInt64; let timeLimit: UInt64
    let tests: [Sample]; let interactive: Bool?; let testType: String?; let input: Input?; let output: Output?

    var requiresSpecialIOMode: Bool {
        if interactive == true { return true }
        if let testType, testType != "single" && testType != "multiNumber" { return true }
        if let input, input.type != "stdin" { return true }
        if let output, output.type != "stdout" { return true }
        return false
    }
}

@MainActor @Observable final class CompanionController {
    var isPresented = false
    var selection: RawIdentifier?
    private(set) var items: [CompanionItem] = []
    private(set) var hasMore = false
    private(set) var isOperating = false
    private(set) var notice: String?
    private(set) var paths: CompanionPaths?
    private(set) var configuration: CompanionConfiguration?
    private var previewsByImportID: [RawIdentifier: CompanionPreview] = [:]
    private var previewsByDocumentID: [RawIdentifier: CompanionPreview] = [:]
    @ObservationIgnored private weak var workspace: Workspace?
    @ObservationIgnored private let coordinator = CoreCoordinator(maximumRequestBytes: SessionLimits.maximumRequestBytes)
    @ObservationIgnored private var inboxID: RawIdentifier?
    @ObservationIgnored private var inboxRevision: UInt64?
    @ObservationIgnored private var diskDigest: String?
    @ObservationIgnored private var after: UInt64 = 0
    @ObservationIgnored private var isClosed = false

    init() { do { configuration = try .load() } catch { notice = L10n.describe(error) } }
    var selectedItem: CompanionItem? { items.first { $0.id == selection } }
    var pendingCount: Int { items.reduce(0) { $0 + ($1.state == .pending ? 1 : 0) } }
    var appliedCount: Int { items.reduce(0) { $0 + ($1.state == .applied ? 1 : 0) } }

    func preview(for item: CompanionItem) -> CompanionPreview? {
        previewsByImportID[item.importID]
    }

    func preview(forDocumentID documentID: RawIdentifier) -> CompanionPreview? {
        previewsByDocumentID[documentID]
    }

    func bootstrap(_ workspace: Workspace) async {
        self.workspace = workspace
        guard !isOperating, let recovery = workspace.recovery, let configuration else { return }
        isOperating = true; defer { isOperating = false }
        do {
            let existing = try workspace.companionStoragePreference(namespace: configuration.storageNamespace)
            let binding = existing ?? CompanionStorageBinding(workspaceID: workspace.workspaceID,
                paths: configuration.paths(for: URL(fileURLWithPath: recovery.paths.recoveryFile)))
            guard binding.workspaceID == workspace.workspaceID else { throw WireError.unsupportedSchema }
            let reserved = [recovery.paths.recoveryFile, recovery.paths.backupFile, recovery.paths.lockFile] + workspace.sessions.compactMap { $0.document.url?.path }
            try await Self.validatePaths(binding.paths, against: reserved)
            paths = binding.paths
            try await readPage(reset: true)
        } catch { notice = L10n.string("companion_unavailable") + " " + L10n.describe(error) }
    }

    private var context: CompanionContext? {
        guard let workspace, let paths else { return nil }
        return CompanionContext(workspaceID: workspace.workspaceID, paths: paths)
    }

    func refresh() async {
        guard !isOperating, !isClosed else { return }
        isOperating = true; defer { isOperating = false }
        do { try await readPage(reset: true) } catch { notice = L10n.describe(error) }
    }
    func loadMore() async {
        guard !isOperating, !isClosed, hasMore else { return }
        isOperating = true; defer { isOperating = false }
        do { try await readPage(reset: false) } catch { notice = L10n.describe(error) }
    }
    private func readPage(reset: Bool) async throws {
        guard let context, let configuration else { throw WireError.busy }
        let cursor: UInt64 = reset ? 0 : after
        let page = try await coordinator.companionPage(context, after: cursor, maxItems: configuration.pageItems, maxPageBytes: configuration.pageBytes)
        guard page.status == .loaded || page.status == .missing else {
            notice = L10n.string("companion_preserved") + " " + page.detail; return
        }
        if !reset, (page.inboxID != inboxID || page.revision != inboxRevision || page.diskDigest != diskDigest) {
            try await readPage(reset: true); notice = L10n.string("companion_refreshed"); return
        }
        guard !page.hasMore || (!page.items.isEmpty && page.nextAfterSequence > cursor),
              page.items.enumerated().allSatisfy({ index, item in item.sequence > (index == 0 ? cursor : page.items[index - 1].sequence) }) else { throw WireError.malformedTerminal }
        let mergedItems = reset ? page.items : items + page.items
        var importPreviews = reset ? [RawIdentifier: CompanionPreview]() : previewsByImportID
        var documentPreviews = reset ? [RawIdentifier: CompanionPreview]() : previewsByDocumentID
        let decoder = JSONDecoder()
        for item in page.items {
            if let existing = previewsByImportID[item.importID] {
                importPreviews[item.importID] = existing
                documentPreviews[item.documentID] = existing
            } else if let decoded = try? decoder.decode(CompanionPreview.self, from: Data(item.originalJSON.utf8)) {
                importPreviews[item.importID] = decoded
                documentPreviews[item.documentID] = decoded
            }
        }
        previewsByImportID = importPreviews
        previewsByDocumentID = documentPreviews
        items = mergedItems
        inboxID = page.inboxID; inboxRevision = page.revision; diskDigest = page.diskDigest
        after = page.nextAfterSequence; hasMore = page.hasMore
        if selection == nil || !items.contains(where: { $0.id == selection }) { selection = items.first?.id }
    }

    func importPanel() async {
        guard !isOperating, !isClosed else { return }
        let panel = NSOpenPanel(); panel.allowsMultipleSelection = false; panel.canChooseDirectories = false
        panel.allowedContentTypes = [.json]
        guard panel.runModal() == .OK, let file = panel.url else { return }
        await accept(file: file)
    }
    func accept(file: URL) async {
        guard !isOperating, !isClosed, let context, let configuration, let workspace, !workspace.isBusy, let recovery = workspace.recovery else { return }
        guard let operation = try? workspace.persistence.beginOperation() else { notice = L10n.string("workspace_revision_frozen"); return }
        defer { workspace.persistence.endOperation(operation) }
        isOperating = true; defer { isOperating = false }
        do {
            try ensureStorageBinding(workspace, recovery: recovery)
            guard await recovery.flush() else { notice = recovery.notice ?? L10n.string("companion_revision_budget"); return }
            let original = try await Task.detached {
                try CoordinatedSourceFile.read(file, maximumBytes: configuration.maxImportBytes).text
            }.value
            let response = try await coordinator.companionAccept(context, originalJSON: original, lockTimeoutMS: configuration.lockTimeoutMS)
            notice = describe(response)
            // 包含有副作用的未知持久性结果也重新读取；不盲重发原载荷。
            try await readPage(reset: true)
        } catch { notice = (error as? DocumentError) == .revisionExhausted ? L10n.string("companion_revision_budget") : L10n.describe(error) }
    }

    func apply(importID: RawIdentifier) async {
        guard !isOperating, !isClosed, let context, let configuration, let workspace, !workspace.isBusy, let recovery = workspace.recovery else { return }
        let operation: UInt64?
        if workspace.persistence.isFrozen { operation = nil }
        else {
            guard let token = try? workspace.persistence.beginOperation() else { notice = L10n.string("workspace_revision_frozen"); return }
            operation = token
        }
        defer { if let operation { workspace.persistence.endOperation(operation) } }
        isOperating = true; defer { isOperating = false }
        do {
            try ensureStorageBinding(workspace, recovery: recovery)
            guard workspace.captureForRecoveryReplacement(), await recovery.flush() else {
                notice = workspace.errorMessage ?? recovery.notice ?? L10n.string("companion_revision_budget"); return
            }
            let base = workspace.persistence.isFrozen
                ? try workspace.sessionRecord(revision: 0, configuration: recovery.configuration)
                : try recovery.snapshotForCompanion()
            let defaults = try workspace.companionDefaults(source: configuration.sourceTemplate, configuration: recovery.configuration)
            let response = try await coordinator.companionPrepare(context, importID: importID, session: base, defaults: defaults)
            guard response.inboxStatus == .loaded, let receipt = response.receipt, receipt.importID == importID,
                  receipt.inboxID == inboxID else { notice = L10n.string("companion_preserved") + " " + response.detail; return }
            switch response.status {
            case "Prepared":
                guard operation != nil, !workspace.persistence.isFrozen, let candidate = response.session else { throw DocumentError.revisionExhausted }
                _ = try recovery.snapshotForCompanion()
                try workspace.appendCompanionCandidate(candidate, base: base, receipt: receipt, configuration: recovery.configuration)
            case "AlreadyApplied":
                // 只读 MAX 也可确认实际保存的回执；不重建已删除／已编辑的文档。
                guard try CompanionProvenance.receipts(base.editorPreferences).contains(receipt) else { throw WireError.malformedTerminal }
            default: notice = L10n.string("companion_preserved") + " " + response.detail; return
            }
            // 同步 delta 已完成；先结束持久操作，再允许最后一次恢复提交独占捕获。
            // isOperating 继续保护整个 UI 链路直到实际回执确认。
            if let operation { workspace.persistence.endOperation(operation) }
            guard await recovery.flush() else { notice = L10n.string("companion_pending_save") + " " + (recovery.notice ?? ""); return }
            let marked = try await coordinator.companionMarkApplied(context, importID: importID, sessionPaths: recovery.paths, lockTimeoutMS: configuration.lockTimeoutMS)
            notice = describe(marked)
            try await readPage(reset: true)
        } catch { notice = L10n.string("companion_preserved") + " " + L10n.describe(error) }
    }

    private func ensureStorageBinding(_ workspace: Workspace, recovery: RecoveryController) throws {
        guard let paths, let configuration else { throw WireError.unsupportedSchema }
        if let binding = try workspace.companionStoragePreference(namespace: configuration.storageNamespace) {
            guard binding.workspaceID == workspace.workspaceID, binding.paths == paths else { throw WireError.unsupportedSchema }
        } else {
            let current = try recovery.snapshotForCompanion()
            guard current.revision < UInt64.max - 1 else { throw DocumentError.revisionExhausted }
            guard try CompanionProvenance.receipts(current.editorPreferences).isEmpty else { throw WireError.unsupportedSchema }
            try workspace.installCompanionStorage(CompanionStorageBinding(workspaceID: workspace.workspaceID, paths: paths),
                namespace: configuration.storageNamespace, configuration: recovery.configuration)
        }
    }

    private func describe(_ response: CompanionWriteResponse) -> String {
        let key = configuration?.statusLocalizationKeys[response.status] ?? "companion_preserved"
        return L10n.string(key) + (response.detail.isEmpty ? "" : " " + response.detail)
    }

    nonisolated private static func validatePaths(_ paths: CompanionPaths, against reserved: [String]) async throws {
        try await Task.detached {
            let all = paths.all
            for (index, path) in all.enumerated() {
                guard path.hasPrefix("/"), !path.utf8.contains(0) else { throw WireError.unsupportedSchema }
                for other in Array(all.dropFirst(index + 1)) + reserved {
                    if try CoordinatedSourceFile.aliases(URL(fileURLWithPath: path), URL(fileURLWithPath: other)) { throw DocumentError.reservedRecoveryPath }
                }
            }
        }.value
    }

    func shutdown() async { isClosed = true; await coordinator.shutdown() }
}
