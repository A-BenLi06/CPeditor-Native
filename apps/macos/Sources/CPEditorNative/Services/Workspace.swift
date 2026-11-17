import AppKit
import Observation
import NativeEditorModel
import NativeCoreBridge

struct WorkspaceConfiguration: Decodable {
    let schemaVersion: Int
    let defaultLanguageID: String
    let languageIDs: [String]
    let defaultEditor: EditorKind
    let maxSourceBytes: Int

    static func load() throws -> Self {
        guard let url = Bundle.module.url(forResource: "workspace", withExtension: "json") else { throw CocoaError(.fileNoSuchFile) }
        let config = try JSONDecoder().decode(Self.self, from: Data(contentsOf: url))
        let ids = config.languageIDs.map { RawIdentifier($0) }
        guard config.schemaVersion == 1, ids.contains(RawIdentifier(config.defaultLanguageID)),
              Set(ids).count == ids.count, config.maxSourceBytes > 0 else { throw CocoaError(.fileReadCorruptFile) }
        return config
    }
}

@MainActor @Observable final class Workspace {
    let persistence = PersistenceGate()
    private var storedSessions: [NativeEditorSession] = []
    var sessions: [NativeEditorSession] {
        get { storedSessions }
        set {
            guard persistence.canMutate else { errorMessage = L10n.string("workspace_revision_frozen"); return }
            newValue.forEach { $0.attachPersistence(persistence) }
            storedSessions = newValue
        }
    }
    var selection: RawIdentifier? {
        didSet {
            if persistence.canMutate, let id = activeSession?.toolchainProfileID, runner.profiles.contains(where: { $0.id == id }) {
                runner.selectedProfileID = id
            }
        }
    }
    private var storedNewEditor: EditorKind = .codeEditorView
    private var storedWrapText = false
    private var storedShowMinimap = false
    var newEditor: EditorKind {
        get { storedNewEditor }
        set { guard persistence.canMutate else { errorMessage = L10n.string("workspace_revision_frozen"); return }; storedNewEditor = newValue }
    }
    var wrapText: Bool {
        get { storedWrapText }
        set { guard persistence.canMutate else { errorMessage = L10n.string("workspace_revision_frozen"); return }; storedWrapText = newValue }
    }
    var showMinimap: Bool {
        get { storedShowMinimap }
        set { guard persistence.canMutate else { errorMessage = L10n.string("workspace_revision_frozen"); return }; storedShowMinimap = newValue }
    }
    var errorMessage: String?
    private(set) var preferenceNotice: String?
    private(set) var isLoading = false
    private(set) var isClosing = false
    private(set) var isShutDown = false
    private(set) var savingDocumentIDs: Set<RawIdentifier> = []
    private(set) var closingDocumentID: RawIdentifier?
    private(set) var retainedCloseSession: NativeEditorSession?
    var editorDeckSessions: [NativeEditorSession] {
        guard let retainedCloseSession, !sessions.contains(where: { $0 === retainedCloseSession }) else { return sessions }
        return sessions + [retainedCloseSession]
    }
    private(set) var decidingCloseID: RawIdentifier?
    private(set) var pendingOpenCount = 0
    private(set) var workspaceID = RawIdentifier(UUID().uuidString)
    let runner: RunController
    let recovery: RecoveryController?
    let companion = CompanionController()
    @ObservationIgnored private var configuration: WorkspaceConfiguration?
    @ObservationIgnored private var profiles: [RawIdentifier: LanguageProfile] = [:]
    @ObservationIgnored private var preservedPreferences: [NativePreferences] = []
    @ObservationIgnored private var startupFiles: [URL] = []
    @ObservationIgnored private var bootstrapped = false
    @ObservationIgnored private var frozenSession: WorkspaceSessionRecord?
    @ObservationIgnored private var frozenFingerprint: Data?
    var persistentChangesAllowed: Bool { persistence.canMutate }
    var persistentStateFrozen: Bool { frozenSession != nil }


    var activeSession: NativeEditorSession? { sessions.first { $0.document.id == selection } }
    var isBusy: Bool {
        persistence.isFinalizing || isLoading || isClosing || !savingDocumentIDs.isEmpty || closingDocumentID != nil || decidingCloseID != nil || pendingOpenCount > 0
            || runner.pendingImportCount > 0 || recovery?.isChangingLocation == true || recovery?.isRestoring == true || companion.isOperating
    }

