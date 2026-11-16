#include "CompanionModel.h"
#include "PersistenceGate.h"
#include "RevisionPolicy.h"
#include "cpn/digest.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
using namespace cpn::windows;
namespace {
using Json = nlohmann::json;
void expect(bool condition, char const* message) { if (!condition) throw std::runtime_error(message); }
template<class Action> void rejects(Action action, char const* message) { bool rejected{}; try { action(); } catch (std::exception const&) { rejected = true; } expect(rejected, message); }
std::string read(std::filesystem::path const& path) { std::ifstream file(path, std::ios::binary); if (!file) throw std::runtime_error("Missing configuration."); return {std::istreambuf_iterator<char>{file}, {}}; }
struct Workspace {
    std::shared_ptr<cpn::FileStore> files = cpn::native_file_store();
    std::filesystem::path root = files->create_workspace();
    std::filesystem::path path = root / cpn::path_from_utf8("Windows 原生导入 😀");
    Workspace() { std::filesystem::create_directory(path); }
    ~Workspace() { files->remove_workspace(root); }
};
RunnerConfiguration runner() {
    RunnerConfiguration result; result.maximum_documents = 16; result.maximum_tests = 128;
    result.default_comparison = "trailing"; result.comparisons = {{"trailing", "ComparisonTrailing"}};
    ToolchainChoice choice; choice.id = "fixture"; choice.file = "fixture.json"; choice.label_resource = "ToolchainMSVC";
    choice.profile = {{"schema_version", 1}, {"id", choice.id}, {"language_id", "cpp"}, {"source_filename", "main.cpp"}, {"artifact_name", ""},
        {"run", {{"executable", "fixture"}, {"arguments", Json::array()}}}};
    result.toolchains = {choice}; result.default_toolchains = {{"cpp", choice.id}}; return result;
}
cpn::WorkspaceSession base_session(RecoveryConfiguration const& recovery, CompanionConfiguration const& companion, cpn::CompanionInboxPaths const& paths) {
    cpn::SessionDocument doc; doc.document_id = "existing 中文"; doc.revision = 9007199254740993ULL; doc.language_id = "cpp"; doc.toolchain_profile_id = "fixture";
    doc.source_text = std::string{"A\r\n\0中", 7}; doc.tests = {{"old-test", "A", std::string{}, true}};
    DocumentPreferences prefs; prefs.comparison = "trailing";
    doc.editor_preferences = {RecoveryModel::document_preferences(prefs, recovery.document_namespace), {"opaque.editor", 99, "{ \"large\":18446744073709551616 }"}};
    auto choices = runner();
    return {"workspace 中文", 9007199254740993ULL, doc.document_id, {doc}, {CompanionModel::binding_preference({"workspace 中文", paths}, companion.storage_namespace),
        RecoveryModel::workspace_preferences(choices.toolchains, recovery.workspace_namespace), {"opaque.workspace", 99, "{ \"f\":1.2300e+02 }"}}};
}
std::string payload(std::string const& name = "题目 😀", bool interactive = false) {
    auto value = Json{{"name", name}, {"group", "Group"}, {"url", "https://cpnative.test/task"}, {"timeLimit", 1000}, {"memoryLimit", 256},
        {"tests", {{{"input", std::string{"x\0\r\n", 4}}, {"output", ""}}, {{"input", "中"}, {"output", "中\n"}}}}, {"interactive", interactive}}.dump();
    value.pop_back(); return "  " + value + ", \"unknown\":{\"large\":18446744073709551616,\"f\":1.2300e+02}}\n";
}
cpn::ImportDocumentDefaults import_defaults(RecoveryConfiguration const& config) {
    cpn::ImportDocumentDefaults result; result.language_id = "cpp"; result.toolchain_profile_id = "fixture"; result.source_text = "// template\r\n";
    DocumentPreferences prefs; prefs.comparison = "trailing"; result.document_preferences = {RecoveryModel::document_preferences(prefs, config.document_namespace)}; return result;
}
CompanionPage page(CompanionWorker& worker, CompanionConfiguration const& config, cpn::CompanionInboxPaths const& paths, std::uint64_t after = 0) {
    return CompanionModel::page_response(worker.request(CompanionWorker::Operation::page, CompanionModel::page_request("workspace 中文", paths, after, config)).get());
}
CompanionPreparation prepare(CompanionWorker& worker, cpn::CompanionInboxPaths const& paths, std::string const& id, cpn::WorkspaceSession const& base, RecoveryConfiguration const& recovery) {
    return CompanionModel::prepare_response(worker.request(CompanionWorker::Operation::prepare, CompanionModel::prepare_request(base.workspace_id, paths, id, base, import_defaults(recovery))).get());
}
void models(CompanionConfiguration const& config, RecoveryConfiguration const& recovery, std::filesystem::path const& directory, std::string const& configuration_bytes) {
    auto paths = config.paths(directory / "recovery.json"); auto base = base_session(recovery, config, paths);
    auto owner = CompanionModel::binding(base.editor_preferences, config.storage_namespace);
    expect(owner && CompanionModel::same_binding(*owner, {base.workspace_id, paths}), "Storage owner round trip changed identity.");
    auto prefs = base.editor_preferences; prefs.push_back(prefs.front());
    rejects([&] { (void)CompanionModel::binding(prefs, config.storage_namespace); }, "Duplicate storage owner accepted.");
    prefs = base.editor_preferences; prefs.front().schema_version = 99;
    rejects([&] { (void)CompanionModel::binding(prefs, config.storage_namespace); }, "Future storage owner downgraded.");
    CompanionModel::validate_paths(paths, {directory / "source.cpp"});
    rejects([&] { CompanionModel::validate_paths(paths, {paths.inbox_file}); }, "Inbox alias accepted as a source target.");
    cpn::native_file_store()->write_atomic(paths.inbox_file, "{}");
    auto alias = directory / "alias.cpp"; std::filesystem::create_hard_link(paths.inbox_file, alias);
    rejects([&] { CompanionModel::validate_paths(paths, {alias}); }, "Hard link inbox alias accepted.");
    std::filesystem::remove(paths.inbox_file); std::filesystem::remove(alias);
    expect(CompanionModel::preview(std::string{"x\0\xFF", 3}, 2) == "78 00 ", "Binary preview lost byte boundaries.");
    expect(CompanionModel::preview("a中", 3) == "a", "UTF-8 preview split a scalar.");
    auto future = base.documents[0]; future.editor_preferences.push_back({std::string{cpn::companion_document_namespace}, 99, "{}"});
    expect(CompanionModel::run_capability(base.documents[0]) == CompanionRunCapability::ordinary && CompanionModel::run_capability(future) == CompanionRunCapability::unavailable, "Future provenance became ordinary runnable.");
    auto bad = Json::parse(configuration_bytes); bad["inboxSuffix"] = "../data";
    rejects([&] { (void)CompanionConfiguration::parse(bad.dump()); }, "Storage suffix could escape its directory.");
    std::cout << "configuration, storage identity, aliases, bounded previews and provenance passed\n";
}
void import_flow(CompanionConfiguration const& config, RecoveryConfiguration const& recovery, std::filesystem::path const& directory) {
    auto paths = config.paths(directory / "recovery.json"); auto saved_paths = recovery.paths(directory / "recovery.json");
    CompanionWorker worker(config.worker); SessionWorker sessions(recovery.options); auto base = base_session(recovery, config, paths);
    auto empty = page(worker, config, paths); expect(empty.status == cpn::CompanionInboxLoadStatus::missing && empty.items.empty(), "Initial inbox was not missing.");
    auto original = payload(); auto accepted = CompanionModel::write_response(worker.request(CompanionWorker::Operation::accept, CompanionModel::accept_request(base.workspace_id, paths, original, config)).get());
    expect(accepted.status == "Accepted" && accepted.inbox_replaced, "Raw import did not reach actual storage.");
    expect(CompanionModel::write_response(worker.request(CompanionWorker::Operation::accept, CompanionModel::accept_request(base.workspace_id, paths, original, config)).get()).status == "Duplicate", "Identical payload replay added an item.");
    auto loaded = page(worker, config, paths); expect(loaded.items.size() == 1 && loaded.items[0].candidate.original_json == original && loaded.items[0].candidate.tests[0].input == std::string{"x\0\r\n", 4}, "Page changed unknown lexemes or NUL sample bytes.");
    CompanionPagingState paging; expect(paging.install(loaded, base.workspace_id, true), "First page failed.");
    auto changed = loaded; ++*changed.revision; expect(!paging.install(changed, base.workspace_id, false) && paging.items.size() == 1, "Changed snapshot appended a misleading page.");
    auto tampered = Json::parse(worker.request(CompanionWorker::Operation::page, CompanionModel::page_request(base.workspace_id, paths, 0, config)).get());
    tampered["items"][0]["import_id"] = "forged";
    rejects([&] { (void)CompanionModel::page_response(tampered.dump()); }, "Page trusted forged stable identities.");
    const auto id = loaded.items[0].import_id; auto prepared = prepare(worker, paths, id, base, recovery);
    auto delta = CompanionModel::delta(base, prepared, *loaded.inbox_id, id, recovery, runner(), {"cpp"}, "cpp");
    auto edited = base; edited.documents[0].source_text = "C\r\n"; ++edited.documents[0].revision; edited.documents[0].tests[0].input = "test C";
    auto merged = CompanionModel::merge(edited, base, delta);
    expect(merged.revision == edited.revision && merged.documents[0].source_text == "C\r\n" && merged.documents[0].tests[0].input == "test C" &&
        merged.documents[1].tests[0].expected == std::optional<std::string>{""} && merged.documents[1].source_text == "// template\r\n", "Delta lost edits made during prepare or empty expected bytes.");
    expect(CompanionModel::run_capability(merged.documents[1]) == CompanionRunCapability::ordinary, "Ordinary imported source was blocked.");
    auto manipulated = prepared; manipulated.session->documents[0].source_text = "replaced";
    rejects([&] { (void)CompanionModel::delta(base, manipulated, *loaded.inbox_id, id, recovery, runner(), {"cpp"}, "cpp"); }, "Delta could overwrite an existing editor.");
    auto mark_request = CompanionModel::mark_request(base.workspace_id, paths, id, saved_paths, config);
    expect(CompanionModel::write_response(worker.request(CompanionWorker::Operation::mark_applied, mark_request).get()).status == "SessionUnavailable", "Unsaved receipt became Applied.");
    merged.revision = RevisionPolicy::persistence_revision(merged.revision, true);
    auto saved = RecoveryModel::save_response(sessions.save(RecoveryModel::save_request(saved_paths, merged, {}, recovery.lock_timeout)).get());
    expect(saved.status == cpn::SessionSaveStatus::saved, "Merged native session did not save.");
    auto restored = RecoveryModel::load_response(sessions.load(cpn::path_to_utf8(saved_paths.recovery_file)).get());
    expect(restored.session && cpn::serialize_workspace_session(*restored.session) == cpn::serialize_workspace_session(merged), "Real restored import lost its atomic receipt.");
    expect(CompanionModel::write_response(worker.request(CompanionWorker::Operation::mark_applied, mark_request).get()).status == "Applied", "Saved receipt could not mark Applied.");
    auto removed = merged; removed.documents.pop_back(); removed.selected_document_id = removed.documents[0].document_id;
    expect(prepare(worker, paths, id, removed, recovery).status == "AlreadyApplied", "Receipt replay recreated a deliberately closed document.");
    auto old_owner = CompanionModel::binding(restored.session->editor_preferences, config.storage_namespace);
    expect(old_owner && old_owner->paths.inbox_file == paths.inbox_file && old_owner->paths.inbox_file != config.paths(directory / "new-recovery.json").inbox_file, "SaveRecoveryAs would silently move inbox identity.");
    auto next = payload("interactive", true); expect(CompanionModel::write_response(worker.request(CompanionWorker::Operation::accept, CompanionModel::accept_request(base.workspace_id, paths, next, config)).get()).status == "Accepted", "Interactive payload could not be preserved.");
    auto all = page(worker, config, paths); expect(all.items.size() == 2 && all.items[1].state == cpn::CompanionInboxState::pending, "Unsupported problem was discarded.");
    auto interactive = prepare(worker, paths, all.items[1].import_id, merged, recovery);
    auto unsupported = CompanionModel::delta(merged, interactive, *all.inbox_id, all.items[1].import_id, recovery, runner(), {"cpp"}, "cpp");
    expect(CompanionModel::run_capability(unsupported.document) == CompanionRunCapability::unsupported, "Interactive source enabled ordinary Run.");
    worker.close(); sessions.close(); rejects([&] { (void)worker.request(CompanionWorker::Operation::page, "{}"); }, "Closed worker accepted a request.");
    std::cout << "real C ABI import/page/prepare/delta/recovery/mark/replay and unsupported mode passed\n";
}
void final_revision(CompanionConfiguration const& config, RecoveryConfiguration const& recovery, std::filesystem::path const& directory) {
    auto paths = config.paths(directory / "max-recovery.json"); auto saved_paths = recovery.paths(directory / "max-recovery.json");
    CompanionWorker worker(config.worker); SessionWorker sessions(recovery.options); auto base = base_session(recovery, config, paths); base.revision = UINT64_MAX - 1;
    auto owner_saved = RecoveryModel::save_response(sessions.save(RecoveryModel::save_request(saved_paths, base, {}, recovery.lock_timeout)).get());
    expect(owner_saved.status == cpn::SessionSaveStatus::saved, "MAX-1 owner did not persist.");
    (void)worker.request(CompanionWorker::Operation::accept, CompanionModel::accept_request(base.workspace_id, paths, payload("last"), config)).get();
    auto loaded = page(worker, config, paths); auto id = loaded.items[0].import_id; auto prepared = prepare(worker, paths, id, base, recovery);
    auto delta = CompanionModel::delta(base, prepared, *loaded.inbox_id, id, recovery, runner(), {"cpp"}, "cpp");
    auto current = base; current.documents[0].source_text = "C";
    auto merged = CompanionModel::merge(current, base, delta);
    PersistenceGate gate; const auto apply_ticket = gate.begin_operation(); const auto pending_open = gate.begin_operation();
    expect(gate.accepts(apply_ticket) && !gate.try_begin_final(false), "Final save crossed pending apply/open.");
    gate.end_operation(); expect(!gate.try_begin_final(false), "Finishing only apply ignored pending Open.");
    expect(gate.accepts(pending_open), "Pending Open lost its capability before completion."); gate.end_operation();
    FinalPersistenceLease final(gate); expect(final.try_acquire(false), "Final save remained blocked by finished apply.");
    merged.revision = RevisionPolicy::persistence_revision(merged.revision, true); expect(merged.revision == UINT64_MAX, "Final append overflowed or spent a second revision.");
    auto saved = RecoveryModel::save_response(sessions.save(RecoveryModel::save_request(saved_paths, merged, owner_saved.disk_digest, recovery.lock_timeout)).get());
    expect(saved.status == cpn::SessionSaveStatus::saved, "Final import did not persist."); final.release();
    auto restored = RecoveryModel::load_response(sessions.load(cpn::path_to_utf8(saved_paths.recovery_file)).get());
    expect(restored.session->revision == UINT64_MAX && restored.session->documents[0].source_text == "C" &&
        cpn::find_import_application_receipt(*restored.session, *loaded.inbox_id, id).has_value(), "Final recovery lost edits or receipt.");
    // Simulate crash after saved receipt but before Inbox mark; MAX replay only confirms it.
    expect(prepare(worker, paths, id, *restored.session, recovery).status == "AlreadyApplied", "MAX receipt replay tried to append.");
    expect(CompanionModel::write_response(worker.request(CompanionWorker::Operation::mark_applied, CompanionModel::mark_request(base.workspace_id, paths, id, saved_paths, config)).get()).status == "Applied", "MAX persisted receipt could not confirm Pending.");
    rejects([&] { (void)CompanionModel::merge(*restored.session, base, delta); }, "MAX workspace accepted another document.");
    worker.close(); sessions.close(); std::cout << "MAX-1 single final save, pending Open gate and MAX replay confirmation passed\n";
}
cpn::WorkspaceSession native_capture(cpn::WorkspaceSession original, RecoveryConfiguration const& recovery) {
    auto settings = runner();
    auto native = RecoveryModel::prepare(std::move(original), recovery, settings, {"cpp"}, "cpp");
    auto captured = std::move(native.session);
    expect(RecoveryModel::merge_preferences(captured.editor_preferences, RecoveryModel::workspace_preferences(native.toolchains ? *native.toolchains : settings.toolchains, recovery.workspace_namespace)), "Native workspace normalization failed.");
    for (std::size_t i = 0; i < captured.documents.size(); ++i) {
        if (!captured.documents[i].toolchain_profile_id) captured.documents[i].toolchain_profile_id = settings.default_toolchain(*captured.documents[i].language_id);
        expect(RecoveryModel::merge_preferences(captured.documents[i].editor_preferences, RecoveryModel::document_preferences(native.documents[i], recovery.document_namespace)), "Native document normalization failed.");
    }
    return captured;
}
void startup_binding_boundaries(CompanionConfiguration const& config, RecoveryConfiguration const& recovery, std::filesystem::path const& directory) {
    SessionWorker sessions(recovery.options); auto settings = runner();
    auto original = base_session(recovery, config, config.paths(directory / "startup.json"));
    original.editor_preferences = {RecoveryModel::workspace_preferences(settings.toolchains, recovery.workspace_namespace)};
    DocumentPreferences prefs; prefs.comparison = settings.default_comparison;
    original.documents[0].editor_preferences = {RecoveryModel::document_preferences(prefs, recovery.document_namespace)};
    for (auto revision : {std::uint64_t{7}, UINT64_MAX - 1, UINT64_MAX}) {
        const auto path = recovery.paths(directory / ("startup-" + std::to_string(revision) + ".json")); original.revision = revision;
        auto first = RecoveryModel::save_response(sessions.save(RecoveryModel::save_request(path, original, {}, recovery.lock_timeout)).get());
        expect(first.status == cpn::SessionSaveStatus::saved, "Startup boundary fixture did not persist.");
        const auto bytes = cpn::native_file_store()->read(path.recovery_file, cpn::SessionBounds{}.max_json_bytes);
        auto loaded = RecoveryModel::load_response(sessions.load(cpn::path_to_utf8(path.recovery_file)).get());
        const auto saved_content = RecoveryModel::logical_content(*loaded.session); // BEFORE native normalization.
        auto captured = native_capture(*loaded.session, recovery);
        const bool changed = RecoveryModel::logical_content(captured) != saved_content;
        expect(!changed && RevisionPolicy::persistence_revision(captured.revision, changed) == revision, "Unchanged Loaded startup consumed a revision.");
        // MainWindow's unchanged branch rechecks the real disk token and does not save.
        auto observed = RecoveryModel::load_response(sessions.load(cpn::path_to_utf8(path.recovery_file)).get());
        expect(observed.status == cpn::SessionLoadStatus::loaded && observed.disk_digest == loaded.disk_digest && observed.session->revision == revision &&
            cpn::native_file_store()->read(path.recovery_file, cpn::SessionBounds{}.max_json_bytes) == bytes, "Unchanged startup altered the real persisted file.");
    }
    // An actual normalization change still saves with a successor; autosave is not disabled.
    auto normalization_path = recovery.paths(directory / "startup-normalization.json"); original.revision = UINT64_MAX - 1; original.documents[0].editor_preferences.clear();
    auto initial = RecoveryModel::save_response(sessions.save(RecoveryModel::save_request(normalization_path, original, {}, recovery.lock_timeout)).get());
    auto loaded = RecoveryModel::load_response(sessions.load(cpn::path_to_utf8(normalization_path.recovery_file)).get());
    const auto baseline = RecoveryModel::logical_content(*loaded.session); auto normalized = native_capture(*loaded.session, recovery);
    expect(RecoveryModel::logical_content(normalized) != baseline, "Native normalization was hidden behind its own baseline.");
    normalized.revision = RevisionPolicy::persistence_revision(normalized.revision, true);
    auto normalized_save = RecoveryModel::save_response(sessions.save(RecoveryModel::save_request(normalization_path, normalized, initial.disk_digest, recovery.lock_timeout)).get());
    auto normalized_disk = RecoveryModel::load_response(sessions.load(cpn::path_to_utf8(normalization_path.recovery_file)).get());
    expect(normalized_save.status == cpn::SessionSaveStatus::saved && normalized_disk.session->revision == UINT64_MAX &&
        RecoveryModel::logical_content(*normalized_disk.session) == RecoveryModel::logical_content(normalized), "A real startup normalization did not consume exactly its legal successor.");
    original.documents[0].editor_preferences = {RecoveryModel::document_preferences(prefs, recovery.document_namespace)};
    for (auto revision : {UINT64_MAX - 1, UINT64_MAX}) {
        const auto paths = recovery.paths(directory / ("first-owner-" + std::to_string(revision) + ".json")); original.revision = revision;
        auto saved = RecoveryModel::save_response(sessions.save(RecoveryModel::save_request(paths, original, {}, recovery.lock_timeout)).get());
        const auto inbox_paths = config.paths(paths.recovery_file); const auto before = cpn::serialize_workspace_session(original);
        expect(!CompanionModel::can_create_storage_binding(revision), "Insufficient first-owner budget was admitted.");
        // The production guard runs before installing owner, accept or start.
        expect(!CompanionModel::binding(original.editor_preferences, config.storage_namespace) && cpn::serialize_workspace_session(original) == before &&
            !std::filesystem::exists(inbox_paths.inbox_file), "First-owner rejection left a partial owner or inbox.");
        CompanionWorker worker(config.worker); const auto inbox = page(worker, config, inbox_paths);
        const auto listener = CompanionModel::listener_response(worker.request(CompanionWorker::Operation::listener_info, CompanionModel::version_request()).get());
        auto disk = RecoveryModel::load_response(sessions.load(cpn::path_to_utf8(paths.recovery_file)).get());
        expect(inbox.status == cpn::CompanionInboxLoadStatus::missing && !listener.has_listener && disk.disk_digest == saved.disk_digest &&
            disk.session->revision == revision && cpn::serialize_workspace_session(*disk.session) == before, "Rejected first-owner operation changed storage, listener or revision.");
        worker.close();
    }
    // Positive boundary: a new owner can persist, then the problem uses the final revision.
    const auto paths = recovery.paths(directory / "first-owner-positive.json"); const auto inbox_paths = config.paths(paths.recovery_file);
    original.revision = UINT64_MAX - 2;
    auto first = RecoveryModel::save_response(sessions.save(RecoveryModel::save_request(paths, original, {}, recovery.lock_timeout)).get());
    expect(CompanionModel::can_create_storage_binding(original.revision), "Two remaining successors could not initialize storage.");
    auto bound = original; bound.editor_preferences.push_back(CompanionModel::binding_preference({bound.workspace_id, inbox_paths}, config.storage_namespace));
    bound.revision = RevisionPolicy::persistence_revision(bound.revision, true);
    auto owner_save = RecoveryModel::save_response(sessions.save(RecoveryModel::save_request(paths, bound, first.disk_digest, recovery.lock_timeout)).get());
    auto owner_disk = RecoveryModel::load_response(sessions.load(cpn::path_to_utf8(paths.recovery_file)).get());
    expect(owner_save.status == cpn::SessionSaveStatus::saved && owner_disk.session->revision == UINT64_MAX - 1 &&
        CompanionModel::binding(owner_disk.session->editor_preferences, config.storage_namespace).has_value(), "First owner did not consume only its own successor.");
    CompanionWorker worker(config.worker);
    expect(CompanionModel::write_response(worker.request(CompanionWorker::Operation::accept, CompanionModel::accept_request(bound.workspace_id, inbox_paths, payload("first owner positive"), config)).get()).status == "Accepted", "Persisted first owner could not receive a problem.");
    auto inbox = page(worker, config, inbox_paths); auto prepared = prepare(worker, inbox_paths, inbox.items[0].import_id, *owner_disk.session, recovery);
    auto delta = CompanionModel::delta(*owner_disk.session, prepared, *inbox.inbox_id, inbox.items[0].import_id, recovery, settings, {"cpp"}, "cpp");
    auto applied = CompanionModel::merge(*owner_disk.session, *owner_disk.session, delta); applied.revision = RevisionPolicy::persistence_revision(applied.revision, true);
    auto apply_save = RecoveryModel::save_response(sessions.save(RecoveryModel::save_request(paths, applied, owner_disk.disk_digest, recovery.lock_timeout)).get());
    expect(apply_save.status == cpn::SessionSaveStatus::saved && applied.revision == UINT64_MAX &&
        CompanionModel::write_response(worker.request(CompanionWorker::Operation::mark_applied, CompanionModel::mark_request(applied.workspace_id, inbox_paths, inbox.items[0].import_id, paths, config)).get()).status == "Applied", "First owner plus application did not complete within its two successors.");
    worker.close(); sessions.close();
    std::cout << "Loaded startup baselines, actual normalization and first-owner write-before-budget boundaries passed\n";
}
struct Network {
#if defined(_WIN32)
    Network() { WSADATA data{}; expect(WSAStartup(MAKEWORD(2, 2), &data) == 0, "WSAStartup failed."); }
    ~Network() { WSACleanup(); }
#endif
};
void cancellation(CompanionConfiguration config, std::filesystem::path const& directory) {
    config.worker.maximum_commands = 1; config.lock_timeout = std::chrono::seconds{60};
    const auto paths = config.paths(directory / "cancel-inbox.json");
    auto lock = cpn::native_file_store()->acquire_lock(paths.lock_file, std::chrono::seconds{1});
    CompanionWorker worker(config.worker);
    auto accepted = worker.request(CompanionWorker::Operation::accept, CompanionModel::accept_request("workspace 中文", paths, payload("cancel"), config));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (!worker.operation_active()) { expect(std::chrono::steady_clock::now() < deadline, "Actual import did not reach its storage wait."); std::this_thread::yield(); }
    auto queued = worker.request(CompanionWorker::Operation::listener_info, CompanionModel::version_request());
    rejects([&] { (void)worker.request(CompanionWorker::Operation::listener_info, CompanionModel::version_request()); }, "Owned command queue capacity was ignored.");
    std::thread first([&] { worker.close(); }); std::thread second([&] { worker.close(); }); first.join(); second.join();
    expect(CompanionModel::write_response(accepted.get()).status == "Cancelled", "Close failed to cancel actual storage wait.");
    rejects([&] { (void)queued.get(); }, "Close left queued command unresolved.");
    expect(!std::filesystem::exists(paths.inbox_file), "Cancelled accept replaced the inbox.");
    std::cout << "bounded command queue and actual lock-wait cancellation passed\n";
}
std::string post(std::uint16_t port, std::string const& body) {
#if defined(_WIN32)
    using Socket = SOCKET; const auto invalid = INVALID_SOCKET;
#else
    using Socket = int; const auto invalid = -1;
#endif
    Socket socket = ::socket(AF_INET, SOCK_STREAM, 0); expect(socket != invalid, "HTTP fixture socket failed.");
    struct Close { Socket value; ~Close() {
#if defined(_WIN32)
        closesocket(value);
#else
        ::close(value);
#endif
    } } close{socket};
#if defined(_WIN32)
    const DWORD timeout = 5000; setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char const*>(&timeout), sizeof(timeout)); setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char const*>(&timeout), sizeof(timeout));
