// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace cpn {

struct ProcessLimits {
    std::chrono::milliseconds spawn_timeout{5000};
    std::chrono::milliseconds wall_timeout{2000};
    std::chrono::milliseconds terminate_grace{100};
    std::chrono::milliseconds drain_timeout{500};
    std::uint64_t stdout_bytes{4 * 1024 * 1024};
    std::uint64_t stderr_bytes{4 * 1024 * 1024};
    std::uint64_t combined_bytes{8 * 1024 * 1024};
    std::optional<std::uint64_t> memory_bytes;
};

struct ProcessSpec {
    std::filesystem::path executable;
    std::vector<std::string> arguments; // UTF-8 arguments; never a shell command.
    std::filesystem::path working_directory;
    std::string input; // Raw stdin bytes, including NUL.
    bool inherit_environment{true};
    std::vector<std::pair<std::string, std::string>> environment;
    ProcessLimits limits;
};

enum class ProcessTermination {
    exited,
    timed_out,
    output_limit,
    cancelled,
    spawn_failed,
    io_failed,
    drain_timeout,
    unsupported_limit
};

struct ProcessResult {
    ProcessTermination termination{ProcessTermination::spawn_failed};
    std::optional<int> exit_code;
    std::optional<int> signal;
    std::chrono::milliseconds elapsed{0};
    std::string standard_output;
    std::string standard_error;
    std::uint64_t stdout_total_bytes{0};
    std::uint64_t stderr_total_bytes{0};
    bool stdout_truncated{false};
    bool stderr_truncated{false};
    int native_error{0};
    std::string detail;
};

struct ProcessCapabilities {
    bool managed_process_group{false};
    bool memory_limit{false};
    bool spawn_deadline{false};
};

// Blocking on the caller's worker thread. All three pipes are serviced together.
// No callback or native UI type crosses this interface.
[[nodiscard]] ProcessResult run_process(const ProcessSpec&, std::stop_token = {});
[[nodiscard]] ProcessCapabilities process_capabilities() noexcept;

} // namespace cpn
