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
    var sessions: [NativeEditorSession] = []
    var selection: RawIdentifier? {
        didSet {
            if let id = activeSession?.toolchainProfileID, runner.profiles.contains(where: { $0.id == id }) {
                runner.selectedProfileID = id
            }
        }
    }
    var newEditor: EditorKind = .codeEditorView
    var wrapText = false
    var showMinimap = false
    var errorMessage: String?
    private(set) var preferenceNotice: String?
    private(set) var isLoading = false
    private(set) var isClosing = false
    private(set) var isShutDown = false
    private(set) var savingDocumentIDs: Set<RawIdentifier> = []
    private(set) var closingDocumentID: RawIdentifier?
    private(set) var decidingCloseID: RawIdentifier?
    private(set) var pendingOpenCount = 0
    private(set) var workspaceID = RawIdentifier(UUID().uuidString)
    let runner = RunController()
    let recovery: RecoveryController?
    @ObservationIgnored private var configuration: WorkspaceConfiguration?
    @ObservationIgnored private var profiles: [RawIdentifier: LanguageProfile] = [:]
    @ObservationIgnored private var preservedPreferences: [NativePreferences] = []
    @ObservationIgnored private var startupFiles: [URL] = []
    @ObservationIgnored private var bootstrapped = false

    var activeSession: NativeEditorSession? { sessions.first { $0.document.id == selection } }
    var isBusy: Bool {
        isLoading || isClosing || !savingDocumentIDs.isEmpty || closingDocumentID != nil || decidingCloseID != nil || pendingOpenCount > 0
            || runner.pendingImportCount > 0 || recovery?.isChangingLocation == true || recovery?.isRestoring == true
    }

    /// 测试默认不访问用户恢复目录；真实 Scene 显式启用 automaticRecovery。
    init(arguments: [String] = [], automaticRecovery: Bool = false, recoveryURL: URL? = nil) {
        let recoveryIndex = arguments.firstIndex(of: "--recovery-file")
        let argumentURL = recoveryIndex.flatMap { $0 + 1 < arguments.count ? URL(fileURLWithPath: arguments[$0 + 1]) : nil }
        let requestedRecovery = recoveryURL ?? argumentURL
        recovery = (automaticRecovery || requestedRecovery != nil) ? try? RecoveryController(file: requestedRecovery, configuration: .load()) : nil
        isLoading = automaticRecovery || requestedRecovery != nil
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
    }

    func finishBootstrap() async {
        isLoading = false
        let files = startupFiles; startupFiles.removeAll()
        for file in files { await open(file) }
        if sessions.isEmpty { createDocument() }
    }

    func title(for session: NativeEditorSession) -> String { session.document.url?.lastPathComponent ?? L10n.string("untitled") }

    @discardableResult func createDocument(text: String = "", editor: EditorKind? = nil, languageID: String? = nil) -> NativeEditorSession? {
        guard !isLoading, !isClosing, let config = configuration,
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
        guard !isBusy else { return }
        let panel = NSOpenPanel(); panel.allowsMultipleSelection = true; panel.canChooseDirectories = false
        if panel.runModal() == .OK { for url in panel.urls { await open(url) } }
    }

    func open(_ url: URL) async {
        guard !isClosing, !isShutDown, let config = configuration else { return }
        let normalized = url.standardizedFileURL
        if let existing = sessions.first(where: { $0.document.url?.standardizedFileURL == normalized }) {
            selection = existing.document.id; return
        }
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
        guard let session = session ?? activeSession, !savingDocumentIDs.contains(session.document.id), captureForAction(session) else { return false }
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
        guard !savingDocumentIDs.contains(session.document.id), captureForAction(session) else { return false }
        savingDocumentIDs.insert(session.document.id)
        defer { savingDocumentIDs.remove(session.document.id) }
        do {
            let snapshot = session.document.snapshot()
            let original = session.document.url
            let expected = session.document.savePoint?.diskDigest
            let paths = recovery.map { [$0.paths.recoveryFile, $0.paths.backupFile, $0.paths.lockFile] } ?? []
            let digest = try await Task.detached {
                let sameOriginal = try original.map { try CoordinatedSourceFile.aliases(destination, $0) } ?? false
                return try CoordinatedSourceFile.write(snapshot, to: destination, expectedDiskDigest: sameOriginal ? expected : nil,
                    allowSelectedReplacement: selectedReplacement && !sameOriginal, reservedPaths: paths)
            }.value
            try session.document.markSaved(at: destination, snapshot: snapshot, diskDigest: digest)
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
        guard !isBusy, let session = activeSession else { return }
        decidingCloseID = session.document.id
        session.setInteractionEnabled(false)
        defer { decidingCloseID = nil; if sessions.contains(where: { $0 === session }) { session.setInteractionEnabled(true) } }
        guard await canClose(session) else { return }
        closingDocumentID = session.document.id
        defer { closingDocumentID = nil; if sessions.contains(where: { $0 === session }) { session.setInteractionEnabled(true) } }
        if let recovery, !(await recovery.flush()) {
            errorMessage = recovery.notice; return
        }
        sessions.removeAll { $0 === session }; selection = sessions.last?.document.id
        runner.cancel(documentID: session.document.id); session.dispose()
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
        await runner.shutdown(); await recovery?.shutdown()
        isShutDown = true
        return true
    }

    func sessionRecord(revision: UInt64, configuration recoveryConfig: RecoveryConfiguration) throws -> WorkspaceSessionRecord {
        let encoder = JSONEncoder(); encoder.outputFormatting = [.sortedKeys]
        let native = WorkspaceNativePreferences(profiles: runner.profiles, selectedProfileID: runner.selectedProfileID,
            comparison: runner.comparison, newEditor: newEditor, wrapText: wrapText, showMinimap: showMinimap)
        let own = NativePreferences(namespaceID: recoveryConfig.workspaceNamespace, schemaVersion: 1,
            objectJSON: String(decoding: try encoder.encode(native), as: UTF8.self))
        let included = sessions.filter { $0.document.id != closingDocumentID }
        let documents = try included.map { session in
            if let failure = session.captureFailure { throw failure }
            let range = session.committedSelection
            let preferences = DocumentNativePreferences(editor: session.document.editorKind, selectionLocation: range.location, selectionLength: range.length)
            let own = NativePreferences(namespaceID: recoveryConfig.documentNamespace, schemaVersion: 1,
                objectJSON: String(decoding: try encoder.encode(preferences), as: UTF8.self))
            let hasOpaqueOwn = session.preservedPreferences.contains { $0.namespaceID.utf8.elementsEqual(recoveryConfig.documentNamespace.utf8) }
            return SessionDocumentRecord(snapshot: session.document.snapshot(), originalPath: session.document.url?.path,
                languageID: session.document.languageID, toolchainProfileID: session.toolchainProfileID,
                savePoint: session.document.savePoint, tests: session.testCases,
                editorPreferences: session.preservedPreferences + (hasOpaqueOwn ? [] : [own]))
        }
        let selected = selection == closingDocumentID ? included.last?.document.id : selection
        let hasOpaqueOwn = preservedPreferences.contains { $0.namespaceID.utf8.elementsEqual(recoveryConfig.workspaceNamespace.utf8) }
        return WorkspaceSessionRecord(workspaceID: workspaceID, revision: revision, selectedDocumentID: selected,
            documents: documents, editorPreferences: preservedPreferences + (hasOpaqueOwn ? [] : [own]))
    }

    func applyRecoveryCandidate(_ candidate: WorkspaceSessionRecord, configuration recoveryConfig: RecoveryConfiguration,
        sessionFactory: (SourceDocument, LanguageProfile) throws -> NativeEditorSession = { try NativeEditorSession(document: $0, profile: $1) }) throws {
        guard candidate.revision < UInt64.max else { throw DocumentError.revisionExhausted }
        guard let config = configuration, candidate.schemaVersion == 1,
              !candidate.workspaceID.isEmpty, !candidate.workspaceID.rawValue.contains("\0"),
              Set(candidate.documents.map(\.documentID)).count == candidate.documents.count,
              candidate.selectedDocumentID == nil || candidate.documents.contains(where: { $0.documentID == candidate.selectedDocumentID }) else { throw WireError.unsupportedSchema }
        let isOwnWorkspace: (NativePreferences) -> Bool = { $0.schemaVersion == 1 && $0.namespaceID.utf8.elementsEqual(recoveryConfig.workspaceNamespace.utf8) }
        let ownWorkspace = candidate.editorPreferences.filter(isOwnWorkspace)
        guard ownWorkspace.count <= 1 else { throw CocoaError(.fileReadCorruptFile) }
        let native = try ownWorkspace.first.map { try JSONDecoder().decode(WorkspaceNativePreferences.self, from: Data($0.objectJSON.utf8)) }
        if let native {
            guard !native.profiles.isEmpty, Set(native.profiles.map(\.id)).count == native.profiles.count,
                  native.profiles.contains(where: { $0.id == native.selectedProfileID }) else { throw WireError.invalidProfile }
        }
        var constructed: [NativeEditorSession] = []
        do {
            for record in candidate.documents {
                guard record.revision < UInt64.max else { throw DocumentError.revisionExhausted }
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
                constructed.append(session)
                try session.validateRestoredBuffer(); try session.restoreSelection(range)
                session.testCases = record.tests; session.toolchainProfileID = record.toolchainProfileID
                session.preservedPreferences = record.editorPreferences.filter { !isOwnDocument($0) }
            }
        } catch { constructed.forEach { $0.dispose() }; throw error }
        sessions.forEach { runner.cancel(documentID: $0.document.id); $0.dispose() }
        workspaceID = candidate.workspaceID
        preservedPreferences = candidate.editorPreferences.filter { !isOwnWorkspace($0) }
        let hasOpaqueWorkspace = preservedPreferences.contains { $0.namespaceID.utf8.elementsEqual(recoveryConfig.workspaceNamespace.utf8) }
        let hasOpaqueDocument = constructed.contains { session in
            session.preservedPreferences.contains { $0.namespaceID.utf8.elementsEqual(recoveryConfig.documentNamespace.utf8) }
        }
        preferenceNotice = (hasOpaqueWorkspace || hasOpaqueDocument) ? L10n.string("future_native_preferences") : nil
        if let native {
            runner.profiles = native.profiles; runner.selectedProfileID = native.selectedProfileID; runner.comparison = native.comparison
            newEditor = native.newEditor; wrapText = native.wrapText; showMinimap = native.showMinimap
        }
        sessions = constructed; selection = candidate.selectedDocumentID
    }
}
