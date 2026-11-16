// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/session.hpp"
#include "cpn/digest.hpp"
#include "cpn/json.hpp"
#include <nlohmann/json.hpp>
#include <barrier>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <thread>

namespace {
using Json = nlohmann::json;
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
cpn::SessionPaths paths_in(const std::filesystem::path& folder) {
    return {folder / cpn::path_from_utf8("恢复 会话.json"), folder / cpn::path_from_utf8("此前 会话.json"), folder / "session.lock"};
}
cpn::WorkspaceSession fixture(const std::filesystem::path& original = {}) {
    cpn::WorkspaceSession value; value.workspace_id = "workspace-stable"; value.revision = 11; value.selected_document_id = "未保存-document";
    cpn::SessionDocument unsaved; unsaved.document_id = "未保存-document"; unsaved.revision = 17;
    unsaved.source_text = "// 中文 😀\r\n"; unsaved.source_text.push_back('\0'); unsaved.source_text += "tail";
    unsaved.toolchain_profile_id = "user-profile-reference";
    unsaved.tests = {{"missing", "", std::nullopt, true}, {"empty", "", std::string{}, true},
        {"raw", std::string("a\0\xff", 3), std::string("a\0\xff", 3), false}};
    unsaved.editor_preferences = {{"future.native.editor", 99, "{ \"opaque\": 18446744073709551616, \"anchor\": [1,2] }"}};
    value.documents.push_back(unsaved);
    cpn::SessionDocument changed; changed.document_id = "original-document"; changed.revision = 5; changed.source_text = "unsaved replacement\n";
    if (!original.empty()) {
        changed.original_path = original;
        changed.save_point = cpn::DocumentSavePoint{4, cpn::sha256("old disk content"), cpn::sha256("old disk content")};
    }
    changed.language_id = "cpp"; value.documents.push_back(changed);
    value.editor_preferences = {{"unknown.native.window", std::numeric_limits<std::uint64_t>::max(), "{\n \"flag\":true, \"unknown\":{\"key\":\"value\"}\n}"}};
    return value;
}
void roundtrip(const cpn::WorkspaceSession& value) {
    const auto encoded = cpn::serialize_workspace_session(value); const auto restored = cpn::parse_workspace_session(encoded);
    expect(cpn::serialize_workspace_session(restored) == encoded, "session canonical roundtrip");
    expect(restored.documents[0].source_text == value.documents[0].source_text && !restored.documents[0].original_path, "unsaved source and path absence");
    expect(!restored.documents[0].tests[0].expected && restored.documents[0].tests[1].expected == "", "absent and empty expected remain distinct");
    expect(restored.documents[0].tests[2].input == std::string("a\0\xff", 3) && !restored.documents[0].tests[2].enabled, "raw test bytes and enabled flag");
    expect(restored.documents[0].editor_preferences[0].object_json == value.documents[0].editor_preferences[0].object_json, "opaque JSON bytes are not interpreted");
    expect(restored.editor_preferences[0].schema_version == std::numeric_limits<std::uint64_t>::max(), "unknown native schema preserved exactly");
}
void format() {
    const auto value = fixture(); roundtrip(value);
    auto maximum = value; maximum.revision = std::numeric_limits<std::uint64_t>::max(); maximum.documents[0].revision = maximum.revision;
    roundtrip(maximum);
    const auto encoded = cpn::serialize_workspace_session(value); const auto baseline = Json::parse(encoded);
    const auto invalid = [&](Json object, const char* message) { rejected([&] { (void)cpn::parse_workspace_session(object.dump()); }, message); };
    for (const auto& revision : std::vector<Json>{-1, 1.5, true, nullptr, "1", 18446744073709551616.0}) {
        auto object = baseline; object["revision"] = revision; invalid(object, "strict revision integer");
    }
    for (const auto& schema : std::vector<Json>{0, 2, -4294967295LL, 4294967297ULL, 1.0, true}) {
        auto object = baseline; object["schema_version"] = schema; invalid(object, "strict supported session schema");
    }
    auto object = baseline; object["documents"][0]["source_text"] = "checksum changed"; invalid(object, "source checksum protects content");
    object = baseline; object["documents"][1]["document_id"] = object["documents"][0]["document_id"]; invalid(object, "document stable IDs unique");
    object = baseline; object["documents"][0]["tests"][1]["id"] = "missing"; invalid(object, "test stable IDs unique");
    object = baseline; object["selected_document_id"] = "does-not-exist"; invalid(object, "selected document exists");
    object = baseline; object["workspace_id"] = std::string("a\0b", 3); invalid(object, "ID NUL rejected");
    object = baseline; object["documents"][0]["original_path"] = std::string("a\0b", 3); invalid(object, "path NUL rejected");
    object = baseline; object["documents"][0]["tests"][0]["enabled"] = 1; invalid(object, "enabled must be boolean");
    object = baseline; object["documents"][0]["tests"][0]["input"] = {{"encoding", "base64"}, {"data", "Zh=="}}; invalid(object, "base64 padding canonical");
    object = baseline; object["documents"][0]["artifact_name"] = "temporary-artifact"; invalid(object, "no process artifacts in session schema");
    object = baseline; object["job_dir"] = "temporary-job"; invalid(object, "no process workspaces in session schema");
    object = baseline; object["documents"][0]["editor_preferences"].push_back(object["documents"][0]["editor_preferences"][0]); invalid(object, "native namespaces unique per version");
    object = baseline; object["documents"][0]["editor_preferences"][0]["object_json"] = R"({"duplicate":1,"duplicate":2})"; invalid(object, "duplicate opaque object keys rejected");
    object = baseline; object["documents"][0]["editor_preferences"][0]["object_json"] = "[]"; invalid(object, "opaque preferences must be object");
    rejected([&] { (void)cpn::parse_workspace_session("{\"schema_version\":1,\"schema_version\":1}"); }, "duplicate root keys rejected");
    rejected([&] { (void)cpn::parse_workspace_session(std::string(70, '[') + "0" + std::string(70, ']')); }, "nested JSON bounded");
    auto invalid_utf8 = value; invalid_utf8.documents[0].source_text.assign(1, static_cast<char>(0xff));
    rejected([&] { (void)cpn::serialize_workspace_session(invalid_utf8); }, "invalid UTF8 source rejected");
    const auto with_bounds = [&](cpn::SessionBounds bounds) { rejected([&] { (void)cpn::serialize_workspace_session(value, bounds); }, "configured session limit"); };
    cpn::SessionBounds bounds; bounds.max_json_bytes = encoded.size() - 1; with_bounds(bounds);
    rejected([&] { (void)cpn::parse_workspace_session(encoded, bounds); }, "load JSON byte limit");
    bounds = {}; bounds.max_documents = 1; with_bounds(bounds); bounds = {}; bounds.max_tests_per_document = 2; with_bounds(bounds);
    bounds = {}; bounds.max_total_tests = 2; with_bounds(bounds); bounds = {}; bounds.max_source_bytes = 1; with_bounds(bounds);
    bounds = {}; bounds.max_test_bytes = 1; with_bounds(bounds); bounds = {}; bounds.max_total_content_bytes = 1; with_bounds(bounds);
    bounds = {}; bounds.max_preferences_per_owner = 0; with_bounds(bounds); bounds = {}; bounds.max_preferences_bytes = 1; with_bounds(bounds);
    bounds = {}; bounds.max_total_preferences_bytes = 1; with_bounds(bounds); bounds = {}; bounds.max_json_depth = 1; with_bounds(bounds);
    bounds = {}; bounds.max_preferences_depth = 1; with_bounds(bounds); bounds = {}; bounds.max_id_bytes = 1; with_bounds(bounds);
    cpn::WorkspaceSession empty; empty.workspace_id = "empty";
    expect(cpn::parse_workspace_session(cpn::serialize_workspace_session(empty)).documents.empty(), "empty workspace supported");
}
class MemoryStore final : public cpn::FileStore {
public:
    explicit MemoryStore(cpn::SessionPaths paths) : paths(std::move(paths)) {}
    struct Lock final : cpn::FileLock {
        explicit Lock(const MemoryStore* store) : store(store) { ++store->locks; }
        ~Lock() override { --store->locks; }
        const MemoryStore* store;
    };
    std::string read(const std::filesystem::path& path, std::size_t maximum) const override {
        const auto value = read_optional(path, maximum); if (!value) throw std::runtime_error("missing"); return *value;
    }
    std::optional<std::string> read_optional(const std::filesystem::path& path, std::size_t maximum) const override {
        if (fail_read && path == *fail_read) throw std::runtime_error("injected read failure");
        const auto found = contents.find(path); if (found == contents.end()) return std::nullopt;
        if (found->second.size() > maximum) throw std::runtime_error("read budget"); return found->second;
    }
    std::unique_ptr<cpn::FileLock> acquire_lock(const std::filesystem::path&, std::chrono::milliseconds, std::stop_token stop) const override {
        if (stop.stop_requested()) throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        if (fail_lock) throw std::system_error(std::make_error_code(*fail_lock)); return std::make_unique<Lock>(this);
    }
    void write_atomic(const std::filesystem::path& path, std::string_view value) const override {
        expect(locks == 1, "all recovery writes protected by cooperative lock");
        if (fail_write && path == *fail_write && !fail_after) throw cpn::AtomicWriteError("injected before replace", false);
        contents[path] = std::string(value); writes.push_back(path);
        if (fail_write && path == *fail_write && fail_after) throw cpn::AtomicWriteError("injected after replace", true);
        if (after_write) after_write(path);
    }
    std::filesystem::path create_workspace() const override { throw std::runtime_error("session must not allocate a job workspace"); }
    void remove_workspace(const std::filesystem::path&) const noexcept override {}
    cpn::SessionPaths paths;
    mutable std::map<std::filesystem::path, std::string> contents;
    mutable std::vector<std::filesystem::path> writes;
    mutable int locks{0};
    std::optional<std::filesystem::path> fail_read, fail_write;
    std::optional<std::errc> fail_lock;
    bool fail_after{false};
    std::function<void(const std::filesystem::path&)> after_write;
};
void faults(const std::filesystem::path& folder) {
    auto paths = paths_in(folder); const auto initial = fixture(); auto replacement = initial; replacement.revision = 12;
    const auto original = cpn::serialize_workspace_session(initial); const auto expected = cpn::sha256(original);
    for (const auto& target : {paths.backup_file, paths.recovery_file}) for (const bool after : {false, true}) {
        auto files = std::make_shared<MemoryStore>(paths); files->contents[paths.recovery_file] = original;
        files->fail_write = target; files->fail_after = after; cpn::SessionService service(files);
        const auto result = service.save(paths, replacement, expected);
        expect(result.status == (after ? cpn::SessionSaveStatus::durability_unknown : cpn::SessionSaveStatus::io_failed), "atomic failure has explicit commit classification");
        expect(result.recovery_replaced == (after && target == paths.recovery_file), "recovery replacement accurately reported");
        expect(result.backup_replaced == (target == paths.recovery_file || after), "backup replacement accurately reported");
        if (!result.recovery_replaced) expect(files->contents[paths.recovery_file] == original && result.disk_digest == expected, "before-commit failure keeps recovery and token");
        else expect(result.disk_digest == cpn::sha256(files->contents[paths.recovery_file]), "post-commit digest reflects replacement");
        expect(service.load(paths.recovery_file).status == cpn::SessionLoadStatus::loaded, "valid recovery survives every injected write failure");
    }
    auto files = std::make_shared<MemoryStore>(paths); files->contents[paths.recovery_file] = original; cpn::SessionService service(files);
    files->fail_read = paths.backup_file; expect(service.save(paths, replacement, expected).status == cpn::SessionSaveStatus::io_failed, "backup read failure is IO failure");
    files->fail_read = paths.recovery_file; expect(service.load(paths.recovery_file).status == cpn::SessionLoadStatus::io_failed, "load read failure distinguished from corrupt JSON"); files->fail_read.reset();
    files->fail_lock = std::errc::timed_out; expect(service.save(paths, replacement, expected).status == cpn::SessionSaveStatus::lock_unavailable, "lock timeout structured"); files->fail_lock.reset();
    std::stop_source stop; files->after_write = [&](const auto& path) { if (path == paths.backup_file) stop.request_stop(); };
    auto result = service.save(paths, replacement, expected, std::chrono::milliseconds{100}, stop.get_token());
    expect(result.status == cpn::SessionSaveStatus::cancelled && result.backup_replaced && !result.recovery_replaced && files->contents[paths.recovery_file] == original, "cancel between backup and recovery preserves original");
    files->writes.clear(); files->after_write = {}; std::stop_source pre; pre.request_stop();
    expect(service.save(paths, replacement, expected, std::chrono::milliseconds{100}, pre.get_token()).status == cpn::SessionSaveStatus::cancelled && files->writes.empty(), "pre-cancelled save writes nothing");
    std::stop_source after_commit; files->after_write = [&](const auto& path) { if (path == paths.recovery_file) after_commit.request_stop(); };
    result = service.save(paths, replacement, expected, std::chrono::milliseconds{100}, after_commit.get_token());
    expect(result.status == cpn::SessionSaveStatus::saved && result.recovery_replaced && after_commit.stop_requested(), "cancellation after atomic commit cannot undo saved status");
    files->contents[paths.recovery_file] = original;
    auto externally_changed = initial; externally_changed.revision = 99; const auto external = cpn::serialize_workspace_session(externally_changed);
    files->after_write = [&](const auto& path) { if (path == paths.backup_file) files->contents[paths.recovery_file] = external; };
    result = service.save(paths, replacement, expected);
    expect(result.status == cpn::SessionSaveStatus::conflict && !result.recovery_replaced && files->contents[paths.recovery_file] == external, "optimistic recheck preserves external modification during backup");
    files->after_write = {}; files->writes.clear();
    expect(service.save(paths, replacement, expected).status == cpn::SessionSaveStatus::conflict && files->writes.empty(), "stale token causes no data writes");
    auto corrupt = Json::parse(original); corrupt["schema_version"] = 2; corrupt["future_field"] = true;
    const auto future = corrupt.dump(); files->contents[paths.recovery_file] = future;
    expect(service.load(paths.recovery_file).status == cpn::SessionLoadStatus::unsupported_schema && !service.load(paths.recovery_file).session, "future schema is not applied");
    expect(service.save(paths, replacement, cpn::sha256(future)).status == cpn::SessionSaveStatus::invalid_existing && files->contents[paths.recovery_file] == future, "future recovery never overwritten even with matching token");
    files->contents[paths.recovery_file] = "{damaged";
    expect(service.load(paths.recovery_file).status == cpn::SessionLoadStatus::invalid && !service.load(paths.recovery_file).session, "corrupt candidate never applied");
    expect(service.save(paths, replacement, cpn::sha256("{damaged")).status == cpn::SessionSaveStatus::invalid_existing && files->contents[paths.recovery_file] == "{damaged", "damaged recovery retained");
    files->contents[paths.recovery_file] = original; files->contents[paths.backup_file] = "broken backup";
    expect(service.save(paths, replacement, expected).status == cpn::SessionSaveStatus::invalid_existing && files->contents[paths.backup_file] == "broken backup" && files->contents[paths.recovery_file] == original, "damaged backup retained before main replacement");
    auto alias = paths; alias.backup_file = alias.recovery_file; files->writes.clear();
    expect(service.save(alias, replacement, expected).status == cpn::SessionSaveStatus::invalid_session && files->writes.empty(), "storage paths cannot alias");
}
void disk(const std::shared_ptr<cpn::FileStore>& files, const std::filesystem::path& folder) {
    const auto paths = paths_in(folder); const auto user_file = folder / cpn::path_from_utf8("用户 源文件.cpp"); files->write_atomic(user_file, "old disk content");
    auto value = fixture(user_file); cpn::SessionService service(files);
    expect(service.load(paths.recovery_file).status == cpn::SessionLoadStatus::missing, "only absent recovery classified missing");
    auto first = service.save(paths, value, std::nullopt);
    expect(first.status == cpn::SessionSaveStatus::saved && first.recovery_replaced && !first.backup_replaced && first.disk_digest, "new native recovery saved");
    const auto original = files->read(paths.recovery_file, 65536);
    files->write_atomic(user_file, "externally changed");
    auto restored = service.load(paths.recovery_file);
    expect(restored.status == cpn::SessionLoadStatus::loaded && restored.disk_digest == first.disk_digest, "native Chinese path recovery roundtrip");
    expect(restored.session->documents[1].source_text == "unsaved replacement\n" && files->read(user_file, 65536) == "externally changed", "unsaved recovery wins without reloading or changing user file");
    expect(restored.session->documents[1].save_point->disk_digest == cpn::sha256("old disk content"), "external disk conflict metadata retained");
    roundtrip(*restored.session);
    value.revision = 12; value.documents[0].revision = 18; value.documents[0].source_text += "new";
    auto second = service.save(paths, value, first.disk_digest);
    expect(second.status == cpn::SessionSaveStatus::saved && second.backup_replaced, "validated previous recovery backed up");
    expect(files->read(paths.backup_file, 65536) == original && service.load(paths.backup_file).session->revision == 11, "backup contains exact previous good data");
    const auto current = files->read(paths.recovery_file, 65536);
    expect(service.save(paths, value, first.disk_digest).status == cpn::SessionSaveStatus::conflict && files->read(paths.recovery_file, 65536) == current, "native optimistic conflict preserves current");
    expect(service.save(paths, value, std::nullopt).status == cpn::SessionSaveStatus::conflict, "create-only cannot overwrite an existing recovery");
    auto invalid = value; invalid.documents[0].original_path = paths.recovery_file;
    expect(service.save(paths, invalid, second.disk_digest).status == cpn::SessionSaveStatus::invalid_session, "recovery path cannot replace source file");
    std::barrier start{3}; cpn::SessionService other(files); cpn::SessionSaveResult left, right;
    auto left_value = value, right_value = value; left_value.revision = 13; right_value.revision = 14;
    std::jthread writer1([&] { start.arrive_and_wait(); left = service.save(paths, left_value, second.disk_digest); });
    std::jthread writer2([&] { start.arrive_and_wait(); right = other.save(paths, right_value, second.disk_digest); });
    start.arrive_and_wait(); writer1.join(); writer2.join();
    expect((left.status == cpn::SessionSaveStatus::saved && right.status == cpn::SessionSaveStatus::conflict) ||
           (right.status == cpn::SessionSaveStatus::saved && left.status == cpn::SessionSaveStatus::conflict), "cooperative two-instance saves have one winner");
    expect(files->read(user_file, 65536) == "externally changed", "all persistence keeps original source untouched");
}
}
int main() {
    auto files = cpn::native_file_store();
    try {
        Workspace workspace(files); format(); faults(workspace.path); disk(files, workspace.path);
        std::cout << "session tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
