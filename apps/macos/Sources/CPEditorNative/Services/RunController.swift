import AppKit
import Observation
import NativeCoreBridge
import NativeEditorModel

struct RunConfiguration: Decodable, Sendable {
    let schemaVersion: Int
    let profileResources: [String]
    let defaultProfileID: String
    let pollingMilliseconds: UInt64
    let drainEvents: UInt32
    let comparison: ComparisonPolicy
    let maxImportBytes: Int
    let maxTestcases: Int
    let binaryPreviewBytes: Int
    let maxCheckerPreferenceBytes: Int
    enum CodingKeys: String, CodingKey {
        case schemaVersion = "schema_version", profileResources = "profile_resources"
        case defaultProfileID = "default_profile_id", pollingMilliseconds = "polling_milliseconds"
        case drainEvents = "drain_events", comparison, maxImportBytes = "max_import_bytes", maxTestcases = "max_testcases", binaryPreviewBytes = "binary_preview_bytes"
        case maxCheckerPreferenceBytes = "max_checker_preference_bytes"
    }
    static func load() throws -> Self {
        guard let url = Bundle.module.url(forResource: "running", withExtension: "json") else { throw CocoaError(.fileNoSuchFile) }
        let configuration = try JSONDecoder().decode(Self.self, from: Data(contentsOf: url))
        guard configuration.schemaVersion == 1, configuration.maxImportBytes > 0,
              configuration.maxTestcases > 0, (1...4096).contains(configuration.binaryPreviewBytes),
              (1...65536).contains(configuration.maxCheckerPreferenceBytes) else { throw WireError.unsupportedSchema }
        return configuration
    }
}

@MainActor @Observable final class RunController {
    @ObservationIgnored private let persistence: PersistenceGate
    var persistentChangesAllowed: Bool { persistence.canMutate }
    private var storedProfiles: [ToolchainProfile] = []
    private var storedProfileID = RawIdentifier("")
    private var storedComparison: ComparisonPolicy = .trailing
    private var storedCheckerEnabled = false
    var profiles: [ToolchainProfile] {
        get { storedProfiles }
        set { guard persistence.canMutate else { errorMessage = L10n.string("workspace_revision_frozen"); return }; storedProfiles = newValue }
    }
    var selectedProfileID: RawIdentifier {
        get { storedProfileID }
        set { guard persistence.canMutate else { errorMessage = L10n.string("workspace_revision_frozen"); return }; storedProfileID = newValue }
    }
    var comparison: ComparisonPolicy {
        get { storedComparison }
        set { guard persistence.canMutate else { errorMessage = L10n.string("workspace_revision_frozen"); return }; storedComparison = newValue }
    }
    var externalCheckerEnabled: Bool {
        get { storedCheckerEnabled }
        set { guard persistence.canMutate else { errorMessage = L10n.string("workspace_revision_frozen"); return }; storedCheckerEnabled = newValue }
    }
    private(set) var externalChecker: CheckerConfiguration?
    var checkerPreferencesAreOpaque = false
    var checkerPreferenceStorageAvailable = true
    private(set) var binaryPreviewBytes = 0
    private(set) var isRunning = false
    private(set) var isClosed = false
    private(set) var pendingImportCount = 0
    private(set) var state: JobState?
    private(set) var result: BatchResult?
    private(set) var submitted: SourceSnapshot?
    var errorMessage: String?
    fileprivate final class RunIdentity: Sendable {}
    @MainActor @Observable fileprivate final class DocumentRun {
        let identity: RunIdentity
        var state: JobState
        var result: BatchResult?
        init(identity: RunIdentity, state: JobState) {
            self.identity = identity
            self.state = state
        }
    }
    private var documentRuns: [RawIdentifier: DocumentRun] = [:]
    @ObservationIgnored private var configuration: RunConfiguration?
    @ObservationIgnored private var coordinator: CoreCoordinator?
    @ObservationIgnored private var task: Task<Void, Never>?

    var selectedProfile: ToolchainProfile? { profiles.first { $0.id == selectedProfileID } }
    var retainedDocumentRunCount: Int { documentRuns.count }
    func result(for documentID: RawIdentifier) -> BatchResult? { documentRuns[documentID]?.result }
    func state(for documentID: RawIdentifier) -> JobState? { documentRuns[documentID]?.state }

