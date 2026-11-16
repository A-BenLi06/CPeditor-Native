// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "cpn/command.hpp"
#include "cpn/files.hpp"
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace cpn {
struct ExternalCheckerSpec {
    CommandTemplate command;
    ProcessLimits limits;
    std::vector<int> accepted_exit_codes{0};
    std::vector<int> wrong_answer_exit_codes{1, 2};
    bool inherit_environment{true};
    std::vector<std::pair<std::string, std::string>> environment;
};
struct CheckerInput {
    std::string input;
    std::string actual;
    std::string expected;
    std::optional<std::filesystem::path> source_dir;
};
struct ExternalCheckerBounds {
    std::size_t max_input_bytes{16 * 1024 * 1024};
    std::size_t max_actual_bytes{8 * 1024 * 1024};
    std::size_t max_expected_bytes{16 * 1024 * 1024};
    std::size_t max_total_input_bytes{32 * 1024 * 1024};
    std::size_t max_arguments{128};
    std::size_t max_command_bytes{64 * 1024};
    std::size_t max_path_bytes{32768};
};
enum class ExternalCheckerVerdict { accepted, wrong_answer, cancelled, checker_error };
struct ExternalCheckerResult {
    ExternalCheckerVerdict verdict{ExternalCheckerVerdict::checker_error};
    ProcessResult process;
    std::string detail;
    bool process_attempted{false};
};
void validate_external_checker(const ExternalCheckerSpec&, ExternalCheckerBounds = {});
// Blocking on the caller's worker thread. Each invocation owns and cleans one independent workspace.
// Templates expand input/actual/expected/job_dir/source_dir once within individual argv elements.
// An absent source_dir expands to that invocation's workspace; stdin remains empty.
// Accepted/rejected exit codes define the caller's protocol. Other exits and all host failures are
// CheckerError; the default testlib mapping never treats score exit 7 as a binary verdict.
class ExternalCheckerService final {
public:
    explicit ExternalCheckerService(std::shared_ptr<FileStore> = native_file_store(),
                                    ProcessExecutor = run_process, ExternalCheckerBounds = {});
    [[nodiscard]] ExternalCheckerResult run(const ExternalCheckerSpec&, const CheckerInput&,
                                           std::stop_token = {}) const;
private:
    std::shared_ptr<FileStore> files_;
    ProcessExecutor process_;
    ExternalCheckerBounds bounds_;
};
[[nodiscard]] const char* external_checker_verdict_name(ExternalCheckerVerdict) noexcept;
} // namespace cpn
