// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "cpn/companion.hpp"
#include "cpn/session.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace cpn {

inline constexpr std::uint64_t companion_inbox_schema_version = 1;
inline constexpr std::uint64_t companion_import_preference_schema_version = 1;
inline constexpr std::string_view companion_receipts_namespace = "cpeditor.native.companion.receipts";
inline constexpr std::string_view companion_document_namespace = "cpeditor.native.companion.document";

struct CompanionInboxBounds {
    CompanionBounds payload;
    std::size_t max_json_bytes{128 * 1024 * 1024};
    std::size_t max_total_payload_bytes{64 * 1024 * 1024};
    std::size_t max_records{256}; // Includes Applied; no automatic eviction.
    std::size_t max_pending{128};
    std::size_t max_batches{64};
    std::size_t max_total_tests{16384};
    std::size_t max_json_depth{48};
    std::size_t max_json_nodes{262144};
    std::size_t max_id_bytes{1024};
    std::size_t max_path_bytes{32768};
};
struct CompanionInboxPaths {
    std::filesystem::path inbox_file, backup_file, lock_file;
};
enum class CompanionInboxState { pending, applied };
struct CompanionInboxItem {
    std::uint64_t sequence{0};
    CompanionInboxState state{CompanionInboxState::pending};
    std::string import_id, problem_id, document_id;
    std::vector<std::string> test_ids;
    std::optional<std::string> previous_import_id;
    std::string payload_digest;
    CompanionCandidate candidate;
    std::optional<std::string> applied_session_digest;
    std::optional<std::uint64_t> applied_session_revision;
};
struct CompanionInboxSnapshot {
    std::string inbox_id, workspace_id;
    std::uint64_t revision{0}, next_sequence{1};
    std::vector<CompanionInboxItem> items;
};
enum class CompanionInboxLoadStatus { loaded, missing, invalid, unsupported_schema, io_failed };
struct CompanionInboxLoadResult {
    CompanionInboxLoadStatus status{CompanionInboxLoadStatus::io_failed};
    std::optional<CompanionInboxSnapshot> snapshot;
    std::optional<std::string> disk_digest;
    std::string detail;
};
enum class CompanionInboxWriteStatus {
    accepted, duplicate, applied, already_applied, invalid_payload, invalid_existing,
    unsupported_schema, identity_mismatch, batch_conflict, capacity_exceeded,
    receipt_missing, receipt_conflict, session_unavailable, lock_unavailable,
    cancelled, io_failed, durability_unknown, storage_conflict, id_exhausted
};
struct CompanionInboxWriteResult {
    CompanionInboxWriteStatus status{CompanionInboxWriteStatus::io_failed};
    std::optional<CompanionInboxItem> item;
    std::optional<std::string> disk_digest;
    bool inbox_replaced{false}, backup_replaced{false};
    std::string detail;
};

struct ImportApplicationReceipt {
    std::string inbox_id, import_id, problem_id, document_id, payload_digest;
    std::vector<std::string> test_ids;
};
struct CompanionDocumentProvenance {
    ImportApplicationReceipt identity;
    bool compatible_with_ordinary_stdio{false};
};
struct ImportApplicationBounds {
    SessionBounds session;
    CompanionBounds payload;
    std::size_t max_receipts{256};
};
struct ImportDocumentDefaults {
    // Native configuration provides the initial buffer and language/toolchain choices.
    std::string source_text;
    std::optional<std::string> language_id, toolchain_profile_id;
    std::vector<NativeEditorPreferences> document_preferences;
};
enum class ImportPreparationStatus {
    prepared, already_applied, invalid_item, invalid_session, incompatible_preferences,
    capacity_exceeded, identity_conflict, revision_exhausted, receipt_missing
};
struct ImportPreparationResult {
    ImportPreparationStatus status{ImportPreparationStatus::invalid_item};
    std::optional<WorkspaceSession> session;
    std::optional<ImportApplicationReceipt> receipt;
    std::string detail;
};

// Pure transformation. Receipt and document are returned in ONE candidate session.
// Save that candidate atomically before mark_applied(). Existing receipts suppress replay
// even if users subsequently edited/deleted the imported document. No source file I/O.
[[nodiscard]] ImportPreparationResult prepare_import_application(
    const WorkspaceSession&, const CompanionInboxSnapshot&, std::string_view import_id,
    const ImportDocumentDefaults& = {}, ImportApplicationBounds = {});
// Own future preference versions (including v1 + future) are opaque and reject application.
// These readers throw on invalid/unsupported own data; unrelated preferences are preserved.
[[nodiscard]] std::optional<ImportApplicationReceipt> find_import_application_receipt(
    const WorkspaceSession&, std::string_view inbox_id, std::string_view import_id,
    ImportApplicationBounds = {});
[[nodiscard]] std::optional<CompanionDocumentProvenance> read_companion_document_provenance(
    const SessionDocument&, ImportApplicationBounds = {});

[[nodiscard]] CompanionInboxSnapshot parse_companion_inbox(std::string_view, CompanionInboxBounds = {});
[[nodiscard]] std::string serialize_companion_inbox(const CompanionInboxSnapshot&, CompanionInboxBounds = {});
// A bounded canonical internal item object for non-destructive paged clients.
[[nodiscard]] std::string serialize_companion_inbox_item(const CompanionInboxItem&, std::string_view inbox_id,
    CompanionInboxBounds = {});

// Synchronous file I/O: invoke on a background coordinator. Methods are thread-safe;
// destruction requires callers to stop accessing the instance. No HTTP/process/UI work.
// All mutations reread the latest file under the same persistent cooperating lock.
// An existing backup must be a non-regressing prefix of the main history. Rolled-back
// or branched histories are preserved for explicit recovery, never merged by counter tricks.
class CompanionInboxService final {
public:
    using IdFactory = std::function<std::string()>; // Called only for a new durable inbox identity.
    explicit CompanionInboxService(std::string workspace_id,
        std::shared_ptr<FileStore> = native_file_store(), CompanionInboxBounds = {}, IdFactory = {});
    [[nodiscard]] CompanionInboxLoadResult load(const CompanionInboxPaths&) const;
    [[nodiscard]] CompanionInboxWriteResult accept(const CompanionInboxPaths&, std::string_view payload,
        std::chrono::milliseconds lock_timeout = std::chrono::milliseconds{5000}, std::stop_token = {}) const;
    // Verifies a receipt in an actually saved recovery file, under its cooperating lock.
    // A caller-owned receipt alone cannot acknowledge application. Lock order is inbox -> session.
    [[nodiscard]] CompanionInboxWriteResult mark_applied(const CompanionInboxPaths&, const SessionPaths&,
        std::string_view import_id, std::chrono::milliseconds lock_timeout = std::chrono::milliseconds{5000},
        std::stop_token = {}, ImportApplicationBounds = {}) const;
private:
    std::string workspace_id_;
    std::shared_ptr<FileStore> files_;
    CompanionInboxBounds bounds_;
    IdFactory ids_;
    mutable std::mutex mutex_;
};
[[nodiscard]] const char* companion_inbox_load_status_name(CompanionInboxLoadStatus) noexcept;
[[nodiscard]] const char* companion_inbox_write_status_name(CompanionInboxWriteStatus) noexcept;
[[nodiscard]] const char* import_preparation_status_name(ImportPreparationStatus) noexcept;

} // namespace cpn
