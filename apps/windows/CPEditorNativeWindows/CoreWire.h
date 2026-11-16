#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cpn::windows {
struct RunCase {
    std::string id;
    std::string input;
    std::optional<std::string> expected;
    bool enabled{true};
};
struct RunRequest {
    std::string token;
    std::string document_id;
    std::uint64_t revision{};
    std::string source;
    std::string original_path;
    nlohmann::json toolchain;
    std::vector<RunCase> tests;
    std::string comparison;
};
struct RunCaseOutput {
    std::string id;
    std::string verdict;
    std::string standard_output;
    std::string standard_error;
    std::string detail;
    std::uint64_t elapsed_ms{};
    bool stdout_truncated{};
    bool stderr_truncated{};
};
struct RunOutput {
    std::string document_id;
    std::uint64_t revision{};
    std::string build_outcome;
    std::string build_output;
    std::string build_error;
    bool build_stdout_truncated{};
    bool build_stderr_truncated{};
    std::string error;
    std::vector<RunCaseOutput> tests;
};
struct RunEvent {
    enum class Kind { ready, job, engine_error };
    Kind kind{Kind::job};
    std::string token;
    std::uint64_t job_id{};
    std::uint64_t sequence{};
    std::string state;
    std::string error;
    std::optional<RunOutput> result;
    bool terminal{};
};
// The versioned Core JSON contract has exactly one Windows serialization boundary.
struct CoreWire {
    [[nodiscard]] static std::string encode(RunRequest const&);
    [[nodiscard]] static std::vector<RunEvent> decode(std::string_view);
};
}