    init(persistence: PersistenceGate? = nil) {
        self.persistence = persistence ?? PersistenceGate()
        do {
            let config = try RunConfiguration.load()
            configuration = config
            comparison = config.comparison
            binaryPreviewBytes = config.binaryPreviewBytes
            for resource in config.profileResources {
                guard let url = Bundle.module.url(forResource: resource, withExtension: "json") else { throw CocoaError(.fileNoSuchFile) }
                profiles.append(try JSONDecoder().decode(ToolchainProfile.self, from: Data(contentsOf: url)))
            }
            selectedProfileID = RawIdentifier(config.defaultProfileID)
            guard selectedProfile != nil else { throw WireError.invalidProfile }
            coordinator = CoreCoordinator(pollingMilliseconds: config.pollingMilliseconds, drainEvents: config.drainEvents)
        } catch { errorMessage = L10n.describe(error) }
    }

    func runAll(_ session: NativeEditorSession) {
        guard !isRunning, !isClosed, let coordinator, let profile = selectedProfile else { return }
        do {
            guard try CompanionProvenance.ordinaryStdioAllowed(documentID: session.document.id, preferences: session.preservedPreferences) else {
                errorMessage = L10n.string("companion_run_incompatible"); return
            }
        } catch { errorMessage = L10n.string("companion_run_opaque"); return }
        session.capture()
        guard !session.isComposing else { errorMessage = L10n.string("finish_composition"); return }
        if let failure = session.captureFailure { errorMessage = L10n.describe(failure); return }
        guard profile.languageID.utf8.elementsEqual(session.document.languageID.utf8) else { errorMessage = L10n.string("toolchain_language_mismatch"); return }
        guard session.testCases.contains(where: \.enabled) else { errorMessage = L10n.string("enable_testcase"); return }
        guard !externalCheckerEnabled || externalChecker != nil else { errorMessage = L10n.string("checker_configuration_required"); return }
        let snapshot = session.document.snapshot()
        if persistence.canMutate && session.canMutatePersistence { session.toolchainProfileID = profile.id }
        let request = BatchRequest(snapshot: snapshot, originalPath: session.document.url?.path,
                                   toolchain: profile, tests: session.testCases, comparison: comparison,
                                   externalChecker: externalCheckerEnabled ? externalChecker : nil)
        submitted = snapshot
        result = nil
        state = .queued
        let identity = RunIdentity()
        documentRuns[snapshot.documentID] = DocumentRun(identity: identity, state: .queued)
        isRunning = true
        task = Task { @MainActor [weak self] in
            do {
                let value = try await coordinator.run(request) { [weak self] event in
                    await self?.receive(event, documentID: snapshot.documentID, identity: identity)
                }
                guard value.documentID == snapshot.documentID, value.revision == snapshot.revision else { throw WireError.malformedTerminal }
                if let self, let run = self.documentRuns[snapshot.documentID], run.identity === identity {
                    self.result = value
                    run.result = value
                }
            } catch {
                self?.state = .failed
                if let self, let run = self.documentRuns[snapshot.documentID], run.identity === identity {
                    run.state = .failed
                    if !self.isClosed { self.errorMessage = L10n.describe(error) }
                }
            }
            self?.isRunning = false
            self?.task = nil
        }
    }

    private func receive(_ event: CoreEvent, documentID: RawIdentifier, identity: RunIdentity) {
        state = event.state
        guard let run = documentRuns[documentID], run.identity === identity else { return }
        run.state = event.state
    }

    func cancel() {
        guard isRunning, let coordinator else { return }
        Task { @MainActor [weak self] in
            do { try await coordinator.cancel() }
            catch { if self?.isClosed == false { self?.errorMessage = L10n.describe(error) } }
        }
    }

    func cancel(documentID: RawIdentifier) { if submitted?.documentID == documentID { cancel() } }

    /// 仅在文档真正销毁时释放结果；运行中的晚事件必须仍匹配当前记录身份才可写回。
    func forget(documentID: RawIdentifier) {
        cancel(documentID: documentID)
        documentRuns.removeValue(forKey: documentID)
        if result?.documentID == documentID { result = nil }
    }

    func shutdown() async {
        guard !isClosed else { if let task { await task.value }; return }
        isClosed = true
        await coordinator?.shutdown()
        if let task { await task.value }
        documentRuns.removeAll()
        result = nil
        submitted = nil
        state = nil
    }

    func waitForCurrentRun() async { if let task { await task.value } }

