#pragma once
#include "cpn/files.hpp"
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace cpn::windows {
struct SourceContents {
    std::filesystem::path path;
    std::string utf8;
    std::string disk_digest;
    bool utf8_bom{};
};
struct SourceSaveResult {
    enum class Status { saved, conflict, lock_unavailable, durability_unknown };
    Status status{Status::conflict};
    std::optional<std::string> disk_digest;
    bool replaced{};
    std::string detail;
};
class SourceStore final {
public:
    struct Options {
        std::filesystem::path lock_directory;
        std::size_t maximum_bytes{16 * 1024 * 1024};
        std::chrono::milliseconds lock_timeout{5000};
    };
    explicit SourceStore(Options, std::shared_ptr<cpn::FileStore> = cpn::native_file_store());
    [[nodiscard]] SourceContents read(std::filesystem::path const&) const;
    [[nodiscard]] std::optional<std::string> disk_digest(std::filesystem::path const&) const;
    // A missing expected digest only permits a missing destination. Cooperating writers share the lock.
    [[nodiscard]] SourceSaveResult save(std::filesystem::path const&, std::string_view utf8, bool bom,
        std::optional<std::string> const& expected_disk_digest, std::stop_token = {}) const;
    [[nodiscard]] static std::filesystem::path resolve(std::filesystem::path const&);
private:
    Options options_;
    std::shared_ptr<cpn::FileStore> files_;
};
}