    /// 测试默认不访问用户恢复目录；真实 Scene 显式启用 automaticRecovery。
    init(arguments: [String] = [], automaticRecovery: Bool = false, recoveryURL: URL? = nil) {
        runner = RunController(persistence: persistence)
        let recoveryIndex = arguments.firstIndex(of: "--recovery-file")
        let argumentURL = recoveryIndex.flatMap { $0 + 1 < arguments.count ? URL(fileURLWithPath: arguments[$0 + 1]) : nil }
        let requestedRecovery = recoveryURL ?? argumentURL
        recovery = (automaticRecovery || requestedRecovery != nil) ? try? RecoveryController(file: requestedRecovery, configuration: .load()) : nil
        isLoading = automaticRecovery || requestedRecovery != nil
        persistence.onModeChange = { [weak self] in
            guard let self else { return }
            self.editorDeckSessions.forEach {
                $0.document.setPersistenceReadOnly(!self.persistence.canMutate)
                $0.setInteractionEnabled(self.persistence.canMutate)
            }
        }
        persistence.onIdle = { [weak self] in self?.recovery?.requestSave() }
        do {
            let config = try WorkspaceConfiguration.load()
            configuration = config
            for id in config.languageIDs { profiles[RawIdentifier(id)] = try LanguageProfile.load(id: id) }
            newEditor = config.defaultEditor
            var cursor = 0
            while cursor < arguments.count {
                if arguments[cursor] == "--recovery-file", cursor + 1 < arguments.count {
                    cursor += 2
                } else if arguments[cursor] == "--editor", cursor + 1 < arguments.count,
                   let kind = EditorKind(rawValue: arguments[cursor + 1]) {
                    newEditor = kind; cursor += 2
                } else if !arguments[cursor].hasPrefix("-") {
                    startupFiles.append(URL(fileURLWithPath: arguments[cursor])); cursor += 1
                } else { cursor += 1 }
            }
            if !isLoading { createDocument() }
            if isLoading, recovery == nil { throw CocoaError(.fileReadCorruptFile) }
        } catch { errorMessage = L10n.describe(error) }
    }

    func bootstrap() async {
        guard !bootstrapped else { return }
        bootstrapped = true
        if let recovery { await recovery.bootstrap(self) }
        else { await finishBootstrap() }
        await companion.bootstrap(self)
    }

    func finishBootstrap() async {
        isLoading = false
        let files = startupFiles; startupFiles.removeAll()
        for file in files { await open(file) }
        if sessions.isEmpty { createDocument() }
    }

    func title(for session: NativeEditorSession) -> String { session.document.url?.lastPathComponent ?? L10n.string("untitled") }

    @discardableResult func createDocument(text: String = "", editor: EditorKind? = nil, languageID: String? = nil) -> NativeEditorSession? {
        guard persistence.canMutate, !isLoading, !isClosing, let config = configuration,
              let profile = profiles[RawIdentifier(languageID ?? config.defaultLanguageID)] else { return nil }
        do {
            let document = SourceDocument(text: text, editorKind: editor ?? newEditor, languageID: profile.id, saved: text.isEmpty)
            let session = try NativeEditorSession(document: document, profile: profile)
            sessions.append(session); selection = document.id
            AppTelemetry.documents.info("Created document with editor \(document.editorKind.rawValue, privacy: .public)")
            return session
        } catch { errorMessage = L10n.describe(error); return nil }
    }

    func openPanel() async {
        guard !isBusy, let operation = try? persistence.beginOperation() else { return }
        defer { persistence.endOperation(operation) }
        let panel = NSOpenPanel(); panel.allowsMultipleSelection = true; panel.canChooseDirectories = false
        if panel.runModal() == .OK { for url in panel.urls { await open(url) } }
    }

