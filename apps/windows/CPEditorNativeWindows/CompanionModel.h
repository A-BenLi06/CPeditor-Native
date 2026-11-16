#pragma once
#include "CompanionWorker.h"
#include "RecoveryModel.h"
#include "cpn/companion_inbox.hpp"
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cpn::windows {
struct CompanionConfiguration {
    CompanionWorker::Options worker;
    std::string storage_namespace, inbox_suffix, backup_suffix, lock_suffix, source_template;
    std::size_t maximum_import_bytes{}, page_items{}, page_bytes{}, preview_bytes{}, preview_samples{};
    std::chrono::milliseconds lock_timeout{};
    nlohmann::json listener_options;
    std::map<std::string, std::string> status_labels, issue_labels;
    [[nodiscard]] static CompanionConfiguration parse(std::string_view);
    [[nodiscard]] cpn::CompanionInboxPaths paths(std::filesystem::path const& recovery) const;
};
struct CompanionBinding { std::string workspace_id; cpn::CompanionInboxPaths paths; };
struct CompanionPage {
    cpn::CompanionInboxLoadStatus status{};
    std::string workspace_id, detail;
    std::optional<std::string> inbox_id, disk_digest;
    std::optional<std::uint64_t> revision;
    std::uint64_t next_after_sequence{};
    bool has_more{};
    std::vector<cpn::CompanionInboxItem> items;
};
struct CompanionWriteResponse {
    std::string status, detail;
    std::optional<std::string> disk_digest;
    bool inbox_replaced{}, backup_replaced{};
};
struct CompanionPreparation {
    cpn::CompanionInboxLoadStatus inbox_status{};
    std::string status, detail;
    std::optional<cpn::WorkspaceSession> session;
    std::optional<cpn::ImportApplicationReceipt> receipt;
};
struct CompanionListenerResponse {
    std::string status, detail, failure_detail;
    bool has_listener{}, engine_closed{}, running{}, closed{}, failed{}, ipv6{};
    std::uint16_t port{};
};
struct CompanionDelta {
    cpn::SessionDocument document;
    DocumentPreferences preferences;
    cpn::NativeEditorPreferences receipt_preference;
    cpn::ImportApplicationReceipt receipt;
};
enum class CompanionRunCapability { ordinary, unsupported, unavailable };
struct CompanionModel {
    [[nodiscard]] static bool can_create_storage_binding(std::uint64_t) noexcept;
    [[nodiscard]] static nlohmann::json context(std::string const&, cpn::CompanionInboxPaths const&);
    [[nodiscard]] static std::string page_request(std::string const&, cpn::CompanionInboxPaths const&, std::uint64_t, CompanionConfiguration const&);
    [[nodiscard]] static std::string accept_request(std::string const&, cpn::CompanionInboxPaths const&, std::string const&, CompanionConfiguration const&);
    [[nodiscard]] static std::string prepare_request(std::string const&, cpn::CompanionInboxPaths const&, std::string const&, cpn::WorkspaceSession const&, cpn::ImportDocumentDefaults const&);
    [[nodiscard]] static std::string mark_request(std::string const&, cpn::CompanionInboxPaths const&, std::string const&, cpn::SessionPaths const&, CompanionConfiguration const&);
    [[nodiscard]] static std::string listener_start_request(std::string const&, cpn::CompanionInboxPaths const&, CompanionConfiguration const&);
    [[nodiscard]] static std::string version_request();
    [[nodiscard]] static CompanionPage page_response(std::string_view);
    [[nodiscard]] static CompanionWriteResponse write_response(std::string_view);
    [[nodiscard]] static CompanionPreparation prepare_response(std::string_view);
    [[nodiscard]] static CompanionListenerResponse listener_response(std::string_view);
    [[nodiscard]] static std::optional<CompanionBinding> binding(std::vector<cpn::NativeEditorPreferences> const&, std::string const& namespace_id);
    [[nodiscard]] static cpn::NativeEditorPreferences binding_preference(CompanionBinding const&, std::string const&);
    [[nodiscard]] static bool same_binding(CompanionBinding const&, CompanionBinding const&) noexcept;
    static void validate_paths(cpn::CompanionInboxPaths const&, std::vector<std::filesystem::path> const& reserved);
    [[nodiscard]] static CompanionRunCapability run_capability(cpn::SessionDocument const&) noexcept;
    [[nodiscard]] static bool same_receipt(cpn::ImportApplicationReceipt const&, cpn::ImportApplicationReceipt const&) noexcept;
    [[nodiscard]] static CompanionDelta delta(cpn::WorkspaceSession const& base, CompanionPreparation const&, std::string const& inbox_id,
        std::string const& import_id, RecoveryConfiguration const&, RunnerConfiguration const&, std::vector<std::string> const&, std::string const&);
    [[nodiscard]] static cpn::WorkspaceSession merge(cpn::WorkspaceSession const& current, cpn::WorkspaceSession const& base, CompanionDelta const&);
    [[nodiscard]] static std::string preview(std::string_view bytes, std::size_t maximum);
};
struct CompanionPagingState {
    std::optional<std::string> inbox_id, disk_digest;
    std::optional<std::uint64_t> revision;
    std::uint64_t after{};
    bool has_more{};
    std::vector<cpn::CompanionInboxItem> items;
    // false asks the caller to restart at zero, preserving the previous visible page until then.
    [[nodiscard]] bool install(CompanionPage, std::string const& workspace, bool reset);
};
}
