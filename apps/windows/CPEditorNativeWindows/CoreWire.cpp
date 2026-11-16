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
    static constexpr auto encoding = "encoding";
    static constexpr auto data = "data";
    static constexpr auto events = "events";
    static constexpr auto job = "job_id";
    static constexpr auto sequence = "sequence";
    static constexpr auto state = "state";
    static constexpr auto result = "result";
    static constexpr auto build = "build";
    static constexpr auto build_outcome = "build_outcome";
    static constexpr auto error = "error";
    static constexpr auto testcase = "testcase_id";
    static constexpr auto verdict = "verdict";
    static constexpr auto process = "process";
    static constexpr auto stdout_bytes = "stdout";
    static constexpr auto stderr_bytes = "stderr";
    static constexpr auto detail = "detail";
    static constexpr auto elapsed = "elapsed_ms";
    static constexpr auto stdout_truncated = "stdout_truncated";
    static constexpr auto stderr_truncated = "stderr_truncated";
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
RunOutput output(Json const& value) {
    if (integer(value.at(Field::version)) != 1) throw std::invalid_argument("Unsupported Core result schema.");
    RunOutput result;
    result.document_id = value.at(Field::document).get<std::string>();
    result.revision = integer(value.at(Field::revision));
    result.build_outcome = value.at(Field::build_outcome).get<std::string>();
    result.error = value.at(Field::error).get<std::string>();
    if (value.contains(Field::build)) {
        result.build_output = bytes(value.at(Field::build).at(Field::stdout_bytes));
        result.build_error = bytes(value.at(Field::build).at(Field::stderr_bytes));
        result.build_stdout_truncated = value.at(Field::build).at(Field::stdout_truncated).get<bool>();
        result.build_stderr_truncated = value.at(Field::build).at(Field::stderr_truncated).get<bool>();
    }
    for (auto const& test : value.at(Field::tests)) {
        auto const& process = test.at(Field::process);
        result.tests.push_back({test.at(Field::testcase).get<std::string>(),
            test.at(Field::verdict).get<std::string>(), bytes(process.at(Field::stdout_bytes)),
            bytes(process.at(Field::stderr_bytes)), test.at(Field::detail).get<std::string>(),
            integer(process.at(Field::elapsed)), process.at(Field::stdout_truncated).get<bool>(),
            process.at(Field::stderr_truncated).get<bool>()});
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
    return Json{{Field::version, 1}, {Field::source, std::move(source)},
                {Field::toolchain, request.toolchain}, {Field::tests, std::move(tests)},
                {Field::comparison, request.comparison}}.dump();
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
