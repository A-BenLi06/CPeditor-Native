#pragma once
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
        checked_.reset();
    }
    void match_native_savepoint() noexcept { native_matches_disk_ = true; checked_.reset(); }
    [[nodiscard]] bool native_matches_disk() const noexcept { return native_matches_disk_; }
    [[nodiscard]] std::optional<bool> cached_dirty(std::uint64_t revision) const noexcept {
        if (checked_ && checked_->first == revision) return checked_->second;
        return {};
    }
    [[nodiscard]] bool compare_committed(std::uint64_t revision, std::string_view utf8) noexcept {
        const bool dirty = utf8 != written_;
        checked_ = std::pair{revision, dirty};
        return dirty;
    }
private:
    std::string written_;
    bool native_matches_disk_{};
    std::optional<std::pair<std::uint64_t, bool>> checked_;
};
}
