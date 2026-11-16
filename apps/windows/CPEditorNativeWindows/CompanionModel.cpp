#include "pch.h"
#include "CompanionModel.h"
#include "SourceStore.h"
#include "RevisionPolicy.h"
#include "cpn/digest.hpp"
#include "cpn/text.hpp"
#include <algorithm>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cpn::windows {
namespace {
using Json = nlohmann::json;
namespace field {
constexpr auto schema = "schema_version", workspace = "workspace_id", paths = "paths", inbox = "inbox_file", backup = "backup_file", lock = "lock_file";
constexpr auto after = "after_sequence", max_items = "max_items", max_page = "max_page_bytes", original = "original_json", timeout = "lock_timeout_ms";
constexpr auto import = "import_id", session = "session", defaults = "defaults", session_paths = "session_paths", recovery = "recovery_file";
constexpr auto status = "status", detail = "detail", inbox_id = "inbox_id", revision = "revision", digest = "disk_digest", next = "next_after_sequence", more = "has_more", items = "items";
constexpr auto sequence = "sequence", state = "state", problem = "problem_id", document = "document_id", test_ids = "test_ids", previous = "previous_import_id", payload_digest = "payload_digest";
constexpr auto applied_digest = "applied_session_digest", applied_revision = "applied_session_revision", inbox_replaced = "inbox_replaced", backup_replaced = "backup_replaced";
constexpr auto inbox_status = "inbox_status", receipt = "receipt", receipts = "receipts", source = "source_text", language = "language_id", toolchain = "toolchain_profile_id", document_preferences = "document_preferences";
constexpr auto namespace_id = "namespace_id", preference_schema = "schema_version", object = "object_json";
constexpr auto options = "options", compatible = "ordinary_stdio_compatible";
}
Json parse(std::string_view bytes) {
    std::vector<std::set<std::string>> keys;
    return Json::parse(bytes, [&](int depth, Json::parse_event_t event, Json& value) {
        if (depth > 64) throw std::invalid_argument("Companion response nesting exceeds native bounds.");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::object_end) keys.pop_back();
        else if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second) throw std::invalid_argument("Duplicate Companion field.");
        return true;
    });
}
void fields(Json const& value, std::initializer_list<std::string_view> allowed) {
    if (!value.is_object()) throw std::invalid_argument("Expected Companion object.");
    for (auto const& [key, entry] : value.items()) { (void)entry;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) throw std::invalid_argument("Unknown Companion field: " + key); }
}
std::uint64_t integer(Json const& value, std::uint64_t maximum = UINT64_MAX) {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 0) || value.get<std::uint64_t>() > maximum)
        throw std::invalid_argument("Companion integer exceeds its native range.");
    return value.get<std::uint64_t>();
}
std::string identity(Json const& value) {
    auto text = value.get<std::string>();
    if (text.empty() || text.size() > cpn::SessionBounds{}.max_id_bytes || text.find('\0') != std::string::npos)
        throw std::invalid_argument("Invalid Companion identity.");
    return text;
}
std::optional<std::string> optional_text(Json const& value) { return value.is_null() ? std::nullopt : std::optional<std::string>{value.get<std::string>()}; }
bool checksum(std::string const& value) { return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }); }
std::optional<std::string> optional_digest(Json const& value) {
    auto result = optional_text(value); if (result && !checksum(*result)) throw std::invalid_argument("Invalid Companion digest."); return result;
}
cpn::CompanionInboxLoadStatus load_status(Json const& value) {
    for (auto status : {cpn::CompanionInboxLoadStatus::loaded, cpn::CompanionInboxLoadStatus::missing, cpn::CompanionInboxLoadStatus::invalid,
        cpn::CompanionInboxLoadStatus::unsupported_schema, cpn::CompanionInboxLoadStatus::io_failed})
        if (value == cpn::companion_inbox_load_status_name(status)) return status;
    throw std::invalid_argument("Unknown Companion load status.");
}
void version(Json const& value) { if (integer(value.at(field::schema)) != 1) throw std::invalid_argument("Unsupported Companion response schema."); }
Json path_json(cpn::CompanionInboxPaths const& paths) { return {{field::inbox, cpn::path_to_utf8(paths.inbox_file)}, {field::backup, cpn::path_to_utf8(paths.backup_file)}, {field::lock, cpn::path_to_utf8(paths.lock_file)}}; }
cpn::CompanionInboxPaths decode_paths(Json const& value) {
    fields(value, {field::inbox, field::backup, field::lock}); cpn::CompanionInboxPaths result;
    auto read = [&](char const* key) { auto path = cpn::path_from_utf8(value.at(key).get<std::string>());
        if (!path.is_absolute() || path.empty() || cpn::path_to_utf8(path).size() > cpn::SessionBounds{}.max_path_bytes) throw std::invalid_argument("Companion storage path must be absolute."); return path; };
    result.inbox_file = read(field::inbox); result.backup_file = read(field::backup); result.lock_file = read(field::lock); return result;
}
cpn::NativeEditorPreferences const* owner(std::vector<cpn::NativeEditorPreferences> const& values, std::string_view ns) {
    cpn::NativeEditorPreferences const* found{};
    for (auto const& value : values) if (value.namespace_id == ns) {
        if (found || value.schema_version != 1) throw std::invalid_argument("Companion owner is unsupported or ambiguous."); found = &value;
    }
    return found;
}
bool same_preferences(std::vector<cpn::NativeEditorPreferences> const& left, std::vector<cpn::NativeEditorPreferences> const& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) if (left[i].namespace_id != right[i].namespace_id || left[i].schema_version != right[i].schema_version || left[i].object_json != right[i].object_json) return false;
    return true;
}
std::vector<cpn::NativeEditorPreferences> without_receipts(std::vector<cpn::NativeEditorPreferences> value) {
    std::erase_if(value, [](auto const& v) { return v.namespace_id == cpn::companion_receipts_namespace; }); return value;
}
cpn::ImportApplicationReceipt receipt(Json value) {
    fields(value, {field::inbox_id, field::import, field::problem, field::document, field::payload_digest, field::test_ids});
    cpn::SessionDocument fake; fake.document_id = identity(value.at(field::document)); value[field::compatible] = true;
    fake.editor_preferences.push_back({std::string{cpn::companion_document_namespace}, 1, value.dump()});
    return cpn::read_companion_document_provenance(fake)->identity;
}
std::vector<cpn::ImportApplicationReceipt> receipts(cpn::WorkspaceSession const& session) {
    auto preference = owner(session.editor_preferences, cpn::companion_receipts_namespace); if (!preference) return {};
    auto value = parse(preference->object_json); fields(value, {field::receipts});
    if (!value.at(field::receipts).is_array() || value.at(field::receipts).size() > 256) throw std::invalid_argument("Companion receipt capacity exceeded.");
    std::vector<cpn::ImportApplicationReceipt> result;
    for (auto const& item : value.at(field::receipts)) {
        auto id = receipt(item); auto actual = cpn::find_import_application_receipt(session, id.inbox_id, id.import_id);
        if (!actual || !CompanionModel::same_receipt(*actual, id)) throw std::invalid_argument("Companion receipt identity differs.");
        result.push_back(std::move(id));
    }
    return result;
}
}
CompanionConfiguration CompanionConfiguration::parse(std::string_view input) {
    auto value = cpn::windows::parse(input);
    fields(value, {"schemaVersion", "storageNamespace", "inboxSuffix", "backupSuffix", "lockSuffix", "sourceTemplate", "maximumImportBytes", "pageItems", "pageBytes", "previewBytes", "previewSamples", "lockTimeoutMilliseconds", "maximumCommands", "maximumRequestBytes", "maximumRetainedOutputBytes", "listenerOptions", "statusLabels", "issueLabels"});
    if (integer(value.at("schemaVersion")) != 1) throw std::invalid_argument("Unsupported Companion configuration.");
    CompanionConfiguration result;
    result.storage_namespace = identity(value.at("storageNamespace")); result.inbox_suffix = identity(value.at("inboxSuffix"));
    result.backup_suffix = identity(value.at("backupSuffix")); result.lock_suffix = identity(value.at("lockSuffix"));
    if (result.storage_namespace == cpn::companion_document_namespace || result.storage_namespace == cpn::companion_receipts_namespace) throw std::invalid_argument("Companion namespaces overlap.");
    for (auto const* suffix : {&result.inbox_suffix, &result.backup_suffix, &result.lock_suffix})
        if (suffix->find_first_of("/\\:") != std::string::npos || *suffix == "." || *suffix == "..") throw std::invalid_argument("Companion suffix is not a filename component.");
    if (std::set<std::string>{result.inbox_suffix, result.backup_suffix, result.lock_suffix}.size() != 3) throw std::invalid_argument("Companion suffixes overlap.");
    result.source_template = value.at("sourceTemplate").get<std::string>();
    result.maximum_import_bytes = integer(value.at("maximumImportBytes"), cpn::CompanionBounds{}.max_json_bytes);
    result.page_items = integer(value.at("pageItems"), cpn::CompanionInboxBounds{}.max_records); result.page_bytes = integer(value.at("pageBytes"), cpn::CompanionInboxBounds{}.max_json_bytes);
    result.preview_bytes = integer(value.at("previewBytes"), 4096); result.lock_timeout = std::chrono::milliseconds(integer(value.at("lockTimeoutMilliseconds"), 60000));
    result.preview_samples = integer(value.at("previewSamples"), 128);
    result.worker.maximum_commands = integer(value.at("maximumCommands"), 64);
    result.worker.engine.struct_size = sizeof(cpn_engine_options); result.worker.engine.abi_version = CPN_ABI_VERSION;
    result.worker.engine.max_workers = 1; result.worker.engine.max_jobs = 1; result.worker.engine.max_buffers = 2;
    result.worker.engine.max_request_bytes = integer(value.at("maximumRequestBytes"), 64 * 1024 * 1024);
    result.worker.engine.max_retained_output_bytes = integer(value.at("maximumRetainedOutputBytes"), 64 * 1024 * 1024);
    if (!result.maximum_import_bytes || !result.page_items || !result.page_bytes || !result.preview_bytes || !result.preview_samples || !result.worker.maximum_commands ||
        !result.worker.engine.max_request_bytes || result.worker.engine.max_retained_output_bytes < result.page_bytes) throw std::invalid_argument("Companion capacities must be positive and consistent.");
    result.listener_options = value.at("listenerOptions");
    fields(result.listener_options, {"port", "enable_ipv6", "max_connections", "max_admissions", "max_body_bytes", "max_inflight_body_bytes", "max_header_bytes", "max_header_count", "max_response_bytes", "socket_send_buffer_bytes", "read_timeout_ms", "write_timeout_ms", "request_timeout_ms", "lock_timeout_ms"});
    (void)integer(result.listener_options.at("port"), 65535); (void)result.listener_options.at("enable_ipv6").get<bool>();
    for (auto const& [key, item] : result.listener_options.items()) if (key != "port" && key != "enable_ipv6") (void)integer(item);
    result.status_labels = value.at("statusLabels").get<std::map<std::string, std::string>>(); result.issue_labels = value.at("issueLabels").get<std::map<std::string, std::string>>();
    return result;
}
cpn::CompanionInboxPaths CompanionConfiguration::paths(std::filesystem::path const& recovery) const {
    auto parent = recovery.parent_path(); auto name = recovery.filename().native();
    return {parent / (name + cpn::path_from_utf8(inbox_suffix).native()), parent / (name + cpn::path_from_utf8(backup_suffix).native()), parent / (name + cpn::path_from_utf8(lock_suffix).native())};
}
Json CompanionModel::context(std::string const& workspace, cpn::CompanionInboxPaths const& paths) { return {{field::schema, 1}, {field::workspace, workspace}, {field::paths, path_json(paths)}}; }
bool CompanionModel::can_create_storage_binding(std::uint64_t revision) noexcept {
    // A new owner must be persisted before receiving; the later application
    // also needs a successor. Existing persisted owners can use the final revision.
    return !RevisionPolicy::exhausted(revision) && !RevisionPolicy::final_revision_pending(revision);
}
std::string CompanionModel::page_request(std::string const& workspace, cpn::CompanionInboxPaths const& paths, std::uint64_t after, CompanionConfiguration const& config) {
    auto value = context(workspace, paths); value[field::after] = after; value[field::max_items] = config.page_items; value[field::max_page] = config.page_bytes; return value.dump();
}
std::string CompanionModel::accept_request(std::string const& workspace, cpn::CompanionInboxPaths const& paths, std::string const& original, CompanionConfiguration const& config) {
    auto value = context(workspace, paths); value[field::original] = original; value[field::timeout] = config.lock_timeout.count(); return value.dump();
}
std::string CompanionModel::prepare_request(std::string const& workspace, cpn::CompanionInboxPaths const& paths, std::string const& import_id, cpn::WorkspaceSession const& session, cpn::ImportDocumentDefaults const& defaults) {
    auto value = context(workspace, paths); value[field::import] = import_id; value[field::session] = Json::parse(cpn::serialize_workspace_session(session));
    auto preferences = Json::array(); for (auto const& pref : defaults.document_preferences) preferences.push_back({{field::namespace_id, pref.namespace_id}, {field::preference_schema, pref.schema_version}, {field::object, pref.object_json}});
    value[field::defaults] = {{field::source, defaults.source_text}, {field::language, defaults.language_id ? Json(*defaults.language_id) : Json(nullptr)},
        {field::toolchain, defaults.toolchain_profile_id ? Json(*defaults.toolchain_profile_id) : Json(nullptr)}, {field::document_preferences, std::move(preferences)}}; return value.dump();
}
std::string CompanionModel::mark_request(std::string const& workspace, cpn::CompanionInboxPaths const& paths, std::string const& import_id, cpn::SessionPaths const& session, CompanionConfiguration const& config) {
    auto value = context(workspace, paths); value[field::import] = import_id; value[field::timeout] = config.lock_timeout.count();
    value[field::session_paths] = {{field::recovery, cpn::path_to_utf8(session.recovery_file)}, {field::backup, cpn::path_to_utf8(session.backup_file)}, {field::lock, cpn::path_to_utf8(session.lock_file)}}; return value.dump();
}
std::string CompanionModel::listener_start_request(std::string const& workspace, cpn::CompanionInboxPaths const& paths, CompanionConfiguration const& config) { auto value = context(workspace, paths); value[field::options] = config.listener_options; return value.dump(); }
std::string CompanionModel::version_request() { return Json{{field::schema, 1}}.dump(); }
CompanionPage CompanionModel::page_response(std::string_view bytes) {
    auto value = parse(bytes); fields(value, {field::schema, field::status, field::detail, field::inbox_id, field::workspace, field::revision, field::digest, field::next, field::more, field::items}); version(value);
    CompanionPage result; result.status = load_status(value.at(field::status)); result.workspace_id = identity(value.at(field::workspace)); result.detail = value.at(field::detail).get<std::string>();
    result.inbox_id = optional_text(value.at(field::inbox_id)); result.disk_digest = optional_digest(value.at(field::digest));
    if (!value.at(field::revision).is_null()) result.revision = integer(value.at(field::revision));
    result.next_after_sequence = integer(value.at(field::next)); result.has_more = value.at(field::more).get<bool>();
    if (!value.at(field::items).is_array() || value.at(field::items).size() > 256) throw std::invalid_argument("Companion page capacity exceeded.");
    for (auto const& item : value.at(field::items)) {
        fields(item, {field::sequence, field::state, field::import, field::problem, field::document, field::test_ids, field::previous, field::payload_digest, field::original, field::applied_digest, field::applied_revision});
        if (!result.inbox_id) throw std::invalid_argument("Companion page lacks its inbox identity.");
        cpn::CompanionInboxItem decoded; decoded.sequence = integer(item.at(field::sequence));
        const auto state = item.at(field::state).get<std::string>();
        if (state == "Pending") decoded.state = cpn::CompanionInboxState::pending; else if (state == "Applied") decoded.state = cpn::CompanionInboxState::applied; else throw std::invalid_argument("Unknown Companion item state.");
        decoded.import_id = identity(item.at(field::import)); decoded.problem_id = identity(item.at(field::problem)); decoded.document_id = identity(item.at(field::document));
        decoded.test_ids = item.at(field::test_ids).get<std::vector<std::string>>(); decoded.previous_import_id = optional_text(item.at(field::previous)); decoded.payload_digest = item.at(field::payload_digest).get<std::string>();
        decoded.candidate = cpn::parse_companion_payload(item.at(field::original).get<std::string>()); decoded.applied_session_digest = optional_digest(item.at(field::applied_digest));
        if (!item.at(field::applied_revision).is_null()) decoded.applied_session_revision = integer(item.at(field::applied_revision));
        (void)cpn::serialize_companion_inbox_item(decoded, *result.inbox_id); result.items.push_back(std::move(decoded));
    }
    if (result.status == cpn::CompanionInboxLoadStatus::loaded && (!result.inbox_id || !result.revision || !result.disk_digest)) throw std::invalid_argument("Loaded Companion page lacks identity.");
    if (result.status != cpn::CompanionInboxLoadStatus::loaded && (!result.items.empty() || result.has_more || result.inbox_id || result.revision)) throw std::invalid_argument("Companion page status contradicts payload.");
    return result;
}
CompanionWriteResponse CompanionModel::write_response(std::string_view bytes) {
    auto value = parse(bytes); fields(value, {field::schema, field::status, field::detail, field::digest, field::inbox_replaced, field::backup_replaced, "item"}); version(value);
    CompanionWriteResponse result{value.at(field::status).get<std::string>(), value.at(field::detail).get<std::string>(), optional_digest(value.at(field::digest)), value.at(field::inbox_replaced).get<bool>(), value.at(field::backup_replaced).get<bool>()};
    bool known{}; for (unsigned index = 0; index <= static_cast<unsigned>(cpn::CompanionInboxWriteStatus::id_exhausted); ++index)
        if (result.status == cpn::companion_inbox_write_status_name(static_cast<cpn::CompanionInboxWriteStatus>(index))) known = true;
    if (!known || (result.inbox_replaced && !result.disk_digest)) throw std::invalid_argument("Invalid Companion write response."); return result;
}
CompanionPreparation CompanionModel::prepare_response(std::string_view bytes) {
    auto value = parse(bytes); fields(value, {field::schema, field::inbox_status, field::status, field::detail, field::digest, field::session, field::receipt}); version(value);
    CompanionPreparation result; result.inbox_status = load_status(value.at(field::inbox_status)); result.status = value.at(field::status).get<std::string>(); result.detail = value.at(field::detail).get<std::string>();
    (void)optional_digest(value.at(field::digest));
    if (!value.at(field::session).is_null()) result.session = cpn::parse_workspace_session(value.at(field::session).dump());
    if (!value.at(field::receipt).is_null()) result.receipt = receipt(value.at(field::receipt));
    bool known{}; for (unsigned index = 0; index <= static_cast<unsigned>(cpn::ImportPreparationStatus::receipt_missing); ++index)
        if (result.status == cpn::import_preparation_status_name(static_cast<cpn::ImportPreparationStatus>(index))) known = true;
    if (!known) throw std::invalid_argument("Invalid Companion preparation status.");
    if ((result.status == "Prepared" || result.status == "AlreadyApplied") && (result.inbox_status != cpn::CompanionInboxLoadStatus::loaded || !result.session || !result.receipt)) throw std::invalid_argument("Companion preparation lacks its atomic candidate."); return result;
}
CompanionListenerResponse CompanionModel::listener_response(std::string_view bytes) {
    auto value = parse(bytes); fields(value, {field::schema, field::status, "has_listener", "engine_closed", "port", "ipv6", "start_status", "native_error", field::detail, "failure_detail", "statistics"}); version(value);
    CompanionListenerResponse result; result.status = value.at(field::status).get<std::string>(); result.has_listener = value.at("has_listener").get<bool>(); result.engine_closed = value.at("engine_closed").get<bool>();
    result.port = static_cast<std::uint16_t>(integer(value.at("port"), 65535)); result.ipv6 = value.at("ipv6").get<bool>(); result.detail = value.at(field::detail).get<std::string>(); result.failure_detail = value.at("failure_detail").get<std::string>();
    static const std::set<std::string> statuses{"Started", "AlreadyStarted", "Closed", "BindFailed", "Failed", "Stopped", "NotStarted", "ListenerInfo"};
    if (!statuses.contains(result.status) || !value.at("native_error").is_number_integer()) throw std::invalid_argument("Invalid Companion listener response.");
    if (!value.at("start_status").is_null() && !std::set<std::string>{"Started", "AlreadyStarted", "Closed", "BindFailed", "Failed"}.contains(value.at("start_status").get<std::string>())) throw std::invalid_argument("Invalid Companion listener start status.");
    auto const& stats = value.at("statistics");
    fields(stats, {"running", "closed", "failed", "active_connections", "outstanding_admissions", "inflight_body_bytes", "accepted", "duplicates", "admission_failures", "connection_rejections", "admission_rejections", "read_timeouts", "write_timeouts", "request_timeouts"});
    result.running = stats.at("running").get<bool>(); result.closed = stats.at("closed").get<bool>(); result.failed = stats.at("failed").get<bool>();
    for (auto const& [key, item] : stats.items()) if (key != "running" && key != "closed" && key != "failed") (void)integer(item);
    if ((!result.has_listener && result.running) || (result.running && (result.closed || result.failed || result.engine_closed))) throw std::invalid_argument("Companion listener state contradicts its capability.");
    return result;
}
std::optional<CompanionBinding> CompanionModel::binding(std::vector<cpn::NativeEditorPreferences> const& preferences, std::string const& ns) {
    auto own = owner(preferences, ns); if (!own) return {};
    auto value = parse(own->object_json); fields(value, {field::workspace, field::paths}); return CompanionBinding{identity(value.at(field::workspace)), decode_paths(value.at(field::paths))};
}
cpn::NativeEditorPreferences CompanionModel::binding_preference(CompanionBinding const& binding, std::string const& ns) { return {ns, 1, Json{{field::workspace, binding.workspace_id}, {field::paths, path_json(binding.paths)}}.dump()}; }
bool CompanionModel::same_binding(CompanionBinding const& left, CompanionBinding const& right) noexcept { return left.workspace_id == right.workspace_id && left.paths.inbox_file == right.paths.inbox_file && left.paths.backup_file == right.paths.backup_file && left.paths.lock_file == right.paths.lock_file; }
void CompanionModel::validate_paths(cpn::CompanionInboxPaths const& paths, std::vector<std::filesystem::path> const& reserved) {
    std::vector<std::filesystem::path> all{paths.inbox_file, paths.backup_file, paths.lock_file};
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (!all[i].is_absolute()) throw std::invalid_argument("Companion storage is not absolute.");
        auto path = SourceStore::resolve(all[i]);
        for (auto const& other : reserved) if (!other.empty()) {
            auto target = SourceStore::resolve(other);
#if defined(_WIN32)
            const bool equal = CompareStringOrdinal(path.c_str(), -1, target.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
            const bool equal = path == target;
#endif
            if (equal || (std::filesystem::exists(path) && std::filesystem::exists(target) && std::filesystem::equivalent(path, target))) throw std::invalid_argument("Companion storage aliases reserved data.");
        }
        for (std::size_t j = i + 1; j < all.size(); ++j) {
            auto next = SourceStore::resolve(all[j]);
#if defined(_WIN32)
            const bool equal = CompareStringOrdinal(path.c_str(), -1, next.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
            const bool equal = path == next;
#endif
            if (equal || (std::filesystem::exists(path) && std::filesystem::exists(next) && std::filesystem::equivalent(path, next))) throw std::invalid_argument("Companion storage paths overlap.");
        }
    }
}
CompanionRunCapability CompanionModel::run_capability(cpn::SessionDocument const& document) noexcept {
    try { auto provenance = cpn::read_companion_document_provenance(document); return !provenance || provenance->compatible_with_ordinary_stdio ? CompanionRunCapability::ordinary : CompanionRunCapability::unsupported; }
    catch (...) { return CompanionRunCapability::unavailable; }
}
bool CompanionModel::same_receipt(cpn::ImportApplicationReceipt const& left, cpn::ImportApplicationReceipt const& right) noexcept { return left.inbox_id == right.inbox_id && left.import_id == right.import_id && left.problem_id == right.problem_id && left.document_id == right.document_id && left.payload_digest == right.payload_digest && left.test_ids == right.test_ids; }
CompanionDelta CompanionModel::delta(cpn::WorkspaceSession const& base, CompanionPreparation const& response, std::string const& inbox_id, std::string const& import_id,
    RecoveryConfiguration const& config, RunnerConfiguration const& defaults, std::vector<std::string> const& profiles, std::string const& default_profile) {
    if (response.status != "Prepared" || !response.session || !response.receipt || response.receipt->inbox_id != inbox_id || response.receipt->import_id != import_id) throw std::invalid_argument("Companion preparation identity mismatch.");
    auto const& candidate = *response.session; auto const& receipt_id = *response.receipt;
    if (candidate.workspace_id != base.workspace_id || base.revision == UINT64_MAX || candidate.revision != base.revision + 1 || candidate.documents.size() != base.documents.size() + 1 || candidate.selected_document_id != receipt_id.document_id) throw std::invalid_argument("Companion candidate changed its base identity.");
    auto reduced = candidate; reduced.revision = base.revision; reduced.selected_document_id = base.selected_document_id; reduced.documents.pop_back(); reduced.editor_preferences = base.editor_preferences;
    if (cpn::serialize_workspace_session(reduced) != cpn::serialize_workspace_session(base) || !same_preferences(without_receipts(candidate.editor_preferences), without_receipts(base.editor_preferences))) throw std::invalid_argument("Companion candidate changed existing native state.");
    auto prior = receipts(base); auto next = receipts(candidate);
    if (next.size() != prior.size() + 1 || !same_receipt(next.back(), receipt_id)) throw std::invalid_argument("Companion candidate receipt delta is invalid.");
    for (std::size_t i = 0; i < prior.size(); ++i) if (!same_receipt(prior[i], next[i])) throw std::invalid_argument("Companion changed an existing receipt.");
    auto const& record = candidate.documents.back(); auto provenance = cpn::read_companion_document_provenance(record);
    if (record.document_id != receipt_id.document_id || record.original_path || record.save_point || record.tests.size() != receipt_id.test_ids.size() || !provenance || !same_receipt(provenance->identity, receipt_id)) throw std::invalid_argument("Companion new document is inconsistent.");
    for (std::size_t i = 0; i < record.tests.size(); ++i) if (record.tests[i].id != receipt_id.test_ids[i]) throw std::invalid_argument("Companion test identity changed.");
    auto native = RecoveryModel::prepare(candidate, config, defaults, profiles, default_profile);
    auto receipt_pref = owner(candidate.editor_preferences, cpn::companion_receipts_namespace); if (!receipt_pref) throw std::invalid_argument("Companion receipt owner is absent.");
    return {record, native.documents.back(), *receipt_pref, receipt_id};
}
cpn::WorkspaceSession CompanionModel::merge(cpn::WorkspaceSession const& current, cpn::WorkspaceSession const& base, CompanionDelta const& delta) {
    if (current.workspace_id != base.workspace_id || current.revision == UINT64_MAX || current.documents.size() >= cpn::SessionBounds{}.max_documents) throw std::invalid_argument("Companion cannot append to this workspace.");
    auto prior = owner(base.editor_preferences, cpn::companion_receipts_namespace); auto latest = owner(current.editor_preferences, cpn::companion_receipts_namespace);
    if ((prior == nullptr) != (latest == nullptr) || (prior && (prior->object_json != latest->object_json || prior->schema_version != latest->schema_version))) throw std::invalid_argument("Companion receipts changed during preparation.");
    for (auto const& document : current.documents) if (document.document_id == delta.document.document_id) throw std::invalid_argument("Companion document already exists without its receipt.");
    auto result = current; result.editor_preferences = without_receipts(result.editor_preferences);
    if (result.editor_preferences.size() >= cpn::SessionBounds{}.max_preferences_per_owner) throw std::invalid_argument("No capacity for the Companion receipt owner.");
    result.editor_preferences.push_back(delta.receipt_preference); result.documents.push_back(delta.document); result.selected_document_id = delta.document.document_id;
    (void)cpn::serialize_workspace_session(result); return result;
}
std::string CompanionModel::preview(std::string_view bytes, std::size_t maximum) {
    auto visible = bytes.substr(0, std::min(bytes.size(), maximum));
    const auto inspection_size = visible.size() + std::min<std::size_t>(3, bytes.size() - visible.size());
    const auto decoded = cpn::decode_utf8(bytes.substr(0, inspection_size));
    if (visible.find('\0') != std::string_view::npos || (decoded.error_offset && *decoded.error_offset < visible.size())) {
        std::ostringstream text; text << std::hex << std::setfill('0'); for (unsigned char byte : visible) text << std::setw(2) << static_cast<unsigned>(byte) << ' '; return text.str();
    }
    std::size_t complete{};
    for (auto const& scalar : decoded.scalars) {
        if (scalar.offset + scalar.length > visible.size()) break;
        complete = scalar.offset + scalar.length;
    }
    return std::string{visible.substr(0, complete)};
}
bool CompanionPagingState::install(CompanionPage page, std::string const& workspace, bool reset) {
    if (page.workspace_id != workspace) throw std::invalid_argument("Companion page workspace differs.");
    if (page.status != cpn::CompanionInboxLoadStatus::loaded && page.status != cpn::CompanionInboxLoadStatus::missing) throw std::invalid_argument(page.detail);
    if (!reset && (page.inbox_id != inbox_id || page.revision != revision || page.disk_digest != disk_digest)) return false;
    auto cursor = reset ? 0 : after;
    for (auto const& item : page.items) { if (item.sequence <= cursor) throw std::invalid_argument("Companion cursor regressed."); cursor = item.sequence; }
    if (page.next_after_sequence != cursor || (page.has_more && page.items.empty())) throw std::invalid_argument("Companion page did not advance its cursor.");
    if (reset) items = std::move(page.items); else items.insert(items.end(), std::make_move_iterator(page.items.begin()), std::make_move_iterator(page.items.end()));
    inbox_id = std::move(page.inbox_id); revision = page.revision; disk_digest = std::move(page.disk_digest); after = cursor; has_more = page.has_more; return true;
}
}
