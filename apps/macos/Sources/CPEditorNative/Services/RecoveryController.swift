import AppKit
import Observation
import NativeCoreBridge
import NativeEditorModel

struct RecoveryConfiguration: Decodable, Sendable {
    let schemaVersion: Int
    let directoryName: String
    let recoveryFilename: String
    let backupFilenameFormat: String
    let lockFilenameFormat: String
    var lockTimeoutMS: UInt64
    let workspaceNamespace: String
    let documentNamespace: String
    let checkerNamespace: String
    let maxPreferencesPerOwner: Int
    let locationPreferenceKey: String
    static func load() throws -> Self {
        guard let url = Bundle.module.url(forResource: "recovery", withExtension: "json") else { throw CocoaError(.fileNoSuchFile) }
        let value = try JSONDecoder().decode(Self.self, from: Data(contentsOf: url))
        guard value.schemaVersion == 1, value.lockTimeoutMS <= 60000,
              (1...16).contains(value.maxPreferencesPerOwner) else { throw WireError.unsupportedSchema }
        return value
    }
    func paths(for file: URL) -> SessionPaths {
        let parent = file.deletingLastPathComponent()
        let stem = file.deletingPathExtension().lastPathComponent
        return SessionPaths(recoveryFile: file.path,
                            backupFile: parent.appendingPathComponent(String(format: backupFilenameFormat, stem)).path,
                            lockFile: parent.appendingPathComponent(String(format: lockFilenameFormat, stem)).path)
    }
}

struct WorkspaceNativePreferences: Codable {
    let profiles: [ToolchainProfile]
    let selectedProfileID: RawIdentifier
    let comparison: ComparisonPolicy
    let newEditor: EditorKind
    let wrapText: Bool
    let showMinimap: Bool
}
struct DocumentNativePreferences: Codable {
    let editor: EditorKind
    let selectionLocation: Int
    let selectionLength: Int
}

struct CheckerNativePreferences: Codable {
    let enabled: Bool
    let configurationJSON: String?
    enum CodingKeys: String, CodingKey { case enabled, configurationJSON = "configuration_json" }
}

private struct RecoveryLocationPreference: Codable {
    let schemaVersion: Int
    let path: String
}

@MainActor @Observable final class RecoveryController {
    private(set) var isLoading = false
    private(set) var isSaving = false
    private(set) var isBlocked = false
    private(set) var isCapacityBlocked = false
    private(set) var isChangingLocation = false
    private(set) var isRestoring = false
    private(set) var notice: String?
    private(set) var paths: SessionPaths
    let configuration: RecoveryConfiguration
    @ObservationIgnored private weak var workspace: Workspace?
    @ObservationIgnored private var coordinator = CoreCoordinator(maximumRequestBytes: SessionLimits.maximumRequestBytes)
    @ObservationIgnored private var exportCoordinator: CoreCoordinator?
    @ObservationIgnored private var expectedDiskDigest: String?
    @ObservationIgnored private var revision: UInt64 = 0
    @ObservationIgnored private var lastContent: WorkspaceSessionRecord?
    @ObservationIgnored private var rejectedContent: WorkspaceSessionRecord?
    @ObservationIgnored private var forceSave = false
    @ObservationIgnored private var cancellationRequested = false
    @ObservationIgnored private var pending = false
    @ObservationIgnored private var saveTask: Task<Void, Never>?
    @ObservationIgnored private var copyTask: Task<Bool, Never>?
    @ObservationIgnored private var restoreTask: Task<Void, Never>?
    @ObservationIgnored private var observationTask: Task<Void, Never>?
    @ObservationIgnored private var stopping = false
    @ObservationIgnored private let persistsLocation: Bool

    init(file: URL?, configuration: RecoveryConfiguration) {
        self.configuration = configuration
        persistsLocation = file == nil
        let directory = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent(configuration.directoryName, isDirectory: true)
        let preference = UserDefaults.standard.data(forKey: configuration.locationPreferenceKey)
            .flatMap { try? JSONDecoder().decode(RecoveryLocationPreference.self, from: $0) }
        let stored = preference.flatMap { $0.schemaVersion == 1 && !$0.path.isEmpty && !$0.path.contains("\0") ? URL(fileURLWithPath: $0.path) : nil }
        paths = configuration.paths(for: file ?? stored ?? directory.appendingPathComponent(configuration.recoveryFilename))
    }

