// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/companion_inbox.hpp"
#include "cpn/digest.hpp"
#include "cpn/process.hpp"
#include <nlohmann/json.hpp>
#include <barrier>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>

namespace {
using Json = nlohmann::json;
using Write = cpn::CompanionInboxWriteStatus;
using Load = cpn::CompanionInboxLoadStatus;
using Prepare = cpn::ImportPreparationStatus;
void expect(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> void rejected(F operation, const char* message) {
    try { operation(); } catch (const std::exception&) { return; } throw std::runtime_error(message);
}
class Workspace {
public:
    explicit Workspace(std::shared_ptr<cpn::FileStore> files) : files_(std::move(files)), path(files_->create_workspace()) {}
    ~Workspace() { files_->remove_workspace(path); }
private:
    std::shared_ptr<cpn::FileStore> files_;
public:
    std::filesystem::path path;
};
cpn::CompanionInboxPaths paths_in(const std::filesystem::path& folder) {
    return {folder / cpn::path_from_utf8("Companion 收件箱.json"), folder / cpn::path_from_utf8("Companion 此前.json"), folder / "inbox.lock"};
}
cpn::SessionPaths session_paths_in(const std::filesystem::path& folder) {
    return {folder / cpn::path_from_utf8("恢复 工作区.json"), folder / "session.backup.json", folder / "session.lock"};
}
std::string payload(std::string_view url = "https://example.test/problem/a", std::string_view output = "") {
    auto value = Json{{"name", "题目 e\u0301 😀"}, {"group", "opaque group"}, {"url", url},
        {"timeLimit", std::numeric_limits<std::uint64_t>::max()}, {"memoryLimit", 256},
        {"tests", {{{"input", std::string("a\0中\r\n", 7)}, {"output", output}}}}};
    auto encoded = value.dump(-1, ' ', false, Json::error_handler_t::strict);
    // The unknown value exceeds uint64 and its number lexeme must never come from DOM dump.
    encoded.pop_back(); encoded += R"(,"unknown":{"integer":18446744073709551616,"float":1.000e0,"raw":"e\u0301"}})";
    return encoded;
}
std::string batched(std::string_view url, std::string_view batch_id, std::uint64_t size, std::string_view output = "") {
    auto value = Json::parse(payload(url, output)); value["batch"] = {{"id", batch_id}, {"size", size}};
    return value.dump(-1, ' ', false, Json::error_handler_t::strict);
}
cpn::WorkspaceSession initial_session() {
    cpn::WorkspaceSession session; session.workspace_id = "workspace-stable"; session.revision = 7;
    cpn::SessionDocument document; document.document_id = "user-document"; document.revision = 3;
    document.source_text = "// user modified source\r\n";
    document.tests = {{"user-sample", "user input", "user answer", false}};
    session.documents.push_back(document); session.selected_document_id = document.document_id;
    session.editor_preferences = {{"unknown.future.native", 99, "{ \"integer\":18446744073709551616,\"unicode\":\"e\\u0301\" }"}};
    return session;
}
cpn::ImportDocumentDefaults document_defaults() {
    cpn::ImportDocumentDefaults defaults;
    defaults.source_text = "\xef\xbb\xbf// 未保存 😀 e\u0301\r\n"; defaults.source_text.push_back('\0'); defaults.source_text += "tail";
    defaults.language_id = "configured-language"; defaults.toolchain_profile_id = "configured-profile";
    defaults.document_preferences = {{"native.future.editor", 47, "{ \"retain\":true }"}};
    return defaults;
}
class MemoryStore final : public cpn::FileStore {
public:
    MemoryStore(cpn::CompanionInboxPaths inbox, cpn::SessionPaths session) : inbox(std::move(inbox)), session(std::move(session)) {}
    struct Lock final : cpn::FileLock {
        Lock(const MemoryStore* owner, std::filesystem::path path) : owner(owner), path(std::move(path)) { owner->locks.insert(this->path); }
        ~Lock() override { owner->locks.erase(path); }
        const MemoryStore* owner; std::filesystem::path path;
    };
    std::string read(const std::filesystem::path& path, std::size_t maximum) const override {
        const auto value = read_optional(path, maximum); if (!value) throw std::runtime_error("missing file"); return *value;
    }
    std::optional<std::string> read_optional(const std::filesystem::path& path, std::size_t maximum) const override {
        if (fail_read && path == *fail_read) throw std::runtime_error("injected read error");
        const auto found = contents.find(path); if (found == contents.end()) return std::nullopt;
        if (found->second.size() > maximum) throw std::runtime_error("read byte capacity"); return found->second;
    }
    std::unique_ptr<cpn::FileLock> acquire_lock(const std::filesystem::path& path, std::chrono::milliseconds timeout, std::stop_token stop) const override {
        expect(timeout.count() >= 0, "lock timeout nonnegative");
        if (stop.stop_requested()) throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        if ((fail_lock && path == *fail_lock) || locks.contains(path)) throw std::system_error(std::make_error_code(std::errc::timed_out));
        if (null_lock) return {};
        acquired.push_back(path); return std::make_unique<Lock>(this, path);
    }
    void write_atomic(const std::filesystem::path& path, std::string_view bytes) const override {
        if (path == inbox.inbox_file || path == inbox.backup_file) expect(locks.contains(inbox.lock_file), "inbox writes require its cooperating lock");
        if (path == session.recovery_file || path == session.backup_file) expect(locks.contains(session.lock_file), "session writes require its cooperating lock");
        if (fail_write && path == *fail_write && !fail_after) throw cpn::AtomicWriteError("injected before replace", false);
        contents[path] = std::string(bytes); writes.push_back(path);
        if (fail_write && path == *fail_write && fail_after) throw cpn::AtomicWriteError("injected after replace", true);
        if (after_write) after_write(path);
    }
    std::filesystem::path create_workspace() const override { throw std::runtime_error("inbox must not create process workspaces"); }
    void remove_workspace(const std::filesystem::path&) const noexcept override {}
    cpn::CompanionInboxPaths inbox; cpn::SessionPaths session;
    mutable std::map<std::filesystem::path, std::string> contents;
    mutable std::set<std::filesystem::path> locks;
    mutable std::vector<std::filesystem::path> acquired, writes;
    std::optional<std::filesystem::path> fail_read, fail_write, fail_lock;
    bool fail_after{false}, null_lock{false};
    std::function<void(const std::filesystem::path&)> after_write;
};
std::shared_ptr<MemoryStore> memory(const std::filesystem::path& folder) {
    return std::make_shared<MemoryStore>(paths_in(folder), session_paths_in(folder));
}
cpn::CompanionInboxService service(const std::shared_ptr<cpn::FileStore>& files, cpn::CompanionInboxBounds bounds = {}) {
    return cpn::CompanionInboxService("workspace-stable", files, bounds, [] { return "fixed-inbox-e\u0301"; });
}
cpn::CompanionInboxSnapshot accepted_snapshot(const std::shared_ptr<MemoryStore>& files) {
    auto inbox = service(files);
    const auto accepted = inbox.accept(files->inbox, payload()); expect(accepted.status == Write::accepted, "fixture accepted");
    const auto loaded = inbox.load(files->inbox); expect(loaded.status == Load::loaded && loaded.snapshot, "fixture loaded");
    return *loaded.snapshot;
}
void format(const std::filesystem::path& folder) {
    const auto files = memory(folder); const auto snapshot = accepted_snapshot(files);
    const auto serialized = cpn::serialize_companion_inbox(snapshot);
    const auto restored = cpn::parse_companion_inbox(serialized);
    expect(cpn::serialize_companion_inbox(restored) == serialized, "canonical inbox roundtrip");
    expect(restored.items[0].candidate.original_json == payload(), "unknown numbers and raw JSON lexemes preserved");
    expect(restored.items[0].candidate.tests[0].input == std::string("a\0中\r\n", 7) && restored.items[0].candidate.tests[0].output.empty(), "NUL/CJK/CRLF and empty answer preserved");
    expect(restored.items[0].payload_digest == cpn::sha256(payload()), "payload digest is original byte digest");
    const auto item_json = Json::parse(cpn::serialize_companion_inbox_item(snapshot.items[0], snapshot.inbox_id));
    expect(item_json == Json::parse(serialized)["items"][0], "page item is identical canonical internal item object");
    const auto baseline = Json::parse(serialized);
    const auto invalid = [&](Json value, const char* message) { rejected([&] { (void)cpn::parse_companion_inbox(value.dump()); }, message); };
    for (const auto& value : std::vector<Json>{-1, 1.0, true, nullptr, "1", 18446744073709551616.0}) {
        auto object = baseline; object["revision"] = value; invalid(object, "strict revision uint64");
        object = baseline; object["items"][0]["sequence"] = value; invalid(object, "strict sequence uint64");
    }
    for (const auto& version : std::vector<Json>{0, 2, 1.0, 4294967297ULL, std::numeric_limits<std::uint64_t>::max()}) {
        auto object = baseline; object["schema_version"] = version; invalid(object, "supported schema exact integer");
    }
    for (const auto* key : {"import_id", "document_id", "problem_id", "payload_digest"}) {
        auto object = baseline; object["items"][0][key] = cpn::sha256("different"); invalid(object, "stable identity and checksum validated");
    }
    auto object = baseline; object["items"][0]["test_ids"][0] = cpn::sha256("different"); invalid(object, "test identity validated");
    object = baseline; object["items"][0]["original_json"] = payload("https://different.test/"); invalid(object, "raw payload mutation rejected");
    object = baseline; object["items"].push_back(object["items"][0]); invalid(object, "duplicate sequence rejected");
    object = baseline; object["next_sequence"] = 1; invalid(object, "sequence high-water mark validated");
    object = baseline; object["items"][0]["state"] = "Applied"; invalid(object, "applied requires actual session evidence");
    object = baseline; object["items"][0]["applied_session_digest"] = cpn::sha256("not-applied"); invalid(object, "pending has no applied evidence");
    object = baseline; object["items"][0]["previous_import_id"] = object["items"][0]["import_id"]; invalid(object, "previous cannot self reference");
    object = baseline; object["unknown_internal"] = true; invalid(object, "unknown internal field rejected");
    object = baseline; object["items"][0]["unknown_internal"] = true; invalid(object, "unknown item field rejected");
    object = baseline; object["workspace_id"] = std::string("a\0b", 3); invalid(object, "NUL identity rejected");
    rejected([&] { (void)cpn::parse_companion_inbox("{\"schema_version\":1,\"schema_version\":1}"); }, "duplicate root keys rejected");
    const auto prefix = serialized.find("\"state\":\"Pending\""); expect(prefix != std::string::npos, "state fixture available");
    auto duplicated = serialized; duplicated.insert(prefix, "\"state\":\"Pending\",");
    rejected([&] { (void)cpn::parse_companion_inbox(duplicated); }, "duplicate nested keys rejected");
    auto changed = snapshot; changed.items[0].candidate.tests[0].output = "tampered";
    rejected([&] { (void)cpn::serialize_companion_inbox(changed); }, "public candidate cannot diverge from original payload");
    auto maximum = snapshot; maximum.revision = std::numeric_limits<std::uint64_t>::max(); maximum.next_sequence = maximum.revision;
    expect(cpn::parse_companion_inbox(cpn::serialize_companion_inbox(maximum)).revision == maximum.revision, "maximum uint64 roundtrip without floating conversion");
    cpn::CompanionInboxBounds bounds; bounds.max_json_bytes = serialized.size() - 1;
    rejected([&] { (void)cpn::parse_companion_inbox(serialized, bounds); }, "JSON byte budget enforced");
    bounds = {}; bounds.max_json_depth = 1;
    rejected([&] { (void)cpn::parse_companion_inbox(serialized, bounds); }, "JSON depth budget enforced");
    bounds = {}; bounds.max_json_nodes = 2;
    rejected([&] { (void)cpn::parse_companion_inbox(serialized, bounds); }, "JSON node budget enforced");
    bounds = {}; bounds.max_id_bytes = 63;
    rejected([&] { (void)service(files, bounds); }, "invalid generated identity bounds rejected");
}
void admission(const std::filesystem::path& folder) {
    auto files = memory(folder); auto inbox = service(files);
    expect(inbox.load(files->inbox).status == Load::missing, "new inbox missing");
    expect(inbox.accept(files->inbox, "{}").status == Write::invalid_payload && files->writes.empty(), "invalid payload has no disk writes");
    std::stop_source pre; pre.request_stop();
    expect(inbox.accept(files->inbox, payload(), std::chrono::milliseconds{0}, pre.get_token()).status == Write::cancelled && files->writes.empty(), "pre-cancelled accept has no writes");
    const auto first = inbox.accept(files->inbox, payload()); expect(first.status == Write::accepted && first.inbox_replaced && !first.backup_replaced && first.item, "accepted only after durable write");
    const auto original = files->contents.at(files->inbox.inbox_file); const auto write_count = files->writes.size();
    auto duplicate = inbox.accept(files->inbox, payload());
    expect(duplicate.status == Write::duplicate && duplicate.item->import_id == first.item->import_id && !duplicate.inbox_replaced && files->writes.size() == write_count, "identical original delivery reuses import without write");
    auto whitespace = inbox.accept(files->inbox, payload() + "\n");
    expect(whitespace.status == Write::accepted && whitespace.item->import_id != first.item->import_id && whitespace.item->problem_id == first.item->problem_id, "different raw bytes form safe pending update");
    expect(whitespace.item->document_id != first.item->document_id && whitespace.item->test_ids != first.item->test_ids && whitespace.item->previous_import_id == first.item->import_id, "update gets new stable document/tests and previous link");
    expect(files->contents.at(files->inbox.backup_file) == original, "backup exact previous accepted snapshot");
    auto changed = inbox.accept(files->inbox, payload("https://example.test/problem/a", "new answer"));
    expect(changed.status == Write::accepted && changed.item->previous_import_id == whitespace.item->import_id, "latest same URL links without overwriting original payload");
    const auto other = inbox.accept(files->inbox, payload("https://example.test/problem/e\u0301"));
    const auto canonical = inbox.accept(files->inbox, payload("https://example.test/problem/é"));
    expect(other.status == Write::accepted && canonical.status == Write::accepted && other.item->problem_id != canonical.item->problem_id, "Unicode normalization is not implicit URL identity merging");
    auto batched_first = inbox.accept(files->inbox, batched("https://example.test/batch/a", "batch-e\u0301", 2));
    expect(batched_first.status == Write::accepted, "opaque batch accepted");
    const auto before_conflict = files->contents.at(files->inbox.inbox_file); const auto before_writes = files->writes.size();
    expect(inbox.accept(files->inbox, batched("https://example.test/batch/a", "batch-e\u0301", 3)).status == Write::batch_conflict, "changed size conflicts");
    expect(files->contents.at(files->inbox.inbox_file) == before_conflict && files->writes.size() == before_writes, "batch conflict has no writes");
    expect(inbox.accept(files->inbox, batched("https://example.test/batch/a", "batch-e\u0301", 2, "updated")).status == Write::accepted, "same URL update does not consume batch project slot");
    expect(inbox.accept(files->inbox, batched("https://example.test/batch/b", "batch-e\u0301", 2)).status == Write::accepted, "second distinct URL completes declared batch");
    const auto full_batch = files->contents.at(files->inbox.inbox_file);
    expect(inbox.accept(files->inbox, batched("https://example.test/batch/c", "batch-e\u0301", 2)).status == Write::batch_conflict && files->contents.at(files->inbox.inbox_file) == full_batch, "overfull distinct URLs rejected");
    cpn::CompanionInboxService other_workspace("workspace-other", files);
    expect(other_workspace.accept(files->inbox, payload()).status == Write::identity_mismatch && other_workspace.load(files->inbox).status == Load::invalid, "wrong workspace identity cannot mutate");
    expect(files->locks.empty(), "all accept locks released");
    auto exhausted = *inbox.load(files->inbox).snapshot; exhausted.next_sequence = std::numeric_limits<std::uint64_t>::max();
    files->contents[files->inbox.inbox_file] = cpn::serialize_companion_inbox(exhausted); files->contents.erase(files->inbox.backup_file);
    expect(inbox.accept(files->inbox, payload("https://new.test/")).status == Write::id_exhausted, "sequence exhaustion explicit");
    expect(inbox.accept(files->inbox, payload()).status == Write::duplicate, "duplicate still readable at sequence exhaustion");
}
void capacity(const std::filesystem::path& folder) {
    const auto bounded = [&](cpn::CompanionInboxBounds bounds, std::string body = payload()) {
        auto files = memory(folder); auto inbox = service(files, bounds);
        const auto rejected = inbox.accept(files->inbox, body);
        expect(rejected.status == Write::capacity_exceeded && !rejected.inbox_replaced && !rejected.item && files->writes.empty(), "admission capacity rejected before any data writes");
    };
    cpn::CompanionInboxBounds bounds; bounds.max_records = 0; bounded(bounds);
    bounds = {}; bounds.max_pending = 0; bounded(bounds);
    bounds = {}; bounds.max_total_payload_bytes = payload().size() - 1; bounded(bounds);
    bounds = {}; bounds.max_total_tests = 0; bounded(bounds);
    bounds = {}; bounds.max_batches = 0; bounded(bounds, batched("https://batch.test/", "id", 1));
    bounds = {}; bounds.max_json_bytes = 100; bounded(bounds);
    bounds = {}; bounds.max_json_depth = 1; bounded(bounds);
    bounds = {}; bounds.max_json_nodes = 2; bounded(bounds);
    auto files = memory(folder); bounds = {}; bounds.max_records = 1; bounds.max_pending = 1;
    auto inbox = service(files, bounds); const auto accepted = inbox.accept(files->inbox, payload()); expect(accepted.status == Write::accepted, "one capacity slot accepted");
    const auto before = files->contents.at(files->inbox.inbox_file);
    expect(inbox.accept(files->inbox, payload()).status == Write::duplicate, "full inbox accepts exact duplicate without new slot");
    expect(inbox.accept(files->inbox, payload("https://new.test/")).status == Write::capacity_exceeded && files->contents.at(files->inbox.inbox_file) == before, "full inbox retains existing metadata and applies backpressure");
}
void preparation(const std::filesystem::path& folder) {
    auto files = memory(folder); auto snapshot = accepted_snapshot(files); const auto& first = snapshot.items[0];
    const auto initial = initial_session(); const auto defaults = document_defaults();
    auto prepared = cpn::prepare_import_application(initial, snapshot, first.import_id, defaults);
    expect(prepared.status == Prepare::prepared && prepared.session && prepared.receipt, "atomic candidate prepared");
    expect(prepared.session->revision == initial.revision + 1 && prepared.session->documents.size() == initial.documents.size() + 1, "new document and workspace revision advanced");
    expect(prepared.session->documents[0].source_text == initial.documents[0].source_text && prepared.session->documents[0].tests[0].expected == initial.documents[0].tests[0].expected, "existing user source/tests are not modified");
    expect(prepared.session->editor_preferences[0].object_json == initial.editor_preferences[0].object_json, "unknown workspace preference preserved exactly");
    const auto& imported = prepared.session->documents.back();
    expect(imported.document_id == first.document_id && imported.source_text == defaults.source_text && imported.revision == 0 && !imported.original_path && !imported.save_point, "new document bytes and stable identity without source file I/O");
    expect(imported.language_id == defaults.language_id && imported.toolchain_profile_id == defaults.toolchain_profile_id, "native language/toolchain defaults data driven");
    expect(imported.tests.size() == 1 && imported.tests[0].id == first.test_ids[0] && imported.tests[0].expected == "" && imported.tests[0].enabled, "allocated test ID and explicit empty answer");
    const auto receipt = cpn::find_import_application_receipt(*prepared.session, snapshot.inbox_id, first.import_id);
    const auto provenance = cpn::read_companion_document_provenance(imported);
    expect(receipt && receipt->payload_digest == first.payload_digest && provenance && provenance->compatible_with_ordinary_stdio, "saved receipt and document capability provenance paired");
    const auto encoded = cpn::serialize_workspace_session(*prepared.session);
    auto restored = cpn::parse_workspace_session(encoded); restored.documents.back().source_text += "user edit"; ++restored.documents.back().revision;
    auto replayed = cpn::prepare_import_application(restored, snapshot, first.import_id, defaults);
    expect(replayed.status == Prepare::already_applied && cpn::serialize_workspace_session(*replayed.session) == cpn::serialize_workspace_session(restored), "replay preserves edited imported source");
    restored.documents.pop_back(); restored.selected_document_id = restored.documents.front().document_id;
    replayed = cpn::prepare_import_application(restored, snapshot, first.import_id, defaults);
    expect(replayed.status == Prepare::already_applied && replayed.session->documents.size() == 1, "receipt prevents recreation after document deletion");
    auto inbox = service(files); expect(inbox.accept(files->inbox, payload("https://example.test/problem/a", "different answer")).status == Write::accepted, "update accepted for preparation");
    snapshot = *inbox.load(files->inbox).snapshot;
    auto update = cpn::prepare_import_application(*prepared.session, snapshot, snapshot.items.back().import_id, defaults);
    expect(update.status == Prepare::prepared && update.session->documents.size() == 3 && update.session->documents[1].tests[0].expected == "" && update.session->documents[2].tests[0].expected == "different answer", "new update does not clear old samples");
    auto collision = initial; auto colliding_document = imported; collision.documents.push_back(colliding_document);
    expect(cpn::prepare_import_application(collision, snapshot, snapshot.items[0].import_id, defaults).status == Prepare::identity_conflict, "existing document without receipt not overwritten");
    auto wrong_workspace = initial; wrong_workspace.workspace_id = "another";
    expect(cpn::prepare_import_application(wrong_workspace, snapshot, snapshot.items[0].import_id).status == Prepare::invalid_item, "workspace mismatch rejected");
    auto maximum = initial; maximum.revision = std::numeric_limits<std::uint64_t>::max();
    expect(cpn::prepare_import_application(maximum, snapshot, snapshot.items[0].import_id).status == Prepare::revision_exhausted, "workspace version cannot wrap");
    auto bad_defaults = defaults; bad_defaults.source_text = std::string(1, static_cast<char>(0xff));
    expect(cpn::prepare_import_application(initial, snapshot, snapshot.items[0].import_id, bad_defaults).status == Prepare::invalid_session, "invalid source UTF8 not replaced");
    bad_defaults = defaults; bad_defaults.language_id = "";
    expect(cpn::prepare_import_application(initial, snapshot, snapshot.items[0].import_id, bad_defaults).status == Prepare::invalid_session, "invalid native language reference rejected");
    auto invalid_item = snapshot; invalid_item.items[0].candidate.tests[0].output = "not original";
    expect(cpn::prepare_import_application(initial, invalid_item, invalid_item.items[0].import_id).status == Prepare::invalid_item, "constructed candidate checked against original");
    auto unavailable = snapshot; unavailable.items[0].state = cpn::CompanionInboxState::applied;
    unavailable.items[0].applied_session_digest = cpn::sha256(encoded); unavailable.items[0].applied_session_revision = prepared.session->revision;
    expect(cpn::prepare_import_application(initial, unavailable, unavailable.items[0].import_id).status == Prepare::receipt_missing, "old restored session cannot silently replay Applied item");
    auto full_preferences = initial; full_preferences.editor_preferences.clear();
    for (int i = 0; i < 16; ++i) full_preferences.editor_preferences.push_back({"unknown-" + std::to_string(i), 1, "{ \"preserve\":true }"});
    expect(cpn::prepare_import_application(full_preferences, snapshot, snapshot.items[0].import_id).status == Prepare::capacity_exceeded, "full workspace preferences do not get evicted");
    auto full_defaults = defaults; full_defaults.document_preferences = full_preferences.editor_preferences;
    expect(cpn::prepare_import_application(initial, snapshot, snapshot.items[0].import_id, full_defaults).status == Prepare::capacity_exceeded, "full new document preferences do not get evicted");
    auto future = initial; future.editor_preferences.push_back({std::string(cpn::companion_receipts_namespace), 2, "{ \"opaque\":true }"});
    expect(cpn::prepare_import_application(future, snapshot, snapshot.items[0].import_id).status == Prepare::incompatible_preferences, "future receipt preference blocks import while preserved");
    future = *prepared.session; future.editor_preferences.push_back({std::string(cpn::companion_receipts_namespace), 2, "{ \"opaque\":true }"});
    expect(cpn::prepare_import_application(future, snapshot, snapshot.items[0].import_id).status == Prepare::incompatible_preferences, "mixed own v1 plus future opaque cannot decode older receipt");
    rejected([&] { (void)cpn::find_import_application_receipt(future, snapshot.inbox_id, snapshot.items[0].import_id); }, "future reader fails explicitly");
    auto wrong_receipt = *prepared.session;
    for (auto& preference : wrong_receipt.editor_preferences) if (preference.namespace_id == cpn::companion_receipts_namespace) {
        auto value = Json::parse(preference.object_json); value["receipts"][0]["payload_digest"] = cpn::sha256("other"); preference.object_json = value.dump();
    }
    expect(cpn::prepare_import_application(wrong_receipt, snapshot, snapshot.items[0].import_id).status == Prepare::identity_conflict, "receipt mismatch rejected without overwrite");
    auto unknown_receipt = *prepared.session;
    for (auto& preference : unknown_receipt.editor_preferences) if (preference.namespace_id == cpn::companion_receipts_namespace) {
        auto value = Json::parse(preference.object_json); value["receipts"][0]["unknown"] = true; preference.object_json = value.dump();
    }
    expect(cpn::prepare_import_application(unknown_receipt, snapshot, snapshot.items[0].import_id).status == Prepare::incompatible_preferences, "unknown own receipt fields cannot be silently stripped");
    auto future_document = imported;
    future_document.editor_preferences.push_back({std::string(cpn::companion_document_namespace), 2, "{ \"opaque\":true }"});
    rejected([&] { (void)cpn::read_companion_document_provenance(future_document); }, "future document provenance cannot expose an older capability flag");
    auto full_with_own = *prepared.session;
    for (int i = 0; i < 14; ++i) full_with_own.editor_preferences.push_back({"unknown-extra-" + std::to_string(i), 1, "{}"});
    expect(full_with_own.editor_preferences.size() == 16, "own receipt preference plus fifteen foreign preferences fixture");
    expect(cpn::prepare_import_application(full_with_own, snapshot, snapshot.items.back().import_id, defaults).status == Prepare::prepared, "existing owned receipt preference updates without consuming a new namespace slot");
    cpn::ImportApplicationBounds limits; limits.max_receipts = 1;
    expect(cpn::prepare_import_application(*prepared.session, snapshot, snapshot.items.back().import_id, defaults, limits).status == Prepare::capacity_exceeded, "receipt count bounded without eviction");
    expect(cpn::prepare_import_application(*prepared.session, snapshot, snapshot.items[0].import_id, defaults, limits).status == Prepare::already_applied, "full receipt history still supports replay");
    limits = {}; limits.session.max_preferences_bytes = 100;
    expect(cpn::prepare_import_application(initial, snapshot, snapshot.items[0].import_id, defaults, limits).status == Prepare::capacity_exceeded, "receipt/provenance byte budget enforced");
    auto unsupported_body = Json::parse(payload("https://interactive.test/")); unsupported_body["interactive"] = true;
    unsupported_body["input"] = {{"type", "file"}, {"fileName", "../never-open-this"}};
    expect(inbox.accept(files->inbox, unsupported_body.dump()).status == Write::accepted, "unsupported mode metadata retained");
    snapshot = *inbox.load(files->inbox).snapshot;
    auto unsupported = cpn::prepare_import_application(initial, snapshot, snapshot.items.back().import_id, defaults);
    expect(unsupported.status == Prepare::prepared && !cpn::read_companion_document_provenance(unsupported.session->documents.back())->compatible_with_ordinary_stdio, "unsupported mode imports with persisted Run capability guard");
}
void acknowledgement(const std::filesystem::path& folder) {
    auto files = memory(folder); auto inbox = service(files); auto snapshot = accepted_snapshot(files);
    const auto id = snapshot.items[0].import_id; auto initial = initial_session();
    expect(inbox.mark_applied(files->inbox, files->session, id).status == Write::session_unavailable, "caller cannot ack before actual recovery exists");
    cpn::SessionService sessions(files);
    auto saved = sessions.save(files->session, initial, std::nullopt); expect(saved.status == cpn::SessionSaveStatus::saved, "initial session saved");
    auto prepared = cpn::prepare_import_application(initial, snapshot, id, document_defaults());
    expect(prepared.status == Prepare::prepared, "prepare before ack");
    expect(inbox.mark_applied(files->inbox, files->session, id).status == Write::receipt_missing, "memory-only prepared receipt cannot ack");
    files->fail_write = files->session.recovery_file;
    auto failed_save = sessions.save(files->session, *prepared.session, saved.disk_digest);
    expect(failed_save.status == cpn::SessionSaveStatus::io_failed && inbox.mark_applied(files->inbox, files->session, id).status == Write::receipt_missing, "failed candidate session write leaves import Pending");
    files->fail_write.reset(); saved = sessions.save(files->session, *prepared.session, saved.disk_digest);
    expect(saved.status == cpn::SessionSaveStatus::saved, "document plus receipt committed together");
    auto restored = *sessions.load(files->session.recovery_file).session;
    restored.documents.pop_back(); restored.selected_document_id = restored.documents.front().document_id; ++restored.revision;
    saved = sessions.save(files->session, restored, saved.disk_digest); expect(saved.status == cpn::SessionSaveStatus::saved, "delete document after save before ack");
    expect(cpn::prepare_import_application(restored, snapshot, id).status == Prepare::already_applied, "crash replay receipt suppresses deleted document recreation");
    files->fail_write = files->inbox.inbox_file;
    const auto failed_ack = inbox.mark_applied(files->inbox, files->session, id);
    expect(failed_ack.status == Write::io_failed && !failed_ack.inbox_replaced && inbox.load(files->inbox).snapshot->items[0].state == cpn::CompanionInboxState::pending, "failed ack keeps Pending for idempotent replay");
    files->fail_after = true;
    const auto uncertain_ack = inbox.mark_applied(files->inbox, files->session, id);
    expect(uncertain_ack.status == Write::durability_unknown && uncertain_ack.inbox_replaced && uncertain_ack.item->state == cpn::CompanionInboxState::applied, "ack rename commit visible despite uncertain durability");
    files->fail_write.reset(); files->fail_after = false;
    const auto acknowledged = inbox.mark_applied(files->inbox, files->session, id);
    expect(acknowledged.status == Write::already_applied && acknowledged.item->applied_session_digest == saved.disk_digest, "ack retry after committed uncertainty is idempotent");
    expect(inbox.accept(files->inbox, payload()).status == Write::duplicate, "retry after Applied never creates new item");
    expect(inbox.mark_applied(files->inbox, files->session, cpn::sha256("unknown")).status == Write::identity_mismatch, "unknown import cannot ack");
    auto alias_paths = files->session; alias_paths.lock_file = files->inbox.lock_file;
    expect(inbox.mark_applied(files->inbox, alias_paths, id).status == Write::invalid_existing, "lock alias rejected before self-deadlock");
    expect(files->locks.empty(), "both inbox and session locks released");
    auto new_item = inbox.accept(files->inbox, payload("https://new.test/")); expect(new_item.status == Write::accepted, "new item for receipt conflict");
    files->fail_lock = files->session.lock_file;
    expect(inbox.mark_applied(files->inbox, files->session, new_item.item->import_id).status == Write::lock_unavailable && files->locks.empty(), "session lock failure releases inbox lock without ack");
    files->fail_lock.reset();
    snapshot = *inbox.load(files->inbox).snapshot; prepared = cpn::prepare_import_application(restored, snapshot, new_item.item->import_id);
    auto wrong = *prepared.session;
    for (auto& preference : wrong.editor_preferences) if (preference.namespace_id == cpn::companion_receipts_namespace) {
        auto value = Json::parse(preference.object_json); value["receipts"].back()["payload_digest"] = cpn::sha256("wrong"); preference.object_json = value.dump();
    }
    saved = sessions.save(files->session, wrong, saved.disk_digest); expect(saved.status == cpn::SessionSaveStatus::saved, "syntactically valid mismatched receipt stored");
    expect(inbox.mark_applied(files->inbox, files->session, new_item.item->import_id).status == Write::receipt_conflict, "actual mismatched receipt rejects ack");
    auto wrong_owner = wrong; wrong_owner.workspace_id = "wrong-owner";
    saved = sessions.save(files->session, wrong_owner, saved.disk_digest); expect(saved.status == cpn::SessionSaveStatus::saved, "different workspace fixture saved");
    expect(inbox.mark_applied(files->inbox, files->session, new_item.item->import_id).status == Write::identity_mismatch, "different workspace cannot prove application");
}
void history(const std::filesystem::path& folder) {
    auto files = memory(folder); auto inbox = service(files);
    const auto first = accepted_snapshot(files);
    const auto first_bytes = cpn::serialize_companion_inbox(first);
    const auto second_result = inbox.accept(files->inbox, payload("https://history.test/second", "original answer"));
    expect(second_result.status == Write::accepted, "history second admission succeeds");
    const auto second = *inbox.load(files->inbox).snapshot;
    const auto second_bytes = cpn::serialize_companion_inbox(second);
    const auto verify_rejected = [&](const cpn::CompanionInboxSnapshot& main, const cpn::CompanionInboxSnapshot& backup, const char* message) {
        const auto main_bytes = cpn::serialize_companion_inbox(main);
        const auto backup_bytes = cpn::serialize_companion_inbox(backup);
        files->contents[files->inbox.inbox_file] = main_bytes;
        files->contents[files->inbox.backup_file] = backup_bytes;
        files->writes.clear();
        const auto result = inbox.accept(files->inbox, payload("https://history.test/new"));
        expect(result.status == Write::invalid_existing && !result.item && !result.inbox_replaced && !result.backup_replaced && files->writes.empty(), message);
        expect(files->contents.at(files->inbox.inbox_file) == main_bytes && files->contents.at(files->inbox.backup_file) == backup_bytes, "both historical originals preserved on rejection");
        expect(files->locks.empty(), "invalid history releases lock");
    };
    verify_rejected(first, second, "old main plus advanced backup cannot reuse old sequence or overwrite sole newer payload");
    expect(inbox.accept(files->inbox, payload()).status == Write::invalid_existing, "even duplicate admission cannot ignore an unsafe newer backup");
    auto revision_only_ahead = first; ++revision_only_ahead.revision;
    verify_rejected(first, revision_only_ahead, "advanced backup revision rejected even at same sequence high-water mark");
    auto sequence_only_ahead = first; ++sequence_only_ahead.next_sequence;
    verify_rejected(first, sequence_only_ahead, "advanced backup sequence rejected even at same revision");
    auto branch_files = memory(folder / "branch"); auto branch_inbox = service(branch_files); (void)accepted_snapshot(branch_files);
    expect(branch_inbox.accept(branch_files->inbox, payload("https://history.test/second", "branch answer")).status == Write::accepted, "alternative valid branch constructed");
    const auto branch = *branch_inbox.load(branch_files->inbox).snapshot;
    expect(branch.next_sequence == second.next_sequence && branch.revision == second.revision && branch.items.back().import_id == second.items.back().import_id && branch.items.back().payload_digest != second.items.back().payload_digest, "same high-water branch has reused ID with different payload evidence");
    verify_rejected(second, branch, "same high-water branch cannot destroy payload behind an existing ID");
    auto applied = first;
    applied.items[0].state = cpn::CompanionInboxState::applied;
    applied.items[0].applied_session_digest = cpn::sha256("saved session A");
    applied.items[0].applied_session_revision = 42; ++applied.revision;
    auto reverted = first; reverted.revision = applied.revision + 1;
    verify_rejected(reverted, applied, "Applied backup cannot regress to Pending in a higher-revision main");
    auto evidence_branch = applied; ++evidence_branch.revision;
    evidence_branch.items[0].applied_session_digest = cpn::sha256("saved session B");
    verify_rejected(evidence_branch, applied, "already-Applied session evidence cannot silently change in later main");
    evidence_branch = applied; ++evidence_branch.revision; ++*evidence_branch.items[0].applied_session_revision;
    verify_rejected(evidence_branch, applied, "already-Applied session revision evidence cannot silently change");
    auto unversioned_state_change = applied; unversioned_state_change.revision = first.revision;
    verify_rejected(unversioned_state_change, first, "different logical snapshots at same revision rejected");
    files->contents[files->inbox.inbox_file] = cpn::serialize_companion_inbox(applied);
    files->contents[files->inbox.backup_file] = first_bytes; files->writes.clear();
    expect(inbox.accept(files->inbox, payload("https://history.test/new")).status == Write::accepted, "valid historical Pending-to-Applied prefix allows further admission");
    const auto valid_current = files->contents.at(files->inbox.inbox_file);
    expect(files->contents.at(files->inbox.backup_file) == cpn::serialize_companion_inbox(applied) && valid_current != second_bytes, "valid transaction preserves immediate predecessor");
}
void faults(const std::filesystem::path& folder) {
    for (const bool after : {false, true}) {
        auto files = memory(folder); auto inbox = service(files); files->fail_write = files->inbox.inbox_file; files->fail_after = after;
        const auto result = inbox.accept(files->inbox, payload());
        expect(result.status == (after ? Write::durability_unknown : Write::io_failed) && result.inbox_replaced == after && !result.backup_replaced, "initial admission write fault classified");
        expect(result.item.has_value() == after, "initial identity exposed only when atomic replace occurred");
        files->fail_write.reset(); const auto retried = inbox.accept(files->inbox, payload());
        expect(retried.status == (after ? Write::duplicate : Write::accepted), "initial fault retry correctly discovers committed identity");
    }
    for (const bool backup : {true, false}) for (const bool after : {false, true}) {
        auto files = memory(folder); auto inbox = service(files); const auto first = inbox.accept(files->inbox, payload());
        const auto original = files->contents.at(files->inbox.inbox_file);
        files->fail_write = backup ? files->inbox.backup_file : files->inbox.inbox_file; files->fail_after = after;
        const auto result = inbox.accept(files->inbox, payload("https://next.test/"));
        expect(result.status == (after ? Write::durability_unknown : Write::io_failed), "atomic write failure explicitly classified");
        expect(result.inbox_replaced == (!backup && after) && result.backup_replaced == (!backup || after), "both replacement flags preserve commit points");
        if (!result.inbox_replaced) expect(files->contents.at(files->inbox.inbox_file) == original && result.disk_digest == first.disk_digest && !result.item, "failed admission exposes no new ID or changed inbox token");
        else expect(result.item && result.disk_digest == cpn::sha256(files->contents.at(files->inbox.inbox_file)), "post-replace result carries new item and digest");
        files->fail_write.reset(); expect(inbox.load(files->inbox).status == Load::loaded, "every injected fault retains valid loadable snapshot");
    }
    auto files = memory(folder); auto inbox = service(files); (void)accepted_snapshot(files);
    files->fail_read = files->inbox.backup_file;
    expect(inbox.accept(files->inbox, payload("https://next.test/")).status == Write::io_failed, "backup read failure is I/O error");
    files->fail_read = files->inbox.inbox_file;
    expect(inbox.load(files->inbox).status == Load::io_failed, "load I/O distinct from malformed JSON"); files->fail_read.reset();
    files->fail_lock = files->inbox.lock_file;
    expect(inbox.accept(files->inbox, payload("https://next.test/")).status == Write::lock_unavailable, "lock timeout structured"); files->fail_lock.reset();
    files->null_lock = true;
    expect(inbox.accept(files->inbox, payload("https://next.test/")).status == Write::io_failed, "null lock never permits data write"); files->null_lock = false;
    const auto original = files->contents.at(files->inbox.inbox_file);
    std::stop_source during; files->after_write = [&](const auto& path) { if (path == files->inbox.backup_file) during.request_stop(); };
    auto result = inbox.accept(files->inbox, payload("https://next.test/"), std::chrono::milliseconds{100}, during.get_token());
    expect(result.status == Write::cancelled && result.backup_replaced && !result.inbox_replaced && files->contents.at(files->inbox.inbox_file) == original, "cancel after backup leaves main original");
    std::stop_source after_commit; files->after_write = [&](const auto& path) { if (path == files->inbox.inbox_file) after_commit.request_stop(); };
    result = inbox.accept(files->inbox, payload("https://next.test/"), std::chrono::milliseconds{100}, after_commit.get_token());
    expect(result.status == Write::accepted && result.inbox_replaced && after_commit.stop_requested(), "post-commit cancellation cannot erase successful accept");
    files->after_write = {}; const auto external_original = files->contents.at(files->inbox.inbox_file);
    auto external = *inbox.load(files->inbox).snapshot; ++external.revision; const auto external_bytes = cpn::serialize_companion_inbox(external);
    files->after_write = [&](const auto& path) { if (path == files->inbox.backup_file) files->contents[files->inbox.inbox_file] = external_bytes; };
    result = inbox.accept(files->inbox, payload("https://other.test/"));
    expect(result.status == Write::storage_conflict && result.backup_replaced && !result.inbox_replaced && files->contents.at(files->inbox.inbox_file) == external_bytes, "writer bypassing lock detected during backup, preserves external data");
    files->after_write = {}; files->contents[files->inbox.inbox_file] = external_original;
    files->contents[files->inbox.backup_file] = "{damaged";
    expect(inbox.accept(files->inbox, payload("https://other.test/")).status == Write::invalid_existing && files->contents.at(files->inbox.backup_file) == "{damaged", "damaged backup not overwritten");
    files->contents.erase(files->inbox.backup_file); auto future = Json::parse(external_original); future["schema_version"] = 2; future["future_field"] = true;
    files->contents[files->inbox.inbox_file] = future.dump();
    const auto loaded_future = inbox.load(files->inbox);
    expect(loaded_future.status == Load::unsupported_schema && loaded_future.disk_digest == cpn::sha256(future.dump()) && !loaded_future.snapshot, "future schema preserved with observed digest");
    expect(inbox.accept(files->inbox, payload()).status == Write::unsupported_schema && files->contents.at(files->inbox.inbox_file) == future.dump(), "future data never reset by retry");
    files->contents[files->inbox.inbox_file] = "{damaged";
    expect(inbox.load(files->inbox).status == Load::invalid && inbox.accept(files->inbox, payload()).status == Write::invalid_existing && files->contents.at(files->inbox.inbox_file) == "{damaged", "corrupt data retained");
    files->contents.erase(files->inbox.inbox_file); files->contents[files->inbox.backup_file] = external_original;
    expect(inbox.accept(files->inbox, payload()).status == Write::invalid_existing && !files->contents.contains(files->inbox.inbox_file), "missing main plus existing backup cannot allocate replacement identity");
    auto alias = files->inbox; alias.lock_file = alias.inbox_file;
    expect(inbox.accept(alias, payload()).status == Write::invalid_existing, "inbox aliases rejected");
    expect(files->locks.empty(), "all fault paths release locks");
}
void disk(const std::shared_ptr<cpn::FileStore>& files, const std::filesystem::path& folder, const std::filesystem::path& executable) {
    const auto paths = paths_in(folder); const auto session_paths = session_paths_in(folder); auto inbox = service(files);
    const auto first = inbox.accept(paths, payload()); expect(first.status == Write::accepted, "native Chinese path durable admission");
    cpn::CompanionInboxService restarted("workspace-stable", files, {}, [] { return "must-not-allocate-new-inbox"; });
    auto loaded = restarted.load(paths); expect(loaded.status == Load::loaded && loaded.snapshot->inbox_id == "fixed-inbox-e\u0301", "restarted instance uses persisted identity");
    expect(restarted.accept(paths, payload()).item->import_id == first.item->import_id, "restart duplicate identity stable");
    const auto initial = initial_session(); const auto prepared = cpn::prepare_import_application(initial, *loaded.snapshot, first.item->import_id, document_defaults());
    cpn::SessionService sessions(files); const auto saved = sessions.save(session_paths, *prepared.session, std::nullopt);
    expect(saved.status == cpn::SessionSaveStatus::saved, "real session atomically contains document and receipt");
    expect(restarted.mark_applied(paths, session_paths, first.item->import_id).status == Write::applied, "real receipt verified and ack persisted");
    expect(restarted.load(paths).snapshot->items[0].state == cpn::CompanionInboxState::applied, "applied state survives reload");
    expect(files->read(paths.backup_file, 65536).find("Pending") != std::string::npos, "real prior Pending snapshot preserved in backup");
    const auto source = folder / cpn::path_from_utf8("用户 文件.cpp"); files->write_atomic(source, "unchanged user source");
    std::barrier start{3}; cpn::CompanionInboxWriteResult left, right; auto other = service(files);
    std::jthread a([&] { start.arrive_and_wait(); left = inbox.accept(paths, payload("https://concurrent.test/same")); });
    std::jthread b([&] { start.arrive_and_wait(); right = other.accept(paths, payload("https://concurrent.test/same")); });
    start.arrive_and_wait(); a.join(); b.join();
    expect((left.status == Write::accepted && right.status == Write::duplicate) || (right.status == Write::accepted && left.status == Write::duplicate), "cooperating instances have one accept and one duplicate");
    expect(left.item->import_id == right.item->import_id, "concurrent duplicates share the durable ID");
    std::vector<cpn::CompanionInboxWriteResult> distinct(8);
    std::vector<std::jthread> writers;
    for (std::size_t i = 0; i < distinct.size(); ++i) writers.emplace_back([&, i] {
        auto independent = service(files);
        distinct[i] = independent.accept(paths, payload("https://concurrent.test/" + std::to_string(i)));
    });
    for (auto& writer : writers) writer.join();
    std::set<std::string> accepted_ids;
    for (const auto& result : distinct) {
        expect(result.status == Write::accepted && result.item, "parallel distinct admission succeeds without overwriting latest snapshot");
        accepted_ids.insert(result.item->import_id);
    }
    expect(accepted_ids.size() == distinct.size(), "parallel distinct deliveries get distinct stable IDs");
    loaded = restarted.load(paths);
    expect(loaded.snapshot->items.size() == distinct.size() + 2, "no accepted record lost across independent writers");
    auto held = files->acquire_lock(paths.lock_file, std::chrono::milliseconds{0});
    cpn::ProcessSpec child; child.executable = executable; child.arguments = {"accept-child", cpn::path_to_utf8(folder)};
    child.limits.wall_timeout = std::chrono::milliseconds{5000}; child.working_directory = folder;
    auto blocked = cpn::run_process(child);
    expect(blocked.termination == cpn::ProcessTermination::exited && blocked.exit_code == 3, "actual other process cannot accept while inbox lock held");
    held.reset(); const auto child_result = cpn::run_process(child);
    expect(child_result.termination == cpn::ProcessTermination::exited && child_result.exit_code == 0, "actual other process accepts after lock release");
    loaded = restarted.load(paths);
    expect(loaded.snapshot->items.back().candidate.url == "https://child.test/problem", "child acceptance visible to parent reload");
    expect(std::filesystem::exists(paths.lock_file), "inbox lock inode retained across operations");
    const auto second = loaded.snapshot->items.back(); auto candidate = *sessions.load(session_paths.recovery_file).session;
    candidate = *cpn::prepare_import_application(candidate, *loaded.snapshot, second.import_id).session;
    candidate.documents.front().original_path = paths.inbox_file; // A user path alias cannot authorize ack to this inbox.
    auto written = sessions.save(session_paths, candidate, saved.disk_digest); expect(written.status == cpn::SessionSaveStatus::saved, "source alias fixture is valid standalone session");
    expect(restarted.mark_applied(paths, session_paths, second.import_id).status == Write::invalid_existing, "ack refuses storage alias with user source path");
    expect(files->read(source, 128) == "unchanged user source", "all intake/apply/ack keeps user file untouched");
    const auto hardlink = folder / "inbox-hardlink.json"; std::error_code error; std::filesystem::create_hard_link(paths.inbox_file, hardlink, error);
    if (!error) {
        auto alias = paths; alias.backup_file = hardlink;
        expect(restarted.accept(alias, payload()).status == Write::invalid_existing, "hardlink storage alias rejected");
    }
}
int child(const std::filesystem::path& folder) {
    auto files = cpn::native_file_store(); auto inbox = service(files);
    const auto result = inbox.accept(paths_in(folder), payload("https://child.test/problem"), std::chrono::milliseconds{0});
    if (result.status == Write::lock_unavailable) return 3;
    if (result.status == Write::accepted || result.status == Write::duplicate) return 0;
    std::cerr << cpn::companion_inbox_write_status_name(result.status) << ": " << result.detail << '\n'; return 4;
}
int verify(const std::filesystem::path& executable) {
    try {
        auto files = cpn::native_file_store(); Workspace workspace(files);
        format(workspace.path / "format"); admission(workspace.path / "admission"); capacity(workspace.path / "capacity");
        preparation(workspace.path / "preparation"); acknowledgement(workspace.path / "ack"); history(workspace.path / "history"); faults(workspace.path / "faults");
        disk(files, workspace.path, std::filesystem::absolute(executable));
        std::cout << "companion inbox tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"accept-child") return child(std::filesystem::path(argv[2]));
    return argc == 1 ? verify(std::filesystem::path(argv[0])) : 2;
}
#else
int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "accept-child") return child(cpn::path_from_utf8(argv[2]));
    return argc == 1 ? verify(cpn::path_from_utf8(argv[0])) : 2;
}
#endif
