// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "cpn/checker.hpp"
#include "cpn/files.hpp"
#include "cpn/process.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace cpn {
struct CommandTemplate {
    std::string executable;
    std::vector<std::string> arguments;
};
struct ToolchainProfile {
    std::string id;
    std::string language_id;
    std::string source_filename;
    std::string artifact_name;
    std::optional<CommandTemplate> build;
    CommandTemplate run;
    ProcessLimits build_limits;
    ProcessLimits run_limits;
    bool inherit_environment{true};
    std::vector<std::pair<std::string,std::string>> environment;
    std::vector<std::pair<std::string,std::string>> variables;
};
struct SourceSnapshot {
    std::string document_id;
    std::uint64_t revision{0};
    std::string text;
    std::filesystem::path original_path;
};
struct TestCase {
    std::string id;
    std::string input;
    std::optional<std::string> expected;
    bool enabled{true};
};
struct BatchRequest {
    SourceSnapshot source;
    ToolchainProfile toolchain;
    std::vector<TestCase> tests;
    ComparisonMode comparison{ComparisonMode::cp_editor_ignore_trailing_spaces};
    std::size_t max_retained_output_bytes{64 * 1024 * 1024};
};
enum class Verdict { accepted, wrong_answer, time_limit, runtime_error, output_limit,
                     unchecked, cancelled, not_run, checker_error, infrastructure_error };
enum class BuildOutcome { not_required, succeeded, source_error, failed, cancelled };
struct TestResult {
    std::string testcase_id;
    Verdict verdict{Verdict::not_run};
    ProcessResult process;
    std::string detail;
};
struct BatchResult {
    std::string document_id;
    std::uint64_t revision{0};
    std::string source_digest;
    std::string profile_id;
    BuildOutcome build_outcome{BuildOutcome::not_required};
    std::optional<ProcessResult> build;
    std::vector<TestResult> tests;
    std::string error;
};
enum class BatchPhase { building, running, checking };
using BatchObserver = std::function<void(BatchPhase, const std::string& testcase_id)>;
using ProcessExecutor = std::function<ProcessResult(const ProcessSpec&,std::stop_token)>;

class BatchService {
public:
    explicit BatchService(std::shared_ptr<FileStore> = native_file_store(),
                          ProcessExecutor = run_process);
    [[nodiscard]] BatchResult run(const BatchRequest&, std::stop_token = {}, BatchObserver = {}) const;
private:
    std::shared_ptr<FileStore> files_;
    ProcessExecutor process_;
};
[[nodiscard]] ToolchainProfile load_toolchain_profile(const std::filesystem::path&, const FileStore&);
[[nodiscard]] ToolchainProfile parse_toolchain_profile(std::string_view);
void validate_toolchain_profile(const ToolchainProfile&);
[[nodiscard]] const char* verdict_name(Verdict) noexcept;
[[nodiscard]] const char* build_outcome_name(BuildOutcome) noexcept;
}
