#pragma once
#include "RevisionPolicy.h"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace cpn::windows {
// UI-thread capability gate. Last-revision snapshots cannot overlap persistent async commands.
class PersistenceGate final {
public:
    [[nodiscard]] std::uint64_t begin_operation() {
        if (final_active_) throw std::logic_error("The final revision transaction excludes new persistent commands.");
        if (operations_ == std::numeric_limits<std::size_t>::max()) throw std::overflow_error("Persistent operation count is exhausted.");
        ++operations_; return generation_;
    }
    void end_operation() noexcept { if (operations_) --operations_; }
    [[nodiscard]] bool try_begin_final(bool unrelated_busy) {
        if (final_active_ || operations_ || unrelated_busy) return false;
        generation_ = RevisionPolicy::next(generation_); final_active_ = true; return true;
    }
    void end_final() noexcept { final_active_ = false; }
    [[nodiscard]] bool accepts(std::uint64_t generation) const noexcept { return !final_active_ && generation == generation_; }
    [[nodiscard]] bool operation_active() const noexcept { return operations_ != 0; }
    [[nodiscard]] bool final_active() const noexcept { return final_active_; }
private:
    std::size_t operations_{};
    std::uint64_t generation_{};
    bool final_active_{};
};
// Register ownership before the caller performs any fallible UI work.
class FinalPersistenceLease final {
public:
    explicit FinalPersistenceLease(PersistenceGate& gate) noexcept : gate_(gate) {}
    ~FinalPersistenceLease() noexcept { release(); }
    FinalPersistenceLease(FinalPersistenceLease const&) = delete;
    FinalPersistenceLease& operator=(FinalPersistenceLease const&) = delete;
    [[nodiscard]] bool try_acquire(bool unrelated_busy) {
        if (acquired_ || !gate_.try_begin_final(unrelated_busy)) return false;
        acquired_ = true; return true;
    }
    void release() noexcept { if (acquired_) { gate_.end_final(); acquired_ = false; } }
    [[nodiscard]] bool acquired() const noexcept { return acquired_; }
private:
    PersistenceGate& gate_;
    bool acquired_{};
};
}
