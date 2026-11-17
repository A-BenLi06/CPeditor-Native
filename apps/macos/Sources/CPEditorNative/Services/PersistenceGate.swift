import Foundation
import Observation
import NativeEditorModel

/// 最后一次恢复提交必须独占所有会改变持久记录的异步操作；计数从不饱和复用。
@MainActor @Observable final class PersistenceGate {
    enum Mode { case mutable, finalizing, frozen }
    private(set) var mode: Mode = .mutable
    private(set) var operationCount = 0
    @ObservationIgnored private var operations: Set<UInt64> = []
    @ObservationIgnored private var nextOperation: UInt64 = 0
    @ObservationIgnored var onIdle: (() -> Void)?
    @ObservationIgnored var onModeChange: (() -> Void)?
    var canMutate: Bool { mode == .mutable }
    var isFinalizing: Bool { mode == .finalizing }
    var isFrozen: Bool { mode == .frozen }

    func beginOperation(readOnlyExport: Bool = false) throws -> UInt64 {
        guard canMutate || (readOnlyExport && isFrozen) else { throw DocumentError.revisionExhausted }
        guard nextOperation < UInt64.max else { throw DocumentError.revisionExhausted }
        nextOperation += 1
        operations.insert(nextOperation); operationCount = operations.count
        return nextOperation
    }
    func endOperation(_ token: UInt64) {
        guard operations.remove(token) != nil else { return }
        operationCount = operations.count
        if operations.isEmpty { onIdle?() }
    }
    func tryBeginFinal() -> Bool {
        guard canMutate, operations.isEmpty else { return false }
        mode = .finalizing; onModeChange?(); return true
    }
    func tryBeginFinal(replacing token: UInt64) -> Bool {
        guard canMutate, operations == Set([token]) else { return false }
        operations.remove(token); operationCount = 0
        mode = .finalizing; onModeChange?(); return true
    }
    func finishFinal(replaced: Bool) {
        guard isFinalizing else { return }
        mode = replaced ? .frozen : .mutable; onModeChange?()
    }
    func freezeRestored() { mode = .frozen; onModeChange?() }
}