    func open(_ url: URL) async {
        guard !isClosing, !isShutDown, let config = configuration else { return }
        let normalized = url.standardizedFileURL
        if let existing = sessions.first(where: { $0.document.url?.standardizedFileURL == normalized }) {
            selection = existing.document.id; return
        }
        guard let operation = try? persistence.beginOperation() else { return }
        defer { persistence.endOperation(operation) }
        guard let profile = profiles.values.first(where: { $0.fileExtensions.contains(url.pathExtension.lowercased()) })
            ?? profiles[RawIdentifier(config.defaultLanguageID)] else { return }
        pendingOpenCount += 1
        defer { pendingOpenCount -= 1 }
        do {
            let contents = try await Task.detached { try CoordinatedSourceFile.read(url, maximumBytes: config.maxSourceBytes) }.value
            guard !isClosing, !isShutDown else { return }
            if let existing = sessions.first(where: { $0.document.url?.standardizedFileURL == normalized }) {
                selection = existing.document.id; return
            }
            let point = SourceSavePoint(revision: 0, sourceDigest: SourceDigest.sha256(Data(contents.text.utf8)), diskDigest: contents.diskDigest)
            let document = SourceDocument(text: contents.text, url: url, editorKind: newEditor, languageID: profile.id, savePoint: point)
            let session = try NativeEditorSession(document: document, profile: profile)
            sessions.append(session); selection = document.id
            AppTelemetry.documents.info("Opened document with editor \(document.editorKind.rawValue, privacy: .public)")
        } catch { errorMessage = L10n.describe(error) }
    }

    func openCopyInAlternativeEditor() {
        guard !isBusy, let source = activeSession, captureForAction(source) else { return }
        createDocument(text: source.document.text, editor: source.document.editorKind.alternative, languageID: source.document.languageID)
    }

    func openLiveBufferCopy() {
        guard !isBusy, let source = activeSession else { return }
        do { createDocument(text: try source.currentNativeText(), editor: source.document.editorKind, languageID: source.document.languageID) }
        catch { errorMessage = L10n.describe(error) }
    }

    private func captureForAction(_ session: NativeEditorSession) -> Bool {
        session.capture()
        if session.isComposing { errorMessage = L10n.string("finish_composition"); return false }
        if let failure = session.captureFailure { errorMessage = L10n.describe(failure); return false }
        return true
    }

    func captureForRecoveryReplacement() -> Bool {
        for session in sessions where !captureForAction(session) { return false }
        return true
    }

    @discardableResult func save(_ session: NativeEditorSession? = nil, chooseURL: Bool = false) async -> Bool {
        guard let operation = try? persistence.beginOperation() else { return false }
        defer { persistence.endOperation(operation) }
        guard let session = session ?? activeSession, session.canMutatePersistence, !savingDocumentIDs.contains(session.document.id), captureForAction(session) else { return false }
        var destination = chooseURL ? nil : session.document.url
        var selectedReplacement = false
        if destination == nil {
            let panel = NSSavePanel()
            panel.nameFieldStringValue = session.document.url?.lastPathComponent
                ?? profiles[RawIdentifier(session.document.languageID)]?.suggestedFileName ?? L10n.string("untitled")
            if panel.runModal() == .OK { destination = panel.url; selectedReplacement = true }
        }
        guard let destination else { return false }
        return await saveSource(session, to: destination, selectedReplacement: selectedReplacement)
    }

    /// 写入时复制快照并保留异步期间的新版本。
    @discardableResult func saveSource(_ session: NativeEditorSession, to destination: URL, selectedReplacement: Bool = false) async -> Bool {
        guard let operation = try? persistence.beginOperation() else { return false }
        defer { persistence.endOperation(operation) }
        guard session.canMutatePersistence, !savingDocumentIDs.contains(session.document.id), captureForAction(session) else { return false }
        guard session.canMutatePersistence else { errorMessage = L10n.string("revision_exhausted"); return false }
        savingDocumentIDs.insert(session.document.id)
        defer { savingDocumentIDs.remove(session.document.id) }
        do {
            let snapshot = session.document.snapshot()
            let sourceSave = try session.document.beginSourceSave(snapshot: snapshot)
            defer { session.document.cancelSourceSave(sourceSave) }
            let original = session.document.url
            let expected = session.document.savePoint?.diskDigest
            let paths = (recovery.map { [$0.paths.recoveryFile, $0.paths.backupFile, $0.paths.lockFile] } ?? []) + (companion.paths?.all ?? [])
            let digest = try await Task.detached {
                let sameOriginal = try original.map { try CoordinatedSourceFile.aliases(destination, $0) } ?? false
                return try CoordinatedSourceFile.write(snapshot, to: destination, expectedDiskDigest: sameOriginal ? expected : nil,
                    allowSelectedReplacement: selectedReplacement && !sameOriginal, reservedPaths: paths)
            }.value
            try session.document.completeSourceSave(sourceSave, at: destination, diskDigest: digest)
            AppTelemetry.documents.info("Saved document revision \(snapshot.revision, privacy: .public)")
            return true
        } catch { errorMessage = L10n.describe(error); return false }
    }

