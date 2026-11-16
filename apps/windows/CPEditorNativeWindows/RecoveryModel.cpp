#ifndef CPN_PORTABLE_ADAPTER_TEST
#include "pch.h"
#endif
#include "RecoveryModel.h"
#include "cpn/json.hpp"
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace cpn::windows {
namespace {
using Json = nlohmann::json;
void fields(Json const& value, std::initializer_list<std::string_view> allowed) {
    if (!value.is_object()) throw std::invalid_argument("Expected native recovery object.");
    for (auto const& [key, entry] : value.items()) {
        (void)entry;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) throw std::invalid_argument("Unknown native recovery field: " + key);
    }
}
Json parse(std::string_view text) {
    std::vector<std::set<std::string>> keys;
    return Json::parse(text, [&](int depth, Json::parse_event_t event, Json& value) {
        if (depth > 64) throw std::invalid_argument("Native recovery JSON is too deeply nested.");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::object_end) keys.pop_back();
        else if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second) throw std::invalid_argument("Duplicate native recovery field.");
        return true;
    });
}
std::uint64_t integer(Json const& value, std::uint64_t maximum = UINT64_MAX) {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 0) || value.get<std::uint64_t>() > maximum)
        throw std::invalid_argument("Native recovery field requires an integer within range.");
    return value.get<std::uint64_t>();
}
std::string text(Json const& value, bool nonempty = true) {
    auto result = value.get<std::string>();
    if ((nonempty && result.empty()) || result.find('\0') != std::string::npos) throw std::invalid_argument("Invalid native recovery text.");
    return result;
}
std::string digest(Json const& value) {
    auto result = text(value);
    if (result.size() != 64 || !std::all_of(result.begin(), result.end(), [](char byte) {
        return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
    })) throw std::invalid_argument("Invalid native recovery SHA-256 digest.");
    return result;
}
void recovery_location(std::filesystem::path const& path) {
    const auto bytes = cpn::path_to_utf8(path);
    if (!path.is_absolute() || path.filename().empty() || path.filename() == "." || path.filename() == ".." ||
        bytes.size() > cpn::SessionBounds{}.max_path_bytes)
        throw std::invalid_argument("Recovery location requires a bounded absolute file path.");
}
bool known_future(std::vector<cpn::NativeEditorPreferences> const& preferences, std::string const& ns) {
    return std::any_of(preferences.begin(), preferences.end(), [&](auto const& preference) { return preference.namespace_id == ns && preference.schema_version != 1; });
}
std::optional<Json> take_known(std::vector<cpn::NativeEditorPreferences>& preferences, std::string const& ns) {
    if (known_future(preferences, ns)) return {};
    auto found = std::find_if(preferences.begin(), preferences.end(), [&](auto const& preference) { return preference.namespace_id == ns && preference.schema_version == 1; });
    if (found == preferences.end()) return {};
    auto result = parse(found->object_json); preferences.erase(found); return result;
}
void coordinate(std::int64_t value, std::string const& source) {
    if (value < 0 || static_cast<std::uint64_t>(value) > source.size() ||
        (static_cast<std::size_t>(value) < source.size() && (static_cast<unsigned char>(source[static_cast<std::size_t>(value)]) & 0xc0) == 0x80))
        throw std::invalid_argument("Restored selection is not a UTF-8 byte boundary.");
}
}
RecoveryConfiguration RecoveryConfiguration::parse(std::string_view input) {
    auto value = cpn::windows::parse(input);
    fields(value, {"schemaVersion", "directory", "filename", "backupFilename", "lockFilename", "locationFilename", "sourceLockDirectory", "workspaceNamespace", "documentNamespace", "lockTimeoutMilliseconds", "autosaveMilliseconds", "maximumLocationBytes", "maximumCommands", "maximumRequestBytes"});
    if (integer(value.at("schemaVersion")) != 1) throw std::invalid_argument("Unsupported native recovery configuration.");
    RecoveryConfiguration result;
    result.directory = text(value.at("directory")); result.filename = text(value.at("filename"));
    result.backup_filename = text(value.at("backupFilename")); result.lock_filename = text(value.at("lockFilename"));
    result.location_filename = text(value.at("locationFilename"));
    result.source_lock_directory = text(value.at("sourceLockDirectory"));
    result.workspace_namespace = text(value.at("workspaceNamespace")); result.document_namespace = text(value.at("documentNamespace"));
    if (result.workspace_namespace == result.document_namespace) throw std::invalid_argument("Recovery preference namespaces overlap.");
    for (auto const* name : {&result.directory, &result.filename, &result.backup_filename, &result.lock_filename, &result.location_filename, &result.source_lock_directory}) {
        auto path = cpn::path_from_utf8(*name);
        if (name->find_first_of("/\\:") != std::string::npos || path.has_parent_path() || path.filename() == "." || path.filename() == "..") throw std::invalid_argument("Recovery filenames must be single path components.");
    }
    if (std::set<std::string>{result.filename, result.backup_filename, result.lock_filename}.size() != 3) throw std::invalid_argument("Recovery transaction filenames overlap.");
    result.lock_timeout = std::chrono::milliseconds(integer(value.at("lockTimeoutMilliseconds"), 60000));
    result.autosave_interval = std::chrono::milliseconds(integer(value.at("autosaveMilliseconds"), 60000));
    if (result.autosave_interval.count() < 250) throw std::invalid_argument("Recovery autosave interval is too short.");
    result.maximum_location_bytes = static_cast<std::size_t>(integer(value.at("maximumLocationBytes"), 1024 * 1024));
    result.options.maximum_commands = static_cast<std::size_t>(integer(value.at("maximumCommands"), 64));
    result.options.engine.struct_size = sizeof(cpn_engine_options); result.options.engine.abi_version = CPN_ABI_VERSION;
    result.options.engine.max_workers = 1; result.options.engine.max_jobs = 1; result.options.engine.max_buffers = 2;
    result.options.engine.max_request_bytes = integer(value.at("maximumRequestBytes"), 64 * 1024 * 1024);
    if (!result.maximum_location_bytes || !result.options.maximum_commands || !result.options.engine.max_request_bytes) throw std::invalid_argument("Recovery capacities must be positive.");
    return result;
}
cpn::SessionPaths RecoveryConfiguration::paths(std::filesystem::path const& recovery) const {
    recovery_location(recovery);
    auto parent = recovery.parent_path();
    // Derive a distinct transaction identity for every chosen recovery basename.
    auto name = recovery.filename();
    return {recovery, parent / (name.native() + cpn::path_from_utf8("." + backup_filename).native()),
        parent / (name.native() + cpn::path_from_utf8("." + lock_filename).native())};
}
std::filesystem::path RecoveryModel::parse_location(std::string_view input, RecoveryConfiguration const& config) {
    if (!config.maximum_location_bytes || input.size() > config.maximum_location_bytes)
        throw std::invalid_argument("Recovery location preference exceeds its byte limit.");
    auto value = parse(input);
    fields(value, {"schema_version", "recovery_file"});
    if (integer(value.at("schema_version")) != 1) throw std::invalid_argument("Unsupported recovery location preference.");
    auto path = cpn::path_from_utf8(text(value.at("recovery_file")));
    recovery_location(path);
    return path;
}
std::string RecoveryModel::serialize_location(std::filesystem::path const& path, RecoveryConfiguration const& config) {
    recovery_location(path);
    auto result = Json{{"schema_version", 1}, {"recovery_file", cpn::path_to_utf8(path)}}.dump();
    if (!config.maximum_location_bytes || result.size() > config.maximum_location_bytes)
        throw std::invalid_argument("Recovery location preference exceeds its byte limit.");
    return result;
}
PreparedRecovery RecoveryModel::prepare(cpn::WorkspaceSession session, RecoveryConfiguration const& config,
    RunnerConfiguration const& defaults, std::vector<std::string> const& profiles, std::string const& default_profile) {
    (void)cpn::serialize_workspace_session(session);
    if (session.documents.size() > defaults.maximum_documents) throw std::invalid_argument("Restored workspace exceeds native capacity.");
    PreparedRecovery result;
    result.workspace_opaque = known_future(session.editor_preferences, config.workspace_namespace);
    if (auto prefs = take_known(session.editor_preferences, config.workspace_namespace)) {
        fields(*prefs, {"toolchains"});
        if (!prefs->at("toolchains").is_array() || prefs->at("toolchains").empty()) throw std::invalid_argument("Restored toolchain catalog is empty.");
        std::vector<ToolchainChoice> choices; std::set<std::string> ids;
        for (auto const& entry : prefs->at("toolchains")) {
            fields(entry, {"id", "file", "label_resource", "profile"});
            ToolchainChoice choice{text(entry.at("id")), text(entry.at("file")), text(entry.at("label_resource")), entry.at("profile")};
            auto profile = cpn::parse_toolchain_profile(choice.profile.dump());
            if (profile.id != choice.id || choice.file.find_first_of("/\\:") != std::string::npos || choice.file == "." || choice.file == ".." ||
                !ids.insert(choice.id).second) throw std::invalid_argument("Restored toolchain identity is invalid.");
            choices.push_back(std::move(choice));
        }
        result.toolchains = std::move(choices);
    }
    auto const& choices = result.toolchains ? *result.toolchains : defaults.toolchains;
    if (std::find(profiles.begin(), profiles.end(), default_profile) == profiles.end())
        throw std::invalid_argument("Default editor language is absent.");
    for (auto const& profile : profiles) {
        const auto& id = defaults.default_toolchain(profile);
        if (std::none_of(choices.begin(), choices.end(), [&](auto const& choice) { return choice.id == id; }))
            throw std::invalid_argument("Restored catalog omits a native default toolchain.");
    }
    for (auto& document : session.documents) {
        if (!document.language_id) document.language_id = default_profile;
        if (document.tests.size() > defaults.maximum_tests ||
            !document.language_id || std::find(profiles.begin(), profiles.end(), *document.language_id) == profiles.end())
            throw std::invalid_argument("Restored document is incompatible with this native editor.");
        if (document.original_path && !document.original_path->is_absolute())
            throw std::invalid_argument("Restored source path must be absolute on this platform.");
        if (document.toolchain_profile_id && std::none_of(choices.begin(), choices.end(), [&](auto const& choice) { return choice.id == *document.toolchain_profile_id; }))
            throw std::invalid_argument("Restored document refers to an absent toolchain.");
        DocumentPreferences preference; preference.comparison = defaults.default_comparison;
        preference.opaque = known_future(document.editor_preferences, config.document_namespace);
        if (auto own = take_known(document.editor_preferences, config.document_namespace)) {
            fields(*own, {"caret_byte", "anchor_byte", "utf8_bom", "comparison", "test_revision"});
            preference.caret_byte = static_cast<std::int64_t>(integer(own->at("caret_byte"), INT64_MAX));
            preference.anchor_byte = static_cast<std::int64_t>(integer(own->at("anchor_byte"), INT64_MAX));
            preference.utf8_bom = own->at("utf8_bom").get<bool>();
            preference.comparison = text(own->at("comparison")); preference.test_revision = integer(own->at("test_revision"));
            if (!preference.test_revision) throw std::invalid_argument("Restored testcase revision must be positive.");
        }
        coordinate(preference.caret_byte, document.source_text); coordinate(preference.anchor_byte, document.source_text);
        if (std::none_of(defaults.comparisons.begin(), defaults.comparisons.end(), [&](auto const& choice) { return choice.id == preference.comparison; }))
            throw std::invalid_argument("Restored comparison policy is unknown.");
        result.documents.push_back(preference);
    }
    result.session = std::move(session); return result;
}
cpn::NativeEditorPreferences RecoveryModel::document_preferences(DocumentPreferences const& value, std::string const& ns) {
    return {ns, 1, Json{{"caret_byte", value.caret_byte}, {"anchor_byte", value.anchor_byte}, {"utf8_bom", value.utf8_bom},
        {"comparison", value.comparison}, {"test_revision", value.test_revision}}.dump()};
}
cpn::NativeEditorPreferences RecoveryModel::workspace_preferences(std::vector<ToolchainChoice> const& choices, std::string const& ns) {
    auto catalog = Json::array();
    for (auto const& choice : choices) catalog.push_back({{"id", choice.id}, {"file", choice.file}, {"label_resource", choice.label_resource}, {"profile", choice.profile}});
    return {ns, 1, Json{{"toolchains", std::move(catalog)}}.dump()};
}
bool RecoveryModel::merge_preferences(std::vector<cpn::NativeEditorPreferences>& preferences, cpn::NativeEditorPreferences own) {
    if (known_future(preferences, own.namespace_id)) return false;
    std::erase_if(preferences, [&](auto const& value) { return value.namespace_id == own.namespace_id && value.schema_version == own.schema_version; });
    if (preferences.size() >= cpn::SessionBounds{}.max_preferences_per_owner) return false;
    preferences.push_back(std::move(own)); return true;
}
cpn::SessionLoadResult RecoveryModel::load_response(std::string_view input) {
    auto value = parse(input);
    fields(value, {"schema_version", "status", "session", "disk_digest", "detail"});
    if (integer(value.at("schema_version")) != 1) throw std::invalid_argument("Unsupported Session load response.");
    cpn::SessionLoadResult result; bool known = false;
    for (auto status : {cpn::SessionLoadStatus::loaded, cpn::SessionLoadStatus::missing, cpn::SessionLoadStatus::invalid,
        cpn::SessionLoadStatus::unsupported_schema, cpn::SessionLoadStatus::io_failed}) {
        if (value.at("status") == cpn::session_load_status_name(status)) { result.status = status; known = true; break; }
    }
    if (!known) throw std::invalid_argument("Unknown Session load status.");
    result.detail = value.at("detail").get<std::string>();
    if (!value.at("disk_digest").is_null()) result.disk_digest = digest(value.at("disk_digest"));
    if (!value.at("session").is_null()) result.session = cpn::parse_workspace_session(value.at("session").dump());
    if (result.status == cpn::SessionLoadStatus::loaded && (!result.session || !result.disk_digest)) throw std::invalid_argument("Loaded Session response is incomplete.");
    if ((result.status != cpn::SessionLoadStatus::loaded && result.session) ||
        (result.status == cpn::SessionLoadStatus::missing && result.disk_digest))
        throw std::invalid_argument("Session load status contradicts its payload.");
    return result;
}
cpn::SessionSaveResult RecoveryModel::save_response(std::string_view input) {
    auto value = parse(input);
    fields(value, {"schema_version", "status", "disk_digest", "recovery_replaced", "backup_replaced", "detail"});
    if (integer(value.at("schema_version")) != 1) throw std::invalid_argument("Unsupported Session save response.");
    cpn::SessionSaveResult result; bool known = false;
    for (auto status : {cpn::SessionSaveStatus::saved, cpn::SessionSaveStatus::conflict, cpn::SessionSaveStatus::invalid_session,
        cpn::SessionSaveStatus::invalid_existing, cpn::SessionSaveStatus::io_failed, cpn::SessionSaveStatus::durability_unknown,
        cpn::SessionSaveStatus::lock_unavailable, cpn::SessionSaveStatus::cancelled}) {
        if (value.at("status") == cpn::session_save_status_name(status)) { result.status = status; known = true; break; }
    }
    if (!known) throw std::invalid_argument("Unknown Session save status.");
    result.detail = value.at("detail").get<std::string>();
    result.recovery_replaced = value.at("recovery_replaced").get<bool>(); result.backup_replaced = value.at("backup_replaced").get<bool>();
    if (!value.at("disk_digest").is_null()) result.disk_digest = digest(value.at("disk_digest"));
    if ((result.recovery_replaced && !result.disk_digest) || (result.status == cpn::SessionSaveStatus::saved && !result.recovery_replaced))
        throw std::invalid_argument("Session replacement response is incomplete.");
    if ((result.recovery_replaced && result.status != cpn::SessionSaveStatus::saved && result.status != cpn::SessionSaveStatus::durability_unknown) ||
        (result.status == cpn::SessionSaveStatus::durability_unknown && !result.recovery_replaced && !result.backup_replaced))
        throw std::invalid_argument("Session save status contradicts its replacement state.");
    return result;
}
std::string RecoveryModel::save_request(cpn::SessionPaths const& paths, cpn::WorkspaceSession const& session,
    std::optional<std::string> const& expected, std::chrono::milliseconds timeout) {
    if (timeout.count() < 0 || timeout.count() > 60000) throw std::invalid_argument("Invalid recovery lock timeout.");
    return Json{{"schema_version", 1}, {"paths", {{"recovery_file", cpn::path_to_utf8(paths.recovery_file)},
        {"backup_file", cpn::path_to_utf8(paths.backup_file)}, {"lock_file", cpn::path_to_utf8(paths.lock_file)}}},
        {"session", Json::parse(cpn::serialize_workspace_session(session))},
        {"expected_disk_digest", expected ? Json(*expected) : Json(nullptr)}, {"lock_timeout_ms", timeout.count()}}.dump();
}
}
