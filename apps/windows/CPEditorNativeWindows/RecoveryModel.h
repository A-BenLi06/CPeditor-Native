#pragma once
#include "CheckerModel.h"
#include "RunnerConfiguration.h"
#include "SessionWorker.h"
#include "cpn/session.hpp"
#include <optional>
#include <string>
#include <vector>

namespace cpn::windows {
struct RecoveryConfiguration {
    SessionWorker::Options options;
    std::string directory;
    std::string filename;
    std::string backup_filename;
    std::string lock_filename;
    std::string location_filename;
    std::string source_lock_directory;
    std::string workspace_namespace;
    std::string document_namespace;
    std::chrono::milliseconds lock_timeout;
    std::chrono::milliseconds autosave_interval;
    std::size_t maximum_location_bytes{};
    [[nodiscard]] static RecoveryConfiguration parse(std::string_view);
    [[nodiscard]] cpn::SessionPaths paths(std::filesystem::path const& recovery) const;
};
struct DocumentPreferences {
    std::int64_t caret_byte{};
    std::int64_t anchor_byte{};
    bool utf8_bom{};
    std::string comparison;
    std::uint64_t test_revision{1};
    bool opaque{};
};
struct WorkspaceUiPreferences {
    std::string theme{"system"};
    bool word_wrap{false};
    bool auto_pair{true};
    std::uint32_t tab_width{4};
    std::optional<CheckerSettings> checker;
    [[nodiscard]] bool operator==(WorkspaceUiPreferences const&) const noexcept = default;
};
struct PreparedRecovery {
    cpn::WorkspaceSession session;
    std::vector<DocumentPreferences> documents;
    std::optional<std::vector<ToolchainChoice>> toolchains;
    WorkspaceUiPreferences ui_preferences;
    bool workspace_opaque{};
};
struct RecoveryModel {
    [[nodiscard]] static std::string logical_content(cpn::WorkspaceSession);
    [[nodiscard]] static std::filesystem::path parse_location(std::string_view, RecoveryConfiguration const&);
    [[nodiscard]] static std::string serialize_location(std::filesystem::path const&, RecoveryConfiguration const&);
    [[nodiscard]] static PreparedRecovery prepare(cpn::WorkspaceSession, RecoveryConfiguration const&,
        RunnerConfiguration const& defaults, std::vector<std::string> const& editor_profiles, std::string const& default_editor_profile);
    [[nodiscard]] static cpn::NativeEditorPreferences document_preferences(DocumentPreferences const&, std::string const& namespace_id);
    [[nodiscard]] static cpn::NativeEditorPreferences workspace_preferences(std::vector<ToolchainChoice> const&, std::string const& namespace_id);
    [[nodiscard]] static cpn::NativeEditorPreferences workspace_preferences(std::vector<ToolchainChoice> const&, WorkspaceUiPreferences const&, std::string const& namespace_id);
    // Unknown/future own versions win: retain all original entries and do not append a downgraded version.
    [[nodiscard]] static bool merge_preferences(std::vector<cpn::NativeEditorPreferences>&, cpn::NativeEditorPreferences);
    [[nodiscard]] static cpn::SessionLoadResult load_response(std::string_view);
    [[nodiscard]] static cpn::SessionSaveResult save_response(std::string_view);
    [[nodiscard]] static std::string save_request(cpn::SessionPaths const&, cpn::WorkspaceSession const&,
        std::optional<std::string> const& expected_digest, std::chrono::milliseconds timeout);
};
}