    @discardableResult func canClose(_ session: NativeEditorSession) async -> Bool {
        guard !savingDocumentIDs.contains(session.document.id) else { return false }
        session.capture()
        guard !session.isComposing else { errorMessage = L10n.string("finish_composition"); return false }
        if let failure = session.captureFailure {
            let alert = NSAlert()
            alert.messageText = L10n.describe(failure)
            alert.addButton(withTitle: L10n.string("copy_current_buffer")); alert.addButton(withTitle: L10n.string("cancel")); alert.addButton(withTitle: L10n.string("discard"))
            switch alert.runModal() {
            case .alertFirstButtonReturn:
                do {
                    return createDocument(text: try session.currentNativeText(), editor: session.document.editorKind,
                                          languageID: session.document.languageID) != nil
                } catch { errorMessage = L10n.describe(error); return false }
            case .alertThirdButtonReturn: return true
            default: return false
            }
        }
        guard session.document.isDirty else { return true }
        let alert = NSAlert()
        alert.messageText = String(format: L10n.string("save_changes_title"), title(for: session))
        alert.informativeText = L10n.string("save_changes_message")
        alert.addButton(withTitle: L10n.string("save")); alert.addButton(withTitle: L10n.string("cancel")); alert.addButton(withTitle: L10n.string("discard"))
        switch alert.runModal() {
        case .alertFirstButtonReturn: return await saveForClose(session)
        case .alertThirdButtonReturn: return true
        default: return false
        }
    }

    func saveForClose(_ session: NativeEditorSession) async -> Bool {
        guard await save(session) else { return false }
        guard captureForAction(session), !session.document.isDirty else {
            errorMessage = L10n.string("changed_during_close_save"); return false
        }
        return true
    }

    func closeActiveDocument() async {
        guard !isBusy else { errorMessage = L10n.string("wait_for_file_operation"); return }
        guard let session = activeSession else { return }
        guard let operation = try? persistence.beginOperation() else { errorMessage = L10n.string("workspace_revision_frozen"); return }
        decidingCloseID = session.document.id
        session.setInteractionEnabled(false)
        defer {
            retainedCloseSession = nil; closingDocumentID = nil; decidingCloseID = nil
            if sessions.contains(where: { $0 === session }) { session.setInteractionEnabled(persistence.canMutate) }
            persistence.endOperation(operation)
        }
        guard await canClose(session), let originalIndex = sessions.firstIndex(where: { $0 === session }) else { return }
        let originalSelection = selection
        closingDocumentID = session.document.id
        // 从持久模型同步移除，再释放自己的 operation；原容器仍隐藏挂载，失败可原位回滚。
        retainedCloseSession = session
        sessions.removeAll { $0 === session }
        if selection == session.document.id { selection = sessions.last?.document.id }
        persistence.endOperation(operation)
        if let recovery, !(await recovery.flush()) {
            if persistence.canMutate {
                if !sessions.contains(where: { $0 === session }) { sessions.insert(session, at: min(originalIndex, sessions.count)) }
                if let originalSelection, sessions.contains(where: { $0.document.id == originalSelection }) { selection = originalSelection }
                else if selection == nil { selection = session.document.id }
                errorMessage = recovery.notice ?? L10n.string("document_close_pending")
                return
            }
            // MAX 已实际替换时保留精确的已移除模型；不能重新插入无法再保存的幽灵标签。
            errorMessage = recovery.notice ?? L10n.string("workspace_revision_frozen")
        }
        retainedCloseSession = nil
        runner.forget(documentID: session.document.id); session.dispose()
        AppTelemetry.documents.info("Closed document")
    }

    func prepareToClose() async -> Bool {
        if isShutDown { return true }
        guard !isBusy else { errorMessage = L10n.string("wait_for_file_operation"); return false }
        isClosing = true
        for session in sessions {
            guard captureForAction(session) else { isClosing = false; return false }
        }
        sessions.forEach { $0.setInteractionEnabled(false) }
        defer { if !isShutDown { sessions.forEach { $0.setInteractionEnabled(true) } } }
        if let recovery {
            guard await recovery.prepareToClose() else { isClosing = false; return false }
        } else {
            for session in sessions {
                guard await canClose(session) else { isClosing = false; return false }
            }
        }
        await companion.shutdown(); await runner.shutdown(); await recovery?.shutdown()
        isShutDown = true
        return true
    }

