import Foundation
import CPNativeCore
import NativeEditorModel

public struct CoreFailure: Error, LocalizedError, Sendable {
    public let status: UInt32
    public let message: String
    public var errorDescription: String? { message }
}

/// 此 actor 的 C ABI 调用在后台执行。句柄／返回 buffer 不跨 actor 传递。
public actor CoreCoordinator {
    public final class RunCancellation: @unchecked Sendable {
        private let lock = NSLock()
        private var requested = false

        public init() {}
        public func request() { lock.withLock { requested = true } }
        fileprivate var isRequested: Bool { lock.withLock { requested } }
    }

    private final class CancellationHandle: @unchecked Sendable {
        private let lock = NSLock()
        private var value: UInt64 = 0
        private var cancellationRequested = false
        @discardableResult func set(_ handle: UInt64) -> Bool {
            lock.lock(); defer { lock.unlock() }; value = handle; return cancellationRequested
        }
        func request() -> UInt64 {
            lock.lock(); defer { lock.unlock() }; cancellationRequested = true; return value
        }
    }
    nonisolated private let cancellationHandle = CancellationHandle()
    private var engine: UInt64 = 0
    private var activeJob: UInt64?
    private var activeCancellation: RunCancellation?
    private var closed = false
    private let pollingMilliseconds: UInt64
    private let drainEvents: UInt32
    private let maximumBuffers: UInt32
    private let maximumJobs: UInt32
    private let maximumRequestBytes: UInt64

    public init(pollingMilliseconds: UInt64 = 30, drainEvents: UInt32 = 16,
                maximumBuffers: UInt32 = 0, maximumJobs: UInt32 = 0, maximumRequestBytes: UInt64 = 0) {
        self.pollingMilliseconds = pollingMilliseconds
        self.drainEvents = drainEvents
        self.maximumBuffers = maximumBuffers
        self.maximumJobs = maximumJobs
        self.maximumRequestBytes = maximumRequestBytes
    }

    deinit {
        // 正常窗口生命周期显式 await shutdown；兜底不能在 UI 释放点同步 join。
        let handle = engine
        if handle != 0 { Task.detached { _ = cpn_engine_destroy(handle, nil) } }
    }

    nonisolated static func errorStorage() -> cpn_error {
        var storage = cpn_error()
        storage.struct_size = UInt32(MemoryLayout<cpn_error>.size)
        return storage
    }

    nonisolated static func check(_ status: cpn_status, _ storage: inout cpn_error) throws {
        guard status != CPN_OK else { return }
        let message = withUnsafeBytes(of: &storage.message) { bytes in
            String(decoding: bytes.prefix { $0 != 0 }, as: UTF8.self)
        }
        throw CoreFailure(status: status.rawValue, message: message)
    }

    private func ensureEngine() throws {
        guard !closed else { throw WireError.closed }
        guard engine == 0 else { return }
        guard cpn_abi_version() == CPN_ABI_VERSION, (1...128).contains(drainEvents),
              (1...1000).contains(pollingMilliseconds) else { throw WireError.unsupportedSchema }
        var options = cpn_engine_options()
        options.struct_size = UInt32(MemoryLayout<cpn_engine_options>.size)
        options.abi_version = CPN_ABI_VERSION
        options.max_buffers = maximumBuffers
        options.max_jobs = maximumJobs
        options.max_request_bytes = maximumRequestBytes
        if maximumJobs == 1 { options.max_workers = 1 }
        var error = Self.errorStorage()
        try Self.check(cpn_engine_create(&options, &engine, &error), &error)
        if cancellationHandle.set(engine) { _ = cpn_engine_close(engine, nil) }
    }

    private func events() throws -> [CoreEvent] {
        var buffer = cpn_buffer()
        var error = Self.errorStorage()
        try Self.check(cpn_engine_drain_json(engine, drainEvents, &buffer, &error), &error)
        defer { _ = cpn_buffer_release(buffer.handle, nil) }
        let data: Data
        if let pointer = buffer.data { data = Data(bytes: pointer, count: buffer.size) }
        else { data = Data() }
        let batch = try JSONDecoder().decode(EventBatch.self, from: data)
        guard batch.schemaVersion == 1 else { throw WireError.unsupportedSchema }
        return batch.events
    }

    public func run(_ request: BatchRequest, cancellation: RunCancellation? = nil,
                    onEvent: @Sendable (CoreEvent) async -> Void = { _ in }) async throws -> BatchResult {
        guard activeJob == nil else { throw WireError.busy }
        try ensureEngine()
        var error = Self.errorStorage()
        var job: UInt64 = 0
        let data = try JSONEncoder().encode(request)
        let status = data.withUnsafeBytes { bytes in
            cpn_engine_submit_json(engine, cpn_bytes(data: bytes.bindMemory(to: UInt8.self).baseAddress,
                                                    size: bytes.count), &job, &error)
        }
        try Self.check(status, &error)
        activeJob = job
        activeCancellation = cancellation
        defer { activeJob = nil; activeCancellation = nil }
        do {
            // A request made while this task was still queued or encoding survives submission.
            if cancellation?.isRequested == true { try cancel() }
            while true {
                try Task.checkCancellation()
                guard !closed, engine != 0 else { throw WireError.closed }
                for event in try events() {
                    guard event.jobID == job else { throw WireError.malformedTerminal }
                    await onEvent(event)
                    if event.state.isTerminal {
                        guard let result = event.result, result.schemaVersion == 1 else { throw WireError.malformedTerminal }
                        var releaseError = Self.errorStorage()
                        try Self.check(cpn_engine_release_job(engine, job, &releaseError), &releaseError)
                        return result
                    }
                }
                // ABI 1 没有阻塞等待接口；可取消的 poll 周期仅用于后台进度读取。
                try await Task.sleep(for: .milliseconds(pollingMilliseconds))
            }
        } catch {
            // 已确认事件可能消费失败时，销毁整个引擎保证任务和 native buffers 有界。
            if engine != 0 { _ = cpn_engine_destroy(engine, nil); engine = 0; cancellationHandle.set(0) }
            throw error
        }
    }

    public func cancel() throws {
        guard let job = activeJob, engine != 0 else { return }
        var error = Self.errorStorage()
        try Self.check(cpn_engine_cancel(engine, job, &error), &error)
    }

    public func cancel(_ cancellation: RunCancellation) throws {
        guard activeCancellation === cancellation else { return }
        try cancel()
    }

    nonisolated private static func copyBuffer(_ buffer: cpn_buffer) -> Data {
        defer { _ = cpn_buffer_release(buffer.handle, nil) }
        let data = buffer.data.map { Data(bytes: $0, count: buffer.size) } ?? Data()
        return data
    }

    public func loadSession(at path: String) throws -> SessionLoadResponse {
        try ensureEngine()
        let data = try Self.sessionBytes(engine: engine, data: Data(path.utf8), saving: false)
        let response = try JSONDecoder().decode(SessionLoadResponse.self, from: data)
        guard response.schemaVersion == 1 else { throw WireError.unsupportedSchema }
        return response
    }

    public func saveSession(_ session: WorkspaceSessionRecord, paths: SessionPaths,
                            expectedDiskDigest: String?, lockTimeoutMS: UInt64) throws -> SessionSaveResponse {
        try ensureEngine()
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.withoutEscapingSlashes]
        let bytes = try encoder.encode(SessionSaveRequest(paths: paths, session: session,
                                                          expectedDiskDigest: expectedDiskDigest, lockTimeoutMS: lockTimeoutMS))
        let data = try Self.sessionBytes(engine: engine, data: bytes, saving: true)
        let response = try JSONDecoder().decode(SessionSaveResponse.self, from: data)
        guard response.schemaVersion == 1 else { throw WireError.unsupportedSchema }
        return response
    }

    nonisolated private static func sessionBytes(engine: UInt64, data: Data, saving: Bool) throws -> Data {
        var buffer = cpn_buffer(); var error = errorStorage()
        let status = data.withUnsafeBytes { bytes in
            let input = cpn_bytes(data: bytes.bindMemory(to: UInt8.self).baseAddress, size: bytes.count)
            return saving ? cpn_engine_session_save_json(engine, input, &buffer, &error)
                : cpn_engine_session_load(engine, input, &buffer, &error)
        }
        if status == CPN_SESSION_CAPACITY { throw SessionCapacityError.exceeded }
        try check(status, &error)
        return copyBuffer(buffer)
    }

    public func companionPage(_ context: CompanionContext, after: UInt64 = 0, maxItems: UInt64, maxPageBytes: UInt64) throws -> CompanionPage {
        let response: CompanionPage = try companion(context, operation: .page(after, maxItems, maxPageBytes))
        guard response.schemaVersion == 1, response.workspaceID == context.workspaceID else { throw WireError.unsupportedSchema }
        return response
    }

    public func companionAccept(_ context: CompanionContext, originalJSON: String, lockTimeoutMS: UInt64) throws -> CompanionWriteResponse {
        let response: CompanionWriteResponse = try companion(context, operation: .accept(originalJSON, lockTimeoutMS))
        guard response.schemaVersion == 1 else { throw WireError.unsupportedSchema }
        return response
    }

    public func companionPrepare(_ context: CompanionContext, importID: RawIdentifier, session: WorkspaceSessionRecord,
                                 defaults: CompanionDefaults) throws -> CompanionPreparation {
        let response: CompanionPreparation = try companion(context, operation: .prepare(importID, session, defaults))
        guard response.schemaVersion == 1 else { throw WireError.unsupportedSchema }
        return response
    }

    public func companionMarkApplied(_ context: CompanionContext, importID: RawIdentifier, sessionPaths: SessionPaths,
                                     lockTimeoutMS: UInt64) throws -> CompanionWriteResponse {
        let response: CompanionWriteResponse = try companion(context, operation: .mark(importID, sessionPaths, lockTimeoutMS))
        guard response.schemaVersion == 1 else { throw WireError.unsupportedSchema }
        return response
    }

    private func companion<Response: Decodable>(_ context: CompanionContext, operation: CompanionRequest.Operation) throws -> Response {
        try ensureEngine()
        let data = try JSONEncoder().encode(CompanionRequest(context: context, operation: operation))
        let bytes = try Self.companionBytes(engine: engine, data: data, operation: operation)
        return try JSONDecoder().decode(Response.self, from: bytes)
    }

    nonisolated private static func companionBytes(engine: UInt64, data: Data, operation: CompanionRequest.Operation) throws -> Data {
        var buffer = cpn_buffer(); var error = errorStorage()
        let status = data.withUnsafeBytes { bytes in
            let input = cpn_bytes(data: bytes.bindMemory(to: UInt8.self).baseAddress, size: bytes.count)
            switch operation {
            case .page: return cpn_engine_companion_page_json(engine, input, &buffer, &error)
            case .accept: return cpn_engine_companion_accept_json(engine, input, &buffer, &error)
            case .prepare: return cpn_engine_companion_prepare_json(engine, input, &buffer, &error)
            case .mark: return cpn_engine_companion_mark_applied_json(engine, input, &buffer, &error)
            }
        }
        try check(status, &error)
        return copyBuffer(buffer)
    }

    /// C ABI 的 close 可并行触发 stop；不能排在阻塞的会话锁等待之后。
    public nonisolated func requestCancellation() {
        let handle = cancellationHandle.request()
        if handle != 0 { Task.detached { _ = cpn_engine_close(handle, nil) } }
    }

    public func shutdown() {
        closed = true
        let handle = engine
        engine = 0
        cancellationHandle.set(0)
        activeJob = nil
        if handle != 0 { _ = cpn_engine_destroy(handle, nil) }
    }
}
