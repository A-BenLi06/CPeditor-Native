// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cpn {

struct CompanionBounds {
    std::size_t max_json_bytes{16 * 1024 * 1024};
    std::size_t max_json_depth{32}; // Root container counts as one level.
    std::size_t max_json_nodes{262144}; // Containers, primitive values and object keys.
    std::size_t max_string_bytes{4 * 1024 * 1024}; // Every decoded key/value, including unknown metadata.
    std::size_t max_total_string_bytes{16 * 1024 * 1024};
    std::size_t max_name_bytes{64 * 1024}; // Applies separately to name and group.
    std::size_t max_url_bytes{32768};
    std::size_t max_tests{4096};
    std::size_t max_test_bytes{4 * 1024 * 1024}; // Each input/output field.
    std::size_t max_total_test_bytes{16 * 1024 * 1024};
    std::size_t max_batch_id_bytes{1024};
    std::uint64_t max_batch_size{4096};
};

enum class CompanionParseErrorCode { invalid_json, invalid_shape, invalid_field, exceeded_bounds };
class CompanionParseError final : public std::invalid_argument {
public:
    CompanionParseError(CompanionParseErrorCode code, std::string detail)
        : std::invalid_argument(std::move(detail)), code(code) {}
    const CompanionParseErrorCode code;
};

struct CompanionSample {
    std::string input;
    std::string output; // Always present. An empty answer is not a missing answer.
};
struct CompanionSourceLimits {
    std::uint64_t time_limit_ms{0};
    std::uint64_t memory_limit_mb{0};
    // These are source metadata, not ProcessLimits. Zero means unspecified.
};
struct CompanionInput {
    std::string type;
    std::optional<std::string> file_name;
    std::optional<std::string> pattern;
};
struct CompanionOutput {
    std::string type;
    std::optional<std::string> file_name;
};
struct CompanionJava {
    std::string main_class;
    std::string task_class;
};
struct CompanionBatch {
    std::string id; // Bounded opaque UTF-8 identity; never a filesystem name.
    std::uint64_t size{1};
};

enum class CompanionStdioIssueCode { interactive, test_type, input_mode, output_mode };
struct CompanionStdioIssue {
    CompanionStdioIssueCode code;
    std::string value; // Source value for a resource-backed native explanation.
};
struct CompanionCandidate {
    std::string name;
    std::string group;
    std::string url;
    CompanionSourceLimits source_limits;
    std::vector<CompanionSample> tests;
    std::optional<bool> interactive;
    std::optional<std::string> test_type;
    std::optional<CompanionInput> input;
    std::optional<CompanionOutput> output;
    std::optional<CompanionJava> java;
    std::optional<CompanionBatch> batch;
    // The entire validated external payload, byte-for-byte. Unknown fields, numeric
    // lexemes, whitespace and Unicode are not lost through DOM reserialization.
    std::string original_json;
    std::vector<CompanionStdioIssue> ordinary_stdio_issues;
    [[nodiscard]] bool compatible_with_ordinary_stdio() const noexcept { return ordinary_stdio_issues.empty(); }
};

// Pure decoder: no filesystem/network/process operations and no stable-ID allocation.
// Unknown external fields are retained; present known fields have strict types.
// Missing modern optional fields use the legacy stdio/single compatibility semantics.
// single/multiNumber samples are kept as complete input; no case splitting occurs.
[[nodiscard]] CompanionCandidate parse_companion_payload(std::string_view, CompanionBounds = {});
[[nodiscard]] const char* companion_stdio_issue_name(CompanionStdioIssueCode) noexcept;

} // namespace cpn