    func bootstrap(_ workspace: Workspace) async {
        self.workspace = workspace
        isLoading = true
        do {
            let folder = URL(fileURLWithPath: paths.recoveryFile).deletingLastPathComponent()
            try await Task.detached { try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true) }.value
            let loaded = try await coordinator.loadSession(at: paths.recoveryFile)
            expectedDiskDigest = loaded.diskDigest
            switch loaded.status {
            case .loaded:
                guard let session = loaded.session else { throw WireError.malformedTerminal }
                try workspace.applyRecoveryCandidate(session, configuration: configuration)
                revision = session.revision
                lastContent = try content(of: workspace)
            case .missing: break
            default:
                isBlocked = true
                notice = L10n.string("recovery_preserved") + " " + loaded.detail
            }
        } catch { isBlocked = true; notice = L10n.describe(error) + " " + L10n.string("recovery_preserved") }
        isLoading = false
        await workspace.finishBootstrap()
        if lastContent == nil { lastContent = try? content(of: workspace) }
        observeNextChange()
        if !isBlocked { requestSave() }
    }

    private func content(of workspace: Workspace) throws -> WorkspaceSessionRecord {
        try workspace.sessionRecord(revision: 0, configuration: configuration)
    }

    private func needsSave(_ contents: WorkspaceSessionRecord) -> Bool {
        if let rejectedContent, contents.hasSameContent(as: rejectedContent) { return false }
        return forceSave || expectedDiskDigest == nil || !contents.hasSameContent(as: lastContent)
    }

    private func saved(_ contents: WorkspaceSessionRecord) {
        lastContent = contents
        rejectedContent = nil
        if isCapacityBlocked { notice = nil }
        isCapacityBlocked = false
        isBlocked = false
    }

    private func blockForCapacity(_ contents: WorkspaceSessionRecord) {
        isCapacityBlocked = true
        isBlocked = true
        forceSave = true
        rejectedContent = contents
        notice = L10n.string("recovery_capacity_exceeded")
    }

    func hasUnsavedTests(in session: NativeEditorSession) -> Bool {
        if let recorded = lastContent?.documents.first(where: { $0.documentID == session.document.id }) {
            return session.testCases != recorded.tests
        }
        // A new blank test is a UI placeholder. It does not represent user input.
        guard !session.testCases.isEmpty else { return false }
        guard session.testCases.count == 1, let test = session.testCases.first else { return true }
        return !test.input.data.isEmpty || test.expected?.data.isEmpty != true || !test.enabled
    }

    private func observeNextChange() {
        guard !stopping, let workspace else { return }
        withObservationTracking {
            _ = try? content(of: workspace)
        } onChange: { [weak self] in
            Task { @MainActor in
                guard let self, !self.stopping else { return }
                self.observationTask?.cancel()
                self.observationTask = Task { @MainActor in
                    guard !Task.isCancelled else { return }
                    self.observeNextChange()
                    self.requestSave()
                }
            }
        }
    }

    func requestSave() {
        guard !isLoading, !isChangingLocation, !stopping, workspace != nil, !isBlocked || isCapacityBlocked else { return }
        pending = true
        guard saveTask == nil else { return }
        saveTask = Task { @MainActor [weak self] in
            guard let self else { return }
            while self.pending, !self.isBlocked || self.isCapacityBlocked {
                self.pending = false
                guard let workspace = self.workspace else { break }
                var finalLease = false
                var recoveryReplaced = false
                var attemptedContent: WorkspaceSessionRecord?
                do {
                    if self.revision == UInt64.max {
                        _ = await self.confirmFrozenSession()
                        continue
                    }
                    var contents = try self.content(of: workspace)
                    guard self.needsSave(contents) else { continue }
                    if self.revision == UInt64.max - 1 {
                        guard workspace.persistence.operationCount == 0 else { self.pending = true; break }
                        // 捕获、检查组合输入和冻结属于同一个 MainActor 段，中间没有 await。
                        guard workspace.captureForRecoveryReplacement() else {
                            self.notice = workspace.errorMessage; self.pending = true; break
                        }
                        contents = try self.content(of: workspace)
                        guard self.needsSave(contents) else { continue }
                        guard workspace.persistence.tryBeginFinal() else { self.pending = true; break }
                        finalLease = true
                    }
                    let record = try workspace.sessionRecord(revision: self.revision + 1, configuration: self.configuration)
                    attemptedContent = contents
                    if finalLease { try workspace.captureFrozenSession(record, configuration: self.configuration) }
                    self.cancellationRequested = false
                    self.isSaving = true
                    try await Self.validateSourcePaths(record, paths: self.paths, additionalSourcePaths: workspace.retainedCloseSession?.document.url.map { [$0.path] } ?? [])
                    let response = try await self.coordinator.saveSession(record, paths: self.paths,
                        expectedDiskDigest: self.expectedDiskDigest, lockTimeoutMS: self.configuration.lockTimeoutMS)
                    recoveryReplaced = response.recoveryReplaced
                    if finalLease {
                        if !recoveryReplaced { workspace.releaseFailedFinalSession() }
                        workspace.persistence.finishFinal(replaced: recoveryReplaced)
                        finalLease = false
                    }
                    if recoveryReplaced {
                        self.expectedDiskDigest = response.diskDigest
                        self.revision = record.revision
                        self.saved(self.revision == UInt64.max ? try self.content(of: workspace) : contents)
                    }
                    if response.status != .saved || !response.recoveryReplaced {
                        self.isCapacityBlocked = false
                        self.isBlocked = true
                        self.forceSave = true
                        self.notice = L10n.sessionSaveStatus(response.status) + " " + response.detail
                    } else {
                        self.forceSave = false
                        if self.revision == UInt64.max { self.notice = L10n.string("workspace_revision_frozen") }
                    }
                } catch {
                    if finalLease {
                        if !recoveryReplaced { workspace.releaseFailedFinalSession() }
                        workspace.persistence.finishFinal(replaced: recoveryReplaced)
                    }
                    if error is SessionCapacityError, let attemptedContent {
                        self.blockForCapacity(attemptedContent)
                    } else {
                        self.isCapacityBlocked = false
                        self.isBlocked = true
                        self.notice = self.cancellationRequested ? L10n.sessionSaveStatus(.cancelled) : L10n.describe(error)
                    }
                }
                self.isSaving = false
            }
            self.saveTask = nil
        }
    }

    private func confirmFrozenSession() async -> Bool {
        guard revision == UInt64.max, let workspace else { return false }
        do {
            guard workspace.captureForRecoveryReplacement() else { notice = workspace.errorMessage; isBlocked = true; return false }
            let record = try workspace.sessionRecord(revision: revision, configuration: configuration)
            let loaded = try await coordinator.loadSession(at: paths.recoveryFile)
            guard loaded.status == .loaded, let actual = loaded.session, let digest = loaded.diskDigest else { throw DocumentError.diskConflict }
            let encoder = JSONEncoder(); encoder.outputFormatting = [.sortedKeys]
            guard try encoder.encode(actual) == encoder.encode(record), expectedDiskDigest == nil || digest == expectedDiskDigest else { throw DocumentError.diskConflict }
            expectedDiskDigest = digest
            saved(try content(of: workspace))
            forceSave = false; pending = false
            notice = L10n.string("workspace_revision_frozen")
            return true
        } catch { isBlocked = true; notice = L10n.describe(error); return false }
    }

    func flush() async -> Bool {
        guard !isLoading, !stopping else { return false }
        if let copyTask { _ = await copyTask.value }
        if let restoreTask { await restoreTask.value }
        requestSave()
        if let saveTask { await saveTask.value }
        if revision == UInt64.max { return await confirmFrozenSession() }
        // 同一 actor 上检查快照，覆盖保存期间发生但观察回调尚未排队的变更。
        if (!isBlocked || isCapacityBlocked), let workspace, let contents = try? content(of: workspace), needsSave(contents) {
            requestSave()
            if let saveTask { await saveTask.value }
        }
        return !isBlocked && !pending
    }

    nonisolated private static func validateSourcePaths(_ record: WorkspaceSessionRecord, paths: SessionPaths, additionalSourcePaths: [String] = []) async throws {
        try await Task.detached {
            for source in record.documents.compactMap(\.originalPath) + additionalSourcePaths {
                for reserved in [paths.recoveryFile, paths.backupFile, paths.lockFile] {
                    if try CoordinatedSourceFile.aliases(URL(fileURLWithPath: source), URL(fileURLWithPath: reserved)) {
                        throw DocumentError.reservedRecoveryPath
                    }
                }
            }
        }.value
    }

    func snapshotForCompanion() throws -> WorkspaceSessionRecord {
        guard !isLoading, !isBlocked, !isChangingLocation, !isRestoring, !stopping, let workspace else { throw WireError.busy }
        guard revision < UInt64.max else { throw DocumentError.revisionExhausted }
        return try workspace.sessionRecord(revision: revision, configuration: configuration)
    }

    func cancelSave() {
        guard isSaving else { return }
        cancellationRequested = true
        (exportCoordinator ?? coordinator).requestCancellation()
    }

    func retry() async {
        guard !isChangingLocation, !isRestoring else { return }
        if let saveTask { await saveTask.value }
        await coordinator.shutdown()
        coordinator = CoreCoordinator(maximumRequestBytes: SessionLimits.maximumRequestBytes)
        rejectedContent = nil
        isCapacityBlocked = false
        isBlocked = false; notice = nil
        requestSave()
    }

    func saveElsewhere() async -> Bool {
        guard workspace != nil, !isChangingLocation, !isRestoring else { return false }
        guard let workspace, let operation = try? workspace.persistence.beginOperation(readOnlyExport: true) else { return false }
        defer { workspace.persistence.endOperation(operation) }
        isChangingLocation = true
        let panel = NSSavePanel()
        panel.nameFieldStringValue = configuration.recoveryFilename
        let result = panel.runModal()
        isChangingLocation = false
        guard result == .OK, let file = panel.url else { return false }
        workspace.persistence.endOperation(operation)
        return await saveCopy(to: file)
    }

    @discardableResult func saveCopy(to file: URL) async -> Bool {
        guard copyTask == nil, !isChangingLocation, !isRestoring, !isLoading, workspace != nil, workspace?.companion.isOperating != true else { return false }
        guard let workspace, let operation = try? workspace.persistence.beginOperation(readOnlyExport: true) else { return false }
        defer { workspace.persistence.endOperation(operation) }
        isChangingLocation = true
        let task = Task { @MainActor in await self.performSaveCopy(to: file, operation: operation) }
        copyTask = task
        let saved = await task.value
        copyTask = nil
        return saved
    }

    private func performSaveCopy(to file: URL, operation: UInt64) async -> Bool {
        guard let workspace else { isChangingLocation = false; return false }
        defer { isChangingLocation = false; isSaving = false; exportCoordinator = nil; requestSave() }
        if let saveTask { await saveTask.value }
        let newPaths = configuration.paths(for: file)
        do {
            let oldPaths = paths
            let inboxPaths = workspace.companion.paths?.all ?? []
            let sourcePaths = workspace.editorDeckSessions.compactMap { $0.document.url?.path }
            let distinct = try await Task.detached {
                for old in [oldPaths.recoveryFile, oldPaths.backupFile, oldPaths.lockFile] + inboxPaths + sourcePaths {
                    for new in [newPaths.recoveryFile, newPaths.backupFile, newPaths.lockFile] {
                        if try CoordinatedSourceFile.aliases(URL(fileURLWithPath: new), URL(fileURLWithPath: old)) { return false }
                    }
                }
                return true
            }.value
            guard distinct else { notice = L10n.string("choose_distinct_recovery"); return false }
            var contents = try content(of: workspace)
            var changed = lastContent.map { !contents.hasSameContent(as: $0) } ?? true
            var finalLease = false
            var replaced = false
            do {
                if changed, revision == UInt64.max - 1 {
                    guard workspace.captureForRecoveryReplacement() else { notice = workspace.errorMessage; return false }
                    contents = try content(of: workspace)
                    changed = lastContent.map { !contents.hasSameContent(as: $0) } ?? true
                    if changed {
                        guard workspace.persistence.tryBeginFinal(replacing: operation) else { notice = L10n.string("wait_for_file_operation"); return false }
                        finalLease = true
                    }
                }
                guard !changed || revision < UInt64.max else { throw DocumentError.revisionExhausted }
                let targetRevision = changed ? revision + 1 : revision
                let record = try workspace.sessionRecord(revision: targetRevision, configuration: configuration)
                if finalLease { try workspace.captureFrozenSession(record, configuration: configuration) }
                let fresh = CoreCoordinator(maximumRequestBytes: SessionLimits.maximumRequestBytes)
                cancellationRequested = false
                exportCoordinator = fresh; isSaving = true
                let response = try await fresh.saveSession(record, paths: newPaths, expectedDiskDigest: nil,
                                                          lockTimeoutMS: configuration.lockTimeoutMS)
                await fresh.shutdown()
                replaced = response.recoveryReplaced
                if finalLease {
                    if !replaced { workspace.releaseFailedFinalSession() }
                    workspace.persistence.finishFinal(replaced: replaced); finalLease = false
                }
                if replaced {
                    await coordinator.shutdown()
                    coordinator = CoreCoordinator(maximumRequestBytes: SessionLimits.maximumRequestBytes)
                    paths = newPaths; expectedDiskDigest = response.diskDigest; revision = record.revision
                    saved(revision == UInt64.max ? try content(of: workspace) : contents)
                    if persistsLocation {
                        let preference = RecoveryLocationPreference(schemaVersion: 1, path: newPaths.recoveryFile)
                        UserDefaults.standard.set(try JSONEncoder().encode(preference), forKey: configuration.locationPreferenceKey)
                    }
                }
                guard response.status == .saved, replaced else {
                    if replaced { isBlocked = true; forceSave = true }
                    notice = L10n.sessionSaveStatus(response.status) + " " + response.detail; return false
                }
                isBlocked = false; notice = revision == UInt64.max ? L10n.string("workspace_revision_frozen") : nil
                forceSave = false
                requestSave()
            } catch {
                if finalLease {
                    if !replaced { workspace.releaseFailedFinalSession() }
                    workspace.persistence.finishFinal(replaced: replaced)
                }
                throw error
            }
            return true
        } catch { notice = cancellationRequested ? L10n.sessionSaveStatus(.cancelled) : L10n.describe(error); return false }
    }

    func loadBackup() async {
        guard !isLoading, !isSaving, !isChangingLocation, !isRestoring, workspace != nil, workspace?.companion.isOperating != true else { return }
        guard let workspace, let operation = try? workspace.persistence.beginOperation() else { return }
        defer { workspace.persistence.endOperation(operation) }
        isRestoring = true
        let task = Task { @MainActor in await self.performLoadBackup() }
        restoreTask = task
        await task.value
        restoreTask = nil
        isRestoring = false
    }

    private func performLoadBackup() async {
        guard let workspace else { return }
        guard workspace.captureForRecoveryReplacement() else { notice = workspace.errorMessage; return }
        // 对已有内存修改先保存副本；不能用损坏原件覆盖工作区。
        if let lastContent, (try? content(of: workspace).hasSameContent(as: lastContent)) != true {
            notice = L10n.string("save_before_restore"); return
        }
        do {
            let before = try content(of: workspace)
            let response = try await coordinator.loadSession(at: paths.backupFile)
            guard response.status == .loaded, let candidate = response.session else {
                notice = L10n.string("backup_unavailable") + " " + response.detail; return
            }
            guard workspace.captureForRecoveryReplacement() else { notice = workspace.errorMessage; return }
            guard try content(of: workspace).hasSameContent(as: before) else { notice = L10n.string("save_before_restore"); return }
            guard workspace.companionBindingMatches(candidate) else { notice = L10n.string("companion_backup_binding"); return }
            try workspace.applyRecoveryCandidate(candidate, configuration: configuration)
            revision = candidate.revision
            lastContent = try content(of: workspace)
            isCapacityBlocked = false
            rejectedContent = nil
            isBlocked = true
            notice = L10n.string("backup_restored_choose_file")
            observeNextChange()
        } catch { notice = L10n.describe(error) }
    }

    func prepareToClose() async -> Bool {
        guard await flush() else {
            let alert = NSAlert()
            alert.messageText = L10n.string("recovery_close_failed")
            alert.informativeText = notice ?? L10n.string("recovery_preserved")
            alert.addButton(withTitle: L10n.string("save_recovery_as"))
            alert.addButton(withTitle: L10n.string("cancel"))
            if alert.runModal() == .alertFirstButtonReturn {
                guard await saveElsewhere() else { return false }
                return await flush()
            }
            return false
        }
        return true
    }

    func revealFile() { NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: paths.recoveryFile)]) }

    func shutdown() async {
        if let copyTask { _ = await copyTask.value }
        if let restoreTask { await restoreTask.value }
        stopping = true
        observationTask?.cancel()
        if let saveTask { await saveTask.value }
        await coordinator.shutdown()
    }
}
