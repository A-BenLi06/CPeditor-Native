// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/external_checker.hpp"
#include "cpn/text.hpp"
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using Verdict = cpn::ExternalCheckerVerdict;
void expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejected(F operation, const char* message) { try { operation(); } catch (const std::exception&) { return; } throw std::runtime_error(message); }
std::string raw(std::string value) { value.push_back('\0'); value.push_back(static_cast<char>(0xff)); return value; }
cpn::CheckerInput fixture() { return {raw("input"), raw("actual"), raw("expected"), std::nullopt}; }
cpn::ExternalCheckerSpec spec_for(const std::filesystem::path& helper, std::string mode) {
    cpn::ExternalCheckerSpec spec; spec.command = {cpn::path_to_utf8(helper), {std::move(mode), "${input}", "${actual}", "${expected}"}}; return spec;
}
class ObservedFiles final : public cpn::FileStore {
public:
    explicit ObservedFiles(std::shared_ptr<cpn::FileStore> underlying = cpn::native_file_store()) : files(std::move(underlying)) {}
    std::string read(const std::filesystem::path& path, std::size_t bytes) const override { return files->read(path, bytes); }
    std::optional<std::string> read_optional(const std::filesystem::path& path, std::size_t bytes) const override { return files->read_optional(path, bytes); }
    std::unique_ptr<cpn::FileLock> acquire_lock(const std::filesystem::path& path, std::chrono::milliseconds timeout, std::stop_token stop) const override { return files->acquire_lock(path, timeout, stop); }
    void write_atomic(const std::filesystem::path& path, std::string_view bytes) const override {
        {
            std::lock_guard lock(mutex); ++writes;
            if (fail_write) throw std::runtime_error("injected checker input I/O failure");
        }
        files->write_atomic(path, bytes); if (write_hook) write_hook();
    }
    std::filesystem::path create_workspace() const override {
        auto path = files->create_workspace();
        if (relative_workspace) {
            const auto relative = path.lexically_relative(std::filesystem::current_path());
            if (!relative.empty()) path = relative;
        }
        std::lock_guard lock(mutex); created.push_back(path); return path;
    }
    void remove_workspace(const std::filesystem::path& path) const noexcept override {
        files->remove_workspace(path); std::lock_guard lock(mutex); ++removed;
    }
    void cleaned() const {
        std::lock_guard lock(mutex); expect(removed == created.size(), "every checker workspace cleanup requested");
        for (const auto& path : created) expect(!std::filesystem::exists(path), "checker workspace removed");
    }
    std::shared_ptr<cpn::FileStore> files;
    mutable std::mutex mutex;
    mutable std::vector<std::filesystem::path> created;
    mutable std::size_t removed{0}, writes{0};
    bool fail_write{false};
    bool relative_workspace{false};
    std::function<void()> write_hook;
};
void validation(const std::filesystem::path& helper) {
    const auto original = spec_for(helper, "verify"); auto spec = original;
    for (const auto& executable : std::vector<std::string>{"", std::string("a\0b", 3), std::string(1, static_cast<char>(0xff))}) {
        spec = original; spec.command.executable = executable; rejected([&] { cpn::validate_external_checker(spec); }, "invalid command executable rejected");
    }
    for (const auto& argument : std::vector<std::string>{"${unknown}", "${", "${}", std::string("a\0b", 3), std::string(1, static_cast<char>(0xff))}) {
        spec = original; spec.command.arguments.push_back(argument); rejected([&] { cpn::validate_external_checker(spec); }, "invalid argument/template rejected");
    }
    for (const auto& codes : std::vector<std::vector<int>>{{}, {-1}, {256}, {0, 0}, {1}}) {
        spec = original; spec.accepted_exit_codes = codes; rejected([&] { cpn::validate_external_checker(spec); }, "exit mapping strict and disjoint");
    }
    spec = original; spec.wrong_answer_exit_codes = {2, 2}; rejected([&] { cpn::validate_external_checker(spec); }, "duplicate WA mapping rejected");
    for (int field = 0; field < 4; ++field) {
        spec = original;
        auto* duration = field == 0 ? &spec.limits.spawn_timeout : field == 1 ? &spec.limits.wall_timeout : field == 2 ? &spec.limits.terminate_grace : &spec.limits.drain_timeout;
        *duration = std::chrono::milliseconds::max(); rejected([&] { cpn::validate_external_checker(spec); }, "duration cannot overflow");
        *duration = std::chrono::milliseconds{-1}; rejected([&] { cpn::validate_external_checker(spec); }, "negative duration rejected");
        *duration = std::chrono::milliseconds{0}; if (field != 2) rejected([&] { cpn::validate_external_checker(spec); }, "zero required deadline rejected");
    }
    spec = original; spec.environment = {{"PATH", "one"}, {"path", "two"}}; rejected([&] { cpn::validate_external_checker(spec); }, "environment portable uniqueness");
    spec.environment = {{"INVALID=KEY", "value"}}; rejected([&] { cpn::validate_external_checker(spec); }, "environment key syntax");
    spec.environment = {{"key", std::string("a\0b", 3)}}; rejected([&] { cpn::validate_external_checker(spec); }, "environment value NUL");
    cpn::ExternalCheckerBounds bounds; bounds.max_command_bytes = 1; rejected([&] { cpn::validate_external_checker(original, bounds); }, "command length bound");
    bounds = {}; bounds.max_arguments = 1; rejected([&] { cpn::validate_external_checker(original, bounds); }, "argument count bound");
    auto files = std::make_shared<ObservedFiles>(); auto input = fixture(); bounds = {}; bounds.max_actual_bytes = 1;
    auto result = cpn::ExternalCheckerService(files, cpn::run_process, bounds).run(original, input);
    expect(result.verdict == Verdict::checker_error && !result.process_attempted && files->created.empty(), "input limit prevents filesystem/process work");
    bounds = {}; bounds.max_total_input_bytes = 1; result = cpn::ExternalCheckerService(files, cpn::run_process, bounds).run(original, input);
    expect(result.verdict == Verdict::checker_error && files->created.empty(), "aggregate input budget checked before workspace");
    input.source_dir = std::filesystem::path{}; result = cpn::ExternalCheckerService(files).run(original, input);
    expect(result.verdict == Verdict::checker_error && files->created.empty(), "present empty source_dir rejected");
#if defined(_WIN32)
    input.source_dir = std::filesystem::path(std::wstring(L"bad\0path", 8));
#else
    input.source_dir = std::filesystem::path(std::string("bad\0path", 8));
#endif
    result = cpn::ExternalCheckerService(files).run(original, input);
    expect(result.verdict == Verdict::checker_error && files->created.empty(), "source_dir NUL rejected before native path normalization");
}
void native(const std::filesystem::path& helper) {
    auto files = std::make_shared<ObservedFiles>(); cpn::ExternalCheckerService service(files); auto input = fixture();
    auto spec = spec_for(helper, "verify"); auto result = service.run(spec, input);
    expect(result.verdict == Verdict::accepted && result.process_attempted && result.process.exit_code == 0, "real checker verifies three raw byte files");
    expect(result.process.standard_output == input.input + input.actual + input.expected && result.process.standard_error == raw("diagnostic"), "raw checker outputs preserve NUL and FF"); files->cleaned();
    for (const auto code : {0, 1, 2, 3, 7, 42}) {
        spec = spec_for(helper, "exit"); spec.command.arguments.push_back(std::to_string(code)); result = service.run(spec, input);
        expect(result.verdict == (code == 0 ? Verdict::accepted : code == 1 || code == 2 ? Verdict::wrong_answer : Verdict::checker_error), "default testlib binary exit mapping");
        if (code == 7) expect(result.process.standard_output == "0.5\n", "partial score output retained without guessing verdict");
    }
    spec.accepted_exit_codes = {42}; spec.wrong_answer_exit_codes = {}; result = service.run(spec, input); expect(result.verdict == Verdict::accepted, "custom accepted code data-driven");
    spec.command.arguments.back() = "7"; spec.accepted_exit_codes = {7}; result = service.run(spec, input); expect(result.verdict == Verdict::accepted, "explicit custom binary code seven is not inferred testlib scoring");
    input = {}; spec = spec_for(helper, "compare"); result = service.run(spec, input); expect(result.verdict == Verdict::accepted, "empty input/actual/expected valid");
    input.actual = raw("same"); input.expected = input.actual; result = service.run(spec, input); expect(result.verdict == Verdict::accepted, "binary comparison checker accepted");
    input.expected = "different"; result = service.run(spec, input); expect(result.verdict == Verdict::wrong_answer, "binary comparison checker rejected");
    input = fixture(); spec = spec_for(helper, "args"); const std::string literal = "中文 参数 \"quote\" tail\\ $(no-shell) `literal`"; spec.command.arguments.push_back(literal);
    result = service.run(spec, input); expect(result.verdict == Verdict::accepted && result.process.standard_output == literal, "argv boundaries/Unicode/quotes preserved without shell");
    auto original = files->files->create_workspace(); const auto user_file = original / "input.bin"; files->files->write_atomic(user_file, "original user data"); input.source_dir = original;
    spec = spec_for(helper, "source-dir"); spec.command.arguments.push_back("${source_dir}"); result = service.run(spec, input);
    expect(result.verdict == Verdict::accepted && result.process.standard_output == cpn::path_to_utf8(original), "source_dir optional metadata expanded explicitly");
    expect(files->files->read(user_file, 100) == "original user data", "checker private files cannot overwrite source directory"); files->files->remove_workspace(original);
    input.source_dir.reset(); result = service.run(spec, input); expect(result.process.standard_output == cpn::path_to_utf8(files->created.back()), "missing source_dir expands to invocation workspace");
    spec = spec_for(helper, "signal"); result = service.run(spec, input); expect(result.verdict == Verdict::checker_error, "abnormal checker exit never solver RE");
    spec = spec_for(helper, "sleep"); spec.limits.wall_timeout = std::chrono::milliseconds{30}; result = service.run(spec, input);
    expect(result.verdict == Verdict::checker_error && result.process.termination == cpn::ProcessTermination::timed_out, "checker timeout never solver TLE");
    spec = spec_for(helper, "flood"); spec.limits.stdout_bytes = 16; spec.limits.stderr_bytes = 16; spec.limits.combined_bytes = 24; result = service.run(spec, input);
    expect(result.verdict == Verdict::checker_error && result.process.termination == cpn::ProcessTermination::output_limit, "checker OLE is checker error");
    expect(result.process.standard_output.size() <= 16 && result.process.standard_error.size() <= 16 && result.process.standard_output.size() + result.process.standard_error.size() <= 24, "checker output budget includes drain");
    spec.command.executable = cpn::path_to_utf8(helper.parent_path() / "missing-checker"); result = service.run(spec, input);
    expect(result.verdict == Verdict::checker_error && result.process.termination == cpn::ProcessTermination::spawn_failed, "missing checker error distinct from solver verdict"); files->cleaned();
#if defined(_WIN32)
    const auto name = "检查 器.exe";
#else
    const auto name = "检查 器";
#endif
    original = files->files->create_workspace(); auto copied = original / cpn::path_from_utf8(name); std::filesystem::copy_file(helper, copied);
    spec = spec_for(copied, "verify"); result = service.run(spec, fixture()); expect(result.verdict == Verdict::accepted, "Unicode checker executable path"); files->files->remove_workspace(original); files->cleaned();
    files->relative_workspace = true; result = service.run(spec_for(helper, "verify"), fixture());
    expect(result.verdict == Verdict::accepted, "relative FileStore workspace uses absolute file argv and cwd"); files->cleaned();
}
void ports(const std::filesystem::path& helper) {
    auto files = std::make_shared<ObservedFiles>(); const auto spec = spec_for(helper, "verify"); const auto input = fixture();
    files->fail_write = true; auto result = cpn::ExternalCheckerService(files).run(spec, input);
    expect(result.verdict == Verdict::checker_error && !result.process_attempted, "input I/O failure cannot launch checker"); files->cleaned(); files->fail_write = false;
    std::stop_source before; before.request_stop(); const auto count = files->created.size(); result = cpn::ExternalCheckerService(files).run(spec, input, before.get_token());
    expect(result.verdict == Verdict::cancelled && !result.process_attempted && files->created.size() == count, "pre-cancelled checker allocates no workspace");
    std::stop_source writing; files->write_hook = [&] { writing.request_stop(); }; result = cpn::ExternalCheckerService(files).run(spec, input, writing.get_token());
    expect(result.verdict == Verdict::cancelled && !result.process_attempted, "input-writing cancellation prevents execution"); files->cleaned(); files->write_hook = {};
    for (const auto termination : {cpn::ProcessTermination::io_failed, cpn::ProcessTermination::drain_timeout, cpn::ProcessTermination::unsupported_limit}) {
        cpn::ProcessExecutor failed = [=](const cpn::ProcessSpec&, std::stop_token) { cpn::ProcessResult process; process.termination = termination; return process; };
        result = cpn::ExternalCheckerService(files, failed).run(spec, input); expect(result.verdict == Verdict::checker_error && result.process.termination == termination, "all host failures are checker errors");
    }
    std::promise<void> entered; std::mutex mutex; std::condition_variable condition; std::stop_source active;
    cpn::ProcessExecutor held = [&](const cpn::ProcessSpec& process, std::stop_token token) {
        expect(process.input.empty(), "checker stdin remains empty"); std::stop_callback wake(token, [&] { condition.notify_all(); }); entered.set_value();
        std::unique_lock lock(mutex); condition.wait(lock, [&] { return token.stop_requested(); }); cpn::ProcessResult stopped; stopped.termination = cpn::ProcessTermination::cancelled; return stopped;
    };
    auto started = entered.get_future(); std::jthread runner([&] { result = cpn::ExternalCheckerService(files, held).run(spec, input, active.get_token()); });
    if (started.wait_for(std::chrono::seconds{2}) != std::future_status::ready) { active.request_stop(); throw std::runtime_error("checker executor did not start"); }
    active.request_stop(); runner.join(); expect(result.verdict == Verdict::cancelled && result.process_attempted, "active checker cancellation propagated independently"); files->cleaned();
    std::stop_source late; cpn::ProcessExecutor completed = [&](const cpn::ProcessSpec&, std::stop_token) { late.request_stop(); cpn::ProcessResult process; process.termination = cpn::ProcessTermination::exited; process.exit_code = 0; return process; };
    result = cpn::ExternalCheckerService(files, completed).run(spec, input, late.get_token()); expect(result.verdict == Verdict::accepted, "late token cannot replace confirmed natural completion");
    auto output_spec = spec; output_spec.limits.stdout_bytes = 20; output_spec.limits.stderr_bytes = 30; output_spec.limits.combined_bytes = 25;
    cpn::ProcessExecutor oversized = [](const cpn::ProcessSpec&, std::stop_token) { cpn::ProcessResult process; process.termination = cpn::ProcessTermination::exited; process.exit_code = 0; process.standard_output = std::string(100, static_cast<char>(0xff)); process.standard_error = std::string(100, '\0'); return process; };
    result = cpn::ExternalCheckerService(files, oversized).run(output_spec, input);
    expect(result.verdict == Verdict::checker_error && result.process.standard_output.size() == 20 && result.process.standard_error.size() == 5 && result.process.stdout_truncated && result.process.stderr_truncated, "injected executor cannot bypass retained output cap");
    const auto long_message = std::string(4095, 'E') + "😀" + std::string(10000, 'E');
    cpn::ProcessExecutor throwing = [&](const cpn::ProcessSpec&, std::stop_token) -> cpn::ProcessResult { throw std::runtime_error(long_message); };
    result = cpn::ExternalCheckerService(files, throwing).run(spec, input);
    expect(result.verdict == Verdict::checker_error && result.process.termination == cpn::ProcessTermination::io_failed && result.detail.size() <= 4096 && !cpn::decode_utf8(result.detail).error_offset, "exception detail bounded at UTF8 boundary"); files->cleaned();
}
int run(const std::filesystem::path& helper) { validation(helper); native(helper); ports(helper); std::cout << "external checker tests passed\n"; return 0; }
}
#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) { try { if (argc != 2) throw std::runtime_error("checker helper required"); return run(std::filesystem::absolute(argv[1])); } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
#else
int main(int argc, char** argv) { try { if (argc != 2) throw std::runtime_error("checker helper required"); return run(std::filesystem::absolute(cpn::path_from_utf8(argv[1]))); } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
#endif
