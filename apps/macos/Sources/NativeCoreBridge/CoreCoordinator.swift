import Foundation
import CPNativeCore

public struct CoreFailure: Error, LocalizedError, Sendable {
    public let status: UInt32
    public let message: String
    public var errorDescription: String? { message }
}

/// 此 actor 的 C ABI 调用在后台执行。句柄／返回 buffer 不跨 actor 传递。
public actor CoreCoordinator {
    private var engine: UInt64 = 0
    private var activeJob: UInt64?
    private var closed = false
    private let pollingMilliseconds: UInt64
    private let drainEvents: UInt32
    private let maximumBuffers: UInt32
    private let maximumJobs: UInt32

    public init(pollingMilliseconds: UInt64 = 30, drainEvents: UInt32 = 16,
                maximumBuffers: UInt32 = 0, maximumJobs: UInt32 = 0) {
        self.pollingMilliseconds = pollingMilliseconds
        self.drainEvents = drainEvents
        self.maximumBuffers = maximumBuffers
        self.maximumJobs = maximumJobs
    }

    deinit {
        // 正常窗口生命周期显式 await shutdown；兜底不能在 UI 释放点同步 join。
        let handle = engine
        if handle != 0 { Task.detached { _ = cpn_engine_destroy(handle, nil) } }
    }

    private func errorStorage() -> cpn_error {
        var storage = cpn_error()
        storage.struct_size = UInt32(MemoryLayout<cpn_error>.size)
        return storage
    }

    private func check(_ status: cpn_status, _ storage: inout cpn_error) throws {
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
        if maximumJobs == 1 { options.max_workers = 1 }
        var error = errorStorage()
        try check(cpn_engine_create(&options, &engine, &error), &error)
    }

    private func events() throws -> [CoreEvent] {
        var buffer = cpn_buffer()
        var error = errorStorage()
        try check(cpn_engine_drain_json(engine, drainEvents, &buffer, &error), &error)
        defer { _ = cpn_buffer_release(buffer.handle, nil) }
        let data: Data
        if let pointer = buffer.data { data = Data(bytes: pointer, count: buffer.size) }
        else { data = Data() }
        let batch = try JSONDecoder().decode(EventBatch.self, from: data)
        guard batch.schemaVersion == 1 else { throw WireError.unsupportedSchema }
        return batch.events
    }

    public func run(_ request: BatchRequest,
                    onEvent: @Sendable (CoreEvent) async -> Void = { _ in }) async throws -> BatchResult {
        guard activeJob == nil else { throw WireError.busy }
        try ensureEngine()
        var error = errorStorage()
        var job: UInt64 = 0
        let data = try JSONEncoder().encode(request)
        let status = data.withUnsafeBytes { bytes in
            cpn_engine_submit_json(engine, cpn_bytes(data: bytes.bindMemory(to: UInt8.self).baseAddress,
                                                    size: bytes.count), &job, &error)
        }
        try check(status, &error)
        activeJob = job
        do {
            while true {
                try Task.checkCancellation()
                guard !closed, engine != 0 else { throw WireError.closed }
                for event in try events() {
                    guard event.jobID == job else { throw WireError.malformedTerminal }
                    await onEvent(event)
                    if event.state.isTerminal {
                        guard let result = event.result, result.schemaVersion == 1 else { throw WireError.malformedTerminal }
                        var releaseError = errorStorage()
                        try check(cpn_engine_release_job(engine, job, &releaseError), &releaseError)
                        activeJob = nil
                        return result
                    }
                }
                // ABI 1 没有阻塞等待接口；可取消的 poll 周期仅用于后台进度读取。
                try await Task.sleep(for: .milliseconds(pollingMilliseconds))
            }
        } catch {
            // 已确认事件可能消费失败时，销毁整个引擎保证任务和 native buffers 有界。
            if engine != 0 { _ = cpn_engine_destroy(engine, nil); engine = 0 }
            activeJob = nil
            throw error
        }
    }

    public func cancel() throws {
        guard let job = activeJob, engine != 0 else { return }
        var error = errorStorage()
        try check(cpn_engine_cancel(engine, job, &error), &error)
    }

    public func shutdown() {
        closed = true
        let handle = engine
        engine = 0
        activeJob = nil
        if handle != 0 { _ = cpn_engine_destroy(handle, nil) }
    }
}
