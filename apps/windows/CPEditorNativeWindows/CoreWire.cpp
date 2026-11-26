#include "pch.h"
#include "CoreWire.h"
#include "cpn/json.hpp"
#include <limits>
#include <stdexcept>

namespace cpn::windows {
namespace {
using Json = nlohmann::json;
struct Field {
    static constexpr auto version = "schema_version";
    static constexpr auto source = "source";
    static constexpr auto document = "document_id";
    static constexpr auto revision = "revision";
    static constexpr auto text = "text";
    static constexpr auto path = "original_path";
    static constexpr auto toolchain = "toolchain";
    static constexpr auto tests = "tests";
    static constexpr auto id = "id";
    static constexpr auto input = "input";
    static constexpr auto expected = "expected";
    static constexpr auto enabled = "enabled";
    static constexpr auto comparison = "comparison";
    static constexpr auto mode = "mode";
    static constexpr auto artifact_path = "artifact_path";
    static constexpr auto external_checker = "external_checker";
    static constexpr auto encoding = "encoding";
    static constexpr auto data = "data";
    static constexpr auto events = "events";
    static constexpr auto job = "job_id";
    static constexpr auto sequence = "sequence";
    static constexpr auto state = "state";
    static constexpr auto result = "result";
    static constexpr auto profile_id = "profile_id";
    static constexpr auto source_digest = "source_digest";
    static constexpr auto build = "build";
    static constexpr auto build_outcome = "build_outcome";
    static constexpr auto error = "error";
    static constexpr auto testcase = "testcase_id";
    static constexpr auto verdict = "verdict";
    static constexpr auto process = "process";
    static constexpr auto termination = "termination";
    static constexpr auto exit_code = "exit_code";
    static constexpr auto signal = "signal";
    static constexpr auto stdout_bytes = "stdout";
    static constexpr auto stderr_bytes = "stderr";
    static constexpr auto stdout_total_bytes = "stdout_total_bytes";
    static constexpr auto stderr_total_bytes = "stderr_total_bytes";
    static constexpr auto detail = "detail";
    static constexpr auto elapsed = "elapsed_ms";
    static constexpr auto stdout_truncated = "stdout_truncated";
    static constexpr auto stderr_truncated = "stderr_truncated";
    static constexpr auto checker = "checker";
    static constexpr auto process_attempted = "process_attempted";
};
Json raw(std::string_view value) {
    return {{Field::encoding, "base64"}, {Field::data, cpn::encode_base64(value)}};
}
std::string bytes(Json const& value) {
    if (!value.is_object() || value.at(Field::encoding) != "base64")
        throw std::invalid_argument("Unsupported Core byte encoding.");
    return cpn::decode_base64(value.at(Field::data).get<std::string>());
}
std::uint64_t integer(Json const& value) {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
        throw std::invalid_argument("Core identifier is not an unsigned integer.");
    return value.get<std::uint64_t>();
}
std::optional<int> optional_int(Json const& parent, char const* key) {
    if (!parent.contains(key) || parent.at(key).is_null()) return std::nullopt;
    return parent.at(key).get<int>();
}
ExternalCheckerOutput checker_output(Json const& value) {
    ExternalCheckerOutput result;
    result.verdict = value.at(Field::verdict).get<std::string>();
    result.process_attempted = value.at(Field::process_attempted).get<bool>();
    result.detail = value.at(Field::detail).get<std::string>();
    auto const& process = value.at(Field::process);
    result.termination = process.value(Field::termination, std::string{});
    result.exit_code = optional_int(process, Field::exit_code);
    result.signal = optional_int(process, Field::signal);
    result.elapsed_ms = integer(process.at(Field::elapsed));
    result.standard_output = bytes(process.at(Field::stdout_bytes));
    result.standard_error = bytes(process.at(Field::stderr_bytes));
    result.stdout_total_bytes = process.contains(Field::stdout_total_bytes)
        ? integer(process.at(Field::stdout_total_bytes)) : result.standard_output.size();
    result.stderr_total_bytes = process.contains(Field::stderr_total_bytes)
        ? integer(process.at(Field::stderr_total_bytes)) : result.standard_error.size();
    result.stdout_truncated = process.at(Field::stdout_truncated).get<bool>();
    result.stderr_truncated = process.at(Field::stderr_truncated).get<bool>();
    return result;
}
RunOutput output(Json const& value) {
    if (integer(value.at(Field::version)) != 1) throw std::invalid_argument("Unsupported Core result schema.");
    RunOutput result;
    result.document_id = value.at(Field::document).get<std::string>();
    result.revision = integer(value.at(Field::revision));
    result.profile_id = value.value(Field::profile_id, std::string{});
    result.source_digest = value.value(Field::source_digest, std::string{});
    result.build_outcome = value.at(Field::build_outcome).get<std::string>();
    result.error = value.at(Field::error).get<std::string>();
    if (value.contains(Field::build) && value.at(Field::build).is_object()) {
        auto const& build = value.at(Field::build);
        result.build_output = bytes(build.at(Field::stdout_bytes));
        result.build_error = bytes(build.at(Field::stderr_bytes));
        result.build_stdout_truncated = build.at(Field::stdout_truncated).get<bool>();
        result.build_stderr_truncated = build.at(Field::stderr_truncated).get<bool>();
        if (build.contains(Field::elapsed)) result.build_elapsed_ms = integer(build.at(Field::elapsed));
    }
    for (auto const& test : value.at(Field::tests)) {
        auto const& process = test.at(Field::process);
        RunCaseOutput row;
        row.id = test.at(Field::testcase).get<std::string>();
        row.verdict = test.at(Field::verdict).get<std::string>();
        row.standard_output = bytes(process.at(Field::stdout_bytes));
        row.standard_error = bytes(process.at(Field::stderr_bytes));
        row.detail = test.at(Field::detail).get<std::string>();
        row.elapsed_ms = integer(process.at(Field::elapsed));
        row.stdout_truncated = process.at(Field::stdout_truncated).get<bool>();
        row.stderr_truncated = process.at(Field::stderr_truncated).get<bool>();
        row.termination = process.value(Field::termination, std::string{});
        row.exit_code = optional_int(process, Field::exit_code);
        row.signal = optional_int(process, Field::signal);
        row.stdout_total_bytes = process.contains(Field::stdout_total_bytes)
            ? integer(process.at(Field::stdout_total_bytes)) : row.standard_output.size();
        row.stderr_total_bytes = process.contains(Field::stderr_total_bytes)
            ? integer(process.at(Field::stderr_total_bytes)) : row.standard_error.size();
        if (test.contains(Field::checker) && test.at(Field::checker).is_object())
            row.checker = checker_output(test.at(Field::checker));
        result.tests.push_back(std::move(row));
    }
    return result;
}
}
std::string CoreWire::encode(RunRequest const& request) {
    Json source{{Field::document, request.document_id}, {Field::revision, request.revision},
                {Field::text, request.source}};
    if (!request.original_path.empty()) source[Field::path] = request.original_path;
    Json tests = Json::array();
    for (auto const& test : request.tests) {
        tests.push_back({{Field::id, test.id}, {Field::input, raw(test.input)},
                        {Field::expected, test.expected ? raw(*test.expected) : Json(nullptr)},
                        {Field::enabled, test.enabled}});
    }
    Json encoded{{Field::version, 1}, {Field::source, std::move(source)},
                 {Field::toolchain, request.toolchain}, {Field::tests, std::move(tests)},
                 {Field::comparison, request.comparison}};
    if (!request.mode.empty()) encoded[Field::mode] = request.mode;
    if (request.artifact_path) encoded[Field::artifact_path] = *request.artifact_path;
    if (request.external_checker) encoded[Field::external_checker] = *request.external_checker;
    return encoded.dump();
}
std::vector<RunEvent> CoreWire::decode(std::string_view text) {
    auto const document = Json::parse(text);
    if (integer(document.at(Field::version)) != 1) throw std::invalid_argument("Unsupported Core event schema.");
    std::vector<RunEvent> result;
    for (auto const& value : document.at(Field::events)) {
        RunEvent event;
        event.job_id = integer(value.at(Field::job));
        event.sequence = integer(value.at(Field::sequence));
        event.state = value.at(Field::state).get<std::string>();
        if (value.contains(Field::result)) { event.result = output(value.at(Field::result)); event.terminal = true; }
        result.push_back(std::move(event));
    }
    return result;
}
}
