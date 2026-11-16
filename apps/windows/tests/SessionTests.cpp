#include "RecoveryModel.h"
#include "SourceStore.h"
#include "SaveBaseline.h"
#include "RevisionPolicy.h"
#include "PersistenceGate.h"
#include "TestTextChange.h"
#include "cpn/digest.hpp"
#include "cpn/process.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

using namespace cpn::windows;
namespace {
using Json = nlohmann::json;
void expect(bool condition, char const* message) { if (!condition) throw std::runtime_error(message); }
template <class Action> void rejects(Action&& action, char const* message) {
    bool rejected = false; try { action(); } catch (std::exception const&) { rejected = true; }
    expect(rejected, message);
}
std::string read(std::filesystem::path const& path) {
    std::ifstream file(path, std::ios::binary); if (!file) throw std::runtime_error("Cannot open test fixture.");
    return {std::istreambuf_iterator<char>{file}, {}};
}
cpn::WorkspaceSession sample() {
    cpn::SessionDocument doc;
    doc.document_id = "document/中文"; doc.revision = 9007199254740993ULL;
    doc.source_text = std::string("// 中文\r\n\0", 12); doc.language_id = "cpp";
    doc.tests = {{"empty", "", std::string(""), true}, {"raw", std::string("x\0\xFF", 3), {}, true}};
    return {"workspace/中文", 7, doc.document_id, {doc}, {{"future.foreign", 99, "{ \"n\": 18446744073709551616 }"}}};
}
RunnerConfiguration defaults() {
    RunnerConfiguration value; value.maximum_documents = 16; value.maximum_tests = 128;
    value.default_comparison = "trailing"; value.comparisons = {{"trailing", "ComparisonTrailing"}, {"byte", "ComparisonByte"}};
    ToolchainChoice choice; choice.id = "fixture"; choice.file = "fixture.json"; choice.label_resource = "ToolchainMSVC";
    choice.profile = {{"schema_version", 1}, {"id", choice.id}, {"language_id", "cpp"}, {"source_filename", "main.cpp"},
        {"artifact_name", ""}, {"run", {{"executable", "fixture"}, {"arguments", Json::array()}}}};
    value.toolchains = {choice}; value.default_toolchains = {{"cpp", choice.id}}; return value;
}
void locations(RecoveryConfiguration const& config, std::filesystem::path const& directory) {
    const auto file = directory / "恢复.json";
    auto encoded = RecoveryModel::serialize_location(file, config);
    expect(RecoveryModel::parse_location(encoded, config) == file, "Recovery location round trip changed its path.");
    auto value = Json::parse(encoded);
    value["recovery_file"] = "relative.json";
    rejects([&] { (void)RecoveryModel::parse_location(value.dump(), config); }, "Relative recovery location was accepted.");
    value["recovery_file"] = cpn::path_to_utf8(directory) + std::string("\0suffix", 7);
    rejects([&] { (void)RecoveryModel::parse_location(value.dump(), config); }, "NUL recovery path was accepted.");
    value["recovery_file"] = cpn::path_to_utf8(file); value["schema_version"] = 2;
    rejects([&] { (void)RecoveryModel::parse_location(value.dump(), config); }, "Future location schema was applied.");
    value["schema_version"] = 1; value["unexpected"] = true;
    rejects([&] { (void)RecoveryModel::parse_location(value.dump(), config); }, "Unknown location field was accepted.");
    const auto duplicate = std::string("{\"schema_version\":1,\"schema_version\":1,\"recovery_file\":") + Json(cpn::path_to_utf8(file)).dump() + "}";
    rejects([&] { (void)RecoveryModel::parse_location(duplicate, config); }, "Duplicate location field was accepted.");
    auto small = config; small.maximum_location_bytes = encoded.size() - 1;
    rejects([&] { (void)RecoveryModel::parse_location(encoded, small); }, "Location read byte budget was ignored.");
    rejects([&] { (void)RecoveryModel::serialize_location(file, small); }, "Location write byte budget was ignored.");
    rejects([&] { (void)config.paths(directory / "."); }, "Directory recovery destination was accepted.");
}
void save_baselines() {
    const std::string bytes{"a\r\n\0中", 7};
    SaveBaseline baseline; baseline.restore_digest(cpn::sha256(bytes));
    expect(!baseline.native_matches_disk() && !baseline.never_written(), "Restored savepoint was assigned a native fast path.");
    expect(!baseline.compare_committed(9007199254740993ULL, bytes), "Restored saved body stayed dirty.");
    expect(baseline.cached_dirty(9007199254740993ULL) == std::optional<bool>{false} && !baseline.cached_dirty(9007199254740994ULL),
        "Restored digest cache lost revision precision.");
    expect(baseline.compare_committed(9007199254740994ULL, "a\r\n"), "Restored digest comparison truncated NUL/Unicode.");
    baseline.mark_unwritten();
    expect(baseline.never_written() && !baseline.native_matches_disk() && !baseline.cached_dirty(9007199254740994ULL) &&
        baseline.compare_committed(1, ""), "Recovered unsaved empty body was marked clean.");
    baseline.record_written(bytes);
    expect(!baseline.never_written() && !baseline.compare_committed(2, bytes) && baseline.compare_committed(3, "later"),
        "Late save did not replace an unwritten/restored baseline.");
    baseline.restore_digest(cpn::sha256("saved"));
    expect(baseline.compare_committed(4, bytes) && !baseline.compare_committed(5, "saved"), "Restored digest reused earlier written bytes.");
}
void response_models() {
    Json loaded{{"schema_version", 1}, {"status", "Loaded"}, {"session", Json::parse(cpn::serialize_workspace_session(sample()))},
        {"disk_digest", cpn::sha256("disk")}, {"detail", ""}};
    auto response = RecoveryModel::load_response(loaded.dump());
    expect(response.session && response.disk_digest == cpn::sha256("disk"), "Valid Session load response was rejected.");
    loaded["status"] = "Missing";
    rejects([&] { (void)RecoveryModel::load_response(loaded.dump()); }, "Missing response included a candidate/digest.");
    loaded["status"] = "Loaded"; loaded["disk_digest"] = "invalid";
    rejects([&] { (void)RecoveryModel::load_response(loaded.dump()); }, "Malformed recovery digest was accepted.");
    loaded["disk_digest"] = cpn::sha256("disk"); loaded["session"] = nullptr;
    rejects([&] { (void)RecoveryModel::load_response(loaded.dump()); }, "Loaded response omitted its candidate.");
    Json saved{{"schema_version", 1}, {"status", "Saved"}, {"disk_digest", cpn::sha256("disk")},
        {"recovery_replaced", true}, {"backup_replaced", false}, {"detail", ""}};
    expect(RecoveryModel::save_response(saved.dump()).recovery_replaced, "Valid Session save response was rejected.");
    saved["status"] = "Conflict";
    rejects([&] { (void)RecoveryModel::save_response(saved.dump()); }, "Conflict response claimed replacement.");
    saved["status"] = "DurabilityUnknown"; saved["recovery_replaced"] = false;
    rejects([&] { (void)RecoveryModel::save_response(saved.dump()); }, "Unknown durability response had no committed file.");
    saved["backup_replaced"] = true;
    expect(!RecoveryModel::save_response(saved.dump()).recovery_replaced, "Backup-only durability result was rejected.");
    saved["recovery_replaced"] = true; saved["disk_digest"] = nullptr;
    rejects([&] { (void)RecoveryModel::save_response(saved.dump()); }, "Committed recovery omitted its new token.");
}
void model(RecoveryConfiguration const& config) {
    auto settings = defaults(); auto candidate = sample();
    candidate.documents[0].source_text = "a中\r\n";
    candidate.documents[0].toolchain_profile_id = "fixture";
    (void)RecoveryModel::merge_preferences(candidate.editor_preferences, RecoveryModel::workspace_preferences(settings.toolchains, config.workspace_namespace));
    DocumentPreferences prefs; prefs.caret_byte = 4; prefs.anchor_byte = 1; prefs.utf8_bom = true; prefs.comparison = "byte";
    prefs.test_revision = 9007199254740993ULL;
    (void)RecoveryModel::merge_preferences(candidate.documents[0].editor_preferences, RecoveryModel::document_preferences(prefs, config.document_namespace));
    auto restored = RecoveryModel::prepare(candidate, config, settings, {"cpp"}, "cpp");
    expect(restored.documents[0].caret_byte == 4 && restored.documents[0].anchor_byte == 1 && restored.documents[0].utf8_bom &&
        restored.documents[0].test_revision == 9007199254740993ULL && restored.toolchains->at(0).profile == settings.toolchains[0].profile,
        "Native document/configuration identity was lost.");
    auto invalid = candidate;
    auto point = prefs; point.caret_byte = 2;
    (void)RecoveryModel::merge_preferences(invalid.documents[0].editor_preferences, RecoveryModel::document_preferences(point, config.document_namespace));
    bool rejected = false; try { (void)RecoveryModel::prepare(invalid, config, settings, {"cpp"}, "cpp"); } catch (...) { rejected = true; }
    expect(rejected && candidate.documents[0].source_text == "a中\r\n", "Invalid candidate mutated the original model.");
    const auto raw = "{ \"future\": 18446744073709551616 }";
    candidate.documents[0].editor_preferences.push_back({config.document_namespace, 99, raw});
    candidate.editor_preferences.push_back({config.workspace_namespace, 99, raw});
    restored = RecoveryModel::prepare(candidate, config, settings, {"cpp"}, "cpp");
    expect(restored.workspace_opaque && !restored.toolchains && restored.documents[0].opaque, "Future own preferences were downgraded.");
    auto before = cpn::serialize_workspace_session(restored.session);
    (void)RecoveryModel::merge_preferences(restored.session.editor_preferences, RecoveryModel::workspace_preferences(settings.toolchains, config.workspace_namespace));
    expect(cpn::serialize_workspace_session(restored.session) == before, "Mixed current/future own preference bytes were discarded.");
    std::vector<cpn::NativeEditorPreferences> full;
    for (std::size_t index = 0; index < cpn::SessionBounds{}.max_preferences_per_owner; ++index)
        full.push_back({"foreign/" + std::to_string(index), 1, "{}"});
    const auto previous = full;
    expect(!RecoveryModel::merge_preferences(full, RecoveryModel::document_preferences(prefs, config.document_namespace)) && full.size() == previous.size(),
        "Full preference owner discarded foreign entries.");
    for (std::size_t index = 0; index < full.size(); ++index)
        expect(full[index].namespace_id == previous[index].namespace_id && full[index].object_json == previous[index].object_json, "Full owner preference bytes changed.");
    candidate = sample(); candidate.documents[0].language_id = "missing";
    rejects([&] { (void)RecoveryModel::prepare(candidate, config, settings, {"cpp"}, "cpp"); }, "Absent editor language was accepted.");
    candidate = sample(); candidate.documents[0].original_path = std::filesystem::path{"relative.cpp"};
    rejects([&] { (void)RecoveryModel::prepare(candidate, config, settings, {"cpp"}, "cpp"); }, "Relative restored source path was accepted.");
    candidate = sample(); candidate.documents[0].revision = UINT64_MAX;
    expect(RecoveryModel::prepare(candidate, config, settings, {"cpp"}, "cpp").session.documents[0].revision == UINT64_MAX,
        "Valid exhausted document revision could not be restored read only.");
    candidate = sample(); candidate.revision = UINT64_MAX;
    expect(RecoveryModel::prepare(candidate, config, settings, {"cpp"}, "cpp").session.revision == UINT64_MAX,
        "Valid exhausted workspace revision could not be restored read only.");
    candidate = sample(); auto removed = settings.toolchains; removed[0].id = "replacement"; removed[0].profile["id"] = "replacement";
    (void)RecoveryModel::merge_preferences(candidate.editor_preferences, RecoveryModel::workspace_preferences(removed, config.workspace_namespace));
    rejects([&] { (void)RecoveryModel::prepare(candidate, config, settings, {"cpp"}, "cpp"); }, "Restored catalog omitted a required default toolchain.");
}
void sessions(std::filesystem::path const& directory, RecoveryConfiguration const& config) {
    SessionWorker worker(config.options); const auto paths = config.paths(directory / "workspace.json");
    auto missing = Json::parse(worker.load(cpn::path_to_utf8(paths.recovery_file)).get()); expect(missing.at("status") == "Missing", "Missing recovery was not reported.");
    auto value = sample();
    auto saved = Json::parse(worker.save(RecoveryModel::save_request(paths, value, {}, config.lock_timeout)).get());
    expect(saved.at("status") == "Saved" && saved.at("recovery_replaced") == true, "Session did not commit.");
    auto loaded = Json::parse(worker.load(cpn::path_to_utf8(paths.recovery_file)).get());
    expect(loaded.at("status") == "Loaded", "Committed recovery was not loaded.");
    auto restored = cpn::parse_workspace_session(loaded.at("session").dump());
    expect(restored.documents[0].document_id == value.documents[0].document_id && restored.documents[0].revision == 9007199254740993ULL &&
        restored.documents[0].tests[1].input == std::string("x\0\xFF", 3) && !restored.documents[0].tests[1].expected &&
        restored.documents[0].tests[0].expected == std::optional<std::string>{""} && restored.editor_preferences[0].object_json == value.editor_preferences[0].object_json,
        "Recovery changed raw bytes, null/empty answers, identity or opaque preferences.");
    auto original = read(paths.recovery_file);
    auto conflict = Json::parse(worker.save(RecoveryModel::save_request(paths, value, {}, config.lock_timeout)).get());
    expect(conflict.at("status") == "Conflict" && read(paths.recovery_file) == original, "Conflict replaced recovery.");
    ++value.revision; value.documents[0].source_text += "new";
    saved = Json::parse(worker.save(RecoveryModel::save_request(paths, value, saved.at("disk_digest").get<std::string>(), config.lock_timeout)).get());
    expect(saved.at("status") == "Saved" && read(paths.backup_file) == original, "Previous recovery was not preserved.");
    cpn::native_file_store()->write_atomic(paths.recovery_file, "{broken");
    auto broken = Json::parse(worker.load(cpn::path_to_utf8(paths.recovery_file)).get()); expect(broken.at("status") == "Invalid", "Corrupt session was accepted.");
    auto refused = Json::parse(worker.save(RecoveryModel::save_request(paths, value, {}, config.lock_timeout)).get());
    expect(refused.at("status") == "InvalidExisting" && read(paths.recovery_file) == "{broken", "Corrupt original was overwritten.");
    auto first = std::async(std::launch::async, [&] { worker.close(); });
    auto second = std::async(std::launch::async, [&] { worker.close(); }); first.get(); second.get();
    bool rejected = false; try { (void)worker.load("after-close"); } catch (...) { rejected = true; }
    expect(rejected, "Closed session worker accepted a command.");
}
void revision_boundaries(std::filesystem::path const& directory, RecoveryConfiguration const& config) {
    const auto top = std::numeric_limits<std::uint64_t>::max();
    expect(!RevisionPolicy::exhausted(top - 1) && RevisionPolicy::exhausted(RevisionPolicy::next(top - 1)),
        "The last legal mutation did not exhaust its revision budget.");
    expect(RevisionPolicy::document(10, 10, top) == top && RevisionPolicy::document(11, 10, top - 1) == top,
        "Exhausted restored/native committed revision was truncated or rejected.");
    rejects([&] { (void)RevisionPolicy::document(12, 10, top - 1); }, "Native source edits saturated past the document budget.");
    rejects([&] { (void)RevisionPolicy::document(9, 10, 1); }, "A regressed native revision was accepted.");
    expect(RevisionPolicy::tests_mutable(top - 1, top - 1) && !RevisionPolicy::tests_mutable(top, 1) && !RevisionPolicy::tests_mutable(1, top),
        "Readonly test/workspace limits were applied after another mutation.");
    expect(RevisionPolicy::persistence_revision(top, false) == top && RevisionPolicy::persistence_revision(top - 1, true) == top,
        "Unchanged exhausted workspace consumed a revision or the final save was rejected.");
    rejects([&] { (void)RevisionPolicy::persistence_revision(top, true); }, "Changed exhausted workspace saturated and reused its revision.");
    SessionWorker worker(config.options); auto settings = defaults();
    for (const auto kind : {"workspace", "document", "tests", "all"}) {
        auto value = sample(); value.revision = 1; value.documents[0].revision = 1;
        value.documents[0].toolchain_profile_id = "fixture";
        value.documents[0].original_path = directory / "saved.cpp";
        DocumentPreferences prefs; prefs.comparison = "trailing"; prefs.test_revision = 1;
        const bool workspace = std::string_view{kind} == "workspace" || std::string_view{kind} == "all";
        const bool document = std::string_view{kind} == "document" || std::string_view{kind} == "all";
        const bool tests = std::string_view{kind} == "tests" || std::string_view{kind} == "all";
        if (workspace) value.revision = top - 1;
        if (document) value.documents[0].revision = top - 1;
        if (tests) prefs.test_revision = top - 1;
        const auto source_digest = cpn::sha256(value.documents[0].source_text);
        value.documents[0].save_point = cpn::DocumentSavePoint{value.documents[0].revision, source_digest, cpn::sha256("disk")};
        (void)RecoveryModel::merge_preferences(value.documents[0].editor_preferences, RecoveryModel::document_preferences(prefs, config.document_namespace));
        (void)RecoveryModel::prepare(value, config, settings, {"cpp"}, "cpp");
        if (workspace) value.revision = RevisionPolicy::next(value.revision);
        if (document) value.documents[0].revision = RevisionPolicy::document(11, 10, value.documents[0].revision);
        if (tests) prefs.test_revision = RevisionPolicy::next(prefs.test_revision);
        (void)RecoveryModel::merge_preferences(value.documents[0].editor_preferences, RecoveryModel::document_preferences(prefs, config.document_namespace));
        const auto paths = config.paths(directory / (std::string{kind} + "-max.json"));
        auto saved = RecoveryModel::save_response(worker.save(RecoveryModel::save_request(paths, value, {}, config.lock_timeout)).get());
        expect(saved.status == cpn::SessionSaveStatus::saved, "Core refused the final legal revision.");
        auto loaded = RecoveryModel::load_response(worker.load(cpn::path_to_utf8(paths.recovery_file)).get());
        auto restored = RecoveryModel::prepare(*loaded.session, config, settings, {"cpp"}, "cpp");
        expect(restored.session.revision == value.revision && restored.session.documents[0].revision == value.documents[0].revision &&
            restored.documents[0].test_revision == prefs.test_revision && restored.session.documents[0].source_text == value.documents[0].source_text,
            "Real C ABI MAX round trip could not restore the application's own final candidate.");
        SaveBaseline baseline; baseline.restore_digest(restored.session.documents[0].save_point->source_digest);
        expect(!baseline.compare_committed(restored.session.documents[0].revision, restored.session.documents[0].source_text),
            "Exhausted source savepoint lost its clean/dirty baseline.");
        const auto frozen = *loaded.session; const auto original = read(paths.recovery_file);
        // A durability retry rewrites the exact frozen snapshot without inventing a successor.
        auto retry = RecoveryModel::save_response(worker.save(RecoveryModel::save_request(paths, frozen, saved.disk_digest, config.lock_timeout)).get());
        expect(retry.status == cpn::SessionSaveStatus::saved && read(paths.recovery_file) == original,
            "Exact exhausted retry changed the frozen snapshot or consumed a revision.");
        const auto copy_paths = config.paths(directory / (std::string{kind} + "-copy.json"));
        auto copy = RecoveryModel::save_response(worker.save(RecoveryModel::save_request(copy_paths, frozen, {}, config.lock_timeout)).get());
        expect(copy.status == cpn::SessionSaveStatus::saved && read(copy_paths.recovery_file) == original,
            "Exporting an unchanged exhausted session modified its identity or revision.");
        auto verified = RecoveryModel::load_response(worker.load(cpn::path_to_utf8(paths.recovery_file)).get());
        expect(verified.status == cpn::SessionLoadStatus::loaded && verified.disk_digest == retry.disk_digest,
            "Unchanged exhausted close could not verify the actual persisted disk token.");
    }
    worker.close();
}
void final_transaction_gate(std::filesystem::path const& directory, RecoveryConfiguration const& config) {
    const auto top = std::numeric_limits<std::uint64_t>::max();
    PersistenceGate gate;
    expect(!gate.try_begin_final(true) && !gate.final_active(), "Unrelated busy command was mistaken for the close flush.");
    auto ticket = gate.begin_operation();
    SourceStore sources({directory / "final-source-locks", 1024, std::chrono::seconds(1)});
    const auto source_path = directory / "final-source.cpp";
    auto initial = sources.save(source_path, "A", false, {});
    std::promise<void> started, finish; auto finish_signal = finish.get_future();
    auto pending = std::async(std::launch::async, [&] {
        started.set_value(); finish_signal.get(); return sources.save(source_path, "B", false, initial.disk_digest);
    });
    struct Release { std::promise<void>& signal; bool done{}; ~Release() { if (!done) signal.set_value(); } } release{finish};
    started.get_future().get();
    expect(gate.operation_active() && !gate.try_begin_final(false) && gate.accepts(ticket),
        "The last candidate crossed a pending source-save completion.");
    // Ordinary source saving remains editable. C and test C are captured only after B's savepoint completes.
    auto candidate = sample(); candidate.revision = top - 1;
    candidate.documents[0].revision = 3; candidate.documents[0].source_text = "C";
    candidate.documents[0].tests = {{"late-test", "test C", std::string{"expected C"}, true}};
    candidate.documents[0].original_path = source_path; candidate.documents[0].toolchain_profile_id = "fixture";
    DocumentPreferences prefs; prefs.comparison = "trailing";
    (void)RecoveryModel::merge_preferences(candidate.documents[0].editor_preferences, RecoveryModel::document_preferences(prefs, config.document_namespace));
    finish.set_value(); release.done = true; const auto saved_source = pending.get();
    expect(saved_source.replaced && gate.accepts(ticket), "A source-save result lost its admission generation.");
    candidate.documents[0].save_point = cpn::DocumentSavePoint{2, cpn::sha256("B"), saved_source.disk_digest};
    gate.end_operation();
    expect(gate.try_begin_final(false) && gate.final_active() && !gate.accepts(ticket),
        "Final capability freeze did not invalidate older completion tickets.");
    rejects([&] { (void)gate.begin_operation(); }, "A late open/save was admitted while the final snapshot was frozen.");
    expect(!(!RevisionPolicy::exhausted(candidate.revision) && !gate.final_active()) &&
        !(RevisionPolicy::tests_mutable(candidate.revision, prefs.test_revision) && !gate.final_active()),
        "Final UI data/test capability remained mutable during the owned snapshot.");
    auto frozen = candidate; frozen.revision = RevisionPolicy::persistence_revision(candidate.revision, true);
    SessionWorker worker(config.options); const auto paths = config.paths(directory / "final-transaction.json");
    auto saved = RecoveryModel::save_response(worker.save(RecoveryModel::save_request(paths, frozen, {}, config.lock_timeout)).get());
    expect(saved.status == cpn::SessionSaveStatus::saved, "The excluded final transaction could not commit.");
    gate.end_final(); candidate.revision = frozen.revision;
    auto loaded = RecoveryModel::load_response(worker.load(cpn::path_to_utf8(paths.recovery_file)).get());
    auto restored = RecoveryModel::prepare(*loaded.session, config, defaults(), {"cpp"}, "cpp");
    expect(restored.session.revision == top && restored.session.documents[0].source_text == "C" &&
        restored.session.documents[0].tests[0].input == "test C" && restored.session.documents[0].tests[0].expected == std::optional<std::string>{"expected C"} &&
        restored.session.documents[0].save_point->source_digest == cpn::sha256("B") &&
        restored.session.documents[0].save_point->disk_digest == saved_source.disk_digest,
        "Late source/text/test/savepoint state was trapped outside the final persisted candidate.");
    expect(RevisionPolicy::exhausted(candidate.revision), "Releasing the temporary gate made a committed MAX workspace editable.");
    // A pending Open is also excluded; its fully read candidate is admitted before the final freeze.
    PersistenceGate opening; auto open_ticket = opening.begin_operation();
    expect(!opening.try_begin_final(false), "Final commit crossed an outstanding Open.");
    const auto opened = sources.read(source_path);
    expect(opening.accepts(open_ticket) && opened.utf8 == "B", "Open candidate lost its original generation or bytes.");
    opening.end_operation(); expect(opening.try_begin_final(false), "Completed Open permanently blocked the final transaction.");
    opening.end_final();
    // Before-replace failures release capabilities without advancing the committed workspace revision.
    PersistenceGate failed; expect(failed.try_begin_final(false), "Failure fixture could not acquire final capability.");
    const auto broken = config.paths(directory / "final-failed.json"); cpn::native_file_store()->write_atomic(broken.recovery_file, "{broken");
    auto refused = RecoveryModel::save_response(worker.save(RecoveryModel::save_request(broken, frozen, {}, config.lock_timeout)).get());
    failed.end_final();
    expect(!refused.recovery_replaced && !failed.final_active() && read(broken.recovery_file) == "{broken",
        "Before-replace failure retained the temporary freeze or replaced damaged recovery.");
    auto new_ticket = failed.begin_operation(); expect(failed.accepts(new_ticket), "Failed final commit did not restore persistent capabilities.");
    failed.end_operation();
    // A MAX recovery copy that reached disk consumes the budget even if its locator cannot commit.
    const auto old_paths = config.paths(directory / "copy-old-active.json");
    auto old = frozen; old.revision = top - 1; old.documents[0].source_text = "old active";
    auto old_saved = RecoveryModel::save_response(worker.save(RecoveryModel::save_request(old_paths, old, {}, config.lock_timeout)).get());
    expect(old_saved.status == cpn::SessionSaveStatus::saved, "Copy fixture could not save its old active recovery.");
    const auto old_bytes = read(old_paths.recovery_file);
    PersistenceGate copy_gate; expect(copy_gate.try_begin_final(false), "Copy fixture could not freeze its final candidate.");
    const auto new_paths = config.paths(directory / "copy-max-before-locator.json");
    auto copied = RecoveryModel::save_response(worker.save(RecoveryModel::save_request(new_paths, frozen, {}, config.lock_timeout)).get());
    expect(copied.recovery_replaced && copied.status == cpn::SessionSaveStatus::saved, "MAX copy did not reach disk.");
    const auto invalid_locator = directory / "locator-is-directory"; std::filesystem::create_directory(invalid_locator);
    rejects([&] { cpn::native_file_store()->write_atomic(invalid_locator, RecoveryModel::serialize_location(new_paths.recovery_file, config)); },
        "Locator failure fixture unexpectedly committed.");
    const auto copied_revision = frozen.revision; copy_gate.end_final();
    expect(RevisionPolicy::exhausted(copied_revision) && read(old_paths.recovery_file) == old_bytes &&
        cpn::parse_workspace_session(read(new_paths.recovery_file)).documents[0].source_text == "C",
        "Locator failure reopened the consumed budget or lost either recovery original.");
    worker.close();
}
void synchronous_test_text(std::filesystem::path const& directory, RecoveryConfiguration const& config) {
    const auto top = std::numeric_limits<std::uint64_t>::max();
    PersistenceGate gate;
    std::string input{"input B"}, expected{"expected B"}; std::uint64_t revision = 7;
    bool refresh_pending = false;
    const auto synchronous = [&](std::string& bytes, std::string text) {
        const auto result = TestTextChange::record(bytes, revision, std::move(text), !gate.final_active());
        expect(result == TestTextChange::Result::changed, "Synchronous sample text did not enter the authoritative model.");
        refresh_pending = true;
    };
    // Text is already C while the dispatcher/TextChanged refresh is deliberately still pending.
    synchronous(input, "输入 C\r\n"); synchronous(expected, "期望 C\r\n");
    expect(refresh_pending && revision == 9, "Deferred UI refresh consumed or lost the synchronous revision.");
    FinalPersistenceLease lease(gate); expect(lease.try_acquire(false), "Text fixture could not freeze its final snapshot.");
    expect(refresh_pending && input == "输入 C\r\n" && expected == "期望 C\r\n", "Final freeze captured stale B text.");
    const auto recorded_revision = revision;
    expect(TestTextChange::record(input, revision, "D", !gate.final_active()) == TestTextChange::Result::rejected &&
        input == "输入 C\r\n" && revision == recorded_revision, "A later edit crossed the frozen sample capability.");
    auto session = sample(); session.revision = top;
    session.documents[0].source_text = "source C"; session.documents[0].toolchain_profile_id = "fixture";
    session.documents[0].tests = {{"synchronous", input, expected, true}};
    DocumentPreferences prefs; prefs.comparison = "trailing"; prefs.test_revision = revision;
    (void)RecoveryModel::merge_preferences(session.documents[0].editor_preferences, RecoveryModel::document_preferences(prefs, config.document_namespace));
    SessionWorker worker(config.options); const auto paths = config.paths(directory / "synchronous-test-text.json");
    const auto saved = RecoveryModel::save_response(worker.save(RecoveryModel::save_request(paths, session, {}, config.lock_timeout)).get());
    expect(saved.recovery_replaced, "Synchronous final sample candidate did not commit through the real C ABI.");
    const auto loaded = RecoveryModel::load_response(worker.load(cpn::path_to_utf8(paths.recovery_file)).get());
    const auto restored = RecoveryModel::prepare(*loaded.session, config, defaults(), {"cpp"}, "cpp");
    expect(restored.session.revision == top && restored.session.documents[0].tests[0].input == input &&
        restored.session.documents[0].tests[0].expected == std::optional<std::string>{expected} && restored.documents[0].test_revision == revision,
        "Deferred sample refresh lost C bytes/revision in the persisted final session.");
    lease.release(); refresh_pending = false;
    expect(TestTextChange::record(input, revision, input, false) == TestTextChange::Result::unchanged && revision == 9,
        "An unchanged late notification advanced the recorded revision.");
    std::uint64_t last = top - 1; std::string last_bytes{"old"};
    expect(TestTextChange::record(last_bytes, last, "last", true) == TestTextChange::Result::changed && last == top,
        "The last legal sample edit was lost.");
    expect(TestTextChange::record(last_bytes, last, "overflow", true) == TestTextChange::Result::rejected &&
        last == top && last_bytes == "last", "Exhausted sample text threw or overwrote its last legal state.");
    std::string binary{"x\0\xFF", 3};
    expect(TestTextChange::record(binary, revision, "hex display", false) == TestTextChange::Result::rejected &&
        binary == std::string("x\0\xFF", 3), "Read-only binary bytes were reconstructed from the display.");
    worker.close();
    PersistenceGate ui_failure;
    rejects([&] {
        FinalPersistenceLease failing(ui_failure);
        expect(failing.try_acquire(false) && failing.acquired(), "Final lease did not record gate ownership synchronously.");
        throw std::runtime_error("Injected UpdateControls failure after acquire");
    }, "UI failure fixture did not throw.");
    expect(!ui_failure.final_active(), "Fallible UI work stranded the final capability without RAII ownership.");
    const auto ticket = ui_failure.begin_operation(); expect(ui_failure.accepts(ticket), "UI exception did not restore Open/Save capabilities.");
    ui_failure.end_operation();
    FinalPersistenceLease first(ui_failure), second(ui_failure);
    expect(first.try_acquire(false) && !second.try_acquire(false), "Two leases owned the same final gate.");
    second.release(); expect(ui_failure.final_active(), "A rejected lease released another lease's gate.");
    first.release(); first.release(); expect(!ui_failure.final_active(), "Explicit final lease release was not idempotent.");
}
void worker_boundaries(std::filesystem::path const& directory, RecoveryConfiguration const& config) {
    auto options = config.options; options.engine.max_request_bytes = 128;
    SessionWorker worker(options);
    rejects([&] { (void)worker.save(std::string(129, 'x')); }, "Worker accepted an oversized owned payload.");
    rejects([&] { (void)worker.load(std::string("path\0suffix", 11)); }, "Worker accepted a NUL path.");
    rejects([&] { (void)worker.load(std::string(32769, 'x')); }, "Worker accepted an oversized ABI path.");
    const auto missing = cpn::path_to_utf8(directory / "worker-missing.json");
    for (int index = 0; index < 32; ++index)
        expect(RecoveryModel::load_response(worker.load(missing).get()).status == cpn::SessionLoadStatus::missing, "Session buffers accumulated across released commands.");
    worker.close();
    options = config.options; options.engine.abi_version = CPN_ABI_VERSION + 1;
    SessionWorker invalid(options);
    rejects([&] { (void)invalid.load(missing).get(); }, "Worker startup failure did not settle its promise.");
    invalid.close();
    for (int index = 0; index < 8; ++index) { SessionWorker closing(config.options); closing.close(); }
}
void cancellation(std::filesystem::path const& directory, RecoveryConfiguration const& config, std::filesystem::path const& executable) {
    auto paths = config.paths(directory / "cancel.json"); const auto marker = directory / "lock-ready";
    cpn::ProcessSpec process; process.executable = executable; process.working_directory = directory;
    process.arguments = {"--hold-lock", cpn::path_to_utf8(paths.lock_file), cpn::path_to_utf8(marker)};
    process.limits.wall_timeout = std::chrono::seconds(30);
    std::stop_source child_stop;
    auto child = std::async(std::launch::async, [&] { return cpn::run_process(process, child_stop.get_token()); });
    struct StopChild { std::stop_source& stop; std::future<cpn::ProcessResult>& process; ~StopChild() { stop.request_stop(); if (process.valid()) process.wait(); } } stop_child{child_stop, child};
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!std::filesystem::exists(marker)) {
        expect(std::chrono::steady_clock::now() < deadline, "Lock fixture did not start.");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto options = config.options; options.maximum_commands = 1;
    SessionWorker worker(options);
    (void)worker.load(cpn::path_to_utf8(paths.recovery_file)).get();
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (worker.operation_active()) {
        expect(std::chrono::steady_clock::now() < deadline, "Session worker did not finish the initial load.");
        std::this_thread::yield();
    }
    auto blocked = worker.save(RecoveryModel::save_request(paths, sample(), {}, std::chrono::milliseconds(60000)));
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!worker.operation_active()) {
        expect(std::chrono::steady_clock::now() < deadline, "Session worker did not start the save operation.");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expect(blocked.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout, "Session lock fixture did not block save.");
    auto queued = worker.load(cpn::path_to_utf8(paths.recovery_file));
    bool full = false; try { (void)worker.load("overflow"); } catch (...) { full = true; }
    expect(full, "Bounded session queue accepted overflow.");
    const auto started = std::chrono::steady_clock::now(); worker.close();
    expect(std::chrono::steady_clock::now() - started < std::chrono::seconds(2), "Session close waited for the lock timeout instead of cancelling.");
    auto result = Json::parse(blocked.get());
    expect(result.at("status") == "Cancelled" && result.at("recovery_replaced") == false && !std::filesystem::exists(paths.recovery_file), "Cancelled recovery unexpectedly replaced data.");
    bool refused = false; try { (void)queued.get(); } catch (...) { refused = true; }
    expect(refused, "Closed session worker did not settle the queued promise.");
    child_stop.request_stop(); (void)child.get();
}
void source_files(std::filesystem::path const& directory) {
    SourceStore store({directory / "locks", 1024, std::chrono::milliseconds(1000)});
    const auto path = directory / "中文.cpp";
    const std::string text{"a\r\n\0z", 5};
    auto created = store.save(path, text, true, {}); expect(created.replaced, "New source was not saved.");
    auto loaded = store.read(path); expect(loaded.utf8 == text && loaded.utf8_bom && loaded.disk_digest == *created.disk_digest, "Source BOM/CRLF/NUL changed.");
    cpn::native_file_store()->write_atomic(path, "external");
    auto conflict = store.save(path, "new", false, loaded.disk_digest);
    expect(conflict.status == SourceSaveResult::Status::conflict && !conflict.replaced && read(path) == "external", "External source was overwritten.");
    expect(store.save(path, "new", false, {}).status == SourceSaveResult::Status::conflict, "Save As silently replaced an existing file.");
    expect(store.disk_digest(path) == cpn::sha256("external") && !store.disk_digest(directory / "absent.cpp"), "Raw disk digest lost missing/existing identity.");
    rejects([&] { (void)store.save(path, std::string("\xFF", 1), false, cpn::sha256("external")); }, "Invalid UTF-8 source was saved.");
    rejects([&] { (void)store.save(path, std::string(1025, 'x'), false, cpn::sha256("external")); }, "Oversized source was saved.");
    expect(read(path) == "external", "Rejected source candidate changed the original.");
    std::stop_source stop; stop.request_stop();
    auto cancelled = store.save(directory / "cancelled.cpp", "body", false, {}, stop.get_token());
    expect(!cancelled.replaced && !std::filesystem::exists(directory / "cancelled.cpp"), "Cancelled source save changed its destination.");
    const auto link = directory / "source-link.cpp"; std::error_code link_error;
    std::filesystem::create_symlink(path, link, link_error);
    if (!link_error) {
        auto alias = store.read(link);
        expect(alias.path == std::filesystem::canonical(path) && alias.disk_digest == cpn::sha256("external"), "Source alias did not resolve to its target.");
        auto saved = store.save(link, text, true, alias.disk_digest);
        expect(saved.replaced && std::filesystem::is_symlink(link) && store.read(path).utf8 == text, "Saving through a source alias replaced the symlink.");
    }
}
class InterceptedFiles final : public cpn::FileStore {
public:
    std::shared_ptr<cpn::FileStore> native = cpn::native_file_store();
    std::optional<std::filesystem::path> create_after_prepare;
    bool fail_after_prepare{};
    mutable std::vector<std::filesystem::path> acquired_locks;
    std::string read(std::filesystem::path const& p, std::size_t n) const override { return native->read(p, n); }
    std::optional<std::string> read_optional(std::filesystem::path const& p, std::size_t n) const override { return native->read_optional(p, n); }
    std::unique_ptr<cpn::FileLock> acquire_lock(std::filesystem::path const& p, std::chrono::milliseconds n, std::stop_token s) const override {
        acquired_locks.push_back(p); return native->acquire_lock(p, n, s);
    }
    void write_atomic(std::filesystem::path const& p, std::string_view b) const override {
        native->write_atomic(p, b);
        if (create_after_prepare) native->write_atomic(*create_after_prepare, "racer");
        if (fail_after_prepare) throw cpn::AtomicWriteError("Injected durability failure", true);
    }
    std::filesystem::path create_workspace() const override { return native->create_workspace(); }
    void remove_workspace(std::filesystem::path const& p) const noexcept override { native->remove_workspace(p); }
};
void source_lock_identity(std::filesystem::path const& directory) {
    auto intercepted = std::make_shared<InterceptedFiles>();
    SourceStore store({directory / "locks", 1024, std::chrono::milliseconds(1000)}, intercepted);
    for (auto name : {"Älias.cpp", "äLIAS.CPP"})
        expect(store.save(directory / cpn::path_from_utf8(name), "body", false, cpn::sha256("absent")).status == SourceSaveResult::Status::conflict,
            "Lock identity fixture changed a missing source.");
    expect(intercepted->acquired_locks.size() == 2, "Source lock identity was not observed.");
#if defined(_WIN32)
    expect(intercepted->acquired_locks[0] == intercepted->acquired_locks[1], "Windows case aliases bypassed the shared source lock.");
#else
    expect(intercepted->acquired_locks[0] != intercepted->acquired_locks[1], "POSIX distinct case paths shared a source identity.");
#endif
}
void source_commit_boundaries(std::filesystem::path const& directory) {
    const auto raced_path = directory / "raced.cpp";
    auto intercepted = std::make_shared<InterceptedFiles>(); intercepted->create_after_prepare = raced_path;
    SourceStore store({directory / "locks", 1024, std::chrono::milliseconds(1000)}, intercepted);
    auto result = store.save(raced_path, "ours", false, {});
    expect(result.status == SourceSaveResult::Status::conflict && !result.replaced && read(raced_path) == "racer", "New-source commit overwrote a racer.");
    intercepted->create_after_prepare.reset(); intercepted->fail_after_prepare = true;
    const auto failed_path = directory / "failed.cpp";
    bool rejected = false;
    try { (void)store.save(failed_path, "ours", false, {}); }
    catch (cpn::AtomicWriteError const& error) { rejected = !error.destination_replaced; }
    expect(rejected && !std::filesystem::exists(failed_path), "Private-file durability failure was reported as source replacement.");
    cpn::native_file_store()->write_atomic(failed_path, "old");
    result = store.save(failed_path, "new", false, cpn::sha256("old"));
    expect(result.status == SourceSaveResult::Status::durability_unknown && result.replaced && read(failed_path) == "new" && result.disk_digest == cpn::sha256("new"),
        "Committed source durability failure lost its actual replacement state.");
}
}
#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        if (argc == 4 && std::filesystem::path{argv[1]} == "--hold-lock") {
            auto files = cpn::native_file_store();
            auto lock = files->acquire_lock(std::filesystem::path{argv[2]}, std::chrono::seconds(5));
            expect(static_cast<bool>(lock), "Fixture could not acquire its lock.");
            files->write_atomic(std::filesystem::path{argv[3]}, "ready");
            std::this_thread::sleep_for(std::chrono::seconds(30)); return 0;
        }
        expect(argc == 2, "Pass recovery-settings.json.");
        auto config = RecoveryConfiguration::parse(read(std::filesystem::path{argv[1]}));
        auto directory = std::filesystem::temp_directory_path() / ("cpn-windows-session-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(directory);
        struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{directory};
        locations(config, directory); save_baselines(); response_models(); model(config); sessions(directory, config); revision_boundaries(directory, config); final_transaction_gate(directory, config); synchronous_test_text(directory, config); worker_boundaries(directory, config); source_files(directory); source_lock_identity(directory); source_commit_boundaries(directory); cancellation(directory, config, std::filesystem::absolute(std::filesystem::path{argv[0]}));
        std::cout << "Windows portable session tests passed: C ABI, native model, raw bytes, conflicts, corrupt preservation, source digests, revision exhaustion, final transaction gating and lock cancellation\n";
        return 0;
    } catch (std::exception const& error) { std::cerr << error.what() << '\n'; return 1; }
}
