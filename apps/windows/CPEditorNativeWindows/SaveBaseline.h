#pragma once
#include "cpn/digest.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cpn::windows {
// The written bytes remain authoritative when an asynchronous save cannot move the native savepoint.
class SaveBaseline final {
public:
    void record_written(std::string utf8) noexcept {
        written_ = std::move(utf8);
        native_matches_disk_ = false;
        restored_digest_.reset(); unwritten_ = false;
        checked_.reset();
    }
    void restore_digest(std::string digest) noexcept {
        written_.clear(); restored_digest_ = std::move(digest); unwritten_ = false;
        native_matches_disk_ = false; checked_.reset();
    }
    void mark_unwritten() noexcept {
        written_.clear(); restored_digest_.reset(); unwritten_ = true;
        native_matches_disk_ = false; checked_.reset();
    }
    void match_native_savepoint() noexcept { native_matches_disk_ = true; checked_.reset(); }
    [[nodiscard]] bool native_matches_disk() const noexcept { return native_matches_disk_; }
    [[nodiscard]] bool never_written() const noexcept { return unwritten_; }
    [[nodiscard]] std::optional<bool> cached_dirty(std::uint64_t revision) const noexcept {
        if (checked_ && checked_->first == revision) return checked_->second;
        return {};
    }
    [[nodiscard]] bool compare_committed(std::uint64_t revision, std::string_view utf8) {
        const bool dirty = unwritten_ || (restored_digest_ ? cpn::sha256(utf8) != *restored_digest_ : utf8 != written_);
        checked_ = std::pair{revision, dirty};
        return dirty;
    }
private:
    std::string written_;
    std::optional<std::string> restored_digest_;
    bool unwritten_{};
    bool native_matches_disk_{};
    std::optional<std::pair<std::uint64_t, bool>> checked_;
};
}
