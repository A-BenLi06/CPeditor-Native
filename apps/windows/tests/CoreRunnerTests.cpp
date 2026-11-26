#include "CoreRunner.h"
#include "CheckerModel.h"
#include "EditorAssist.h"
#include "RunnerConfiguration.h"
#include "SaveBaseline.h"
#include "ToolchainDiscovery.h"
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif
using namespace cpn::windows;
namespace {
void expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::string path_text(std::filesystem::path const& path) {
    auto value = path.u8string(); return {reinterpret_cast<char const*>(value.data()), value.size()};
}
struct Mailbox {
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<RunEvent> events;
    void receive(RunEvent event) {
        { std::lock_guard lock(mutex); events.push_back(std::move(event)); }
        wake.notify_all();
    }
    template<class Predicate> RunEvent wait(Predicate predicate) {
        std::unique_lock lock(mutex);
        auto matches = [&]() {
            for (auto const& event : events) if (event.kind == RunEvent::Kind::engine_error || predicate(event)) return true;
            return false;
        };
        expect(wake.wait_for(lock, std::chrono::seconds(15), matches), "Timed out waiting for Core event");
        for (auto const& event : events) {
            if (event.kind == RunEvent::Kind::engine_error) throw std::runtime_error(event.error);
            if (predicate(event)) return event; // Copy while holding the lock: callbacks may append after return.
        }
        throw std::logic_error("Missing matched event");
    }
    RunEvent terminal(std::string const& token) { return wait([&](auto const& event) { return event.token == token && event.terminal; }); }
    void audit(std::size_t expected_terminals) {
        std::lock_guard lock(mutex); std::map<std::string, std::size_t> counts;
        for (auto const& event : events) {
            expect(event.kind != RunEvent::Kind::engine_error, "Engine delivery failed");
            if (event.terminal) ++counts[event.token];
        }
        expect(counts.size() == expected_terminals, "Missing terminal event");
        for (auto const& [token, count] : counts) { (void)token; expect(count == 1, "Duplicate terminal event"); }
    }
};
CoreRunner::Options options() {
    CoreRunner::Options value;
    value.engine.struct_size = sizeof(cpn_engine_options); value.engine.abi_version = CPN_ABI_VERSION;
    value.engine.max_workers = 1; value.engine.max_jobs = 2; value.maximum_drain_events = 1;
    return value;
}
nlohmann::json fixture_profile(std::string const& executable, bool build_error = false) {
    nlohmann::json profile{{"schema_version", 1}, {"id", "native-fixture"}, {"language_id", "fixture"},
        {"source_filename", "fixture.json"}, {"artifact_name", ""},
        {"run", {{"executable", executable}, {"arguments", {"--fixture", "${source}"}}}},
        {"run_limits", {{"wall_timeout_ms", 60000}}}};
    if (build_error) {
        profile["artifact_name"] = "unused.exe";
        profile["build"] = {{"executable", executable}, {"arguments", {"--build-error"}}};
    }
    return profile;
}
RunRequest request(std::string token, std::string const& executable) {
    RunRequest value;
    value.token = std::move(token); value.document_id = "document-A"; value.revision = 9007199254740993ULL;
    value.source = "{\"mode\":\"echo\"}"; value.toolchain = fixture_profile(executable); value.comparison = "byte";
    value.tests = {{"empty-checked", "", std::string(""), true}, {"empty-WA", "x", std::string(""), true},
        {"NUL-unchecked", std::string("a\0b", 3), {}, true}, {"disabled", "x", std::string("x"), false}};
    return value;
}
int fixture(std::filesystem::path const& source) {
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY); _setmode(_fileno(stdout), _O_BINARY); _setmode(_fileno(stderr), _O_BINARY);
#endif
    std::ifstream input(source, std::ios::binary); nlohmann::json settings; input >> settings;
    const auto mode = settings.at("mode").get<std::string>();
    if (mode == "sleep") {
        const auto marker_text = settings.at("marker").get<std::string>();
        const auto path = std::filesystem::path{std::u8string{marker_text.begin(), marker_text.end()}};
        { std::ofstream marker(path, std::ios::binary); marker << "started"; }
        std::this_thread::sleep_for(std::chrono::seconds(30)); return 0;
    }
    if (mode == "exit-error") return 3;
    const std::string bytes{std::istreambuf_iterator<char>{std::cin}, {}};
    std::cout.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); return 0;
}
void wait_marker(std::filesystem::path const& marker) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!std::filesystem::exists(marker)) {
        expect(std::chrono::steady_clock::now() < deadline, "Fixture process did not start");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
void save_baseline() {
    SaveBaseline baseline;
    baseline.record_written("A"); baseline.match_native_savepoint();
    expect(baseline.native_matches_disk(), "Loaded document did not match the native savepoint");
    // Captured B reaches disk while current text has already advanced to C.
    baseline.record_written("B");
    expect(!baseline.native_matches_disk(), "Earlier save incorrectly retained the native savepoint");
    expect(baseline.compare_committed(3, "C"), "Concurrent edit was marked clean");
    expect(baseline.cached_dirty(3).value_or(false) && !baseline.cached_dirty(4), "Dirty cache crossed a committed revision");
    // Undo A can make native Modify false, but A is no longer the bytes written to disk.
    expect(baseline.compare_committed(4, "A"), "Undo to the obsolete native savepoint was marked clean");
    expect(!baseline.compare_committed(5, "B"), "Undo to the actual written bytes stayed dirty");
    baseline.match_native_savepoint();
    expect(baseline.native_matches_disk(), "Matching committed bytes did not restore the native fast path");
    // Failed writes never call record_written, so the B baseline remains authoritative.
    expect(baseline.compare_committed(6, "C") && !baseline.compare_committed(7, "B"), "Failed write replaced the saved baseline");
    baseline.record_written(std::string("a\0b", 3));
    expect(baseline.compare_committed(8, "a") && !baseline.compare_committed(9, std::string("a\0b", 3)), "Saved NUL bytes were truncated");
}
void pipeline(std::string const& executable) {
    Mailbox mailbox;
    CoreRunner runner(options(), [&](RunEvent event) { mailbox.receive(std::move(event)); });
    mailbox.wait([](auto const& event) { return event.kind == RunEvent::Kind::ready; });
    for (int index = 0; index < 5; ++index) {
        const auto token = "released-" + std::to_string(index);
        runner.submit(request(token, executable)); const auto event = mailbox.terminal(token);
        expect(event.result && event.result->document_id == "document-A" && event.result->revision == 9007199254740993ULL,
               "Source identity lost precision or crossed documents");
        auto const& tests = event.result->tests;
        expect(tests.size() == 3 && tests[0].verdict == "AC" && tests[1].verdict == "WA" &&
            tests[2].verdict == "Unchecked" && tests[2].standard_output == std::string("a\0b", 3),
            "Empty expected, unchecked, disabled or raw NUL contract changed");
    }
    auto maximum = request("uint64-max", executable); maximum.document_id = "document-B"; maximum.revision = UINT64_MAX; maximum.tests.resize(1);
    runner.submit(std::move(maximum)); const auto maximum_result = mailbox.terminal("uint64-max");
    expect(maximum_result.result && maximum_result.result->document_id == "document-B" && maximum_result.result->revision == UINT64_MAX,
           "Maximum uint64 or second document result was corrupted");
    auto malformed = request("malformed", executable); malformed.source = std::string("\xFF", 1);
    runner.submit(std::move(malformed));
    const auto failure = mailbox.terminal("malformed"); expect(!failure.error.empty() && !failure.result, "Invalid source was accepted");
    auto compile = request("compile-error", executable); compile.toolchain = fixture_profile(executable, true);
    runner.submit(std::move(compile)); auto const compiled = mailbox.terminal("compile-error");
    expect(compiled.result && compiled.result->build_outcome == "CompilationError" && !compiled.result->build_error.empty(), "Compiler failure was hidden");
    auto runtime = request("runtime-error", executable); runtime.source = "{\"mode\":\"exit-error\"}";
    runner.submit(std::move(runtime)); auto const crashed = mailbox.terminal("runtime-error");
    expect(crashed.result && crashed.result->tests[0].verdict == "RE", "Runtime error was hidden");
    runner.close(); mailbox.audit(9);
    bool rejected = false; try { runner.submit(request("after-close", executable)); } catch (...) { rejected = true; }
    expect(rejected, "Closed coordinator accepted work");
}
void cancellation(std::string const& executable, std::filesystem::path const& temporary) {
    const auto marker = temporary / "running.marker";
    Mailbox mailbox; CoreRunner runner(options(), [&](RunEvent event) { mailbox.receive(std::move(event)); });
    mailbox.wait([](auto const& event) { return event.kind == RunEvent::Kind::ready; });
    auto running = request("running-cancel", executable);
    running.source = nlohmann::json{{"mode", "sleep"}, {"marker", path_text(marker)}}.dump(); running.tests.resize(1);
    runner.submit(std::move(running)); wait_marker(marker);
    auto queued = request("queued-cancel", executable); queued.document_id = "document-B"; queued.revision = UINT64_MAX;
    runner.submit(std::move(queued)); runner.cancel("queued-cancel"); runner.cancel("queued-cancel");
    auto const queued_result = mailbox.terminal("queued-cancel"); expect(queued_result.state == "Cancelled", "Queued cancellation failed");
    runner.cancel("running-cancel"); const auto ended = mailbox.terminal("running-cancel");
    expect(ended.result && ended.result->tests[0].verdict == "Cancelled", "Started child was not cancelled");
    runner.close(); mailbox.audit(2);
}
void callback_close_rejected(std::string const& executable) {
    Mailbox mailbox; std::atomic<CoreRunner*> coordinator{};
    std::atomic_bool attempted{}, rejected{};
    CoreRunner runner(options(), [&](RunEvent event) {
        if (event.kind == RunEvent::Kind::job && !attempted.exchange(true)) {
            try { coordinator.load()->close(); } catch (std::logic_error const&) { rejected.store(true); }
        }
        mailbox.receive(std::move(event));
    });
    coordinator.store(&runner);
    mailbox.wait([](auto const& event) { return event.kind == RunEvent::Kind::ready; });
    runner.submit(request("callback-close", executable)); mailbox.terminal("callback-close");
    expect(attempted.load() && rejected.load() && !runner.delivery_failed(), "Callback close was not explicitly rejected");
    runner.submit(request("after-rejected-close", executable));
    expect(mailbox.terminal("after-rejected-close").result.has_value(), "Rejected callback close quietly changed shutdown state");
    runner.close(); mailbox.audit(2);
}
void delivery_failure() {
    CoreRunner runner(options(), [](RunEvent) { throw std::runtime_error("Injected consumer failure"); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!runner.delivery_failed()) {
        expect(std::chrono::steady_clock::now() < deadline, "Consumer failure was hidden");
        std::this_thread::yield();
    }
    runner.close();
    expect(runner.delivery_failed(), "Coordinator failure state was lost");
}
void closing(std::string const& executable, std::filesystem::path const& temporary) {
    const auto marker = temporary / "closing.marker";
    Mailbox mailbox; CoreRunner runner(options(), [&](RunEvent event) { mailbox.receive(std::move(event)); });
    mailbox.wait([](auto const& event) { return event.kind == RunEvent::Kind::ready; });
    auto running = request("close-running", executable); running.tests.resize(1);
    running.source = nlohmann::json{{"mode", "sleep"}, {"marker", path_text(marker)}}.dump(); runner.submit(std::move(running)); wait_marker(marker);
    auto first = std::async(std::launch::async, [&] { runner.close(); });
    auto second = std::async(std::launch::async, [&] { runner.close(); }); first.get(); second.get();
    auto const event = mailbox.terminal("close-running"); expect(event.result && event.state == "Cancelled", "Shutdown lost terminal result");
    mailbox.audit(1);
}
void compile_run_and_checker(std::string const& executable, std::filesystem::path const& temporary) {
    Mailbox mailbox;
    CoreRunner runner(options(), [&](RunEvent event) { mailbox.receive(std::move(event)); });
    mailbox.wait([](auto const& event) { return event.kind == RunEvent::Kind::ready; });

    const auto exported = temporary / "compiled-fixture.exe";
    nlohmann::json build_profile{
        {"schema_version", 1}, {"id", "fixture-compiled"}, {"language_id", "cpp"},
        {"source_filename", "main.cpp"}, {"artifact_name", "compiled-fixture.exe"},
        {"build", {{"executable", executable}, {"arguments", {"--build-copy", executable, "${artifact}"}}}},
        {"run", {{"executable", "${artifact}"}, {"arguments", {"--fixture", "${source}"}}}},
        {"build_limits", {{"wall_timeout_ms", 30000}}},
        {"run_limits", {{"wall_timeout_ms", 30000}}}
    };

    RunRequest compile_only;
    compile_only.token = "compile-only";
    compile_only.document_id = "doc-sep";
    compile_only.revision = 1;
    compile_only.source = "{\"mode\":\"echo\"}";
    compile_only.toolchain = build_profile;
    compile_only.comparison = "byte";
    compile_only.mode = "compile_only";
    compile_only.artifact_path = path_text(exported);
    compile_only.tests = {{"t1", "hello", std::string("hello"), true}};
    runner.submit(compile_only);
    auto const compiled = mailbox.terminal("compile-only");
    expect(compiled.result && compiled.result->build_outcome == "Succeeded" &&
        compiled.result->tests.empty() && std::filesystem::exists(exported),
        "compile_only did not export reusable artifact");

    RunRequest run_only;
    run_only.token = "run-only";
    run_only.document_id = "doc-sep";
    run_only.revision = 1;
    run_only.source = "{\"mode\":\"echo\"}";
    nlohmann::json fail_if_built = build_profile;
    fail_if_built["build"]["arguments"] = {"--build-error"};
    run_only.toolchain = fail_if_built;
    run_only.comparison = "byte";
    run_only.mode = "run_only";
    run_only.artifact_path = path_text(exported);
    run_only.tests = {{"t1", "42\n", std::string("42\n"), true}};
    runner.submit(run_only);
    auto const ran = mailbox.terminal("run-only");
    expect(ran.result && ran.result->build_outcome == "NotRequired" &&
        ran.result->tests.size() == 1 && ran.result->tests[0].verdict == "AC",
        "run_only re-invoked build or failed to run exported artifact");

    RunRequest with_checker = request("with-checker", executable);
    with_checker.tests = {{"chk-ac", "in", std::string("exp"), true}, {"chk-err", "err", std::string("exp"), true}};
    with_checker.external_checker = nlohmann::json{
        {"schema_version", 1},
        {"command", {{"executable", executable}, {"arguments", {"--checker-fixture", "${input}", "${actual}", "${expected}"}}}},
        {"accepted_exit_codes", {0}},
        {"wrong_answer_exit_codes", {1, 2}},
        {"inherit_environment", true},
        {"environment", nlohmann::json::array()}
    };
    runner.submit(with_checker);
    auto const checked = mailbox.terminal("with-checker");
    expect(checked.result && checked.result->tests.size() == 2 &&
        checked.result->tests[0].verdict == "AC" && checked.result->tests[0].checker &&
        checked.result->tests[0].checker->verdict == "Accepted" &&
        checked.result->tests[0].checker->standard_output == "ok-checker\n" &&
        checked.result->tests[1].verdict == "CheckerError" && checked.result->tests[1].checker &&
        checked.result->tests[1].checker->exit_code == 7,
        "External checker diagnostics or verdict decoding failed");

    runner.close();
    mailbox.audit(3);
}
void checker_and_discovery_models() {
    auto cfg = CheckerConfiguration::parse(R"({
        "schemaVersion": 1,
        "maxCheckerJsonBytes": 65536,
        "defaultChecker": {
            "schema_version": 1,
            "command": {"executable": "checker.exe", "arguments": ["${input}", "${actual}", "${expected}"]},
            "limits": {"spawn_timeout_ms": 5000, "wall_timeout_ms": 2000, "terminate_grace_ms": 100, "drain_timeout_ms": 500, "stdout_bytes": 1024, "stderr_bytes": 1024, "combined_bytes": 2048},
            "accepted_exit_codes": [0],
            "wrong_answer_exit_codes": [1, 2],
            "inherit_environment": true,
            "environment": [{"name": "KEEP_ME", "value": "1"}]
        }
    })");
    auto draft = CheckerModel::visual_from_json(cfg.default_checker.dump(), cfg.max_checker_json_bytes);
    expect(draft.executable == "checker.exe" && draft.arguments.size() == 3 && draft.wall_timeout_ms == 2000,
        "Checker visual extraction failed");
    draft.executable = "my_spj.exe";
    draft.wall_timeout_ms = 3500;
    draft.accepted_exit_codes = CheckerModel::parse_exit_codes("0, 4");
    auto updated = CheckerModel::apply_visual_to_json(cfg.default_checker.dump(), draft, cfg.max_checker_json_bytes);
    auto parsed = CheckerModel::validate_json(updated, cfg.max_checker_json_bytes);
    expect(parsed.at("environment").size() == 1 && parsed.at("limits").at("spawn_timeout_ms") == 5000 &&
        parsed.at("limits").at("wall_timeout_ms") == 3500 && parsed.at("accepted_exit_codes").size() == 2,
        "Checker visual apply lost preserved fields");
    bool bad_rejected = false;
    try { (void)CheckerModel::validate_json("{\"schema_version\":1,\"unknown\":true}"); } catch (...) { bad_rejected = true; }
    expect(bad_rejected, "Invalid checker JSON was accepted");
    expect(CheckerModel::fingerprint({false, updated}) == "disabled" &&
        CheckerModel::fingerprint({true, updated}).starts_with("enabled:"),
        "Checker fingerprint mismatch");

    ToolchainChoice sample_tc;
    sample_tc.id = "cpp-gcc-windows";
    sample_tc.file = "cpp-gcc-windows.json";
    sample_tc.label_resource = "ToolchainGCC";
    sample_tc.profile = {
        {"schema_version", 1}, {"id", "cpp-gcc-windows"}, {"language_id", "cpp"},
        {"source_filename", "main.cpp"}, {"artifact_name", "solution.exe"},
        {"build", {{"executable", "g++"}, {"arguments", {"-std=c++20", "${source}", "-o", "${artifact}"}}}},
        {"run", {{"executable", "${artifact}"}, {"arguments", nlohmann::json::array()}}},
        {"build_limits", {{"wall_timeout_ms", 30000}}}, {"run_limits", {{"wall_timeout_ms", 2000}}},
        {"inherit_environment", true}, {"environment", nlohmann::json::array()}, {"variables", nlohmann::json::array()}
    };
    auto tc_draft = RunnerConfiguration::visual_from_json(sample_tc.profile);
    expect(tc_draft.has_build && tc_draft.build_executable == "g++" && tc_draft.build_wall_timeout_ms == 30000,
        "Toolchain visual draft extraction failed");
    tc_draft.build_arguments.push_back("-Wall");
    auto tc_updated = RunnerConfiguration::apply_visual_to_json(sample_tc.profile, tc_draft);
    expect(RunnerConfiguration::is_compiled_toolchain(tc_updated) &&
        tc_updated.at("build").at("arguments").size() == 5,
        "Toolchain visual apply failed");
    auto probes = ToolchainDiscovery::probe_all({sample_tc}, ToolchainDiscoveryConfiguration::defaults());
    expect(probes.size() == 1 && probes[0].id == "cpp-gcc-windows", "Toolchain probe count mismatch");
}
void editor_assist_tests() {
    auto p1 = EditorAssist::plan_char_input("int main", "", "", '(', "cpp");
    expect(p1 && p1->insert_text == "()" && p1->caret_offset_in_insert == 1, "Auto-pair '(' failed");
    auto p2 = EditorAssist::plan_char_input("auto x = ", "", "a+b", '(', "cpp");
    expect(p2 && p2->insert_text == "(a+b)" && p2->caret_offset_in_insert == 5, "Wrap selection failed");
    auto p3 = EditorAssist::plan_char_input("foo(", ")", "", ')', "cpp");
    expect(p3 && p3->erase_after == 1 && p3->insert_text == ")" && p3->caret_offset_in_insert == 1, "Closing skip failed");
    auto p4 = EditorAssist::plan_char_input("int x = 1", "", "", '\'', "cpp");
    expect(!p4, "C++ digit separator wrongly auto-paired");
    auto p5 = EditorAssist::plan_char_input("// comment ", "", "", '(', "cpp");
    expect(!p5, "Auto-pair triggered inside line comment");
    auto p6 = EditorAssist::plan_char_input("std::string s = \"hi", "\"", "", '"', "cpp");
    expect(p6 && p6->erase_after == 1 && p6->insert_text == "\"", "Closing quote skip failed");
    auto p7 = EditorAssist::plan_backspace("(", ")");
    expect(p7 && p7->erase_before == 1 && p7->erase_after == 1 && p7->insert_text.empty(), "Pair backspace failed");
    auto p8 = EditorAssist::plan_backspace("\\\"", "\"");
    expect(!p8, "Escaped quote was wrongly deleted as pair");
    auto n1 = EditorAssist::plan_newline("    if (ok) {", "} else {}", "\r\n", 4);
    expect(n1 && n1->insert_text == "\r\n        \r\n    " && n1->caret_offset_in_insert == 10, "CRLF brace newline failed");
    auto t1 = EditorAssist::plan_tab_indent("  int a = 1;\r\n  int b = 2;\r\n", 3, 18, false, 4);
    expect(t1 && t1->replacement == "      int a = 1;\r\n      int b = 2;" && t1->new_sel_start == 7, "Multi-line tab indent failed");
    auto t2 = EditorAssist::plan_tab_indent("    int a = 1;\r\n  int b = 2;", 4, 20, true, 4);
    expect(t2 && t2->replacement == "int a = 1;\r\nint b = 2;" && t2->new_sel_start == 0, "Multi-line Shift+Tab unindent failed");
}
}
int run_tests(std::vector<std::filesystem::path> const& arguments) {
    const auto argc = arguments.size();
    try {
        if (argc == 3 && arguments[1] == "--fixture") return fixture(arguments[2]);
        if (argc == 2 && arguments[1] == "--build-error") { std::cerr << "fixture compiler diagnostic\n"; return 2; }
        if (argc == 4 && arguments[1] == "--build-copy") {
            std::filesystem::copy_file(arguments[2], arguments[3], std::filesystem::copy_options::overwrite_existing);
            return 0;
        }
        if (argc == 5 && arguments[1] == "--checker-fixture") {
#if defined(_WIN32)
            _setmode(_fileno(stdout), _O_BINARY);
            _setmode(_fileno(stderr), _O_BINARY);
#endif
            std::ifstream in(arguments[2], std::ios::binary);
            std::string input_bytes{std::istreambuf_iterator<char>{in}, {}};
            if (input_bytes == "err") { std::cerr << "bad checker exit\n"; return 7; }
            std::cout << "ok-checker\n";
            return 0;
        }
        const auto executable = path_text(std::filesystem::absolute(arguments[0]));
        auto const temporary = std::filesystem::temp_directory_path() / ("cpn-adapter-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(temporary);
        struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{temporary};
        save_baseline(); pipeline(executable); compile_run_and_checker(executable, temporary); checker_and_discovery_models(); editor_assist_tests(); cancellation(executable, temporary); closing(executable, temporary); callback_close_rejected(executable); delivery_failure();
        if (argc == 2) {
            std::ifstream file(arguments[1], std::ios::binary);
            const std::string text{std::istreambuf_iterator<char>{file}, {}};
            const auto settings = RunnerConfiguration::parse(text);
            expect(settings.maximum_documents > 0 && !settings.toolchains.empty(), "Runner settings missing");
        }
        std::cout << "Core adapter tests passed: saved baseline, identity, raw bytes, release, failures, cancellation, callback rejection and concurrent close\n";
        return 0;
    } catch (std::exception const& error) { std::cerr << error.what() << '\n'; return 1; }
}

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    std::vector<std::filesystem::path> arguments;
    for (int index = 0; index < argc; ++index) arguments.emplace_back(argv[index]);
    return run_tests(arguments);
}
