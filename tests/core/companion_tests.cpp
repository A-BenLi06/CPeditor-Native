// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/companion.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Json = nlohmann::json;
void expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejected(F operation, cpn::CompanionParseErrorCode code, const char* message) {
    try { operation(); }
    catch (const cpn::CompanionParseError& ex) { expect(ex.code == code, message); return; }
    throw std::runtime_error(message);
}
std::string read_fixture(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Could not read Companion fixture");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
Json baseline() {
    return {{"name", "A. Sum"}, {"group", ""}, {"url", "https://example.org/sum"},
        {"timeLimit", 1500}, {"memoryLimit", 256}, {"tests", Json::array({{{"input", "7 8\n"}, {"output", "15\n"}}})}};
}
void invalid(const Json& value, cpn::CompanionParseErrorCode code, const char* message) {
    rejected([&] { (void)cpn::parse_companion_payload(value.dump()); }, code, message);
}
void fixtures(const std::filesystem::path& folder) {
    const auto basic_text = read_fixture(folder / "basic.json");
    const auto basic = cpn::parse_companion_payload(basic_text);
    expect(basic.name == "A. Sum" && basic.group == "Native integration fixtures", "basic source strings");
    expect(basic.source_limits.time_limit_ms == 1500 && basic.source_limits.memory_limit_mb == 256, "source limit units remain explicit");
    expect(basic.tests.size() == 1 && basic.tests[0].input == "7 8\n" && basic.tests[0].output == "15\n", "sample bytes preserved");
    expect(!basic.batch && !basic.test_type && !basic.interactive && !basic.input && !basic.output && !basic.java, "legacy missing metadata remains absent");
    expect(basic.compatible_with_ordinary_stdio() && basic.original_json == basic_text, "legacy stdio compatibility and exact raw payload");
    const auto full_text = read_fixture(folder / "full.json");
    const auto full = cpn::parse_companion_payload(full_text);
    const auto raw = std::string("中文 😀 e\xcc\x81\r\n") + std::string("\0tail", 5);
    expect(full.tests.size() == 2 && full.tests[0].input == raw && full.tests[0].output == raw, "CJK emoji NFD CRLF NUL sample bytes");
    expect(full.tests[1].input.empty() && full.tests[1].output.empty(), "explicit empty input and answer stay present");
    expect(full.compatible_with_ordinary_stdio() && full.test_type == "multiNumber", "multiNumber complete sample is never split");
    expect(full.input->type == "stdin" && full.output->type == "stdout" && full.java->main_class == "Main", "modern known metadata");
    expect(full.batch->size == 2 && full.batch->id == "f7fe2c29-078e-4d21-9c32-2b12ec576476", "batch identity not regenerated");
    expect(full.original_json == full_text && full.original_json.find("18446744073709551616") != std::string::npos &&
        full.original_json.find("1.2300e+03") != std::string::npos, "unknown nested metadata and large numeric lexemes retained exactly");
    const auto unsupported_text = read_fixture(folder / "unsupported.json");
    const auto unsupported = cpn::parse_companion_payload(unsupported_text);
    expect(unsupported.tests.empty() && unsupported.source_limits.time_limit_ms == 0 && unsupported.source_limits.memory_limit_mb == 0, "empty sample collection and unspecified limits");
    expect(!unsupported.compatible_with_ordinary_stdio() && unsupported.ordinary_stdio_issues.size() == 4, "every incompatible mode has a separate structured issue");
    expect(unsupported.input->pattern == "../../secret.*" && unsupported.output->file_name == "../result.txt", "untrusted filename and regex are preserved only as metadata");
    const std::vector<cpn::CompanionStdioIssueCode> codes{cpn::CompanionStdioIssueCode::interactive, cpn::CompanionStdioIssueCode::test_type,
        cpn::CompanionStdioIssueCode::input_mode, cpn::CompanionStdioIssueCode::output_mode};
    const std::vector<std::string> names{"Interactive", "TestType", "InputMode", "OutputMode"};
    for (std::size_t i = 0; i < codes.size(); ++i) {
        expect(unsupported.ordinary_stdio_issues[i].code == codes[i], "unsupported mode order and code");
        expect(cpn::companion_stdio_issue_name(codes[i]) == names[i], "unsupported mode name for native mapping");
    }
    expect(unsupported.original_json == unsupported_text, "unsupported payload retained without executing metadata");
    const auto builder = cpn::parse_companion_payload(read_fixture(folder / "builder-defaults.json"));
    expect(builder.name.empty() && builder.group.empty() && builder.java->task_class.empty() && builder.java->main_class == "Main",
        "fixed TaskBuilder defaults remain valid metadata including empty taskClass");
    expect(builder.tests.empty() && builder.compatible_with_ordinary_stdio(), "empty builder sample collection does not promise a runnable job");
}
void strict_fields() {
    using Code = cpn::CompanionParseErrorCode;
    for (const auto* key : {"name", "group", "url", "timeLimit", "memoryLimit", "tests"}) {
        auto value = baseline(); value.erase(key); invalid(value, Code::invalid_field, "required core Companion fields");
    }
    for (const auto& root : std::vector<Json>{nullptr, true, "string", Json::array()}) invalid(root, Code::invalid_shape, "root object required");
    for (const auto* key : {"name", "group", "url"}) for (const auto& bad : std::vector<Json>{nullptr, 1, false, Json::object(), Json::array()}) {
        auto value = baseline(); value[key] = bad; invalid(value, Code::invalid_field, "known source string fields strictly typed");
    }
    for (const auto* key : {"timeLimit", "memoryLimit"}) for (const auto& bad : std::vector<Json>{nullptr, -1, true, 1.0, "1500", 18446744073709551616.0}) {
        auto value = baseline(); value[key] = bad; invalid(value, Code::invalid_field, "source limits require nonnegative uint64");
    }
    auto value = baseline(); value["timeLimit"] = std::numeric_limits<std::uint64_t>::max(); value["memoryLimit"] = value["timeLimit"];
    auto parsed = cpn::parse_companion_payload(value.dump());
    expect(parsed.source_limits.time_limit_ms == std::numeric_limits<std::uint64_t>::max() &&
        parsed.source_limits.memory_limit_mb == parsed.source_limits.time_limit_ms, "source metadata is not converted to timeout or memory bytes");
    value = baseline(); value["url"] = ""; invalid(value, Code::invalid_field, "empty URL identity rejected");
    value = baseline(); value["url"] = std::string("a\0b", 3); invalid(value, Code::invalid_field, "NUL URL identity rejected");
    value = baseline(); value["tests"] = nullptr; invalid(value, Code::invalid_shape, "tests array required");
    value = baseline(); value["tests"][0] = "sample"; invalid(value, Code::invalid_shape, "sample object required");
    for (const auto* key : {"input", "output"}) {
        value = baseline(); value["tests"][0].erase(key); invalid(value, Code::invalid_field, "both sample fields required");
        for (const auto& bad : std::vector<Json>{nullptr, 1, true, Json::array(), Json::object()}) {
            value = baseline(); value["tests"][0][key] = bad; invalid(value, Code::invalid_field, "sample field never silently becomes empty or absent");
        }
    }
    for (const auto& bad : std::vector<Json>{nullptr, 1, "false", Json::object()}) {
        value = baseline(); value["interactive"] = bad; invalid(value, Code::invalid_field, "optional interactive strictly boolean when present");
    }
    for (const auto& bad : std::vector<Json>{nullptr, 1, true}) {
        value = baseline(); value["testType"] = bad; invalid(value, Code::invalid_field, "optional testType strictly string when present");
    }
    for (const auto* key : {"input", "output", "languages", "batch"}) {
        value = baseline(); value[key] = nullptr; invalid(value, Code::invalid_shape, "modern metadata must be object when present");
    }
    for (const auto* key : {"input", "output"}) {
        value = baseline(); value[key] = Json::object(); invalid(value, Code::invalid_field, "mode type is required");
        value = baseline(); value[key] = {{"type", 1}}; invalid(value, Code::invalid_field, "mode type strictly string");
        value = baseline(); value[key] = {{"type", ""}}; invalid(value, Code::invalid_field, "empty mode type rejected");
        value = baseline(); value[key] = {{"type", "file"}}; invalid(value, Code::invalid_field, "file mode requires filename");
        value = baseline(); value[key] = {{"type", "file"}, {"fileName", ""}}; invalid(value, Code::invalid_field, "empty filename rejected for file mode");
        value = baseline(); value[key] = {{"type", "file"}, {"fileName", "../../outside"}};
        expect(!cpn::parse_companion_payload(value.dump()).compatible_with_ordinary_stdio(), "file mode is stored as unsupported, not opened");
        value = baseline(); value[key] = {{"type", "futureMode"}, {"futureMetadata", 1}};
        expect(!cpn::parse_companion_payload(value.dump()).compatible_with_ordinary_stdio(), "unknown mode is preserved but cannot run as stdio");
    }
    value = baseline(); value["input"] = {{"type", "regex"}}; invalid(value, Code::invalid_field, "regex requires pattern");
    value = baseline(); value["input"] = {{"type", "regex"}, {"pattern", ""}}; invalid(value, Code::invalid_field, "empty regex rejected");
    value = baseline(); value["input"] = {{"type", "stdin"}, {"fileName", 1}}; invalid(value, Code::invalid_field, "known unused fields still have strict types");
    value = baseline(); value["input"] = {{"type", "stdin"}, {"pattern", false}}; invalid(value, Code::invalid_field, "known unused pattern strictly typed");
    value = baseline(); value["languages"] = {{"java", Json::array()}}; invalid(value, Code::invalid_shape, "Java metadata object required");
    for (const auto* key : {"mainClass", "taskClass"}) {
        value = baseline(); value["languages"] = {{"java", {{"mainClass", "Main"}, {"taskClass", "Task"}}}};
        value["languages"]["java"].erase(key); invalid(value, Code::invalid_field, "complete Java metadata required when present");
        value["languages"]["java"][key] = 1; invalid(value, Code::invalid_field, "Java fields strictly typed");
        value["languages"]["java"][key] = "";
        expect(cpn::parse_companion_payload(value.dump()).java.has_value(), "empty Java metadata string is retained rather than guessed");
    }
    value = baseline(); value["testType"] = "multiEOF";
    expect(!cpn::parse_companion_payload(value.dump()).compatible_with_ordinary_stdio(), "multiEOF is not in the fixed protocol enum");
    value = baseline(); value["batch"] = {{"id", "opaque"}}; invalid(value, Code::invalid_field, "batch size required");
    value = baseline(); value["batch"] = {{"size", 1}}; invalid(value, Code::invalid_field, "batch identity required");
    for (const auto& bad : std::vector<Json>{0, -1, 1.0, true, "1", nullptr}) {
        value = baseline(); value["batch"] = {{"id", "opaque"}, {"size", bad}}; invalid(value, Code::invalid_field, "batch size positive integer");
    }
    value = baseline(); value["batch"] = {{"id", ""}, {"size", 1}}; invalid(value, Code::invalid_field, "batch ID not empty");
    value = baseline(); value["batch"] = {{"id", 1}, {"size", 1}}; invalid(value, Code::invalid_field, "batch ID strictly typed");
    value = baseline(); value["batch"] = {{"id", std::string("x\0y", 3)}, {"size", 1}}; invalid(value, Code::invalid_field, "NUL batch ID rejected");
}
void syntax_and_identity() {
    using Code = cpn::CompanionParseErrorCode;
    const auto tail = baseline().dump();
    for (const auto& bad : std::vector<std::string>{"", "{", tail + " garbage", "[1,]", "{\"name\":\"\\uD800\"}"})
        rejected([&] { (void)cpn::parse_companion_payload(bad); }, Code::invalid_json, "malformed JSON and invalid surrogate rejected");
    for (const auto& suffix : std::vector<std::string>{",\"name\":\"duplicate\"}", ",\"future\":{\"nested\":1,\"nested\":2}}",
        ",\"future\":[{\"nested\":1,\"nested\":2}]}", ",\"future\":{\"a\":1,\"\\u0061\":2}}"}) {
        const auto bad = tail.substr(0, tail.size() - 1) + suffix;
        rejected([&] { (void)cpn::parse_companion_payload(bad); }, Code::invalid_json, "recursive duplicate keys including escaped spellings rejected");
    }
    auto raw_bad = tail; raw_bad.insert(raw_bad.find("A. Sum"), 1, static_cast<char>(0xff));
    rejected([&] { (void)cpn::parse_companion_payload(raw_bad); }, Code::invalid_json, "invalid UTF8 never replacement-decoded");
    auto nfd = baseline(); nfd["name"] = "e\xcc\x81"; nfd["tests"][0] = {{"input", "e\xcc\x81"}, {"output", "e\xcc\x81"}};
    auto nfc = nfd; nfc["name"] = "\xc3\xa9"; nfc["tests"][0] = {{"input", "\xc3\xa9"}, {"output", "\xc3\xa9"}};
    const auto first = cpn::parse_companion_payload(nfd.dump()), second = cpn::parse_companion_payload(nfc.dump());
    expect(first.name != second.name && first.tests[0].input != second.tests[0].input, "canonical equivalent Unicode is not merged or normalized");
    const auto with_bom = std::string("\xef\xbb\xbf") + tail;
    expect(cpn::parse_companion_payload(with_bom).original_json == with_bom, "accepted UTF8 BOM is preserved in original payload");
}
void bounds() {
    using Code = cpn::CompanionParseErrorCode;
    const auto text = baseline().dump();
    const auto limited = [&](cpn::CompanionBounds bounds) {
        rejected([&] { (void)cpn::parse_companion_payload(text, bounds); }, Code::exceeded_bounds, "configured Companion bounds");
    };
    cpn::CompanionBounds bound; bound.max_json_bytes = text.size();
    expect(cpn::parse_companion_payload(text, bound).original_json == text, "exact raw payload byte bound accepted");
    --bound.max_json_bytes; limited(bound);
    bound = {}; bound.max_json_depth = 2; limited(bound);
    bound = {}; bound.max_json_depth = 3;
    expect(cpn::parse_companion_payload(text, bound).tests.size() == 1, "exact nesting bound accepted");
    bound = {}; bound.max_json_nodes = 1; limited(bound);
    bound = {}; bound.max_string_bytes = 2; limited(bound);
    bound = {}; bound.max_total_string_bytes = 2; limited(bound);
    bound = {}; bound.max_name_bytes = 2; limited(bound);
    bound = {}; bound.max_url_bytes = 2; limited(bound);
    bound = {}; bound.max_tests = 0; limited(bound);
    bound = {}; bound.max_test_bytes = 3; limited(bound);
    bound = {}; bound.max_total_test_bytes = 6; limited(bound);
    bound = {}; bound.max_test_bytes = 4; bound.max_total_test_bytes = 7;
    expect(cpn::parse_companion_payload(text, bound).tests.size() == 1, "exact per-field and aggregate test budgets accepted");
    auto value = baseline(); value["batch"] = {{"id", "opaque"}, {"size", 2}};
    bound = {}; bound.max_batch_id_bytes = 5;
    rejected([&] { (void)cpn::parse_companion_payload(value.dump(), bound); }, Code::exceeded_bounds, "batch ID byte bound");
    bound = {}; bound.max_batch_size = 1;
    rejected([&] { (void)cpn::parse_companion_payload(value.dump(), bound); }, Code::exceeded_bounds, "batch advertised size bound");
    value = baseline(); value["future"] = { {"unknown", std::string(64, 'x')} };
    bound = {}; bound.max_string_bytes = 63;
    rejected([&] { (void)cpn::parse_companion_payload(value.dump(), bound); }, Code::exceeded_bounds, "unknown metadata strings obey limit");
    value = baseline(); value["future"] = {{std::string(64, 'x'), 1}};
    rejected([&] { (void)cpn::parse_companion_payload(value.dump(), bound); }, Code::exceeded_bounds, "unknown metadata keys obey limit");
    value = baseline(); value["future"] = Json::array({1, 2, 3, 4, 5, 6});
    bound = {}; bound.max_json_nodes = 22;
    rejected([&] { (void)cpn::parse_companion_payload(value.dump(), bound); }, Code::exceeded_bounds, "unknown small-value arrays bounded by node count");
    value = baseline(); value["tests"] = Json::array();
    bound = {}; bound.max_tests = 0; bound.max_test_bytes = 0; bound.max_total_test_bytes = 0;
    expect(cpn::parse_companion_payload(value.dump(), bound).tests.empty(), "zero test budgets allow genuinely empty sample array");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Pass the Companion fixture directory");
        fixtures(std::filesystem::path(argv[1])); strict_fields(); syntax_and_identity(); bounds();
        std::cout << "Companion decoder tests passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
