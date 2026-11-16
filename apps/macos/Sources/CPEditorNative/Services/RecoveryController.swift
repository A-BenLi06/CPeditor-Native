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
    let lockTimeoutMS: UInt64
    let workspaceNamespace: String
    let documentNamespace: String
    let locationPreferenceKey: String
    static func load() throws -> Self {
        guard let url = Bundle.module.url(forResource: "recovery", withExtension: "json") else { throw CocoaError(.fileNoSuchFile) }
        let value = try JSONDecoder().decode(Self.self, from: Data(contentsOf: url))
        guard value.schemaVersion == 1, value.lockTimeoutMS <= 60000 else { throw WireError.unsupportedSchema }
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

private struct RecoveryLocationPreference: Codable {
    let schemaVersion: Int
    let path: String
}

@MainActor @Observable final class RecoveryController {
    private(set) var isLoading = false
    private(set) var isSaving = false
    private(set) var isBlocked = false
    private(set) var isChangingLocation = false
    private(set) var isRestoring = false
    private(set) var notice: String?
    private(set) var paths: SessionPaths
    let configuration: RecoveryConfiguration
    @ObservationIgnored private weak var workspace: Workspace?
    @ObservationIgnored private var coordinator = CoreCoordinator()
    @ObservationIgnored private var exportCoordinator: CoreCoordinator?
    @ObservationIgnored private var expectedDiskDigest: String?
    @ObservationIgnored private var revision: UInt64 = 0
    @ObservationIgnored private var lastContent: Data?
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

    private func content(of workspace: Workspace) throws -> Data {
        let encoder = JSONEncoder(); encoder.outputFormatting = [.sortedKeys]
        return try encoder.encode(workspace.sessionRecord(revision: 0, configuration: configuration))
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
        guard !isLoading, !isChangingLocation, !isBlocked, !stopping, workspace != nil else { return }
        pending = true
        guard saveTask == nil else { return }
        saveTask = Task { @MainActor [weak self] in
            guard let self else { return }
            while self.pending, !self.isBlocked {
                self.pending = false
                guard let workspace = self.workspace else { break }
                do {
                    let contents = try self.content(of: workspace)
                    if !self.forceSave, contents == self.lastContent, self.expectedDiskDigest != nil { continue }
                    guard self.revision < UInt64.max else { throw DocumentError.revisionExhausted }
                    let record = try workspace.sessionRecord(revision: self.revision + 1, configuration: self.configuration)
                    self.cancellationRequested = false
                    self.isSaving = true
                    let response = try await self.coordinator.saveSession(record, paths: self.paths,
                        expectedDiskDigest: self.expectedDiskDigest, lockTimeoutMS: self.configuration.lockTimeoutMS)
                    if response.recoveryReplaced {
                        self.expectedDiskDigest = response.diskDigest
                        self.revision = record.revision
                        self.lastContent = contents
                    }
                    if response.status != .saved {
                        self.isBlocked = true
                        self.forceSave = true
                        self.notice = L10n.sessionSaveStatus(response.status) + " " + response.detail
                    } else {
                        self.forceSave = false
                    }
                } catch {
                    self.isBlocked = true
                    self.notice = self.cancellationRequested ? L10n.sessionSaveStatus(.cancelled) : L10n.describe(error)
                }
                self.isSaving = false
            }
            self.saveTask = nil
        }
    }

    func flush() async -> Bool {
        guard !isLoading, !stopping else { return false }
        if let copyTask { _ = await copyTask.value }
        if let restoreTask { await restoreTask.value }
        requestSave()
        if let saveTask { await saveTask.value }
        // 同一 actor 上检查快照，覆盖保存期间发生但观察回调尚未排队的变更。
        if !isBlocked, let workspace, (try? content(of: workspace)) != lastContent {
            requestSave()
            if let saveTask { await saveTask.value }
        }
        return !isBlocked
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
        coordinator = CoreCoordinator()
        isBlocked = false; notice = nil
        requestSave()
    }

    func saveElsewhere() async -> Bool {
        guard workspace != nil, !isChangingLocation, !isRestoring else { return false }
        isChangingLocation = true
        let panel = NSSavePanel()
        panel.nameFieldStringValue = configuration.recoveryFilename
        let result = panel.runModal()
        isChangingLocation = false
        guard result == .OK, let file = panel.url else { return false }
        return await saveCopy(to: file)
    }

    @discardableResult func saveCopy(to file: URL) async -> Bool {
        guard copyTask == nil, !isChangingLocation, !isRestoring, !isLoading, workspace != nil else { return false }
        isChangingLocation = true
        let task = Task { @MainActor in await self.performSaveCopy(to: file) }
        copyTask = task
        let saved = await task.value
        copyTask = nil
        return saved
    }

    private func performSaveCopy(to file: URL) async -> Bool {
        guard let workspace else { isChangingLocation = false; return false }
        defer { isChangingLocation = false; isSaving = false; exportCoordinator = nil; requestSave() }
        if let saveTask { await saveTask.value }
        let newPaths = configuration.paths(for: file)
        do {
            let oldPaths = paths
            let distinct = try await Task.detached {
                for old in [oldPaths.recoveryFile, oldPaths.backupFile, oldPaths.lockFile] {
                    for new in [newPaths.recoveryFile, newPaths.backupFile, newPaths.lockFile] {
                        if try CoordinatedSourceFile.aliases(URL(fileURLWithPath: new), URL(fileURLWithPath: old)) { return false }
                    }
                }
                return true
            }.value
            guard distinct else { notice = L10n.string("choose_distinct_recovery"); return false }
            let record = try workspace.sessionRecord(revision: revision, configuration: configuration)
            let contents = try content(of: workspace)
            let fresh = CoreCoordinator()
            cancellationRequested = false
            exportCoordinator = fresh; isSaving = true
            let response = try await fresh.saveSession(record, paths: newPaths, expectedDiskDigest: nil,
                                                      lockTimeoutMS: configuration.lockTimeoutMS)
            await fresh.shutdown()
            guard response.status == .saved else { notice = L10n.sessionSaveStatus(response.status) + " " + response.detail; return false }
            await coordinator.shutdown(); coordinator = CoreCoordinator()
            paths = newPaths; expectedDiskDigest = response.diskDigest
            lastContent = contents
            isBlocked = false; notice = nil
            forceSave = false
            if persistsLocation {
                let preference = RecoveryLocationPreference(schemaVersion: 1, path: newPaths.recoveryFile)
                UserDefaults.standard.set(try JSONEncoder().encode(preference), forKey: configuration.locationPreferenceKey)
            }
            requestSave()
            return true
        } catch { notice = cancellationRequested ? L10n.sessionSaveStatus(.cancelled) : L10n.describe(error); return false }
    }

    func loadBackup() async {
        guard !isLoading, !isSaving, !isChangingLocation, !isRestoring, workspace != nil else { return }
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
        if let lastContent, (try? content(of: workspace)) != lastContent {
            notice = L10n.string("save_before_restore"); return
        }
        do {
            let before = try content(of: workspace)
            let response = try await coordinator.loadSession(at: paths.backupFile)
            guard response.status == .loaded, let candidate = response.session else {
                notice = L10n.string("backup_unavailable") + " " + response.detail; return
            }
            guard workspace.captureForRecoveryReplacement() else { notice = workspace.errorMessage; return }
            guard try content(of: workspace) == before else { notice = L10n.string("save_before_restore"); return }
            try workspace.applyRecoveryCandidate(candidate, configuration: configuration)
            revision = candidate.revision
            lastContent = try content(of: workspace)
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
