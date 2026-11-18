import AppKit
import Observation
import NativeCoreBridge
import NativeEditorModel

struct RunnerConfigurationBusy: LocalizedError {
    var errorDescription: String? { L10n.string("runner_configuration_busy") }
}

struct ToolchainPreferenceCapacityError: LocalizedError {
    var errorDescription: String? { L10n.string("toolchain_preference_capacity") }
}

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
    @ObservationIgnored private(set) var bundledProfiles: [ToolchainProfile] = []
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
    var configurationDraft: ConfigurationJSONDraft?
    var errorMessage: String?
    @MainActor @Observable fileprivate final class DocumentRun {
        let cancellation = CoreCoordinator.RunCancellation()
        let originalPath: RawIdentifier?
        let toolchain: ToolchainProfile
        let tests: [TestCaseInput]
        let comparison: ComparisonPolicy
        let checker: CheckerConfiguration?
        var state: JobState
        var result: BatchResult?
        init(originalPath: String?, toolchain: ToolchainProfile, tests: [TestCaseInput],
             comparison: ComparisonPolicy, checker: CheckerConfiguration?) {
            self.originalPath = originalPath.map(RawIdentifier.init(_:))
            self.toolchain = toolchain
            self.tests = tests
            self.comparison = comparison
            self.checker = checker
            self.state = .queued
        }
    }
    private var documentRuns: [RawIdentifier: DocumentRun] = [:]
    @ObservationIgnored private var configuration: RunConfiguration?
    @ObservationIgnored private var coordinator: CoreCoordinator?
    @ObservationIgnored private var task: Task<Void, Never>?
    @ObservationIgnored private var activeCancellation: CoreCoordinator.RunCancellation?

    var selectedProfile: ToolchainProfile? { profiles.first { $0.id == selectedProfileID } }
    var canConfigureChecker: Bool { !checkerPreferencesAreOpaque && checkerPreferenceStorageAvailable }
    var retainedDocumentRunCount: Int { documentRuns.count }
    func result(for documentID: RawIdentifier) -> BatchResult? { documentRuns[documentID]?.result }
    func state(for documentID: RawIdentifier) -> JobState? { documentRuns[documentID]?.state }

    private var effectiveChecker: CheckerConfiguration? { externalCheckerEnabled ? externalChecker : nil }

    private func toolchain(for session: NativeEditorSession) -> ToolchainProfile? {
        if let id = session.toolchainProfileID, let recorded = profiles.first(where: { $0.id == id }) {
            return recorded
        }
        return selectedProfile
    }

    func resultIsCurrent(for session: NativeEditorSession) -> Bool {
        guard let run = documentRuns[session.document.id], let result = run.result,
              result.revision == session.document.revision, result.sourceDigest == session.document.sourceDigest,
              !session.isComposing, session.captureFailure == nil, !externalCheckerEnabled || externalChecker != nil,
              run.originalPath == session.document.url.map({ RawIdentifier($0.path) }) else { return false }
        // A background tab retains its own toolchain when another tab is selected.
        return run.toolchain == toolchain(for: session) && run.tests == session.testCases && run.comparison == comparison &&
            run.checker.map { RawIdentifier($0.originalJSON) } == effectiveChecker.map { RawIdentifier($0.originalJSON) }
    }

    func useOutputAsExpected(_ displayedOutput: WireBytes, for session: NativeEditorSession, testcaseID: RawIdentifier) {
        guard !isRunning, session.canMutatePersistence else { return }
        session.capture()
        guard resultIsCurrent(for: session),
              let index = session.testCases.firstIndex(where: { $0.id == testcaseID }),
              let output = result(for: session.document.id)?.tests.first(where: { RawIdentifier($0.testcaseID) == testcaseID })?.process.completeTextOutput,
              displayedOutput == output
        else { return }
        session.testCases[index].expected = output
    }

    func canRun(_ session: NativeEditorSession?) -> Bool {
        guard let session else { return false }
        return !isRunning && !isClosed && toolchain(for: session) != nil && session.companionRunNotice == nil
    }

    func beginCheckerEdit(enableOnApply: Bool = false) {
        guard configurationDraft == nil, canConfigureChecker else { return }
        do {
            configurationDraft = ConfigurationJSONDraft(kind: .checker, jsonText: try checkerDraft(),
                                                       enableCheckerOnApply: enableOnApply)
        }
        catch { errorMessage = L10n.describe(error) }
    }

    func beginProfileEdit() {
        guard configurationDraft == nil else { return }
        do {
            guard let profile = selectedProfile else { throw WireError.invalidProfile }
            configurationDraft = ConfigurationJSONDraft(kind: .toolchain, jsonText: try profile.jsonText())
        } catch { errorMessage = L10n.describe(error) }
    }

    init(persistence: PersistenceGate? = nil) {
        self.persistence = persistence ?? PersistenceGate()
        do {
            let config = try RunConfiguration.load()
            configuration = config
            comparison = config.comparison
            binaryPreviewBytes = config.binaryPreviewBytes
            bundledProfiles = try config.profileResources.map { resource in
                guard let url = Bundle.module.url(forResource: resource, withExtension: "json") else { throw CocoaError(.fileNoSuchFile) }
                return try ToolchainProfile(validating: Data(contentsOf: url))
            }
            profiles = bundledProfiles
            selectedProfileID = RawIdentifier(config.defaultProfileID)
            guard selectedProfile != nil else { throw WireError.invalidProfile }
            coordinator = CoreCoordinator(pollingMilliseconds: config.pollingMilliseconds, drainEvents: config.drainEvents)
        } catch { errorMessage = L10n.describe(error) }
    }

    func runAll(_ session: NativeEditorSession) {
        guard !isRunning, !isClosed, let coordinator, let profile = toolchain(for: session) else { return }
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
        let run = DocumentRun(originalPath: session.document.url?.path, toolchain: profile,
                              tests: session.testCases, comparison: comparison, checker: effectiveChecker)
        let request = BatchRequest(snapshot: snapshot, originalPath: run.originalPath?.rawValue,
                                   toolchain: run.toolchain, tests: run.tests, comparison: run.comparison,
                                   externalChecker: run.checker)
        submitted = snapshot
        result = nil
        state = .queued
        let cancellation = run.cancellation
        activeCancellation = cancellation
        documentRuns[snapshot.documentID] = run
        isRunning = true
        task = Task { @MainActor [weak self] in
            do {
                let value = try await coordinator.run(request, cancellation: cancellation) { [weak self] event in
                    await self?.receive(event, documentID: snapshot.documentID, cancellation: cancellation)
                }
                guard value.documentID == snapshot.documentID, value.revision == snapshot.revision else { throw WireError.malformedTerminal }
                if let self, let run = self.documentRuns[snapshot.documentID], run.cancellation === cancellation {
                    self.result = value
                    run.result = value
                }
            } catch {
                self?.state = .failed
                if let self, let run = self.documentRuns[snapshot.documentID], run.cancellation === cancellation {
                    run.state = .failed
                    if !self.isClosed { self.errorMessage = L10n.describe(error) }
                }
            }
            self?.activeCancellation = nil
            self?.isRunning = false
            self?.task = nil
        }
    }

    private func receive(_ event: CoreEvent, documentID: RawIdentifier, cancellation: CoreCoordinator.RunCancellation) {
        state = event.state
        guard let run = documentRuns[documentID], run.cancellation === cancellation else { return }
        run.state = event.state
    }

    func cancel() {
        guard let cancellation = activeCancellation, let coordinator else { return }
        cancellation.request()
        Task { @MainActor [weak self] in
            do { try await coordinator.cancel(cancellation) }
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

    func applyProfile(_ profile: ToolchainProfile, validatePreferences: ([ToolchainProfile], RawIdentifier) throws -> Void) throws {
        guard persistence.canMutate else { throw DocumentError.revisionExhausted }
        guard profile.id.rawValue.utf8.count <= SessionLimits.maximumIdentifierBytes else { throw WireError.invalidProfile }
        try profile.validateForExecution()
        var candidate = profiles
        if let index = candidate.firstIndex(where: { $0.id == profile.id }) { candidate[index] = profile }
        else { candidate.append(profile) }
        try validatePreferences(candidate, profile.id)
        profiles = candidate
        selectedProfileID = profile.id
    }

    func recoverToolchains(_ saved: [ToolchainProfile], selected: RawIdentifier) throws
        -> (profiles: [ToolchainProfile], selected: RawIdentifier, repaired: Bool) {
        guard !saved.isEmpty, Set(saved.map(\.id)).count == saved.count,
              saved.contains(where: { $0.id == selected }) else { throw WireError.invalidProfile }
        var repaired = false
        var recovered = saved.compactMap { profile in
            do {
                guard profile.id.rawValue.utf8.count <= SessionLimits.maximumIdentifierBytes else { throw WireError.invalidProfile }
                try profile.validateForExecution()
                return profile
            } catch {
                repaired = true
                return bundledProfiles.first { $0.id == profile.id }
            }
        }
        if recovered.isEmpty { recovered = bundledProfiles }
        guard let first = recovered.first else { throw WireError.invalidProfile }
        let fallbackID = RawIdentifier(configuration?.defaultProfileID ?? "")
        let preferred = recovered.first { $0.id == selected } ?? recovered.first { $0.id == fallbackID } ?? first
        return (recovered, preferred.id, repaired)
    }

    func loadProfile(apply: @escaping (ToolchainProfile) throws -> Void, onError: ((String) -> Void)? = nil) {
        guard !isRunning, !isClosed, pendingImportCount == 0, let config = configuration else { return }
        guard let operation = try? persistence.beginOperation() else { return }
        let panel = NSOpenPanel(); panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else { persistence.endOperation(operation); return }
        pendingImportCount += 1
        Task { @MainActor [weak self] in
            guard let self else { return }
            defer { self.pendingImportCount -= 1; self.persistence.endOperation(operation) }
            do {
                let profile = try await Task.detached {
                    let data = try Self.boundedRead(url, limit: config.maxImportBytes)
                    return try ToolchainProfile(validating: data)
                }.value
                guard !self.isClosed else { return }
                try apply(profile)
            } catch {
                guard !self.isClosed else { return }
                let message = L10n.describe(error)
                if let onError { onError(message) } else { self.errorMessage = message }
            }
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
