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
    @ObservationIgnored private var discoveredTemplates: [ToolchainProfile] = []
    @ObservationIgnored private var discoveredCatalog: ToolchainDiscovery.Catalog?
    @ObservationIgnored private var unavailableProfiles: [ToolchainProfile] = []
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
        let mode: BatchMode
        var state: JobState
        var result: BatchResult?
        init(originalPath: String?, toolchain: ToolchainProfile, tests: [TestCaseInput],
             comparison: ComparisonPolicy, checker: CheckerConfiguration?, mode: BatchMode) {
            self.originalPath = originalPath.map(RawIdentifier.init(_:))
            self.toolchain = toolchain
            self.tests = tests
            self.comparison = comparison
            self.checker = checker
            self.mode = mode
            self.state = .queued
        }
    }
    private struct CompiledArtifact {
        let artifactURL: URL
        let directoryURL: URL
        let snapshot: SourceSnapshot
        let originalPath: RawIdentifier?
        let toolchain: ToolchainProfile
    }
    private var documentRuns: [RawIdentifier: DocumentRun] = [:]
    private var compiledArtifacts: [RawIdentifier: CompiledArtifact] = [:]
    nonisolated private let artifactRootURL = FileManager.default.temporaryDirectory
        .appendingPathComponent("cpn-artifacts-\(UUID().uuidString)", isDirectory: true)
    @ObservationIgnored private var configuration: RunConfiguration?
    @ObservationIgnored private var coordinator: CoreCoordinator?
    @ObservationIgnored private var task: Task<Void, Never>?
    @ObservationIgnored private var activeCancellation: CoreCoordinator.RunCancellation?

    deinit {
        try? FileManager.default.removeItem(at: artifactRootURL)
    }

    var selectedProfile: ToolchainProfile? { profiles.first { $0.id == selectedProfileID } }
    var canConfigureChecker: Bool { !checkerPreferencesAreOpaque && checkerPreferenceStorageAvailable }
    var retainedDocumentRunCount: Int { documentRuns.count }
    func result(for documentID: RawIdentifier) -> BatchResult? { documentRuns[documentID]?.result }
    func state(for documentID: RawIdentifier) -> JobState? { documentRuns[documentID]?.state }
    func artifactURL(for documentID: RawIdentifier) -> URL? { compiledArtifacts[documentID]?.artifactURL }

    private var effectiveChecker: CheckerConfiguration? { externalCheckerEnabled ? externalChecker : nil }

    private func toolchain(for session: NativeEditorSession) -> ToolchainProfile? {
        if let id = session.toolchainProfileID, let recorded = profiles.first(where: { $0.id == id }) {
            return recorded
        }
        return selectedProfile
    }

    func supportsCompile(for session: NativeEditorSession?) -> Bool {
        guard let session else { return false }
        return toolchain(for: session)?.buildExecutable != nil
    }

    func canCompile(_ session: NativeEditorSession?) -> Bool {
        guard let session else { return false }
        return !isRunning && !isClosed && supportsCompile(for: session) && toolchainNotice(for: session) == nil
    }

    func hasReusableArtifact(for session: NativeEditorSession?) -> Bool {
        guard let session,
              let profile = toolchain(for: session),
              let artifact = compiledArtifacts[session.document.id] else { return false }
        return artifact.toolchain == profile && FileManager.default.isReadableFile(atPath: artifact.artifactURL.path)
    }

    func isArtifactCurrent(for session: NativeEditorSession?) -> Bool {
        guard let session,
              let artifact = compiledArtifacts[session.document.id],
              hasReusableArtifact(for: session) else { return false }
        return artifact.snapshot.revision == session.document.revision
            && artifact.snapshot.sourceDigest == session.document.sourceDigest
            && artifact.originalPath == session.document.url.map({ RawIdentifier($0.path) })
    }

    func canRunOnly(_ session: NativeEditorSession?) -> Bool {
        canRun(session) && (!supportsCompile(for: session) || hasReusableArtifact(for: session))
    }

    func runOnlyHelpText(for session: NativeEditorSession?) -> String {
        if !hasReusableArtifact(for: session) {
            return L10n.string("help_run_only_needs_compile")
        }
        if !isArtifactCurrent(for: session) {
            return L10n.string("help_run_only_outdated")
        }
        return L10n.string("help_run_only")
    }

    func resultIsCurrent(for session: NativeEditorSession) -> Bool {
        guard let run = documentRuns[session.document.id], let result = run.result,
              result.revision == session.document.revision, result.sourceDigest == session.document.sourceDigest,
              !session.isComposing, session.captureFailure == nil,
              run.originalPath == session.document.url.map({ RawIdentifier($0.path) }),
              run.toolchain == toolchain(for: session) else { return false }
        if run.mode == .compileOnly {
            return true
        }
        guard !externalCheckerEnabled || externalChecker != nil else { return false }
        // A background tab retains its own toolchain when another tab is selected.
        return run.tests == session.testCases && run.comparison == comparison &&
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
        return !isRunning && !isClosed && toolchain(for: session) != nil && toolchainNotice(for: session) == nil && session.companionRunNotice == nil
    }

    func toolchainNotice(for session: NativeEditorSession) -> String? {
        guard let profile = toolchain(for: session), unavailableProfiles.contains(where: {
            $0.id == profile.id && $0.buildExecutable == profile.buildExecutable
        }) else { return nil }
        return L10n.string("gnu_cpp_required")
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

    init(persistence: PersistenceGate? = nil, discoverToolchains: () throws -> ToolchainDiscovery.Catalog = { try ToolchainDiscovery.load().discover() }) {
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
            let discovered = try discoverToolchains()
            discoveredCatalog = discovered
            discoveredTemplates = discovered.templates
            unavailableProfiles = discovered.unavailable
            bundledProfiles += discovered.profiles
            profiles = bundledProfiles
            selectedProfileID = RawIdentifier(config.defaultProfileID)
            guard selectedProfile != nil else { throw WireError.invalidProfile }
            coordinator = CoreCoordinator(pollingMilliseconds: config.pollingMilliseconds, drainEvents: config.drainEvents)
        } catch { errorMessage = L10n.describe(error) }
    }

    func compile(_ session: NativeEditorSession) {
        execute(session, mode: .compileOnly)
    }

    func runOnly(_ session: NativeEditorSession) {
        execute(session, mode: .runOnly)
    }

    func runAll(_ session: NativeEditorSession) {
        execute(session, mode: .compileAndRun)
    }

    private func allocateArtifactStaging(documentID: RawIdentifier, profile: ToolchainProfile) throws -> (artifactURL: URL, directoryURL: URL) {
        let attributes: [FileAttributeKey: Any] = [.posixPermissions: 0o700]
        if !FileManager.default.fileExists(atPath: artifactRootURL.path) {
            try FileManager.default.createDirectory(at: artifactRootURL, withIntermediateDirectories: true, attributes: attributes)
        }
        let directoryURL = artifactRootURL
            .appendingPathComponent(documentID.rawValue, isDirectory: true)
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: directoryURL, withIntermediateDirectories: true, attributes: attributes)
        let name = (profile.artifactName?.isEmpty == false ? profile.artifactName! : "program")
        return (directoryURL.appendingPathComponent(name, isDirectory: false), directoryURL)
    }

    private func removeArtifact(for documentID: RawIdentifier) {
        guard let existing = compiledArtifacts.removeValue(forKey: documentID) else { return }
        try? FileManager.default.removeItem(at: existing.directoryURL)
    }

    private func execute(_ session: NativeEditorSession, mode: BatchMode) {
        guard !isRunning, !isClosed, let coordinator, let profile = toolchain(for: session) else { return }
        if let notice = toolchainNotice(for: session) { errorMessage = notice; return }

        if mode == .compileOnly {
            guard profile.buildExecutable != nil else {
                errorMessage = L10n.string("toolchain_no_compile_step"); return
            }
        } else {
            do {
                guard try CompanionProvenance.ordinaryStdioAllowed(documentID: session.document.id, preferences: session.preservedPreferences) else {
                    errorMessage = L10n.string("companion_run_incompatible"); return
                }
            } catch { errorMessage = L10n.string("companion_run_opaque"); return }
        }

        session.capture()
        guard !session.isComposing else { errorMessage = L10n.string("finish_composition"); return }
        if let failure = session.captureFailure { errorMessage = L10n.describe(failure); return }
        guard profile.languageID.utf8.elementsEqual(session.document.languageID.utf8) else { errorMessage = L10n.string("toolchain_language_mismatch"); return }

        if mode != .compileOnly {
            guard session.testCases.contains(where: \.enabled) else { errorMessage = L10n.string("enable_testcase"); return }
            guard !externalCheckerEnabled || externalChecker != nil else { errorMessage = L10n.string("checker_configuration_required"); return }
        }

        let liveSnapshot = session.document.snapshot()
        let liveOriginalPath = session.document.url?.path
        let requestSnapshot: SourceSnapshot
        let requestOriginalPath: String?
        let requestTests: [TestCaseInput]
        let stagingArtifact: (artifactURL: URL, directoryURL: URL)?
        let requestArtifactPath: String?

        switch mode {
        case .compileOnly:
            do {
                let staging = try allocateArtifactStaging(documentID: liveSnapshot.documentID, profile: profile)
                stagingArtifact = staging
                requestArtifactPath = staging.artifactURL.path
            } catch {
                errorMessage = L10n.describe(error); return
            }
            requestSnapshot = liveSnapshot
            requestOriginalPath = liveOriginalPath
            requestTests = []
        case .compileAndRun:
            if profile.buildExecutable != nil {
                do {
                    let staging = try allocateArtifactStaging(documentID: liveSnapshot.documentID, profile: profile)
                    stagingArtifact = staging
                    requestArtifactPath = staging.artifactURL.path
                } catch {
                    errorMessage = L10n.describe(error); return
                }
            } else {
                stagingArtifact = nil
                requestArtifactPath = nil
            }
            requestSnapshot = liveSnapshot
            requestOriginalPath = liveOriginalPath
            requestTests = session.testCases
        case .runOnly:
            if profile.buildExecutable != nil {
                guard hasReusableArtifact(for: session), let compiled = compiledArtifacts[session.document.id] else {
                    errorMessage = L10n.string("compile_before_run"); return
                }
                requestSnapshot = compiled.snapshot
                requestOriginalPath = compiled.originalPath?.rawValue
                requestArtifactPath = compiled.artifactURL.path
            } else {
                requestSnapshot = liveSnapshot
                requestOriginalPath = liveOriginalPath
                requestArtifactPath = nil
            }
            stagingArtifact = nil
            requestTests = session.testCases
        }

        if persistence.canMutate && session.canMutatePersistence { session.toolchainProfileID = profile.id }
        let run = DocumentRun(originalPath: requestOriginalPath, toolchain: profile,
                              tests: requestTests, comparison: comparison,
                              checker: mode == .compileOnly ? nil : effectiveChecker, mode: mode)
        let request = BatchRequest(snapshot: requestSnapshot, originalPath: run.originalPath?.rawValue,
                                   toolchain: run.toolchain, tests: run.tests, comparison: run.comparison,
                                   externalChecker: run.checker, mode: mode, artifactPath: requestArtifactPath)
        submitted = requestSnapshot
        result = nil
        state = .queued
        let cancellation = run.cancellation
        activeCancellation = cancellation
        documentRuns[requestSnapshot.documentID] = run
        isRunning = true
        task = Task { @MainActor [weak self] in
            do {
                let value = try await coordinator.run(request, cancellation: cancellation) { [weak self] event in
                    await self?.receive(event, documentID: requestSnapshot.documentID, cancellation: cancellation)
                }
                guard value.documentID == requestSnapshot.documentID, value.revision == requestSnapshot.revision else { throw WireError.malformedTerminal }
                if let self, let run = self.documentRuns[requestSnapshot.documentID], run.cancellation === cancellation {
                    self.result = value
                    run.result = value
                    if let staging = stagingArtifact {
                        if value.buildOutcome == "Succeeded" && FileManager.default.fileExists(atPath: staging.artifactURL.path) {
                            let previous = self.compiledArtifacts.updateValue(
                                CompiledArtifact(
                                    artifactURL: staging.artifactURL,
                                    directoryURL: staging.directoryURL,
                                    snapshot: requestSnapshot,
                                    originalPath: run.originalPath,
                                    toolchain: profile
                                ),
                                forKey: requestSnapshot.documentID
                            )
                            if let previous, previous.directoryURL != staging.directoryURL {
                                try? FileManager.default.removeItem(at: previous.directoryURL)
                            }
                        } else {
                            try? FileManager.default.removeItem(at: staging.directoryURL)
                            if value.buildOutcome == "CompilationError" || value.buildOutcome == "Failed" {
                                self.removeArtifact(for: requestSnapshot.documentID)
                            }
                        }
                    }
                } else if let staging = stagingArtifact {
                    try? FileManager.default.removeItem(at: staging.directoryURL)
                }
            } catch {
                if let staging = stagingArtifact {
                    try? FileManager.default.removeItem(at: staging.directoryURL)
                }
                self?.state = .failed
                if let self, let run = self.documentRuns[requestSnapshot.documentID], run.cancellation === cancellation {
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
        removeArtifact(for: documentID)
        if result?.documentID == documentID { result = nil }
    }

    func shutdown() async {
        guard !isClosed else { if let task { await task.value }; return }
        isClosed = true
        await coordinator?.shutdown()
        if let task { await task.value }
        documentRuns.removeAll()
        compiledArtifacts.removeAll()
        try? FileManager.default.removeItem(at: artifactRootURL)
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
                // Resolve a placeholder or removed compiler in a managed install directory.
                // Existing version pins and explicit paths elsewhere remain user-owned.
                if discoveredTemplates.contains(where: { template in
                    template.id == profile.id && (template.buildExecutable == profile.buildExecutable ||
                        profile.buildExecutable.map { discoveredCatalog?.isMissingManagedCompiler($0) == true } == true)
                }),
                   let resolved = bundledProfiles.first(where: { $0.id == profile.id }),
                   let executable = resolved.buildExecutable,
                   var root = profile.payload.object, var build = root[ToolchainProfile.Field.build.rawValue]?.object {
                    build[ToolchainProfile.Field.executable.rawValue] = .string(executable)
                    root[ToolchainProfile.Field.build.rawValue] = .object(build)
                    return try ToolchainProfile(payload: .object(root))
                }
                return profile
            } catch {
                repaired = true
                return bundledProfiles.first { $0.id == profile.id }
            }
        }
        // Newly discovered compilers must also appear in workspaces saved by older app versions.
        for profile in bundledProfiles where !recovered.contains(where: { $0.id == profile.id }) { recovered.append(profile) }
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