    func sessionRecord(revision: UInt64, configuration recoveryConfig: RecoveryConfiguration) throws -> WorkspaceSessionRecord {
        if let frozenSession {
            guard try persistentFingerprint(configuration: recoveryConfig) == frozenFingerprint else { throw DocumentError.revisionExhausted }
            return frozenSession
        }
        return try liveSessionRecord(revision: revision, configuration: recoveryConfig)
    }

    private func persistentFingerprint(configuration: RecoveryConfiguration) throws -> Data {
        let encoder = JSONEncoder(); encoder.outputFormatting = [.sortedKeys]
        return try encoder.encode(liveSessionRecord(revision: 0, configuration: configuration))
    }

    func captureFrozenSession(_ record: WorkspaceSessionRecord, configuration: RecoveryConfiguration) throws {
        frozenFingerprint = try persistentFingerprint(configuration: configuration)
        frozenSession = record
    }
    func releaseFailedFinalSession() { frozenSession = nil; frozenFingerprint = nil }

    private func liveSessionRecord(revision: UInt64, configuration recoveryConfig: RecoveryConfiguration) throws -> WorkspaceSessionRecord {
        let encoder = JSONEncoder(); encoder.outputFormatting = [.sortedKeys]
        let native = WorkspaceNativePreferences(profiles: runner.profiles, selectedProfileID: runner.selectedProfileID,
            comparison: runner.comparison, newEditor: newEditor, wrapText: wrapText, showMinimap: showMinimap)
        let own = NativePreferences(namespaceID: recoveryConfig.workspaceNamespace, schemaVersion: 1,
            objectJSON: String(decoding: try encoder.encode(native), as: UTF8.self))
        let included = sessions
        let documents = try included.map { session in
            if let failure = session.captureFailure { throw failure }
            if let immutable = try session.immutableRecordForPersistence() { return immutable }
            let range = session.committedSelection
            let preferences = DocumentNativePreferences(editor: session.document.editorKind, selectionLocation: range.location, selectionLength: range.length)
            let own = NativePreferences(namespaceID: recoveryConfig.documentNamespace, schemaVersion: 1,
                objectJSON: String(decoding: try encoder.encode(preferences), as: UTF8.self))
            let hasOpaqueOwn = session.preservedPreferences.contains { $0.namespaceID.utf8.elementsEqual(recoveryConfig.documentNamespace.utf8) }
            let canAddOwn = session.preservedPreferences.count < recoveryConfig.maxPreferencesPerOwner
            return SessionDocumentRecord(snapshot: session.document.snapshot(), originalPath: session.document.url?.path,
                languageID: session.document.languageID, toolchainProfileID: session.toolchainProfileID,
                savePoint: session.document.savePoint, tests: session.testCases,
                editorPreferences: session.preservedPreferences + (hasOpaqueOwn || !canAddOwn ? [] : [own]))
        }
        let selected = frozenSession != nil ? frozenSession!.selectedDocumentID : selection
        let hasOpaqueOwn = preservedPreferences.contains { $0.namespaceID.utf8.elementsEqual(recoveryConfig.workspaceNamespace.utf8) }
        let hasOpaqueChecker = preservedPreferences.contains { $0.namespaceID.utf8.elementsEqual(recoveryConfig.checkerNamespace.utf8) }
        let checkerHasData = runner.externalCheckerEnabled || runner.externalChecker != nil
        let checkerSlots = checkerHasData && !hasOpaqueChecker ? 1 : 0
        let canAddWorkspace = preservedPreferences.count + checkerSlots < recoveryConfig.maxPreferencesPerOwner
        let checker = CheckerNativePreferences(enabled: runner.externalCheckerEnabled, configurationJSON: runner.externalChecker?.originalJSON)
        let checkerPreference = NativePreferences(namespaceID: recoveryConfig.checkerNamespace, schemaVersion: 1,
            objectJSON: String(decoding: try encoder.encode(checker), as: UTF8.self))
        return WorkspaceSessionRecord(workspaceID: workspaceID, revision: revision, selectedDocumentID: selected,
            documents: documents, editorPreferences: preservedPreferences + (hasOpaqueOwn || !canAddWorkspace ? [] : [own])
                + (hasOpaqueChecker || !checkerHasData ? [] : [checkerPreference]))
    }