    func applyChecker(_ jsonText: String, enabled: Bool = true) throws {
        guard persistence.canMutate else { throw DocumentError.revisionExhausted }
        guard !checkerPreferencesAreOpaque else { throw WireError.unsupportedSchema }
        guard checkerPreferenceStorageAvailable else { throw CheckerConfigurationError("checker_error_preference_capacity") }
        let candidate = try validateChecker(jsonText)
        externalChecker = candidate; externalCheckerEnabled = enabled
    }

    func validateChecker(_ jsonText: String) throws -> CheckerConfiguration {
        guard let configuration else { throw WireError.unsupportedSchema }
        let candidate = try CheckerConfiguration(jsonText: jsonText)
        // false 的 JSON 表示更长，确保开关的两种状态均能保存在同一偏好上限内。
        let preference = CheckerNativePreferences(enabled: false, configurationJSON: candidate.originalJSON)
        guard try JSONEncoder().encode(preference).count <= configuration.maxCheckerPreferenceBytes else {
            throw CheckerConfigurationError("checker_error_preference_size")
        }
        return candidate
    }

    func restoreChecker(_ configuration: CheckerConfiguration?, enabled: Bool, opaque: Bool) {
        guard persistence.canMutate else { errorMessage = L10n.string("workspace_revision_frozen"); return }
        externalChecker = configuration; externalCheckerEnabled = enabled; checkerPreferencesAreOpaque = opaque
    }

    func checkerDraft() throws -> String {
        if let externalChecker { return externalChecker.originalJSON }
        guard let url = Bundle.module.url(forResource: "checker", withExtension: "json") else { throw CocoaError(.fileNoSuchFile) }
        return try String(contentsOf: url, encoding: .utf8)
    }

    func applyProfile(_ jsonText: String) throws {
        guard persistence.canMutate else { throw DocumentError.revisionExhausted }
        let profile = try JSONDecoder().decode(ToolchainProfile.self, from: Data(jsonText.utf8))
        if let index = profiles.firstIndex(where: { $0.id == profile.id }) { profiles[index] = profile }
        else { profiles.append(profile) }
        selectedProfileID = profile.id
    }

    func loadProfile() {
        guard !isClosed, pendingImportCount == 0, let config = configuration else { return }
        guard let operation = try? persistence.beginOperation() else { return }
        let panel = NSOpenPanel(); panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else { persistence.endOperation(operation); return }
        pendingImportCount += 1
        Task { @MainActor [weak self] in
            guard let self else { return }
            defer { self.pendingImportCount -= 1; self.persistence.endOperation(operation) }
            do {
                let text = try await Task.detached {
                    let data = try Self.boundedRead(url, limit: config.maxImportBytes)
                    guard let text = String(data: data, encoding: .utf8) else { throw DocumentError.invalidUTF8 }
                    return text
                }.value
                guard !self.isClosed else { return }
                try self.applyProfile(text)
            } catch { if !self.isClosed { self.errorMessage = L10n.describe(error) } }
        }
    }

    func importTests(into session: NativeEditorSession) {
        guard !isClosed, pendingImportCount == 0, let config = configuration else { return }
        guard let operation = try? persistence.beginOperation() else { return }
        let panel = NSOpenPanel(); panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else { persistence.endOperation(operation); return }
        pendingImportCount += 1
        Task { @MainActor [weak self, weak session] in
            guard let self else { return }
            defer { self.pendingImportCount -= 1; self.persistence.endOperation(operation) }
            do {
                let samples = try await Task.detached {
                    let data = try Self.boundedRead(url, limit: config.maxImportBytes)
                    let tests = try JSONDecoder().decode([TestCaseInput].self, from: data)
                    guard tests.count <= config.maxTestcases, Set(tests.map(\.id)).count == tests.count,
                          tests.allSatisfy({ !$0.id.isEmpty && !$0.id.rawValue.contains("\0") }) else { throw CocoaError(.fileReadCorruptFile) }
                    return tests
                }.value
                guard !self.isClosed else { return }
                session?.testCases = samples
            } catch { if !self.isClosed { self.errorMessage = L10n.describe(error) } }
        }
    }

    nonisolated private static func boundedRead(_ url: URL, limit: Int) throws -> Data {
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        let data = try handle.read(upToCount: limit + 1) ?? Data()
        guard data.count <= limit else { throw CocoaError(.fileReadTooLarge) }
        return data
    }
}
