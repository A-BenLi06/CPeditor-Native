// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/companion.hpp"

#include <nlohmann/json.hpp>
#include <set>
#include <utility>

namespace cpn {
namespace {
using Json = nlohmann::json;

[[noreturn]] void fail(CompanionParseErrorCode code, std::string detail) {
    throw CompanionParseError(code, std::move(detail));
}
void bounded(std::size_t size, std::size_t maximum, std::string_view field) {
    if (size > maximum) fail(CompanionParseErrorCode::exceeded_bounds, std::string(field) + " exceeds byte limit");
}
void consume(std::size_t size, std::size_t maximum, std::size_t& total, std::string_view field) {
    if (size > maximum - total) fail(CompanionParseErrorCode::exceeded_bounds, std::string(field) + " exceeds total byte limit");
    total += size;
}
Json parse(std::string_view text, const CompanionBounds& bounds) {
    bounded(text.size(), bounds.max_json_bytes, "Companion JSON");
    std::vector<std::optional<std::set<std::string>>> containers;
    std::size_t nodes = 0, string_bytes = 0;
    try {
        return Json::parse(text, [&](int, Json::parse_event_t event, Json& value) {
            const bool starts = event == Json::parse_event_t::object_start || event == Json::parse_event_t::array_start;
            const bool ends = event == Json::parse_event_t::object_end || event == Json::parse_event_t::array_end;
            if (starts || event == Json::parse_event_t::key || event == Json::parse_event_t::value) {
                if (nodes == bounds.max_json_nodes) fail(CompanionParseErrorCode::exceeded_bounds, "Companion JSON exceeds node limit");
                ++nodes;
            }
            if (starts) {
                if (containers.size() >= bounds.max_json_depth) fail(CompanionParseErrorCode::exceeded_bounds, "Companion JSON exceeds nesting limit");
                if (event == Json::parse_event_t::object_start) containers.emplace_back(std::in_place);
                else containers.emplace_back(std::nullopt);
            } else if (ends) {
                containers.pop_back();
            }
            if (event == Json::parse_event_t::key || (event == Json::parse_event_t::value && value.is_string())) {
                const auto& decoded = value.get_ref<const std::string&>();
                bounded(decoded.size(), bounds.max_string_bytes, "Companion string");
                consume(decoded.size(), bounds.max_total_string_bytes, string_bytes, "Companion strings");
                if (event == Json::parse_event_t::key && !containers.back()->insert(decoded).second)
                    fail(CompanionParseErrorCode::invalid_json, "Duplicate Companion JSON field");
            }
            return true;
        });
    } catch (const Json::exception& ex) {
        fail(CompanionParseErrorCode::invalid_json, std::string("Invalid Companion JSON: ") + ex.what());
    }
}
const Json& object(const Json& value, std::string_view field) {
    if (!value.is_object()) fail(CompanionParseErrorCode::invalid_shape, std::string(field) + " must be an object");
    return value;
}
const Json& required(const Json& value, const char* field) {
    const auto found = value.find(field);
    if (found == value.end()) fail(CompanionParseErrorCode::invalid_field, std::string("Missing Companion field: ") + field);
    return *found;
}
std::string string(const Json& value, std::string_view field) {
    if (!value.is_string()) fail(CompanionParseErrorCode::invalid_field, std::string(field) + " must be a string");
    return value.get<std::string>();
}
std::string required_string(const Json& value, const char* field) { return string(required(value, field), field); }
std::optional<std::string> optional_string(const Json& value, const char* field) {
    const auto found = value.find(field);
    return found == value.end() ? std::nullopt : std::optional{string(*found, field)};
}
void nonempty(const std::string& value, std::string_view field) {
    if (value.empty()) fail(CompanionParseErrorCode::invalid_field, std::string(field) + " must not be empty");
}
std::uint64_t integer(const Json& value, std::string_view field) {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
        fail(CompanionParseErrorCode::invalid_field, std::string(field) + " must be a nonnegative 64-bit integer");
    return value.get<std::uint64_t>();
}
void identity(const std::string& value, std::size_t maximum, std::string_view field) {
    nonempty(value, field);
    bounded(value.size(), maximum, field);
    if (value.find('\0') != std::string::npos) fail(CompanionParseErrorCode::invalid_field, std::string(field) + " must not contain NUL");
}
void issue(CompanionCandidate& candidate, CompanionStdioIssueCode code, std::string value) {
    candidate.ordinary_stdio_issues.push_back({code, std::move(value)});
}
} // namespace

CompanionCandidate parse_companion_payload(std::string_view text, CompanionBounds bounds) {
    const auto parsed = parse(text, bounds);
    const auto& root = object(parsed, "Companion payload");
    CompanionCandidate result;
    result.name = required_string(root, "name");
    result.group = required_string(root, "group");
    bounded(result.name.size(), bounds.max_name_bytes, "name");
    bounded(result.group.size(), bounds.max_name_bytes, "group");
    result.url = required_string(root, "url");
    identity(result.url, bounds.max_url_bytes, "url");
    result.source_limits.time_limit_ms = integer(required(root, "timeLimit"), "timeLimit");
    result.source_limits.memory_limit_mb = integer(required(root, "memoryLimit"), "memoryLimit");
    const auto& tests = required(root, "tests");
    if (!tests.is_array()) fail(CompanionParseErrorCode::invalid_shape, "tests must be an array");
    if (tests.size() > bounds.max_tests) fail(CompanionParseErrorCode::exceeded_bounds, "Companion tests exceed count limit");
    std::size_t test_bytes = 0;
    result.tests.reserve(tests.size());
    for (const auto& value : tests) {
        const auto& test = object(value, "test");
        CompanionSample sample{required_string(test, "input"), required_string(test, "output")};
        bounded(sample.input.size(), bounds.max_test_bytes, "test.input");
        bounded(sample.output.size(), bounds.max_test_bytes, "test.output");
        consume(sample.input.size(), bounds.max_total_test_bytes, test_bytes, "Companion tests");
        consume(sample.output.size(), bounds.max_total_test_bytes, test_bytes, "Companion tests");
        result.tests.push_back(std::move(sample));
    }
    if (const auto found = root.find("interactive"); found != root.end()) {
        if (!found->is_boolean()) fail(CompanionParseErrorCode::invalid_field, "interactive must be a boolean");
        result.interactive = found->get<bool>();
        if (*result.interactive) issue(result, CompanionStdioIssueCode::interactive, "true");
    }
    result.test_type = optional_string(root, "testType");
    if (result.test_type && *result.test_type != "single" && *result.test_type != "multiNumber")
        issue(result, CompanionStdioIssueCode::test_type, *result.test_type);
    if (const auto found = root.find("input"); found != root.end()) {
        const auto& input = object(*found, "input");
        result.input = CompanionInput{required_string(input, "type"), optional_string(input, "fileName"), optional_string(input, "pattern")};
        nonempty(result.input->type, "input.type");
        if (result.input->type == "file") {
            if (!result.input->file_name) fail(CompanionParseErrorCode::invalid_field, "Missing Companion field: input.fileName");
            nonempty(*result.input->file_name, "input.fileName");
        } else if (result.input->type == "regex") {
            if (!result.input->pattern) fail(CompanionParseErrorCode::invalid_field, "Missing Companion field: input.pattern");
            nonempty(*result.input->pattern, "input.pattern");
        }
        if (result.input->type != "stdin") issue(result, CompanionStdioIssueCode::input_mode, result.input->type);
    }
    if (const auto found = root.find("output"); found != root.end()) {
        const auto& output = object(*found, "output");
        result.output = CompanionOutput{required_string(output, "type"), optional_string(output, "fileName")};
        nonempty(result.output->type, "output.type");
        if (result.output->type == "file") {
            if (!result.output->file_name) fail(CompanionParseErrorCode::invalid_field, "Missing Companion field: output.fileName");
            nonempty(*result.output->file_name, "output.fileName");
        }
        if (result.output->type != "stdout") issue(result, CompanionStdioIssueCode::output_mode, result.output->type);
    }
    if (const auto found = root.find("languages"); found != root.end()) {
        const auto& languages = object(*found, "languages");
        if (const auto java = languages.find("java"); java != languages.end()) {
            const auto& language = object(*java, "languages.java");
            result.java = CompanionJava{required_string(language, "mainClass"), required_string(language, "taskClass")};
        }
    }
    if (const auto found = root.find("batch"); found != root.end()) {
        const auto& batch = object(*found, "batch");
        result.batch = CompanionBatch{required_string(batch, "id"), integer(required(batch, "size"), "batch.size")};
        identity(result.batch->id, bounds.max_batch_id_bytes, "batch.id");
        if (result.batch->size == 0) fail(CompanionParseErrorCode::invalid_field, "batch.size must be positive");
        if (result.batch->size > bounds.max_batch_size) fail(CompanionParseErrorCode::exceeded_bounds, "batch.size exceeds count limit");
    }
    result.original_json.assign(text);
    return result;
}

const char* companion_stdio_issue_name(CompanionStdioIssueCode code) noexcept {
    switch (code) {
        case CompanionStdioIssueCode::interactive: return "Interactive";
        case CompanionStdioIssueCode::test_type: return "TestType";
        case CompanionStdioIssueCode::input_mode: return "InputMode";
        case CompanionStdioIssueCode::output_mode: return "OutputMode";
    }
    return "Unknown";
}
} // namespace cpn