    func applyRecoveryCandidate(_ candidate: WorkspaceSessionRecord, configuration recoveryConfig: RecoveryConfiguration,
        sessionFactory: (SourceDocument, LanguageProfile) throws -> NativeEditorSession = { try NativeEditorSession(document: $0, profile: $1) }) throws {
        guard persistence.canMutate else { throw DocumentError.revisionExhausted }
        guard configuration != nil, candidate.schemaVersion == 1,
              !candidate.workspaceID.isEmpty, !candidate.workspaceID.rawValue.contains("\0"),
              Set(candidate.documents.map(\.documentID)).count == candidate.documents.count,
              candidate.selectedDocumentID == nil || candidate.documents.contains(where: { $0.documentID == candidate.selectedDocumentID }) else { throw WireError.unsupportedSchema }
        let isOwnWorkspace: (NativePreferences) -> Bool = { $0.schemaVersion == 1 && $0.namespaceID.utf8.elementsEqual(recoveryConfig.workspaceNamespace.utf8) }
        let ownWorkspace = candidate.editorPreferences.filter(isOwnWorkspace)
        guard ownWorkspace.count <= 1 else { throw CocoaError(.fileReadCorruptFile) }
        let native = try ownWorkspace.first.map { try JSONDecoder().decode(WorkspaceNativePreferences.self, from: Data($0.objectJSON.utf8)) }
        let isOwnChecker: (NativePreferences) -> Bool = { $0.schemaVersion == 1 && $0.namespaceID.utf8.elementsEqual(recoveryConfig.checkerNamespace.utf8) }
        let ownChecker = candidate.editorPreferences.filter(isOwnChecker)
        let opaqueChecker = candidate.editorPreferences.contains { $0.schemaVersion != 1 && $0.namespaceID.utf8.elementsEqual(recoveryConfig.checkerNamespace.utf8) }
        guard ownChecker.count <= 1 else { throw CocoaError(.fileReadCorruptFile) }
        let checkerPrefs = try (opaqueChecker ? nil : ownChecker.first).map { try JSONDecoder().decode(CheckerNativePreferences.self, from: Data($0.objectJSON.utf8)) }
        let checker = try checkerPrefs?.configurationJSON.map { try runner.validateChecker($0) }
        guard checkerPrefs?.enabled != true || checker != nil else { throw CheckerConfigurationError("checker_error_required_configuration") }
        if let native {
            guard !native.profiles.isEmpty, Set(native.profiles.map(\.id)).count == native.profiles.count,
                  native.profiles.contains(where: { $0.id == native.selectedProfileID }) else { throw WireError.invalidProfile }
        }
        var constructed: [NativeEditorSession] = []
        do {
            for record in candidate.documents {
                constructed.append(try constructRecoveredSession(record, configuration: recoveryConfig, sessionFactory: sessionFactory))
            }
        } catch { constructed.forEach { $0.dispose() }; throw error }
        sessions.forEach { runner.forget(documentID: $0.document.id); $0.dispose() }
        workspaceID = candidate.workspaceID
        preservedPreferences = candidate.editorPreferences.filter { !isOwnWorkspace($0) && (opaqueChecker || !isOwnChecker($0)) }
        let hasOpaqueWorkspace = preservedPreferences.contains { $0.namespaceID.utf8.elementsEqual(recoveryConfig.workspaceNamespace.utf8) }
        let hasOpaqueDocument = constructed.contains { session in
            session.preservedPreferences.contains { $0.namespaceID.utf8.elementsEqual(recoveryConfig.documentNamespace.utf8) }
        }
        let checkerHasData = checkerPrefs?.enabled == true || checker != nil
        let checkerSlots = checkerHasData ? 1 : 0
        let workspaceSlots = !hasOpaqueWorkspace && preservedPreferences.count + checkerSlots < recoveryConfig.maxPreferencesPerOwner ? 1 : 0
        runner.checkerPreferenceStorageAvailable = checkerHasData || preservedPreferences.count + workspaceSlots < recoveryConfig.maxPreferencesPerOwner
        let documentCapacityFull = constructed.contains { session in
            !session.preservedPreferences.contains { $0.namespaceID.utf8.elementsEqual(recoveryConfig.documentNamespace.utf8) }
                && session.preservedPreferences.count >= recoveryConfig.maxPreferencesPerOwner
        }
        let workspaceCapacityFull = !hasOpaqueWorkspace && workspaceSlots == 0
        let notices = [hasOpaqueWorkspace || hasOpaqueDocument || opaqueChecker ? L10n.string("future_native_preferences") : nil,
                       workspaceCapacityFull || documentCapacityFull || !runner.checkerPreferenceStorageAvailable ? L10n.string("preference_capacity_notice") : nil].compactMap { $0 }
        preferenceNotice = notices.isEmpty ? nil : notices.joined(separator: " ")
        runner.restoreChecker(checker, enabled: checkerPrefs?.enabled ?? false, opaque: opaqueChecker)
        if let native {
            runner.profiles = native.profiles; runner.selectedProfileID = native.selectedProfileID; runner.comparison = native.comparison
            newEditor = native.newEditor; wrapText = native.wrapText; showMinimap = native.showMinimap
        }
        sessions = constructed; selection = candidate.selectedDocumentID
        if candidate.revision == UInt64.max {
            try captureFrozenSession(candidate, configuration: recoveryConfig)
            persistence.freezeRestored()
        }
    }
    private func constructRecoveredSession(_ record: SessionDocumentRecord, configuration recoveryConfig: RecoveryConfiguration,
        sessionFactory: (SourceDocument, LanguageProfile) throws -> NativeEditorSession = { try NativeEditorSession(document: $0, profile: $1) }) throws -> NativeEditorSession {
        guard let config = configuration else { throw WireError.unsupportedSchema }
        guard !record.documentID.isEmpty, !record.documentID.rawValue.contains("\0"),
              record.sourceText.utf8.count <= config.maxSourceBytes,
              record.sourceDigest == SourceDigest.sha256(Data(record.sourceText.utf8)),
              let profile = profiles[RawIdentifier(record.languageID ?? config.defaultLanguageID)] else { throw CocoaError(.fileReadCorruptFile) }
        if let point = record.savePoint {
            guard record.originalPath != nil, point.revision <= record.revision,
                  point.revision != record.revision || point.sourceDigest == record.sourceDigest else { throw CocoaError(.fileReadCorruptFile) }
        }
        let isOwnDocument: (NativePreferences) -> Bool = { $0.schemaVersion == 1 && $0.namespaceID.utf8.elementsEqual(recoveryConfig.documentNamespace.utf8) }
        let own = record.editorPreferences.filter(isOwnDocument)
        guard own.count <= 1 else { throw CocoaError(.fileReadCorruptFile) }
        let prefs = try own.first.map { try JSONDecoder().decode(DocumentNativePreferences.self, from: Data($0.objectJSON.utf8)) }
        let range = NSRange(location: prefs?.selectionLocation ?? 0, length: prefs?.selectionLength ?? 0)
        _ = try TextCoordinateIndex(record.sourceText).utf8Range(for: range)
        let document = SourceDocument(id: record.documentID, text: record.sourceText,
            url: record.originalPath.map { URL(fileURLWithPath: $0) }, editorKind: prefs?.editor ?? config.defaultEditor,
            languageID: profile.id, saved: false, revision: record.revision, savePoint: record.savePoint)
        let session = try sessionFactory(document, profile)
        do { try session.validateRestoredBuffer(); try session.restoreSelection(range) }
        catch { session.dispose(); throw error }
        try session.initializePersistentFields(tests: record.tests, profileID: record.toolchainProfileID,
            preferences: record.editorPreferences.filter { !isOwnDocument($0) }, immutableRecord: record.revision == UInt64.max ? record : nil)
        if record.revision == UInt64.max { session.document.setPersistenceReadOnly(true); session.setInteractionEnabled(false) }
        return session
    }

