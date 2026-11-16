// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/process.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace {
using namespace std::chrono_literals;
void expect(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
std::string utf8_path(const std::filesystem::path& path) {
    const auto value = path.u8string(); return {reinterpret_cast<const char*>(value.data()), value.size()};
}
std::filesystem::path path_from_utf8(const std::string& value) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}
class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto base = std::filesystem::temp_directory_path();
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int index = 0; index < 100; ++index) {
            path = base / ("cpn-process-tests-" + std::to_string(stamp) + "-" + std::to_string(index));
            if (std::filesystem::create_directory(path)) return;
        }
        throw std::runtime_error("Unable to create isolated test directory");
    }
    ~TemporaryDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    std::filesystem::path path;
};
std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path); return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
int test(const std::filesystem::path& helper) {
    TemporaryDirectory temporary;
    auto spec_for = [&](std::vector<std::string> arguments) {
        cpn::ProcessSpec spec; spec.executable = helper; spec.arguments = std::move(arguments); return spec;
    };
    int count = 0;
    auto check = [&](const std::string& name, const std::function<void()>& body) {
        body(); ++count; std::cout << "PASS " << name << '\n';
    };
    check("empty stdin is executed", [&] {
        auto result = cpn::run_process(spec_for({"echo"}));
        expect(result.termination == cpn::ProcessTermination::exited && result.exit_code == 0, "Empty-input execution failed: " + result.detail);
        expect(result.standard_output.empty(), "Empty input gained output");
    });
    check("raw Unicode and NUL round trip", [&] {
        auto spec = spec_for({"echo"}); spec.input = "UTF-8 中文 😀\r\n"; spec.input.push_back('\0'); spec.input += "tail"; spec.input.push_back(static_cast<char>(0xff));
        auto result = cpn::run_process(spec);
        expect(result.exit_code == 0 && result.standard_output == spec.input, "Raw-byte echo was changed: " + result.detail);
        expect(result.stdout_total_bytes == spec.input.size(), "Raw-byte output count is wrong");
    });
    check("large stdin while stderr is blocked", [&] {
        auto spec = spec_for({"duplex"}); spec.input.assign(2 * 1024 * 1024, 'I');
        auto result = cpn::run_process(spec);
        expect(result.termination == cpn::ProcessTermination::exited && result.exit_code == 0, "Duplex process failed: " + result.detail);
        expect(result.standard_output == spec.input && result.standard_error == std::string(262144, 'E'), "Concurrent streams corrupted or deadlocked");
    });
    check("explicit argv and quoting", [&] {
        const std::vector<std::string> arguments{"args", "", "space argument", "中文 参数", "quote\"tail\\", "\\\\", "$(no-shell) `literal`"};
        auto result = cpn::run_process(spec_for(arguments));
        std::string expected;
        for (std::size_t index = 1; index < arguments.size(); ++index) expected += std::to_string(arguments[index].size()) + ":" + arguments[index] + "\n";
        expect(result.exit_code == 0 && result.standard_output == expected, "Argument boundaries changed: " + result.detail);
    });
    check("configured environment without inheritance", [&] {
        auto spec = spec_for({"env", "CPN_TEST_VALUE"}); spec.inherit_environment = false;
        spec.environment = {{"CPN_TEST_VALUE", "覆盖 中文"}};
        auto result = cpn::run_process(spec);
        expect(result.exit_code == 0 && result.standard_output == "覆盖 中文", "Configured environment lost value: " + result.detail);
    });
    check("configured PATH and Unicode executable/cwd", [&] {
        auto directory = temporary.path / path_from_utf8("中文 空格目录"); std::filesystem::create_directory(directory);
#if defined(_WIN32)
        auto copied = directory / path_from_utf8("辅助 进程.exe");
#else
        auto copied = directory / path_from_utf8("辅助 进程");
#endif
        std::filesystem::copy_file(helper, copied);
        auto spec = spec_for({"cwd"}); spec.executable = copied.filename(); spec.working_directory = directory;
        spec.inherit_environment = false; spec.environment = {{"PATH", utf8_path(directory)}};
        auto result = cpn::run_process(spec);
        expect(result.exit_code == 0 && std::filesystem::equivalent(path_from_utf8(result.standard_output), directory),
               "Unicode path/cwd failed: " + result.detail + " observed=" + result.standard_output);
    });
    check("nonzero exit stays distinct from spawn failure", [&] {
        auto result = cpn::run_process(spec_for({"exit", "17"}));
        expect(result.termination == cpn::ProcessTermination::exited && result.exit_code == 17, "Exit status was lost");
    });
    check("missing executable and invalid argv", [&] {
        auto spec = spec_for({}); spec.executable = temporary.path / "does-not-exist";
        auto missing = cpn::run_process(spec);
        expect(missing.termination == cpn::ProcessTermination::spawn_failed && missing.native_error != 0, "Missing executable became a run result");
        spec = spec_for({std::string("a\0b", 3)});
        auto invalid = cpn::run_process(spec);
        expect(invalid.termination == cpn::ProcessTermination::spawn_failed && invalid.native_error != 0, "NUL argument was accepted");
    });
    check("invalid control text is rejected", [&] {
        const std::string invalid(1, static_cast<char>(0xff));
        auto spec = spec_for({"args", invalid});
        expect(cpn::run_process(spec).termination == cpn::ProcessTermination::spawn_failed, "Invalid UTF-8 argv was accepted");
        spec = spec_for({"echo"}); spec.environment = {{"CPN_TEST_VALUE", invalid}};
        expect(cpn::run_process(spec).termination == cpn::ProcessTermination::spawn_failed, "Invalid UTF-8 environment value was accepted");
        spec.environment = {{invalid, "value"}};
        expect(cpn::run_process(spec).termination == cpn::ProcessTermination::spawn_failed, "Invalid UTF-8 environment key was accepted");
        spec.environment = {{"CPN_TEST_VALUE", std::string("a\0b", 3)}};
        expect(cpn::run_process(spec).termination == cpn::ProcessTermination::spawn_failed, "NUL environment value was accepted");
        spec.environment = {{"INVALID=KEY", "value"}};
        expect(cpn::run_process(spec).termination == cpn::ProcessTermination::spawn_failed, "Environment key containing '=' was accepted");
        spec = spec_for({"echo"});
#if defined(_WIN32)
        spec.executable = std::filesystem::path(std::wstring(1, static_cast<wchar_t>(0xd800)));
#else
        spec.executable = std::filesystem::path(invalid);
#endif
        expect(cpn::run_process(spec).termination == cpn::ProcessTermination::spawn_failed, "Invalid Unicode program path was accepted");
    });
    check("duration validation cannot overflow", [&] {
        for (int field = 0; field < 4; ++field) {
            auto spec = spec_for({"echo"});
            auto* limit = field == 0 ? &spec.limits.spawn_timeout : field == 1 ? &spec.limits.wall_timeout :
                          field == 2 ? &spec.limits.terminate_grace : &spec.limits.drain_timeout;
            *limit = std::chrono::milliseconds::max();
            expect(cpn::run_process(spec).termination == cpn::ProcessTermination::spawn_failed, "Unrepresentable duration was accepted");
            *limit = -1ms;
            expect(cpn::run_process(spec).termination == cpn::ProcessTermination::spawn_failed, "Negative duration was accepted");
            *limit = 0ms;
            if (field != 2) expect(cpn::run_process(spec).termination == cpn::ProcessTermination::spawn_failed, "Zero deadline was accepted");
        }
    });
    check("wall timeout is bounded", [&] {
        auto spec = spec_for({"sleep", "5000"}); spec.limits.wall_timeout = 80ms;
        const auto begin = std::chrono::steady_clock::now(); auto result = cpn::run_process(spec);
        expect(result.termination == cpn::ProcessTermination::timed_out, "Timeout reason was lost: " + result.detail);
        expect(std::chrono::steady_clock::now() - begin < 2s, "Timeout cleanup blocked");
    });
    check("cancellation and pre-cancellation", [&] {
        std::stop_source source; auto spec = spec_for({"sleep", "5000"});
        std::jthread canceller([&] { std::this_thread::sleep_for(80ms); source.request_stop(); });
        auto result = cpn::run_process(spec, source.get_token());
        expect(result.termination == cpn::ProcessTermination::cancelled, "Cancellation became timeout");
        auto already_cancelled = cpn::run_process(spec_for({"exit", "17"}), source.get_token());
        expect(already_cancelled.termination == cpn::ProcessTermination::cancelled && !already_cancelled.exit_code, "Pre-cancelled request executed");
    });
    check("stream and combined output caps", [&] {
        auto spec = spec_for({"flood"}); spec.limits.stdout_bytes = 1024; spec.limits.stderr_bytes = 1024; spec.limits.combined_bytes = 1536;
        auto result = cpn::run_process(spec);
        expect(result.termination == cpn::ProcessTermination::output_limit, "Flood did not hit output limit");
        expect(result.standard_output.size() <= 1024 && result.standard_error.size() <= 1024 &&
               result.standard_output.size() + result.standard_error.size() <= 1536, "Retained output exceeded cap during drain");
        expect(result.stdout_truncated || result.stderr_truncated, "Truncation was not reported");
        spec = spec_for({"emit", "128"}); spec.limits.stdout_bytes = 128; spec.limits.combined_bytes = 128;
        result = cpn::run_process(spec);
        expect(result.termination == cpn::ProcessTermination::exited && result.standard_output.size() == 128 && !result.stdout_truncated,
               "Exactly-at-limit output was rejected");
        spec = spec_for({"emit", "1"}); spec.limits.stdout_bytes = 0; spec.limits.combined_bytes = 0;
        result = cpn::run_process(spec);
        expect(result.termination == cpn::ProcessTermination::output_limit && result.standard_output.empty(), "Zero cap retained output");
    });
    check("early stdin close does not kill host", [&] {
        auto spec = spec_for({"close-input"}); spec.input.assign(2 * 1024 * 1024, 'I');
        auto result = cpn::run_process(spec);
        expect(result.termination == cpn::ProcessTermination::exited && result.exit_code == 0 && result.standard_output == "closed",
               "Early stdin close failed: " + result.detail);
    });
    check("normal exit cleans inherited-pipe descendants", [&] {
        const auto marker = temporary.path / "managed-marker";
        auto spec = spec_for({"descendant", utf8_path(marker)}); auto result = cpn::run_process(spec);
        expect(result.termination == cpn::ProcessTermination::exited && result.exit_code == 0, "Inherited descendant cleanup failed: " + result.detail);
        expect(!result.standard_output.empty(), "Descendant ID was not captured");
        const auto before = read_file(marker); std::this_thread::sleep_for(150ms);
        expect(read_file(marker) == before, "Managed descendant remained running");
    });
#if !defined(_WIN32)
    check("signal is preserved", [&] {
        auto result = cpn::run_process(spec_for({"signal"}));
        expect(result.termination == cpn::ProcessTermination::exited && result.signal == SIGTERM && !result.exit_code, "Signal became ordinary exit");
    });
    check("escaped inherited pipe has bounded drain", [&] {
        const auto marker = temporary.path / "escaped-marker";
        auto spec = spec_for({"escaped", utf8_path(marker)}); spec.limits.drain_timeout = 120ms;
        const auto begin = std::chrono::steady_clock::now(); auto result = cpn::run_process(spec);
        expect(!result.standard_output.empty(), "Escaped descendant ID was not captured");
        const auto pid = static_cast<pid_t>(std::stol(result.standard_output));
        ::kill(-pid, SIGKILL); // Only the intentionally escaped helper created by this test.
        expect(result.termination == cpn::ProcessTermination::drain_timeout, "Escaped pipe was not classified: " + result.detail);
        expect(std::chrono::steady_clock::now() - begin < 2s, "Escaped pipe blocked indefinitely");
    });
    check("SIGPIPE disposition and mask are preserved", [&] {
        struct sigaction before{}, after{}; ::sigaction(SIGPIPE, nullptr, &before);
        sigset_t before_mask{}, after_mask{}; ::pthread_sigmask(SIG_SETMASK, nullptr, &before_mask);
        auto spec = spec_for({"close-input"}); spec.input.assign(262144, 'I'); (void)cpn::run_process(spec);
        ::sigaction(SIGPIPE, nullptr, &after); ::pthread_sigmask(SIG_SETMASK, nullptr, &after_mask);
        expect(before.sa_handler == after.sa_handler && sigismember(&before_mask, SIGPIPE) == sigismember(&after_mask, SIGPIPE),
               "Host SIGPIPE state was changed");
    });
    check("repeated runs release descriptors", [&] {
        auto fd_count = [] { int count = 0; for (int fd = 0; fd < 1024; ++fd) if (::fcntl(fd, F_GETFD) >= 0) ++count; return count; };
        const int before = fd_count();
        for (int index = 0; index < 40; ++index) expect(cpn::run_process(spec_for({"echo"})).exit_code == 0, "Repeated run failed");
        expect(fd_count() == before, "File descriptors leaked");
    });
#endif
    check("unsupported memory and invalid duration are explicit", [&] {
        auto spec = spec_for({"echo"}); spec.limits.memory_bytes = 1024;
        expect(cpn::run_process(spec).termination == cpn::ProcessTermination::unsupported_limit, "Unsupported memory limit was silently ignored");
        spec.limits.memory_bytes.reset(); spec.limits.wall_timeout = 0ms;
        expect(cpn::run_process(spec).termination == cpn::ProcessTermination::spawn_failed, "Invalid duration was accepted");
        const auto capabilities = cpn::process_capabilities();
        expect(capabilities.managed_process_group && !capabilities.memory_limit && !capabilities.spawn_deadline, "Capabilities overstate guarantees");
    });
    std::cout << count << " process integration checks passed\n"; return 0;
}
} // namespace
#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    try { if (argc != 2) throw std::runtime_error("Expected process helper path"); return test(std::filesystem::absolute(argv[1])); }
    catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
#else
int main(int argc, char** argv) {
    try { if (argc != 2) throw std::runtime_error("Expected process helper path"); return test(std::filesystem::absolute(argv[1])); }
    catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
#endif
