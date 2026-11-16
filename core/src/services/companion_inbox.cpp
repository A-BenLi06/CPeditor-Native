// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/companion_inbox.hpp"
#include "cpn/digest.hpp"
#include "cpn/text.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace cpn {
namespace {
using Json = nlohmann::json;
namespace field {
constexpr auto schema_version = "schema_version";
constexpr auto inbox_id = "inbox_id";
constexpr auto workspace_id = "workspace_id";
constexpr auto revision = "revision";
constexpr auto next_sequence = "next_sequence";
constexpr auto items = "items";
constexpr auto sequence = "sequence";
constexpr auto state = "state";
constexpr auto import_id = "import_id";
constexpr auto problem_id = "problem_id";
constexpr auto document_id = "document_id";
constexpr auto test_ids = "test_ids";
constexpr auto previous_import_id = "previous_import_id";
constexpr auto payload_digest = "payload_digest";
constexpr auto original_json = "original_json";
constexpr auto applied_session_digest = "applied_session_digest";
constexpr auto applied_session_revision = "applied_session_revision";
constexpr auto receipts = "receipts";
constexpr auto ordinary_stdio_compatible = "ordinary_stdio_compatible";
}
class UnsupportedInbox final : public std::invalid_argument {
public:
    UnsupportedInbox() : std::invalid_argument("Unsupported Companion inbox schema version") {}
};
class FuturePreference final : public std::invalid_argument {
public:
    FuturePreference() : std::invalid_argument("Unsupported Companion import preference version") {}
};
class InboxFailure final : public std::invalid_argument {
public:
    InboxFailure(CompanionInboxWriteStatus status, std::string detail)
        : std::invalid_argument(std::move(detail)), status(status) {}
    const CompanionInboxWriteStatus status;
};
[[noreturn]] void fail(CompanionInboxWriteStatus status, std::string detail) {
    throw InboxFailure(status, std::move(detail));
}
constexpr std::size_t max_detail_bytes=4096;
std::string_view detail_view(std::string_view value) {
    auto length = std::min(max_detail_bytes, value.size());
    if (length < value.size()) while (length && (static_cast<unsigned char>(value[length]) & 0xc0) == 0x80) --length;
    return value.substr(0, length);
}
std::string detail(std::string_view value) {return std::string(detail_view(value));}
void check_bounds(const CompanionInboxBounds& bounds) {
    if (!bounds.max_json_bytes || !bounds.max_json_depth || !bounds.max_json_nodes ||
        bounds.max_id_bytes < 64 || !bounds.max_path_bytes)
        throw std::invalid_argument("Invalid Companion inbox bounds");
}
void text_identity(std::string_view value, std::size_t maximum) {
    if (value.empty() || value.size() > maximum || value.find('\0') != std::string_view::npos || decode_utf8(value).error_offset)
        throw std::invalid_argument("Invalid Companion identity or byte limit");
}
void digest(std::string_view value) {
    if (value.size() != 64 || !std::all_of(value.begin(), value.end(), [](char byte) {
        return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
    })) throw std::invalid_argument("Invalid Companion SHA-256 identity/digest");
}
void consume(std::size_t size, std::size_t maximum, std::size_t& total) {
    if (total > maximum || size > maximum - total)
        fail(CompanionInboxWriteStatus::capacity_exceeded, "Companion inbox exceeds aggregate capacity");
    total += size;
}
Json parse_json(std::string_view bytes, std::size_t maximum, std::size_t depth, std::size_t maximum_nodes) {
    if (bytes.size() > maximum) fail(CompanionInboxWriteStatus::capacity_exceeded, "Companion JSON exceeds byte capacity");
    std::vector<std::optional<std::set<std::string>>> containers;
    std::size_t nodes = 0;
    return Json::parse(bytes, [&](int, Json::parse_event_t event, Json& value) {
        const bool starts = event == Json::parse_event_t::object_start || event == Json::parse_event_t::array_start;
        const bool ends = event == Json::parse_event_t::object_end || event == Json::parse_event_t::array_end;
        if (starts || event == Json::parse_event_t::key || event == Json::parse_event_t::value) {
            if (nodes == maximum_nodes) fail(CompanionInboxWriteStatus::capacity_exceeded, "Companion JSON exceeds node capacity");
            ++nodes;
        }
        if (starts) {
            if (containers.size() >= depth) fail(CompanionInboxWriteStatus::capacity_exceeded, "Companion JSON exceeds depth capacity");
            if (event == Json::parse_event_t::object_start) containers.emplace_back(std::in_place);
            else containers.emplace_back(std::nullopt);
        } else if (ends) containers.pop_back();
        else if (event == Json::parse_event_t::key && !containers.back()->insert(value.get<std::string>()).second)
            throw std::invalid_argument("Duplicate Companion internal JSON field");
        return true;
    });
}
void fields(const Json& object, std::initializer_list<std::string_view> allowed) {
    if (!object.is_object()) throw std::invalid_argument("Companion internal value must be an object");
    for (const auto& [key, value] : object.items()) {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
            throw std::invalid_argument("Unknown Companion internal JSON field: " + key);
    }
}
std::uint64_t integer(const Json& value) {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
        throw std::invalid_argument("Companion internal integer must be a nonnegative uint64");
    return value.get<std::uint64_t>();
}
std::string text(const Json& value, std::size_t maximum) {
    if (!value.is_string() || value.get_ref<const Json::string_t&>().size() > maximum)
        throw std::invalid_argument("Companion internal string type/byte limit");
    return value.get<std::string>();
}
std::optional<std::string> optional_text(const Json& value, std::size_t maximum) {
    return value.is_null() ? std::nullopt : std::optional<std::string>{text(value, maximum)};
}
std::optional<std::uint64_t> optional_integer(const Json& value) {
    return value.is_null() ? std::nullopt : std::optional<std::uint64_t>{integer(value)};
}
void frame(std::string& output, std::string_view value) {
    output += std::to_string(value.size()); output += ':'; output += value;
}
std::string stable_id(std::string_view domain, std::string_view inbox_id, std::string_view first, std::string_view second = {}) {
    std::string input;
    frame(input, "CPEditorNative.CompanionIdentity.v1"); frame(input, domain); frame(input, inbox_id);
    frame(input, first); frame(input, second);
    return sha256(input);
}
std::string new_inbox_identity() {
    std::random_device random;
    constexpr char digits[] = "0123456789abcdef";
    std::string result(32, '0');
    for (std::size_t i = 0; i < 16; ++i) {
        const auto byte = static_cast<unsigned>(random()) & 0xff;
        result[2 * i] = digits[byte >> 4]; result[2 * i + 1] = digits[byte & 15];
    }
    return result;
}
bool same_candidate(const CompanionCandidate& left, const CompanionCandidate& right) {
    if (left.name != right.name || left.group != right.group || left.url != right.url ||
        left.source_limits.time_limit_ms != right.source_limits.time_limit_ms ||
        left.source_limits.memory_limit_mb != right.source_limits.memory_limit_mb ||
        left.tests.size() != right.tests.size() || left.interactive != right.interactive ||
        left.test_type != right.test_type || left.input.has_value() != right.input.has_value() ||
        left.output.has_value() != right.output.has_value() || left.java.has_value() != right.java.has_value() ||
        left.batch.has_value() != right.batch.has_value() || left.original_json != right.original_json ||
        left.ordinary_stdio_issues.size() != right.ordinary_stdio_issues.size()) return false;
    for (std::size_t i = 0; i < left.tests.size(); ++i)
        if (left.tests[i].input != right.tests[i].input || left.tests[i].output != right.tests[i].output) return false;
    if (left.input && (left.input->type != right.input->type || left.input->file_name != right.input->file_name || left.input->pattern != right.input->pattern)) return false;
    if (left.output && (left.output->type != right.output->type || left.output->file_name != right.output->file_name)) return false;
    if (left.java && (left.java->main_class != right.java->main_class || left.java->task_class != right.java->task_class)) return false;
    if (left.batch && (left.batch->id != right.batch->id || left.batch->size != right.batch->size)) return false;
    for (std::size_t i = 0; i < left.ordinary_stdio_issues.size(); ++i)
        if (left.ordinary_stdio_issues[i].code != right.ordinary_stdio_issues[i].code ||
            left.ordinary_stdio_issues[i].value != right.ordinary_stdio_issues[i].value) return false;
    return true;
}
void validate_item(const CompanionInboxItem& item, std::string_view inbox_id, CompanionBounds bounds, std::size_t maximum_id) {
    if (!item.sequence) throw std::invalid_argument("Companion import sequence must be positive");
    const auto candidate = parse_companion_payload(item.candidate.original_json, bounds);
    if (!same_candidate(candidate, item.candidate)) throw std::invalid_argument("Companion candidate differs from its original payload");
    digest(item.payload_digest);
    if (sha256(candidate.original_json) != item.payload_digest) throw std::invalid_argument("Companion payload checksum mismatch");
    const auto sequence = std::to_string(item.sequence);
    if (item.import_id != stable_id("import", inbox_id, sequence) ||
        item.document_id != stable_id("document", inbox_id, sequence) ||
        item.problem_id != stable_id("problem", inbox_id, candidate.url))
        throw std::invalid_argument("Companion stable identity mismatch");
    text_identity(item.import_id, maximum_id); text_identity(item.document_id, maximum_id); text_identity(item.problem_id, maximum_id);
    if (item.test_ids.size() != candidate.tests.size()) throw std::invalid_argument("Companion test identity count mismatch");
    for (std::size_t i = 0; i < item.test_ids.size(); ++i) {
        text_identity(item.test_ids[i], maximum_id);
        if (item.test_ids[i] != stable_id("test", inbox_id, sequence, std::to_string(i)))
            throw std::invalid_argument("Companion test identity mismatch");
    }
    if (item.previous_import_id) text_identity(*item.previous_import_id, maximum_id);
    if (item.state == CompanionInboxState::pending) {
        if (item.applied_session_digest || item.applied_session_revision) throw std::invalid_argument("Pending import cannot have an applied receipt");
    } else if (item.state == CompanionInboxState::applied) {
        if (!item.applied_session_digest || !item.applied_session_revision) throw std::invalid_argument("Applied import requires session evidence");
        digest(*item.applied_session_digest);
    } else throw std::invalid_argument("Unknown Companion import state");
}
struct BatchSummary { std::uint64_t size; std::set<std::string> urls; };
void validate_snapshot(const CompanionInboxSnapshot& snapshot, const CompanionInboxBounds& bounds) {
    check_bounds(bounds);
    text_identity(snapshot.inbox_id, bounds.max_id_bytes); text_identity(snapshot.workspace_id, bounds.max_id_bytes);
    if (!snapshot.next_sequence || snapshot.items.size() > bounds.max_records)
        fail(CompanionInboxWriteStatus::capacity_exceeded, "Invalid next sequence or Companion record capacity");
    if (snapshot.revision < snapshot.items.size()) throw std::invalid_argument("Companion revision is older than accepted records");
    std::size_t payload_bytes = 0, tests = 0, pending = 0;
    std::uint64_t last_sequence = 0;
    std::map<std::string, const CompanionInboxItem*> identities;
    std::set<std::string> document_ids, test_ids;
    std::map<std::string, BatchSummary> batches;
    for (const auto& item : snapshot.items) {
        validate_item(item, snapshot.inbox_id, bounds.payload, bounds.max_id_bytes);
        if (item.sequence <= last_sequence || item.sequence >= snapshot.next_sequence)
            throw std::invalid_argument("Invalid Companion sequence order/high-water mark");
        last_sequence = item.sequence;
        if (!identities.emplace(item.import_id, &item).second || !document_ids.insert(item.document_id).second)
            throw std::invalid_argument("Duplicate Companion import/document identity");
        for (const auto& id : item.test_ids) if (!test_ids.insert(id).second) throw std::invalid_argument("Duplicate Companion test identity");
        if (item.previous_import_id) {
            const auto previous = identities.find(*item.previous_import_id);
            if (previous == identities.end() || previous->second == &item || previous->second->problem_id != item.problem_id)
                throw std::invalid_argument("Invalid Companion previous import link");
        }
        consume(item.candidate.original_json.size(), bounds.max_total_payload_bytes, payload_bytes);
        consume(item.test_ids.size(), bounds.max_total_tests, tests);
        if (item.state == CompanionInboxState::pending && ++pending > bounds.max_pending)
            fail(CompanionInboxWriteStatus::capacity_exceeded, "Companion pending capacity exceeded");
        if (item.candidate.batch) {
            const auto& source = *item.candidate.batch;
            auto [position, inserted] = batches.try_emplace(source.id, BatchSummary{source.size, {}});
            (void)inserted;
            if (position->second.size != source.size) fail(CompanionInboxWriteStatus::batch_conflict, "Companion batch size changed");
            position->second.urls.insert(item.candidate.url);
            if (position->second.urls.size() > position->second.size)
                fail(CompanionInboxWriteStatus::batch_conflict, "Companion batch contains too many distinct URLs");
            if (batches.size() > bounds.max_batches) fail(CompanionInboxWriteStatus::capacity_exceeded, "Companion batch capacity exceeded");
        }
    }
}
bool same_delivery_group(const CompanionCandidate& left, const CompanionCandidate& right) {
    return left.url == right.url && left.batch.has_value() == right.batch.has_value() &&
        (!left.batch || left.batch->id == right.batch->id);
}
std::string path_key(const std::filesystem::path& path) {
    auto key = path_to_utf8(std::filesystem::weakly_canonical(std::filesystem::absolute(path)));
#if defined(_WIN32)
    for (auto& byte : key) if (byte >= 'A' && byte <= 'Z') byte = static_cast<char>(byte - 'A' + 'a');
#endif
    return key;
}
bool aliases(const std::filesystem::path& left, const std::filesystem::path& right) {
    if (path_key(left) == path_key(right)) return true;
    const auto exists = [](const std::filesystem::path& path) {
        std::error_code error;
        const bool present = std::filesystem::exists(path, error);
        if (error && error != std::errc::no_such_file_or_directory) throw std::system_error(error);
        return present;
    };
    if (!exists(left) || !exists(right)) return false;
    std::error_code error;
    const bool equivalent = std::filesystem::equivalent(left, right, error);
    if (error && error != std::errc::no_such_file_or_directory) throw std::system_error(error);
    return !error && equivalent;
}
void storage_path(const std::filesystem::path& path, std::size_t maximum) {
    text_identity(path_to_utf8(path), maximum);
    std::error_code error;
    const auto state = std::filesystem::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory) throw std::system_error(error);
    if (!error && state.type() != std::filesystem::file_type::not_found && state.type() != std::filesystem::file_type::regular)
        throw std::invalid_argument("Companion storage must be a regular file or missing");
}
std::vector<std::filesystem::path> inbox_paths(const CompanionInboxPaths& paths) {
    return {paths.inbox_file, paths.backup_file, paths.lock_file};
}
void storage_paths(const std::vector<std::filesystem::path>& paths, std::size_t maximum) {
    for (const auto& path : paths) storage_path(path, maximum);
    for (std::size_t left = 0; left < paths.size(); ++left)
        for (std::size_t right = left + 1; right < paths.size(); ++right)
            if (aliases(paths[left], paths[right])) throw std::invalid_argument("Companion storage paths must be distinct");
}
void separate_from_sources(const std::vector<std::filesystem::path>& paths, const WorkspaceSession& session) {
    for (const auto& document : session.documents) if (document.original_path)
        for (const auto& path : paths) if (aliases(path, *document.original_path))
            throw std::invalid_argument("Companion storage must be separate from source files");
}
std::unique_ptr<FileLock> acquire(const FileStore& files, const std::filesystem::path& path,
    std::chrono::milliseconds timeout, std::stop_token stop) {
    if (timeout.count() < 0) throw std::invalid_argument("Invalid Companion lock timeout");
    if (stop.stop_requested()) fail(CompanionInboxWriteStatus::cancelled, "Companion operation cancelled");
    try {
        auto lock = files.acquire_lock(path, timeout, stop);
        if (!lock) throw std::runtime_error("FileStore returned no Companion lock");
        return lock;
    } catch (const std::system_error& error) {
        if (error.code() == std::errc::operation_canceled) fail(CompanionInboxWriteStatus::cancelled, error.what());
        if (error.code() == std::errc::timed_out) fail(CompanionInboxWriteStatus::lock_unavailable, error.what());
        throw;
    }
}
struct ExistingInbox {
    std::optional<std::string> bytes;
    std::optional<std::string> disk_digest;
    std::optional<CompanionInboxSnapshot> snapshot;
};
void validate_backup_history(const CompanionInboxSnapshot& backup, const CompanionInboxSnapshot& current) {
    if (backup.revision > current.revision || backup.next_sequence > current.next_sequence || backup.items.size() > current.items.size())
        throw std::invalid_argument("Companion backup is ahead of the current inbox; explicit recovery is required");
    bool same_snapshot = backup.next_sequence == current.next_sequence && backup.items.size() == current.items.size();
    for (std::size_t i = 0; i < backup.items.size(); ++i) {
        const auto& prior = backup.items[i];
        const auto& latest = current.items[i];
        if (prior.sequence != latest.sequence || prior.import_id != latest.import_id || prior.problem_id != latest.problem_id ||
            prior.document_id != latest.document_id || prior.test_ids != latest.test_ids || prior.previous_import_id != latest.previous_import_id ||
            prior.payload_digest != latest.payload_digest || prior.candidate.original_json != latest.candidate.original_json)
            throw std::invalid_argument("Companion backup is not a prefix of the current import history");
        if (prior.state == CompanionInboxState::applied &&
            (latest.state != CompanionInboxState::applied || prior.applied_session_digest != latest.applied_session_digest ||
                prior.applied_session_revision != latest.applied_session_revision))
            throw std::invalid_argument("Companion Applied history/evidence has regressed or diverged");
        if (prior.state != latest.state || prior.applied_session_digest != latest.applied_session_digest ||
            prior.applied_session_revision != latest.applied_session_revision) same_snapshot = false;
    }
    if (backup.revision == current.revision && !same_snapshot)
        throw std::invalid_argument("Companion snapshots have different histories at the same revision");
}
ExistingInbox existing_inbox(const FileStore& files, const CompanionInboxPaths& paths,
    const CompanionInboxBounds& bounds, std::string_view workspace_id, bool validate_backup) {
    storage_paths(inbox_paths(paths), bounds.max_path_bytes);
    ExistingInbox result;
    result.bytes = files.read_optional(paths.inbox_file, bounds.max_json_bytes);
    if (result.bytes) {
        result.disk_digest = sha256(*result.bytes);
        result.snapshot = parse_companion_inbox(*result.bytes, bounds);
        if (result.snapshot->workspace_id != workspace_id)
            fail(CompanionInboxWriteStatus::identity_mismatch, "Companion inbox belongs to another workspace");
    }
    if (validate_backup) {
        const auto backup = files.read_optional(paths.backup_file, bounds.max_json_bytes);
        if (backup) {
            const auto previous = parse_companion_inbox(*backup, bounds);
            if (!result.snapshot) throw std::invalid_argument("Missing inbox with existing backup requires explicit recovery");
            if (previous.workspace_id != workspace_id || previous.inbox_id != result.snapshot->inbox_id)
                fail(CompanionInboxWriteStatus::identity_mismatch, "Companion backup identity mismatch");
            validate_backup_history(previous, *result.snapshot);
        }
    }
    return result;
}
CompanionInboxWriteResult commit(const FileStore& files, const CompanionInboxPaths& paths,
    const ExistingInbox& previous, const CompanionInboxSnapshot& candidate, CompanionInboxItem item,
    CompanionInboxWriteStatus success, const CompanionInboxBounds& bounds, std::stop_token stop) {
    CompanionInboxWriteResult result;
    result.detail.reserve(max_detail_bytes);
    result.disk_digest = previous.disk_digest;
    const auto replacement = serialize_companion_inbox(candidate, bounds);
    std::optional<std::string> replacement_digest{sha256(replacement)};
    if (stop.stop_requested()) { result.status = CompanionInboxWriteStatus::cancelled; return result; }
    if (previous.bytes) {
        try { files.write_atomic(paths.backup_file, *previous.bytes); result.backup_replaced = true; }
        catch (const AtomicWriteError& error) {
            result.backup_replaced = error.destination_replaced;
            result.status = error.destination_replaced ? CompanionInboxWriteStatus::durability_unknown : CompanionInboxWriteStatus::io_failed;
            result.detail.assign(detail_view(error.what())); return result;
        } catch (const std::exception& error) { result.detail.assign(detail_view(error.what())); return result; }
    }
    if (stop.stop_requested()) { result.status = CompanionInboxWriteStatus::cancelled; return result; }
    try {
        const auto current = files.read_optional(paths.inbox_file, bounds.max_json_bytes);
        const auto current_digest = current ? std::optional<std::string>{sha256(*current)} : std::nullopt;
        if (current_digest != previous.disk_digest) {
            result.disk_digest = current_digest; result.status = CompanionInboxWriteStatus::storage_conflict; return result;
        }
        storage_paths(inbox_paths(paths), bounds.max_path_bytes);
    } catch (const std::exception& error) { result.detail.assign(detail_view(error.what())); return result; }
    if (stop.stop_requested()) { result.status = CompanionInboxWriteStatus::cancelled; return result; }
    try {
        files.write_atomic(paths.inbox_file, replacement);
        result.inbox_replaced = true; result.disk_digest = std::move(replacement_digest);
        result.item = std::move(item); result.status = success;
    } catch (const AtomicWriteError& error) {
        result.inbox_replaced = error.destination_replaced;
        result.status = error.destination_replaced ? CompanionInboxWriteStatus::durability_unknown : CompanionInboxWriteStatus::io_failed;
        if (error.destination_replaced) { result.disk_digest = std::move(replacement_digest); result.item = std::move(item); }
        result.detail.assign(detail_view(error.what()));
    } catch (const std::exception& error) { result.detail.assign(detail_view(error.what())); }
    return result;
}
const NativeEditorPreferences* own_preference(const std::vector<NativeEditorPreferences>& preferences, std::string_view namespace_id) {
    const NativeEditorPreferences* found = nullptr;
    for (const auto& preference : preferences) if (preference.namespace_id == namespace_id) {
        if (preference.schema_version != companion_import_preference_schema_version) throw FuturePreference{};
        if (found) throw std::invalid_argument("Duplicate Companion preference");
        found = &preference;
    }
    return found;
}
void validate_receipt(const ImportApplicationReceipt& receipt, const ImportApplicationBounds& bounds) {
    text_identity(receipt.inbox_id, bounds.session.max_id_bytes);
    for (const auto* value : {&receipt.import_id, &receipt.problem_id, &receipt.document_id, &receipt.payload_digest}) {
        text_identity(*value, bounds.session.max_id_bytes); digest(*value);
    }
    if (receipt.test_ids.size() > bounds.session.max_tests_per_document) throw std::invalid_argument("Companion receipt test capacity exceeded");
    std::set<std::string> ids;
    for (const auto& id : receipt.test_ids) {
        text_identity(id, bounds.session.max_id_bytes); digest(id);
        if (!ids.insert(id).second) throw std::invalid_argument("Duplicate Companion receipt test identity");
    }
}
Json receipt_json(const ImportApplicationReceipt& receipt) {
    return Json{{field::inbox_id, receipt.inbox_id}, {field::import_id, receipt.import_id},
        {field::problem_id, receipt.problem_id}, {field::document_id, receipt.document_id},
        {field::payload_digest, receipt.payload_digest}, {field::test_ids, receipt.test_ids}};
}
ImportApplicationReceipt parse_receipt(const Json& value, const ImportApplicationBounds& bounds, bool provenance = false) {
    if (provenance) fields(value, {field::inbox_id, field::import_id, field::problem_id, field::document_id,
        field::payload_digest, field::test_ids, field::ordinary_stdio_compatible});
    else fields(value, {field::inbox_id, field::import_id, field::problem_id, field::document_id, field::payload_digest, field::test_ids});
    ImportApplicationReceipt receipt{text(value.at(field::inbox_id), bounds.session.max_id_bytes),
        text(value.at(field::import_id), bounds.session.max_id_bytes), text(value.at(field::problem_id), bounds.session.max_id_bytes),
        text(value.at(field::document_id), bounds.session.max_id_bytes), text(value.at(field::payload_digest), bounds.session.max_id_bytes), {}};
    const auto& tests = value.at(field::test_ids);
    if (!tests.is_array() || tests.size() > bounds.session.max_tests_per_document) throw std::invalid_argument("Invalid Companion receipt test array");
    for (const auto& id : tests) receipt.test_ids.push_back(text(id, bounds.session.max_id_bytes));
    validate_receipt(receipt, bounds); return receipt;
}
std::vector<ImportApplicationReceipt> receipt_preferences(const WorkspaceSession& session, const ImportApplicationBounds& bounds) {
    const auto* preference = own_preference(session.editor_preferences, companion_receipts_namespace);
    if (!preference) return {};
    const auto value = parse_json(preference->object_json, bounds.session.max_preferences_bytes, bounds.session.max_preferences_depth, 262144);
    fields(value, {field::receipts});
    const auto& values = value.at(field::receipts);
    if (!values.is_array() || values.size() > bounds.max_receipts) throw std::invalid_argument("Invalid Companion receipt capacity/type");
    std::vector<ImportApplicationReceipt> result;
    std::set<std::pair<std::string, std::string>> identities;
    for (const auto& value : values) {
        auto receipt = parse_receipt(value, bounds);
        if (!identities.emplace(receipt.inbox_id, receipt.import_id).second) throw std::invalid_argument("Duplicate Companion application receipt");
        result.push_back(std::move(receipt));
    }
    return result;
}
ImportApplicationReceipt receipt_for(const CompanionInboxSnapshot& snapshot, const CompanionInboxItem& item) {
    return {snapshot.inbox_id, item.import_id, item.problem_id, item.document_id, item.payload_digest, item.test_ids};
}
bool same_receipt(const ImportApplicationReceipt& left, const ImportApplicationReceipt& right) {
    return left.inbox_id == right.inbox_id && left.import_id == right.import_id && left.problem_id == right.problem_id &&
        left.document_id == right.document_id && left.payload_digest == right.payload_digest && left.test_ids == right.test_ids;
}
void upsert_preference(std::vector<NativeEditorPreferences>& preferences, std::string_view namespace_id, std::string object_json, const SessionBounds& bounds) {
    if (object_json.size() > bounds.max_preferences_bytes) fail(CompanionInboxWriteStatus::capacity_exceeded, "Companion preference byte capacity exceeded");
    for (auto& preference : preferences) if (preference.namespace_id == namespace_id) {
        if (preference.schema_version != companion_import_preference_schema_version) throw FuturePreference{};
        preference.object_json = std::move(object_json); return;
    }
    if (preferences.size() >= bounds.max_preferences_per_owner) fail(CompanionInboxWriteStatus::capacity_exceeded, "No free Companion preference namespace capacity");
    preferences.push_back({std::string(namespace_id), companion_import_preference_schema_version, std::move(object_json)});
}
Json item_json(const CompanionInboxItem& item) {
    return Json{{field::sequence, item.sequence}, {field::state, item.state == CompanionInboxState::pending ? "Pending" : "Applied"},
        {field::import_id, item.import_id}, {field::problem_id, item.problem_id}, {field::document_id, item.document_id}, {field::test_ids, item.test_ids},
        {field::previous_import_id, item.previous_import_id ? Json(*item.previous_import_id) : Json(nullptr)},
        {field::payload_digest, item.payload_digest}, {field::original_json, item.candidate.original_json},
        {field::applied_session_digest, item.applied_session_digest ? Json(*item.applied_session_digest) : Json(nullptr)},
        {field::applied_session_revision, item.applied_session_revision ? Json(*item.applied_session_revision) : Json(nullptr)}};
}
} // namespace