    func companionDefaults(source: String, configuration: RecoveryConfiguration) throws -> CompanionDefaults {
        guard let language = runner.selectedProfile?.languageID else { throw WireError.invalidProfile }
        let preferences = DocumentNativePreferences(editor: newEditor, selectionLocation: 0, selectionLength: 0)
        let own = NativePreferences(namespaceID: configuration.documentNamespace, schemaVersion: 1,
            objectJSON: String(decoding: try JSONEncoder().encode(preferences), as: UTF8.self))
        return CompanionDefaults(sourceText: source, languageID: language, toolchainProfileID: runner.selectedProfileID, documentPreferences: [own])
    }

    func companionStoragePreference(namespace: String) throws -> CompanionStorageBinding? {
        try CompanionStorageBinding.read(preservedPreferences, namespace: namespace)
    }

    func installCompanionStorage(_ binding: CompanionStorageBinding, namespace: String, configuration: RecoveryConfiguration) throws {
        guard persistence.canMutate, binding.workspaceID == workspaceID, try companionStoragePreference(namespace: namespace) == nil else { throw WireError.unsupportedSchema }
        let current = try sessionRecord(revision: 0, configuration: configuration)
        guard current.editorPreferences.count < configuration.maxPreferencesPerOwner else { throw WireError.busy }
        let data = try JSONEncoder().encode(binding)
        preservedPreferences.append(NativePreferences(namespaceID: namespace, schemaVersion: 1, objectJSON: String(decoding: data, as: UTF8.self)))
    }

