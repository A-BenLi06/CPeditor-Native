// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "cpn/batch.hpp"
#include "cpn/files.hpp"
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace cpn {
struct SessionBounds {
    std::size_t max_json_bytes{64 * 1024 * 1024};
    std::size_t max_documents{128};
    std::size_t max_tests_per_document{4096};
    std::size_t max_total_tests{16384};
    std::size_t max_source_bytes{16 * 1024 * 1024};
    std::size_t max_test_bytes{4 * 1024 * 1024};
    std::size_t max_total_content_bytes{32 * 1024 * 1024};
    std::size_t max_preferences_per_owner{16};
    std::size_t max_preferences_bytes{64 * 1024};
    std::size_t max_total_preferences_bytes{4 * 1024 * 1024};
    std::size_t max_json_depth{64};
    std::size_t max_preferences_depth{16};
    std::size_t max_id_bytes{1024};
    std::size_t max_path_bytes{32768};
};
struct NativeEditorPreferences {
    std::string namespace_id;
    std::uint64_t schema_version{1};
    // A bounded JSON object, kept byte-for-byte as an opaque string.
    // Core validates syntax and limits; the native owner interprets its namespace/version.
    std::string object_json{"{}"};
};
struct DocumentSavePoint {
    std::uint64_t revision{0};
    std::string source_digest;
    std::optional<std::string> disk_digest;
};
struct SessionDocument {
    std::string document_id;
    std::uint64_t revision{0};
    std::string source_text;
    std::optional<std::filesystem::path> original_path;
    std::optional<std::string> language_id;
    std::optional<std::string> toolchain_profile_id;
    std::optional<DocumentSavePoint> save_point;
    std::vector<TestCase> tests;
    std::vector<NativeEditorPreferences> editor_preferences;
};
struct WorkspaceSession {
    std::string workspace_id;
    std::uint64_t revision{0};
    std::optional<std::string> selected_document_id;
    std::vector<SessionDocument> documents;
    std::vector<NativeEditorPreferences> editor_preferences;
};
struct SessionPaths {
    std::filesystem::path recovery_file;
    std::filesystem::path backup_file;
    std::filesystem::path lock_file;
};
enum class SessionLoadStatus { loaded, missing, invalid, unsupported_schema, io_failed };
struct SessionLoadResult {
    SessionLoadStatus status{SessionLoadStatus::io_failed};
    std::optional<WorkspaceSession> session;
    std::optional<std::string> disk_digest;
    std::string detail;
};
enum class SessionSaveStatus { saved, conflict, invalid_session, invalid_existing, io_failed,
                               durability_unknown, lock_unavailable, cancelled };
struct SessionSaveResult {
    SessionSaveStatus status{SessionSaveStatus::io_failed};
    // Last observed recovery-file digest. Advance the token only when recovery_replaced is true.
    std::optional<std::string> disk_digest;
    bool recovery_replaced{false};
    bool backup_replaced{false};
    std::string detail;
};
[[nodiscard]] WorkspaceSession parse_workspace_session(std::string_view, SessionBounds = {});
[[nodiscard]] std::string serialize_workspace_session(const WorkspaceSession&, SessionBounds = {});

// Recovery data is independent of user files and process job/artifact workspaces.
// load never reads source files. The caller applies a successfully validated candidate atomically.
// save validates existing recovery/backup data before replacing it, preserving damaged candidates.
// Cooperating writers must use the same persistent lock path. External writers bypassing that lock
// can still race the final replace; this is optimistic validation, not an operating-system CAS.
class SessionService final {
public:
    explicit SessionService(std::shared_ptr<FileStore> = native_file_store(), SessionBounds = {});
    [[nodiscard]] SessionLoadResult load(const std::filesystem::path&) const;
    // nullopt requires a missing recovery file; a digest requires an exact existing disk match.
    [[nodiscard]] SessionSaveResult save(const SessionPaths&, const WorkspaceSession&,
                                         std::optional<std::string> expected_disk_digest,
                                         std::chrono::milliseconds lock_timeout = std::chrono::milliseconds{5000},
                                         std::stop_token = {}) const;
private:
    std::shared_ptr<FileStore> files_;
    SessionBounds bounds_;
    mutable std::mutex mutex_;
};
[[nodiscard]] const char* session_load_status_name(SessionLoadStatus) noexcept;
[[nodiscard]] const char* session_save_status_name(SessionSaveStatus) noexcept;
} // namespace cpn