static CompanionInboxSnapshot parse_companion_inbox_impl(std::string_view bytes, CompanionInboxBounds bounds) {
    check_bounds(bounds);
    const auto value = parse_json(bytes, bounds.max_json_bytes, bounds.max_json_depth, bounds.max_json_nodes);
    if (!value.is_object()) throw std::invalid_argument("Companion inbox must be an object");
    if (integer(value.at(field::schema_version)) != companion_inbox_schema_version) throw UnsupportedInbox{};
    fields(value, {field::schema_version, field::inbox_id, field::workspace_id, field::revision, field::next_sequence, field::items});
    CompanionInboxSnapshot snapshot{text(value.at(field::inbox_id), bounds.max_id_bytes), text(value.at(field::workspace_id), bounds.max_id_bytes),
        integer(value.at(field::revision)), integer(value.at(field::next_sequence)), {}};
    const auto& items = value.at(field::items);
    if (!items.is_array() || items.size() > bounds.max_records) fail(CompanionInboxWriteStatus::capacity_exceeded, "Invalid Companion record array/capacity");
    std::size_t total_payload = 0, total_tests = 0;
    for (const auto& item : items) {
        fields(item, {field::sequence, field::state, field::import_id, field::problem_id, field::document_id, field::test_ids,
            field::previous_import_id, field::payload_digest, field::original_json, field::applied_session_digest, field::applied_session_revision});
        CompanionInboxItem record;
        record.sequence = integer(item.at(field::sequence));
        const auto state = text(item.at(field::state), 16);
        if (state == "Pending") record.state = CompanionInboxState::pending;
        else if (state == "Applied") record.state = CompanionInboxState::applied;
        else throw std::invalid_argument("Unknown Companion inbox state");
        record.import_id = text(item.at(field::import_id), bounds.max_id_bytes);
        record.problem_id = text(item.at(field::problem_id), bounds.max_id_bytes);
        record.document_id = text(item.at(field::document_id), bounds.max_id_bytes);
        record.previous_import_id = optional_text(item.at(field::previous_import_id), bounds.max_id_bytes);
        record.payload_digest = text(item.at(field::payload_digest), 64);
        const auto original = text(item.at(field::original_json), bounds.payload.max_json_bytes);
        consume(original.size(), bounds.max_total_payload_bytes, total_payload);
        record.candidate = parse_companion_payload(original, bounds.payload);
        const auto& tests = item.at(field::test_ids);
        if (!tests.is_array() || tests.size() > bounds.payload.max_tests) throw std::invalid_argument("Invalid Companion test identities array");
        consume(tests.size(), bounds.max_total_tests, total_tests);
        for (const auto& id : tests) record.test_ids.push_back(text(id, bounds.max_id_bytes));
        record.applied_session_digest = optional_text(item.at(field::applied_session_digest), 64);
        record.applied_session_revision = optional_integer(item.at(field::applied_session_revision));
        snapshot.items.push_back(std::move(record));
    }
    validate_snapshot(snapshot, bounds); return snapshot;
}
CompanionInboxSnapshot parse_companion_inbox(std::string_view bytes, CompanionInboxBounds bounds) {
    try { return parse_companion_inbox_impl(bytes, bounds); }
    catch (const Json::exception& error) { throw std::invalid_argument("Invalid Companion inbox JSON: " + detail(error.what())); }
}
std::string serialize_companion_inbox(const CompanionInboxSnapshot& snapshot, CompanionInboxBounds bounds) {
    validate_snapshot(snapshot, bounds);
    Json value{{field::schema_version, companion_inbox_schema_version}, {field::inbox_id, snapshot.inbox_id},
        {field::workspace_id, snapshot.workspace_id}, {field::revision, snapshot.revision}, {field::next_sequence, snapshot.next_sequence}, {field::items, Json::array()}};
    for (const auto& item : snapshot.items) value[field::items].push_back(item_json(item));
    auto output = value.dump(-1, ' ', false, Json::error_handler_t::strict) + "\n";
    if (output.size() > bounds.max_json_bytes) fail(CompanionInboxWriteStatus::capacity_exceeded, "Serialized Companion inbox exceeds byte capacity");
    // The serializer and parser use the same structural budgets.
    (void)parse_json(output, bounds.max_json_bytes, bounds.max_json_depth, bounds.max_json_nodes);
    return output;
}
std::string serialize_companion_inbox_item(const CompanionInboxItem& item, std::string_view inbox_id, CompanionInboxBounds bounds) {
    check_bounds(bounds); text_identity(inbox_id, bounds.max_id_bytes);
    validate_item(item, inbox_id, bounds.payload, bounds.max_id_bytes);
    auto result = item_json(item).dump(-1, ' ', false, Json::error_handler_t::strict);
    (void)parse_json(result, bounds.max_json_bytes, bounds.max_json_depth, bounds.max_json_nodes);
    return result;
}

