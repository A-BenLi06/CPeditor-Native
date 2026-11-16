// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/session.hpp"
#include "cpn/digest.hpp"
#include "cpn/json.hpp"
#include "cpn/text.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <system_error>

namespace cpn {
namespace {
using Json = nlohmann::json;
namespace field {
constexpr auto schema_version = "schema_version";
constexpr auto workspace_id = "workspace_id";
constexpr auto revision = "revision";
constexpr auto selected_document_id = "selected_document_id";
constexpr auto documents = "documents";
constexpr auto editor_preferences = "editor_preferences";
constexpr auto document_id = "document_id";
constexpr auto source_text = "source_text";
constexpr auto source_digest = "source_digest";
constexpr auto original_path = "original_path";
constexpr auto language_id = "language_id";
constexpr auto toolchain_profile_id = "toolchain_profile_id";
constexpr auto save_point = "save_point";
constexpr auto disk_digest = "disk_digest";
constexpr auto tests = "tests";
constexpr auto id = "id";
constexpr auto input = "input";
constexpr auto expected = "expected";
constexpr auto enabled = "enabled";
constexpr auto encoding = "encoding";
constexpr auto data = "data";
constexpr auto namespace_id = "namespace_id";
constexpr auto object_json = "object_json";
}
constexpr std::uint64_t session_schema_version = 1;
class UnsupportedSchema final : public std::invalid_argument {
public:
    UnsupportedSchema() : std::invalid_argument("Unsupported workspace session schema version") {}
};
void bounds_valid(const SessionBounds& bounds) {
    if (!bounds.max_json_bytes || !bounds.max_json_depth || !bounds.max_preferences_depth ||
        !bounds.max_id_bytes || !bounds.max_path_bytes)
        throw std::invalid_argument("Invalid session bounds");
}
constexpr std::size_t max_detail_bytes=4096;
std::string_view detail_view(std::string_view text) {
    auto end = std::min(max_detail_bytes, text.size());
    if (end < text.size()) while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
    return text.substr(0, end);
}
std::string detail(std::string_view text) {return std::string(detail_view(text));}
Json parse_json(std::string_view text, std::size_t bytes, std::size_t depth) {
    if (text.size() > bytes) throw std::invalid_argument("Session JSON exceeds byte limit");
    std::vector<std::set<std::string>> keys;
    return Json::parse(text, [&](int current_depth, Json::parse_event_t event, Json& parsed) {
        if (static_cast<std::size_t>(current_depth) > depth) throw std::invalid_argument("Session JSON exceeds nesting limit");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::object_end) keys.pop_back();
        else if (event == Json::parse_event_t::key && !keys.back().insert(parsed.get<std::string>()).second)
            throw std::invalid_argument("Duplicate session JSON field");
        return true;
    });
}
void fields(const Json& object, std::initializer_list<std::string_view> allowed) {
    if (!object.is_object()) throw std::invalid_argument("Session field must be an object");
    for (const auto& [key, value] : object.items()) {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
            throw std::invalid_argument("Unknown session field: " + key);
    }
}
std::uint64_t integer(const Json& value) {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
        throw std::invalid_argument("Session version/revision must be a nonnegative integer");
    return value.get<std::uint64_t>();
}
std::string text(const Json& value) {
    if (!value.is_string()) throw std::invalid_argument("Session field must be a string");
    return value.get<std::string>();
}
std::string bounded_text(const Json& value, std::size_t maximum) {
    if (!value.is_string() || value.get_ref<const Json::string_t&>().size() > maximum)
        throw std::invalid_argument("Session string exceeds its limit or type");
    return value.get<std::string>();
}
void utf8(std::string_view value, std::size_t maximum, bool nonempty = false, bool allow_nul = false) {
    if (value.size() > maximum || (nonempty && value.empty()) ||
        (!allow_nul && value.find('\0') != std::string_view::npos) || decode_utf8(value).error_offset)
        throw std::invalid_argument("Invalid session UTF-8 string or byte limit");
}
bool digest_valid(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char byte) {
        return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
    });
}
void digest(std::string_view value) {
    if (!digest_valid(value)) throw std::invalid_argument("Invalid session SHA-256 digest");
}
void consume(std::size_t& total, std::size_t size, std::size_t maximum) {
    if (size > maximum - total) throw std::invalid_argument("Session aggregate limit exceeded");
    total += size;
}
std::optional<std::string> optional_text(const Json& object, const char* key) {
    auto found = object.find(key);
    return found == object.end() || found->is_null() ? std::nullopt : std::optional<std::string>{text(*found)};
}
std::string raw(const Json& value, std::size_t maximum) {
    if (value.is_string()) {
        return bounded_text(value, maximum);
    }
    fields(value, {field::encoding, field::data});
    if (text(value.at(field::encoding)) != "base64") throw std::invalid_argument("Unsupported test byte encoding");
    const auto& encoded_value = value.at(field::data);
    if (!encoded_value.is_string()) throw std::invalid_argument("Base64 data must be a string");
    const auto& encoded = encoded_value.get_ref<const Json::string_t&>();
    if (encoded.size() % 4) throw std::invalid_argument("Invalid base64 length");
    auto size = encoded.size() / 4 * 3;
    if (!encoded.empty() && encoded.back() == '=') --size;
    if (encoded.size() >= 2 && encoded[encoded.size() - 2] == '=' && size) --size;
    if (size > maximum) throw std::invalid_argument("Test bytes exceed limit");
    return decode_base64(encoded);
}
Json bytes(std::string_view value) {
    return Json{{field::encoding, "base64"}, {field::data, encode_base64(value)}};
}
std::vector<NativeEditorPreferences> parse_preferences(const Json& owner, const SessionBounds& bounds, std::size_t& aggregate) {
    std::vector<NativeEditorPreferences> result;
    const auto found = owner.find(field::editor_preferences);
    if (found == owner.end()) return result;
    if (!found->is_array() || found->size() > bounds.max_preferences_per_owner)
        throw std::invalid_argument("Invalid editor preferences array count/type");
    for (const auto& preference : *found) {
        fields(preference, {field::namespace_id, field::schema_version, field::object_json});
        auto object = bounded_text(preference.at(field::object_json), bounds.max_preferences_bytes);
        consume(aggregate, object.size(), bounds.max_total_preferences_bytes);
        result.push_back({bounded_text(preference.at(field::namespace_id), bounds.max_id_bytes), integer(preference.at(field::schema_version)), std::move(object)});
    }
    return result;
}
Json preferences(const std::vector<NativeEditorPreferences>& values) {
    auto result = Json::array();
    for (const auto& value : values) result.push_back({{field::namespace_id, value.namespace_id},
        {field::schema_version, value.schema_version}, {field::object_json, value.object_json}});
    return result;
}
void json_depth(const Json& value, std::size_t maximum, std::size_t current = 0) {
    if (current > maximum) throw std::invalid_argument("Serialized session exceeds nesting limit");
    if (value.is_structured()) for (const auto& child : value) json_depth(child, maximum, current + 1);
}
void validate_preferences(const std::vector<NativeEditorPreferences>& values, const SessionBounds& bounds, std::size_t& aggregate) {
    if (values.size() > bounds.max_preferences_per_owner) throw std::invalid_argument("Too many native editor preferences");
    std::set<std::pair<std::string, std::uint64_t>> namespaces;
    for (const auto& value : values) {
        utf8(value.namespace_id, bounds.max_id_bytes, true);
        if (!value.schema_version || !namespaces.emplace(value.namespace_id, value.schema_version).second)
            throw std::invalid_argument("Invalid or duplicate native preference namespace/version");
        consume(aggregate, value.object_json.size(), bounds.max_total_preferences_bytes);
        const auto object = parse_json(value.object_json, bounds.max_preferences_bytes, bounds.max_preferences_depth);
        if (!object.is_object()) throw std::invalid_argument("Opaque native preferences must contain a JSON object");
    }
}
void validate(const WorkspaceSession& value, const SessionBounds& bounds) {
    bounds_valid(bounds);
    utf8(value.workspace_id, bounds.max_id_bytes, true);
    if (value.documents.size() > bounds.max_documents) throw std::invalid_argument("Too many session documents");
    std::size_t content = 0, test_count = 0, preference_bytes = 0;
    std::set<std::string> document_ids;
    for (const auto& document : value.documents) {
        utf8(document.document_id, bounds.max_id_bytes, true);
        if (!document_ids.insert(document.document_id).second) throw std::invalid_argument("Duplicate session document ID");
        utf8(document.source_text, bounds.max_source_bytes, false, true);
        consume(content, document.source_text.size(), bounds.max_total_content_bytes);
        if (document.original_path) utf8(path_to_utf8(*document.original_path), bounds.max_path_bytes, true);
        if (document.language_id) utf8(*document.language_id, bounds.max_id_bytes, true);
        if (document.toolchain_profile_id) utf8(*document.toolchain_profile_id, bounds.max_id_bytes, true);
        if (document.save_point) {
            const auto& point = *document.save_point;
            if (!document.original_path || point.revision > document.revision) throw std::invalid_argument("Invalid document save point");
            digest(point.source_digest); if (point.disk_digest) digest(*point.disk_digest);
            if (point.revision == document.revision && point.source_digest != sha256(document.source_text))
                throw std::invalid_argument("Save point conflicts with source revision");
        }
        if (document.tests.size() > bounds.max_tests_per_document) throw std::invalid_argument("Too many document testcases");
        consume(test_count, document.tests.size(), bounds.max_total_tests);
        std::set<std::string> ids;
        for (const auto& test : document.tests) {
            utf8(test.id, bounds.max_id_bytes, true);
            if (!ids.insert(test.id).second) throw std::invalid_argument("Duplicate document testcase ID");
            if (test.input.size() > bounds.max_test_bytes || (test.expected && test.expected->size() > bounds.max_test_bytes))
                throw std::invalid_argument("Test bytes exceed limit");
            consume(content, test.input.size(), bounds.max_total_content_bytes);
            if (test.expected) consume(content, test.expected->size(), bounds.max_total_content_bytes);
        }
        validate_preferences(document.editor_preferences, bounds, preference_bytes);
    }
    if (value.selected_document_id) {
        utf8(*value.selected_document_id, bounds.max_id_bytes, true);
        if (!document_ids.contains(*value.selected_document_id)) throw std::invalid_argument("Selected session document does not exist");
    }
    validate_preferences(value.editor_preferences, bounds, preference_bytes);
}
std::string path_key(const std::filesystem::path& path) {
    auto key = path_to_utf8(std::filesystem::weakly_canonical(std::filesystem::absolute(path)));
    for (auto& byte : key) if (byte >= 'A' && byte <= 'Z') byte = static_cast<char>(byte - 'A' + 'a');
    return key;
}
bool aliases(const std::filesystem::path& left, const std::filesystem::path& right) {
    if (path_key(left) == path_key(right)) return true;
    const auto exists = [](const std::filesystem::path& path) {
        std::error_code error;
        const auto present = std::filesystem::exists(path, error);
        if (error && error != std::errc::no_such_file_or_directory) throw std::system_error(error);
        return present;
    };
    if (!exists(left) || !exists(right)) return false;
    std::error_code error;
    const auto equivalent = std::filesystem::equivalent(left, right, error);
    if (error && error != std::errc::no_such_file_or_directory) throw std::system_error(error);
    return !error && equivalent;
}
void storage_path(const std::filesystem::path& path, const SessionBounds& bounds) {
    utf8(path_to_utf8(path), bounds.max_path_bytes, true);
    std::error_code error;
    const auto state = std::filesystem::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory) throw std::system_error(error);
    if (!error && state.type() != std::filesystem::file_type::not_found && state.type() != std::filesystem::file_type::regular)
        throw std::invalid_argument("Session storage path must be a regular file or missing");
}
void separate_from_sources(const std::vector<std::filesystem::path>& paths, const WorkspaceSession& session) {
    for (const auto& document : session.documents) if (document.original_path)
        for (const auto& path : paths) if (aliases(path, *document.original_path))
            throw std::invalid_argument("Recovery, backup and lock paths must be separate from user source files");
}
void storage_paths(const SessionPaths& paths, const WorkspaceSession& session, const SessionBounds& bounds) {
    const std::vector<std::filesystem::path> values{paths.recovery_file, paths.backup_file, paths.lock_file};
    for (const auto& path : values) storage_path(path, bounds);
    for (std::size_t left = 0; left < values.size(); ++left)
        for (std::size_t right = left + 1; right < values.size(); ++right)
            if (aliases(values[left], values[right])) throw std::invalid_argument("Session storage paths must be distinct");
    separate_from_sources(values, session);
}
}
WorkspaceSession parse_workspace_session(std::string_view bytes, SessionBounds bounds) {
    bounds_valid(bounds);
    const auto object = parse_json(bytes, bounds.max_json_bytes, bounds.max_json_depth);
    if (!object.is_object()) throw std::invalid_argument("Workspace session must be an object");
    if (integer(object.at(field::schema_version)) != session_schema_version) throw UnsupportedSchema{};
    fields(object, {field::schema_version, field::workspace_id, field::revision, field::selected_document_id,
                    field::documents, field::editor_preferences});
    WorkspaceSession result;
    result.workspace_id = bounded_text(object.at(field::workspace_id), bounds.max_id_bytes); result.revision = integer(object.at(field::revision));
    result.selected_document_id = optional_text(object, field::selected_document_id);
    const auto& documents = object.at(field::documents);
    if (!documents.is_array() || documents.size() > bounds.max_documents) throw std::invalid_argument("Invalid session documents array");
    std::size_t test_count = 0, content = 0, preference_bytes = 0;
    for (const auto& document : documents) {
        fields(document, {field::document_id, field::revision, field::source_text, field::source_digest, field::original_path,
                          field::language_id, field::toolchain_profile_id, field::save_point, field::tests, field::editor_preferences});
        SessionDocument value;
        value.document_id = bounded_text(document.at(field::document_id), bounds.max_id_bytes); value.revision = integer(document.at(field::revision));
        value.source_text = bounded_text(document.at(field::source_text), bounds.max_source_bytes);
        consume(content, value.source_text.size(), bounds.max_total_content_bytes);
        if (text(document.at(field::source_digest)) != sha256(value.source_text)) throw std::invalid_argument("Session source checksum mismatch");
        if (const auto path = optional_text(document, field::original_path)) {
            utf8(*path, bounds.max_path_bytes, true); value.original_path = path_from_utf8(*path);
        }
        value.language_id = optional_text(document, field::language_id); value.toolchain_profile_id = optional_text(document, field::toolchain_profile_id);
        if (const auto point = document.find(field::save_point); point != document.end() && !point->is_null()) {
            fields(*point, {field::revision, field::source_digest, field::disk_digest});
            value.save_point = DocumentSavePoint{integer(point->at(field::revision)), text(point->at(field::source_digest)), optional_text(*point, field::disk_digest)};
        }
        const auto& tests = document.at(field::tests);
        if (!tests.is_array() || tests.size() > bounds.max_tests_per_document) throw std::invalid_argument("Invalid document tests array");
        consume(test_count, tests.size(), bounds.max_total_tests);
        for (const auto& test : tests) {
            fields(test, {field::id, field::input, field::expected, field::enabled});
            TestCase sample; sample.id = bounded_text(test.at(field::id), bounds.max_id_bytes); sample.input = raw(test.at(field::input), bounds.max_test_bytes);
            consume(content, sample.input.size(), bounds.max_total_content_bytes);
            if (const auto expected = test.find(field::expected); expected != test.end() && !expected->is_null()) sample.expected = raw(*expected, bounds.max_test_bytes);
            if (sample.expected) consume(content, sample.expected->size(), bounds.max_total_content_bytes);
            const auto& enabled = test.at(field::enabled);
            if (!enabled.is_boolean()) throw std::invalid_argument("Test enabled flag must be boolean");
            sample.enabled = enabled.get<bool>(); value.tests.push_back(std::move(sample));
        }
        value.editor_preferences = parse_preferences(document, bounds, preference_bytes); result.documents.push_back(std::move(value));
    }
    result.editor_preferences = parse_preferences(object, bounds, preference_bytes);
    validate(result, bounds);
    return result;
}
std::string serialize_workspace_session(const WorkspaceSession& value, SessionBounds bounds) {
    validate(value, bounds);
    auto object = Json{{field::schema_version, session_schema_version}, {field::workspace_id, value.workspace_id},
        {field::revision, value.revision}, {field::selected_document_id, value.selected_document_id ? Json(*value.selected_document_id) : Json(nullptr)},
        {field::documents, Json::array()}, {field::editor_preferences, preferences(value.editor_preferences)}};
    for (const auto& document : value.documents) {
        auto row = Json{{field::document_id, document.document_id}, {field::revision, document.revision},
            {field::source_text, document.source_text}, {field::source_digest, sha256(document.source_text)},
            {field::original_path, document.original_path ? Json(path_to_utf8(*document.original_path)) : Json(nullptr)},
            {field::language_id, document.language_id ? Json(*document.language_id) : Json(nullptr)},
            {field::toolchain_profile_id, document.toolchain_profile_id ? Json(*document.toolchain_profile_id) : Json(nullptr)},
            {field::save_point, nullptr}, {field::tests, Json::array()}, {field::editor_preferences, preferences(document.editor_preferences)}};
        if (document.save_point) row[field::save_point] = {{field::revision, document.save_point->revision},
            {field::source_digest, document.save_point->source_digest},
            {field::disk_digest, document.save_point->disk_digest ? Json(*document.save_point->disk_digest) : Json(nullptr)}};
        for (const auto& test : document.tests) row[field::tests].push_back({{field::id, test.id}, {field::input, bytes(test.input)},
            {field::expected, test.expected ? bytes(*test.expected) : Json(nullptr)}, {field::enabled, test.enabled}});
        object[field::documents].push_back(std::move(row));
    }
    json_depth(object, bounds.max_json_depth);
    auto result = object.dump(2, ' ', false, Json::error_handler_t::strict) + "\n";
    if (result.size() > bounds.max_json_bytes) throw std::invalid_argument("Serialized session exceeds byte limit");
    return result;
}
SessionService::SessionService(std::shared_ptr<FileStore> files, SessionBounds bounds) : files_(std::move(files)), bounds_(bounds) {
    if (!files_) throw std::invalid_argument("Missing session FileStore");
    bounds_valid(bounds_);
}
SessionLoadResult SessionService::load(const std::filesystem::path& path) const {
    std::lock_guard guard(mutex_);
    SessionLoadResult result;
    try { storage_path(path, bounds_); }
    catch (const std::invalid_argument& error) { result.status = SessionLoadStatus::invalid; result.detail = detail(error.what()); return result; }
    catch (const std::exception& error) { result.detail = detail(error.what()); return result; }
    std::optional<std::string> bytes;
    try { bytes = files_->read_optional(path, bounds_.max_json_bytes); }
    catch (const std::exception& error) { result.detail = detail(error.what()); return result; }
    if (!bytes) { result.status = SessionLoadStatus::missing; return result; }
    result.disk_digest = sha256(*bytes);
    try {
        auto candidate = parse_workspace_session(*bytes, bounds_);
        separate_from_sources({path}, candidate);
        result.session = std::move(candidate); result.status = SessionLoadStatus::loaded;
    } catch (const UnsupportedSchema& error) { result.status = SessionLoadStatus::unsupported_schema; result.detail = detail(error.what()); }
    catch (const std::exception& error) { result.status = SessionLoadStatus::invalid; result.detail = detail(error.what()); }
    return result;
}
SessionSaveResult SessionService::save(const SessionPaths& paths, const WorkspaceSession& candidate,
    std::optional<std::string> expected_disk_digest, std::chrono::milliseconds lock_timeout, std::stop_token stop) const {
    std::lock_guard guard(mutex_);
    SessionSaveResult result;
    result.detail.reserve(max_detail_bytes);
    std::string replacement;
    try {
        replacement = serialize_workspace_session(candidate, bounds_);
        storage_paths(paths, candidate, bounds_);
        if (expected_disk_digest) digest(*expected_disk_digest);
        if (lock_timeout.count() < 0) throw std::invalid_argument("Invalid session lock timeout");
    } catch (const std::system_error& error) { result.detail = detail(error.what()); return result; }
    catch (const std::exception& error) { result.status = SessionSaveStatus::invalid_session; result.detail = detail(error.what()); return result; }
    if (stop.stop_requested()) { result.status = SessionSaveStatus::cancelled; return result; }
    std::unique_ptr<FileLock> lock;
    try { lock = files_->acquire_lock(paths.lock_file, lock_timeout, stop); }
    catch (const std::system_error& error) {
        result.status = error.code() == std::errc::operation_canceled ? SessionSaveStatus::cancelled :
                        error.code() == std::errc::timed_out ? SessionSaveStatus::lock_unavailable : SessionSaveStatus::io_failed;
        result.detail = detail(error.what()); return result;
    } catch (const std::invalid_argument& error) { result.status = SessionSaveStatus::invalid_session; result.detail = detail(error.what()); return result; }
    catch (const std::exception& error) { result.detail = detail(error.what()); return result; }
    if (!lock) { result.detail = "FileStore returned no session lock"; return result; }
    try { storage_paths(paths, candidate, bounds_); }
    catch (const std::exception& error) { result.status = SessionSaveStatus::invalid_session; result.detail = detail(error.what()); return result; }
    std::optional<std::string> previous;
    try { previous = files_->read_optional(paths.recovery_file, bounds_.max_json_bytes); }
    catch (const std::exception& error) { result.detail = detail(error.what()); return result; }
    if (previous) result.disk_digest = sha256(*previous);
    try {
        if (previous) {
            const auto existing = parse_workspace_session(*previous, bounds_);
            storage_paths(paths, existing, bounds_);
        }
    } catch (const std::system_error& error) { result.detail = detail(error.what()); return result; }
    catch (const std::exception& error) { result.status = SessionSaveStatus::invalid_existing; result.detail = detail(error.what()); return result; }
    if (result.disk_digest != expected_disk_digest) { result.status = SessionSaveStatus::conflict; return result; }
    std::optional<std::string> backup;
    try { backup = files_->read_optional(paths.backup_file, bounds_.max_json_bytes); }
    catch (const std::exception& error) { result.detail = detail(error.what()); return result; }
    try {
        if (backup) {
            const auto existing = parse_workspace_session(*backup, bounds_);
            storage_paths(paths, existing, bounds_);
        }
    } catch (const std::system_error& error) { result.detail = detail(error.what()); return result; }
    catch (const std::exception& error) { result.status = SessionSaveStatus::invalid_existing; result.detail = detail(error.what()); return result; }
    if (stop.stop_requested()) { result.status = SessionSaveStatus::cancelled; return result; }
    auto replacement_digest = sha256(replacement);
    if (previous) {
        try { files_->write_atomic(paths.backup_file, *previous); result.backup_replaced = true; }
        catch (const AtomicWriteError& error) {
            result.backup_replaced = error.destination_replaced;
            result.status = error.destination_replaced ? SessionSaveStatus::durability_unknown : SessionSaveStatus::io_failed;
            result.detail.assign(detail_view(error.what())); return result;
        } catch (const std::exception& error) { result.detail.assign(detail_view(error.what())); return result; }
    }
    if (stop.stop_requested()) { result.status = SessionSaveStatus::cancelled; return result; }
    // Recheck after backup I/O. A writer bypassing the shared lock can still race the final replace.
    try {
        const auto current = files_->read_optional(paths.recovery_file, bounds_.max_json_bytes);
        const auto current_digest = current ? std::optional<std::string>{sha256(*current)} : std::nullopt;
        if (current_digest != result.disk_digest) { result.disk_digest = current_digest; result.status = SessionSaveStatus::conflict; return result; }
    } catch (const std::exception& error) { result.detail.assign(detail_view(error.what())); return result; }
    if (stop.stop_requested()) { result.status = SessionSaveStatus::cancelled; return result; }
    try {
        files_->write_atomic(paths.recovery_file, replacement);
        result.recovery_replaced = true; result.disk_digest = std::move(replacement_digest); result.status = SessionSaveStatus::saved;
    } catch (const AtomicWriteError& error) {
        result.recovery_replaced = error.destination_replaced;
        result.status = error.destination_replaced ? SessionSaveStatus::durability_unknown : SessionSaveStatus::io_failed;
        if (error.destination_replaced) result.disk_digest = std::move(replacement_digest);
        result.detail.assign(detail_view(error.what()));
    } catch (const std::exception& error) { result.detail.assign(detail_view(error.what())); }
    return result;
}
const char* session_load_status_name(SessionLoadStatus status) noexcept {
    constexpr const char* names[]{"Loaded", "Missing", "Invalid", "UnsupportedSchema", "IOFailed"};
    const auto index = static_cast<unsigned>(status); return index < std::size(names) ? names[index] : "Unknown";
}
const char* session_save_status_name(SessionSaveStatus status) noexcept {
    constexpr const char* names[]{"Saved", "Conflict", "InvalidSession", "InvalidExisting", "IOFailed", "DurabilityUnknown", "LockUnavailable", "Cancelled"};
    const auto index = static_cast<unsigned>(status); return index < std::size(names) ? names[index] : "Unknown";
}
} // namespace cpn
