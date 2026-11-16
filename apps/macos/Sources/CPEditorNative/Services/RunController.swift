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
    enum CodingKeys: String, CodingKey {
        case schemaVersion = "schema_version", profileResources = "profile_resources"
        case defaultProfileID = "default_profile_id", pollingMilliseconds = "polling_milliseconds"
        case drainEvents = "drain_events", comparison, maxImportBytes = "max_import_bytes", maxTestcases = "max_testcases"
    }
    static func load() throws -> Self {
        guard let url = Bundle.module.url(forResource: "running", withExtension: "json") else { throw CocoaError(.fileNoSuchFile) }
        let configuration = try JSONDecoder().decode(Self.self, from: Data(contentsOf: url))
        guard configuration.schemaVersion == 1, configuration.maxImportBytes > 0,
              configuration.maxTestcases > 0 else { throw WireError.unsupportedSchema }
        return configuration
    }
}

@MainActor @Observable final class RunController {
    var profiles: [ToolchainProfile] = []
    var selectedProfileID = RawIdentifier("")
    var comparison: ComparisonPolicy = .trailing
    private(set) var isRunning = false
    private(set) var isClosed = false
    private(set) var pendingImportCount = 0
    private(set) var state: JobState?
    private(set) var result: BatchResult?
    private(set) var submitted: SourceSnapshot?
    var errorMessage: String?
    @ObservationIgnored private var configuration: RunConfiguration?
    @ObservationIgnored private var coordinator: CoreCoordinator?
    @ObservationIgnored private var task: Task<Void, Never>?

    var selectedProfile: ToolchainProfile? { profiles.first { $0.id == selectedProfileID } }

    init() {
        do {
            let config = try RunConfiguration.load()
            configuration = config
            comparison = config.comparison
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
        session.capture()
        guard !session.isComposing else { errorMessage = L10n.string("finish_composition"); return }
        if let failure = session.captureFailure { errorMessage = L10n.describe(failure); return }
        guard profile.languageID.utf8.elementsEqual(session.document.languageID.utf8) else { errorMessage = L10n.string("toolchain_language_mismatch"); return }
        guard session.testCases.contains(where: \.enabled) else { errorMessage = L10n.string("enable_testcase"); return }
        let snapshot = session.document.snapshot()
        session.toolchainProfileID = profile.id
        let request = BatchRequest(snapshot: snapshot, originalPath: session.document.url?.path,
                                   toolchain: profile, tests: session.testCases, comparison: comparison)
        submitted = snapshot
        result = nil
        state = .queued
        isRunning = true
        task = Task { @MainActor [weak self] in
            do {
                let value = try await coordinator.run(request) { [weak self] event in
                    await self?.receive(event)
                }
                self?.result = value
            } catch {
                if self?.isClosed == false { self?.errorMessage = L10n.describe(error) }
            }
            self?.isRunning = false
            self?.task = nil
        }
    }

    private func receive(_ event: CoreEvent) { state = event.state }

    func cancel() {
        guard isRunning, let coordinator else { return }
        Task { @MainActor [weak self] in
            do { try await coordinator.cancel() }
            catch { if self?.isClosed == false { self?.errorMessage = L10n.describe(error) } }
        }
    }

    func cancel(documentID: RawIdentifier) { if submitted?.documentID == documentID { cancel() } }

    func shutdown() async {
        guard !isClosed else { if let task { await task.value }; return }
        isClosed = true
        await coordinator?.shutdown()
        if let task { await task.value }
    }

    func waitForCurrentRun() async { if let task { await task.value } }

    func applyProfile(_ jsonText: String) throws {
        let profile = try JSONDecoder().decode(ToolchainProfile.self, from: Data(jsonText.utf8))
        if let index = profiles.firstIndex(where: { $0.id == profile.id }) { profiles[index] = profile }
        else { profiles.append(profile) }
        selectedProfileID = profile.id
    }

    func loadProfile() {
        guard !isClosed, pendingImportCount == 0, let config = configuration else { return }
        let panel = NSOpenPanel(); panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        pendingImportCount += 1
        Task { @MainActor [weak self] in
            guard let self else { return }
            defer { self.pendingImportCount -= 1 }
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
        let panel = NSOpenPanel(); panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        pendingImportCount += 1
        Task { @MainActor [weak self, weak session] in
            guard let self else { return }
            defer { self.pendingImportCount -= 1 }
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