std::optional<ImportApplicationReceipt> find_import_application_receipt(const WorkspaceSession& session,
    std::string_view inbox_id, std::string_view import_id, ImportApplicationBounds bounds) {
    for (auto& receipt : receipt_preferences(session, bounds))
        if (receipt.inbox_id == inbox_id && receipt.import_id == import_id) return std::move(receipt);
    return std::nullopt;
}
std::optional<CompanionDocumentProvenance> read_companion_document_provenance(const SessionDocument& document, ImportApplicationBounds bounds) {
    const auto* preference = own_preference(document.editor_preferences, companion_document_namespace);
    if (!preference) return std::nullopt;
    const auto value = parse_json(preference->object_json, bounds.session.max_preferences_bytes, bounds.session.max_preferences_depth, 262144);
    auto receipt = parse_receipt(value, bounds, true);
    if (receipt.document_id != document.document_id) throw std::invalid_argument("Companion document provenance identity mismatch");
    const auto& compatible = value.at(field::ordinary_stdio_compatible);
    if (!compatible.is_boolean()) throw std::invalid_argument("Companion stdio capability must be boolean");
    return CompanionDocumentProvenance{std::move(receipt), compatible.get<bool>()};
}
ImportPreparationResult prepare_import_application(const WorkspaceSession& session, const CompanionInboxSnapshot& snapshot,
    std::string_view import_id, const ImportDocumentDefaults& defaults, ImportApplicationBounds bounds) {
    ImportPreparationResult result;
    try { (void)serialize_workspace_session(session, bounds.session); }
    catch (const std::exception& error) { result.status = ImportPreparationStatus::invalid_session; result.detail = detail(error.what()); return result; }
    const CompanionInboxItem* item = nullptr;
    try {
        text_identity(snapshot.inbox_id, bounds.session.max_id_bytes); text_identity(snapshot.workspace_id, bounds.session.max_id_bytes);
        if (snapshot.workspace_id != session.workspace_id) throw std::invalid_argument("Companion workspace identity mismatch");
        for (const auto& candidate : snapshot.items) if (candidate.import_id == import_id) {
            if (item) throw std::invalid_argument("Duplicate requested Companion import identity");
            item = &candidate;
        }
        if (!item) throw std::invalid_argument("Unknown Companion import identity");
        validate_item(*item, snapshot.inbox_id, bounds.payload, bounds.session.max_id_bytes);
    } catch (const std::exception& error) { result.detail = detail(error.what()); return result; }
    try {
        auto receipts = receipt_preferences(session, bounds);
        const auto expected = receipt_for(snapshot, *item);
        for (const auto& receipt : receipts) if (receipt.inbox_id == expected.inbox_id && receipt.import_id == expected.import_id) {
            if (!same_receipt(receipt, expected)) {
                result.status = ImportPreparationStatus::identity_conflict; result.detail = "Companion application receipt differs from accepted import"; return result;
            }
            result.session = session; result.receipt = receipt; result.status = ImportPreparationStatus::already_applied; return result;
        }
        if (item->state == CompanionInboxState::applied) {
            result.status = ImportPreparationStatus::receipt_missing; result.detail = "Applied import has no receipt in this workspace recovery"; return result;
        }
        if (decode_utf8(defaults.source_text).error_offset) {
            result.status = ImportPreparationStatus::invalid_session; result.detail = "Invalid UTF-8 initial source buffer"; return result;
        }
        for (const auto* identity : {&defaults.language_id, &defaults.toolchain_profile_id}) if (*identity) {
            try { text_identity(**identity, bounds.session.max_id_bytes); }
            catch (const std::exception& error) { result.status = ImportPreparationStatus::invalid_session; result.detail = detail(error.what()); return result; }
        }
        if (receipts.size() >= bounds.max_receipts || session.documents.size() >= bounds.session.max_documents)
            fail(CompanionInboxWriteStatus::capacity_exceeded, "Companion application receipt/document capacity exceeded");
        if (session.revision == std::numeric_limits<std::uint64_t>::max()) {
            result.status = ImportPreparationStatus::revision_exhausted; return result;
        }
        for (const auto& document : session.documents) if (document.document_id == item->document_id) {
            result.status = ImportPreparationStatus::identity_conflict; result.detail = "Companion document exists without an application receipt"; return result;
        }
        WorkspaceSession candidate = session;
        SessionDocument document;
        document.document_id = item->document_id; document.source_text = defaults.source_text;
        document.language_id = defaults.language_id; document.toolchain_profile_id = defaults.toolchain_profile_id;
        document.editor_preferences = defaults.document_preferences;
        if (own_preference(document.editor_preferences, companion_document_namespace))
            throw std::invalid_argument("Initial document preferences already contain Companion provenance");
        for (std::size_t i = 0; i < item->candidate.tests.size(); ++i)
            document.tests.push_back({item->test_ids[i], item->candidate.tests[i].input, std::optional<std::string>{item->candidate.tests[i].output}, true});
        auto provenance = receipt_json(expected);
        provenance[field::ordinary_stdio_compatible] = item->candidate.compatible_with_ordinary_stdio();
        upsert_preference(document.editor_preferences, companion_document_namespace, provenance.dump(-1, ' ', false, Json::error_handler_t::strict), bounds.session);
        receipts.push_back(expected);
        Json stored = Json::array();
        for (const auto& receipt : receipts) stored.push_back(receipt_json(receipt));
        upsert_preference(candidate.editor_preferences, companion_receipts_namespace,
            Json{{field::receipts, std::move(stored)}}.dump(-1, ' ', false, Json::error_handler_t::strict), bounds.session);
        candidate.documents.push_back(std::move(document)); candidate.selected_document_id = item->document_id; ++candidate.revision;
        try { (void)serialize_workspace_session(candidate, bounds.session); }
        catch (const std::exception& error) {
            result.status = ImportPreparationStatus::capacity_exceeded; result.detail = detail(error.what()); return result;
        }
        result.receipt = expected; result.session = std::move(candidate); result.status = ImportPreparationStatus::prepared;
    } catch (const FuturePreference& error) { result.status = ImportPreparationStatus::incompatible_preferences; result.detail = detail(error.what()); }
    catch (const InboxFailure& error) { result.status = ImportPreparationStatus::capacity_exceeded; result.detail = detail(error.what()); }
    catch (const std::exception& error) { result.status = ImportPreparationStatus::incompatible_preferences; result.detail = detail(error.what()); }
    return result;
}