    /// 只合并 Core 候选的新增文档和回执 owner；既有 editor／撤销栈及异步期间的编辑继续沿用。
    func appendCompanionCandidate(_ candidate: WorkspaceSessionRecord, base: WorkspaceSessionRecord,
                                 receipt: CompanionReceipt, configuration: RecoveryConfiguration) throws {
        guard persistence.canMutate, captureForRecoveryReplacement(), candidate.workspaceID == workspaceID, base.workspaceID == workspaceID,
              base.revision < UInt64.max, candidate.revision == base.revision + 1,
              candidate.documents.count == base.documents.count + 1,
              candidate.selectedDocumentID == receipt.documentID,
              !sessions.contains(where: { $0.document.id == receipt.documentID }) else { throw WireError.malformedTerminal }
        let encoder = JSONEncoder(); encoder.outputFormatting = [.sortedKeys]
        guard try encoder.encode(Array(candidate.documents.dropLast())) == encoder.encode(base.documents) else { throw WireError.malformedTerminal }
        let isReceipt: (NativePreferences) -> Bool = { $0.namespaceID.utf8.elementsEqual(CompanionProvenance.receiptsNamespace.utf8) }
        guard try encoder.encode(candidate.editorPreferences.filter { !isReceipt($0) }) == encoder.encode(base.editorPreferences.filter { !isReceipt($0) }),
              preservedPreferences.filter(isReceipt) == base.editorPreferences.filter(isReceipt) else { throw WireError.malformedTerminal }
        let oldReceipts = try CompanionProvenance.receipts(base.editorPreferences)
        let newReceipts = try CompanionProvenance.receipts(candidate.editorPreferences)
        guard newReceipts == oldReceipts + [receipt], let record = candidate.documents.last,
              record.documentID == receipt.documentID, record.originalPath == nil, record.savePoint == nil,
              record.tests.map(\.id) == receipt.testIDs else { throw WireError.malformedTerminal }
        _ = try CompanionProvenance.ordinaryStdioAllowed(documentID: record.documentID, preferences: record.editorPreferences)
        let receiptsPreference = candidate.editorPreferences.filter(isReceipt)
        guard receiptsPreference.count == 1 else { throw WireError.malformedTerminal }
        let current = try sessionRecord(revision: 0, configuration: configuration)
        let currentWithoutReceipts = current.editorPreferences.filter { !isReceipt($0) }
        guard currentWithoutReceipts.count + 1 <= configuration.maxPreferencesPerOwner else { throw WireError.busy }
        let added = try constructRecoveredSession(record, configuration: configuration)
        preservedPreferences = preservedPreferences.filter { !isReceipt($0) } + receiptsPreference
        sessions.append(added); selection = added.document.id
    }

    func companionBindingMatches(_ candidate: WorkspaceSessionRecord) -> Bool {
        guard let namespace = companion.configuration?.storageNamespace else { return true }
        do {
            let current = try companionStoragePreference(namespace: namespace)
            let binding = try CompanionStorageBinding.read(candidate.editorPreferences, namespace: namespace)
            if current == nil && binding == nil { return true }
            guard candidate.workspaceID == workspaceID, let current, let binding else { return false }
            return current.workspaceID == workspaceID && binding.workspaceID == candidate.workspaceID
                && binding.workspaceID == current.workspaceID && binding.paths == current.paths && current.paths == companion.paths
        } catch { return false }
    }

}