#else
    const timeval timeout{5, 0}; setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)); setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    expect(::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "HTTP fixture could not connect.");
    auto request = "POST / HTTP/1.1\r\nHost: localhost:" + std::to_string(port) + "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    std::size_t offset{}; while (offset < request.size()) { const auto sent = ::send(socket, request.data() + offset, static_cast<int>(request.size() - offset), 0); expect(sent > 0, "HTTP fixture send failed."); offset += static_cast<std::size_t>(sent); }
    std::string response; char bytes[4096]; while (true) { const auto received = ::recv(socket, bytes, sizeof(bytes), 0); expect(received >= 0, "HTTP fixture receive failed."); if (!received) break; response.append(bytes, static_cast<std::size_t>(received)); expect(response.size() < 65536, "HTTP fixture response exceeded budget."); }
    return response;
}
void listener(CompanionConfiguration config, std::filesystem::path const& directory) {
    config.listener_options = {{"port", 0}, {"enable_ipv6", false}}; auto paths = config.paths(directory / "listener.json");
    CompanionWorker worker(config.worker); [[maybe_unused]] Network network;
    auto info = CompanionModel::listener_response(worker.request(CompanionWorker::Operation::listener_info, CompanionModel::version_request()).get());
    expect(!info.has_listener && !info.running && info.port == 0, "Worker started a listener implicitly.");
    auto start = CompanionModel::listener_response(worker.request(CompanionWorker::Operation::listener_start, CompanionModel::listener_start_request("workspace 中文", paths, config)).get());
    expect(start.status == "Started" && start.running && start.port, "Explicit worker listener start failed.");
    auto response = post(start.port, payload("HTTP")); expect(response.find("202 Accepted") != std::string::npos && response.find("\"status\":\"Accepted\"") != std::string::npos, "Real HTTP import did not return durable Accepted.");
    expect(page(worker, config, paths).items[0].candidate.name == "HTTP", "HTTP import did not reach persistent worker page.");
    CompanionWorker conflict(config.worker); auto fixed = config; fixed.listener_options["port"] = start.port;
    auto failed = CompanionModel::listener_response(conflict.request(CompanionWorker::Operation::listener_start, CompanionModel::listener_start_request("workspace 中文", config.paths(directory / "conflict.json"), fixed)).get());
    expect(failed.status == "BindFailed" && !failed.running, "Port conflict pretended to listen.");
    auto stopped = CompanionModel::listener_response(worker.request(CompanionWorker::Operation::listener_stop, CompanionModel::version_request()).get()); expect(stopped.status == "Stopped" && !stopped.running && stopped.closed, "Stop did not join the listener.");
    auto retried = CompanionModel::listener_response(conflict.request(CompanionWorker::Operation::listener_start, CompanionModel::listener_start_request("workspace 中文", config.paths(directory / "conflict.json"), config)).get());
    expect(retried.status == "Started" && retried.running, "BindFailed engine could not retry.");
    std::thread first([&] { conflict.close(); }); std::thread second([&] { conflict.close(); }); first.join(); second.join();
    auto restart = CompanionModel::listener_response(worker.request(CompanionWorker::Operation::listener_start, CompanionModel::listener_start_request("workspace 中文", paths, config)).get()); expect(restart.running, "Stopped listener could not restart.");
    worker.close(); std::cout << "explicit listener/info, real HTTP, port conflict/retry, stop/restart and concurrent close passed\n";
}
int run(std::filesystem::path const& config_path, std::filesystem::path const& recovery_path) {
    auto configuration_bytes = read(config_path); auto config = CompanionConfiguration::parse(configuration_bytes); auto recovery = RecoveryConfiguration::parse(read(recovery_path)); Workspace workspace;
    models(config, recovery, workspace.path, configuration_bytes); import_flow(config, recovery, workspace.path); final_revision(config, recovery, workspace.path); startup_binding_boundaries(config, recovery, workspace.path); listener(config, workspace.path); cancellation(config, workspace.path);
    std::cout << "Windows Companion adapter tests passed (UI/IME require Windows target execution).\n"; return 0;
}
}
#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) { try { if (argc != 3) throw std::invalid_argument("Expected Companion and recovery configurations."); return run(argv[1], argv[2]); } catch (std::exception const& error) { std::cerr << error.what() << '\n'; return 1; } }
#else
int main(int argc, char** argv) { try { if (argc != 3) throw std::invalid_argument("Expected Companion and recovery configurations."); return run(argv[1], argv[2]); } catch (std::exception const& error) { std::cerr << error.what() << '\n'; return 1; } }
#endif