CompanionInboxService::CompanionInboxService(std::string workspace_id, std::shared_ptr<FileStore> files, CompanionInboxBounds bounds, IdFactory ids)
    : workspace_id_(std::move(workspace_id)), files_(std::move(files)), bounds_(bounds), ids_(std::move(ids)) {
    check_bounds(bounds_); text_identity(workspace_id_, bounds_.max_id_bytes);
    if (!files_) throw std::invalid_argument("Missing Companion FileStore");
    if (!ids_) ids_ = new_inbox_identity;
}
CompanionInboxLoadResult CompanionInboxService::load(const CompanionInboxPaths& paths) const {
    std::lock_guard guard(mutex_);
    CompanionInboxLoadResult result;
    try {
        storage_paths(inbox_paths(paths), bounds_.max_path_bytes);
        const auto bytes = files_->read_optional(paths.inbox_file, bounds_.max_json_bytes);
        if (!bytes) result.status = CompanionInboxLoadStatus::missing;
        else {
            result.disk_digest = sha256(*bytes);
            auto snapshot = parse_companion_inbox(*bytes, bounds_);
            if (snapshot.workspace_id != workspace_id_) throw std::invalid_argument("Companion inbox belongs to another workspace");
            result.snapshot = std::move(snapshot); result.status = CompanionInboxLoadStatus::loaded;
        }
    } catch (const UnsupportedInbox& error) { result.status = CompanionInboxLoadStatus::unsupported_schema; result.detail = detail(error.what()); }
    catch (const std::invalid_argument& error) { result.status = CompanionInboxLoadStatus::invalid; result.detail = detail(error.what()); }
    catch (const std::exception& error) { result.detail = detail(error.what()); }
    return result;
}
CompanionInboxWriteResult CompanionInboxService::accept(const CompanionInboxPaths& paths, std::string_view payload,
    std::chrono::milliseconds timeout, std::stop_token stop) const {
    std::lock_guard guard(mutex_);
    CompanionInboxWriteResult result;
    if (stop.stop_requested()) { result.status = CompanionInboxWriteStatus::cancelled; return result; }
    CompanionCandidate candidate;
    try { candidate = parse_companion_payload(payload, bounds_.payload); }
    catch (const std::exception& error) { result.status = CompanionInboxWriteStatus::invalid_payload; result.detail = detail(error.what()); return result; }
    try {
        storage_paths(inbox_paths(paths), bounds_.max_path_bytes);
        auto lock = acquire(*files_, paths.lock_file, timeout, stop);
        auto existing = existing_inbox(*files_, paths, bounds_, workspace_id_, true);
        result.disk_digest = existing.disk_digest;
        auto snapshot = existing.snapshot ? *existing.snapshot : CompanionInboxSnapshot{ids_(), workspace_id_, 0, 1, {}};
        text_identity(snapshot.inbox_id, bounds_.max_id_bytes);
        const auto payload_digest = sha256(candidate.original_json);
        for (const auto& item : snapshot.items) if (same_delivery_group(item.candidate, candidate) &&
            item.payload_digest == payload_digest && item.candidate.original_json == candidate.original_json) {
            result.status = CompanionInboxWriteStatus::duplicate; result.item = item; return result;
        }
        if (snapshot.next_sequence == std::numeric_limits<std::uint64_t>::max() || snapshot.revision == std::numeric_limits<std::uint64_t>::max())
            fail(CompanionInboxWriteStatus::id_exhausted, "Companion sequence/revision exhausted");
        CompanionInboxItem item;
        item.sequence = snapshot.next_sequence; item.candidate = std::move(candidate); item.payload_digest = payload_digest;
        const auto sequence = std::to_string(item.sequence);
        item.import_id = stable_id("import", snapshot.inbox_id, sequence);
        item.problem_id = stable_id("problem", snapshot.inbox_id, item.candidate.url);
        item.document_id = stable_id("document", snapshot.inbox_id, sequence);
        for (std::size_t i = 0; i < item.candidate.tests.size(); ++i) item.test_ids.push_back(stable_id("test", snapshot.inbox_id, sequence, std::to_string(i)));
        for (const auto& previous : snapshot.items) if (previous.problem_id == item.problem_id) item.previous_import_id = previous.import_id;
        ++snapshot.next_sequence; ++snapshot.revision; snapshot.items.push_back(item);
        validate_snapshot(snapshot, bounds_);
        return commit(*files_, paths, existing, snapshot, std::move(item), CompanionInboxWriteStatus::accepted, bounds_, stop);
    } catch (const UnsupportedInbox& error) { result.status = CompanionInboxWriteStatus::unsupported_schema; result.detail = detail(error.what()); }
    catch (const InboxFailure& error) { result.status = error.status; result.detail = detail(error.what()); }
    catch (const std::invalid_argument& error) { result.status = CompanionInboxWriteStatus::invalid_existing; result.detail = detail(error.what()); }
    catch (const std::exception& error) { result.detail = detail(error.what()); }
    return result;
}
CompanionInboxWriteResult CompanionInboxService::mark_applied(const CompanionInboxPaths& paths, const SessionPaths& session_paths,
    std::string_view import_id, std::chrono::milliseconds timeout, std::stop_token stop, ImportApplicationBounds application_bounds) const {
    std::lock_guard guard(mutex_);
    CompanionInboxWriteResult result;
    try {
        text_identity(import_id, bounds_.max_id_bytes);
        auto all_paths = inbox_paths(paths);
        all_paths.push_back(session_paths.recovery_file); all_paths.push_back(session_paths.backup_file); all_paths.push_back(session_paths.lock_file);
        storage_paths(all_paths, std::min(bounds_.max_path_bytes, application_bounds.session.max_path_bytes));
        auto inbox_lock = acquire(*files_, paths.lock_file, timeout, stop);
        auto existing = existing_inbox(*files_, paths, bounds_, workspace_id_, true);
        result.disk_digest = existing.disk_digest;
        if (!existing.snapshot) fail(CompanionInboxWriteStatus::identity_mismatch, "Missing Companion inbox");
        auto snapshot = *existing.snapshot;
        auto found = std::find_if(snapshot.items.begin(), snapshot.items.end(), [&](const auto& item) { return item.import_id == import_id; });
        if (found == snapshot.items.end()) fail(CompanionInboxWriteStatus::identity_mismatch, "Unknown Companion import");
        if (found->state == CompanionInboxState::applied) { result.status = CompanionInboxWriteStatus::already_applied; result.item = *found; return result; }
        auto session_lock = acquire(*files_, session_paths.lock_file, timeout, stop);
        storage_paths(all_paths, std::min(bounds_.max_path_bytes, application_bounds.session.max_path_bytes));
        const auto loaded = SessionService(files_, application_bounds.session).load(session_paths.recovery_file);
        if (loaded.status != SessionLoadStatus::loaded || !loaded.session || !loaded.disk_digest)
            fail(CompanionInboxWriteStatus::session_unavailable, "Cannot verify Companion receipt in a saved recovery: " + loaded.detail);
        if (loaded.session->workspace_id != workspace_id_)
            fail(CompanionInboxWriteStatus::identity_mismatch, "Companion recovery belongs to another workspace");
        separate_from_sources(all_paths, *loaded.session);
        std::optional<ImportApplicationReceipt> receipt;
        try { receipt = find_import_application_receipt(*loaded.session, snapshot.inbox_id, import_id, application_bounds); }
        catch (const std::exception& error) { fail(CompanionInboxWriteStatus::receipt_conflict, error.what()); }
        if (!receipt) fail(CompanionInboxWriteStatus::receipt_missing, "Companion application receipt is not saved");
        if (!same_receipt(*receipt, receipt_for(snapshot, *found))) fail(CompanionInboxWriteStatus::receipt_conflict, "Saved Companion receipt differs from accepted import");
        if (snapshot.revision == std::numeric_limits<std::uint64_t>::max()) fail(CompanionInboxWriteStatus::id_exhausted, "Companion revision exhausted");
        found->state = CompanionInboxState::applied; found->applied_session_digest = loaded.disk_digest;
        found->applied_session_revision = loaded.session->revision; ++snapshot.revision;
        return commit(*files_, paths, existing, snapshot, *found, CompanionInboxWriteStatus::applied, bounds_, stop);
    } catch (const UnsupportedInbox& error) { result.status = CompanionInboxWriteStatus::unsupported_schema; result.detail = detail(error.what()); }
    catch (const InboxFailure& error) { result.status = error.status; result.detail = detail(error.what()); }
    catch (const std::invalid_argument& error) { result.status = CompanionInboxWriteStatus::invalid_existing; result.detail = detail(error.what()); }
    catch (const std::exception& error) { result.detail = detail(error.what()); }
    return result;
}
const char* companion_inbox_load_status_name(CompanionInboxLoadStatus status) noexcept {
    constexpr const char* values[]{"Loaded", "Missing", "Invalid", "UnsupportedSchema", "IOFailed"};
    const auto index = static_cast<unsigned>(status); return index < std::size(values) ? values[index] : "Unknown";
}
const char* companion_inbox_write_status_name(CompanionInboxWriteStatus status) noexcept {
    constexpr const char* values[]{"Accepted", "Duplicate", "Applied", "AlreadyApplied", "InvalidPayload", "InvalidExisting", "UnsupportedSchema",
        "IdentityMismatch", "BatchConflict", "CapacityExceeded", "ReceiptMissing", "ReceiptConflict", "SessionUnavailable", "LockUnavailable", "Cancelled",
        "IOFailed", "DurabilityUnknown", "StorageConflict", "IdExhausted"};
    const auto index = static_cast<unsigned>(status); return index < std::size(values) ? values[index] : "Unknown";
}
const char* import_preparation_status_name(ImportPreparationStatus status) noexcept {
    constexpr const char* values[]{"Prepared", "AlreadyApplied", "InvalidItem", "InvalidSession", "IncompatiblePreferences", "CapacityExceeded",
        "IdentityConflict", "RevisionExhausted", "ReceiptMissing"};
    const auto index = static_cast<unsigned>(status); return index < std::size(values) ? values[index] : "Unknown";
}
} // namespace cpn
