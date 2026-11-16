// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

// No WinRT dependency: this state machine is tested separately from the platform callbacks.
namespace CPNWinUIEdit {
class CommittedTextState final {
public:
    void begin_native_update(std::string baseline) {
        if (!tracking_available()) return;
        if (!transaction_open_) {
            baseline_ = std::move(baseline);
            transaction_open_ = true;
        }
    }
    void begin_composition(std::string baseline) {
        if (active_ == std::numeric_limits<std::size_t>::max()) {
            faulted_ = true;
            throw std::logic_error("Composition tracking is unavailable");
        }
        ++active_;
        // A snapshot fault does not block native input or hide the active composition.
        begin_native_update(std::move(baseline));
    }
    void end_composition() noexcept {
        if (active_ == 0) { faulted_ = true; return; }
        --active_;
        // Keep transaction_open_ until every queued native text notification has drained.
    }
    void text_changed() {
        if (tracking_available() && !transaction_open_) advance_revision();
    }
    // native_idle requires no TSF lock, no native queues and no queue-drain scope.
    void settle(bool native_idle, const std::string& final_text) {
        if (!native_idle || active_ || finalized_ || faulted_ || !transaction_open_) return;
        if (baseline_ != final_text) advance_revision();
        std::string{}.swap(baseline_);
        transaction_open_ = false;
    }
    void invalidate() noexcept { faulted_ = true; }
    void finalize() noexcept { finalized_ = true; }
    [[nodiscard]] bool composing() const noexcept { return active_ != 0; }
    [[nodiscard]] bool transaction_open() const noexcept { return transaction_open_; }
    [[nodiscard]] bool tracking_available() const noexcept { return !finalized_ && !faulted_; }
    [[nodiscard]] bool available(bool native_idle) const noexcept {
        return native_idle && !active_ && !transaction_open_ && !finalized_ && !faulted_;
    }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    template<class Capture, class Notify>
    void publish_availability(bool native_idle, bool& last_available, Capture&& capture, Notify&& notify) noexcept {
        try {
            if (native_idle && tracking_available() && transaction_open_ && !active_)
                settle(true, capture());
            const bool readable = available(native_idle);
            if (readable != last_available) {
                last_available = readable;
                notify(readable);
            }
        } catch (...) {
            invalidate();
            last_available = false;
            // Neither allocation failure nor consumer failure may unwind native input/teardown.
            try { notify(false); } catch (...) { }
        }
    }
private:
    void advance_revision() {
        if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
            faulted_ = true;
            throw std::overflow_error("Committed text revision exhausted");
        }
        ++revision_;
    }
    std::size_t active_{0};
    std::uint64_t revision_{0};
    bool transaction_open_{false};
    bool finalized_{false};
    bool faulted_{false};
    std::string baseline_;
};
}
