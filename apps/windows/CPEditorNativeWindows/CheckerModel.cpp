#include "pch.h"
#include "CheckerModel.h"
#include "cpn/digest.hpp"
#include "cpn/json.hpp"
#include <algorithm>
#include <set>
#include <sstream>
#include <stdexcept>

namespace cpn::windows {
namespace {
using Json = nlohmann::json;
void require_fields(Json const& object, std::initializer_list<std::string_view> allowed) {
    if (!object.is_object()) throw std::invalid_argument("Expected checker JSON object.");
    for (auto const& [key, entry] : object.items()) {
        (void)entry;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
            throw std::invalid_argument("Unknown checker configuration field: " + key);
    }
}
Json parse_strict_json(std::string_view text, std::size_t max_bytes) {
    if (text.empty() || text.size() > max_bytes)
        throw std::invalid_argument("Checker configuration exceeds size bounds.");
    std::vector<std::set<std::string>> keys;
    return Json::parse(text, [&](int depth, Json::parse_event_t event, Json& value) {
        if (depth > 64) throw std::invalid_argument("Checker JSON nesting exceeds limit.");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::object_end) keys.pop_back();
        else if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
            throw std::invalid_argument("Duplicate checker JSON field.");
        return true;
    });
}
std::uint64_t positive_int(Json const& value, std::uint64_t maximum) {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 1))
        throw std::invalid_argument("Invalid positive checker integer.");
    auto const number = value.get<std::uint64_t>();
    if (!number || number > maximum) throw std::invalid_argument("Checker integer exceeds bound.");
    return number;
}
std::string trim(std::string_view text) {
    auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    auto last = text.find_last_not_of(" \t\r\n");
    return std::string{text.substr(first, last - first + 1)};
}
}

CheckerConfiguration CheckerConfiguration::parse(std::string_view input) {
    auto const json = parse_strict_json(input, 1024 * 1024);
    require_fields(json, {"schemaVersion", "maxCheckerJsonBytes", "defaultChecker"});
    if (positive_int(json.at("schemaVersion"), 1) != 1)
        throw std::invalid_argument("Unsupported checker settings schema version.");
    CheckerConfiguration result;
    result.max_checker_json_bytes = static_cast<std::size_t>(positive_int(json.at("maxCheckerJsonBytes"), 1024 * 1024));
    result.default_checker = CheckerModel::validate_json(json.at("defaultChecker").dump(), result.max_checker_json_bytes);
    return result;
}

nlohmann::json CheckerModel::validate_json(std::string_view json_text, std::size_t max_bytes) {
    if (json_text.empty() || json_text.size() > max_bytes)
        throw std::invalid_argument("Checker JSON exceeds configured size limit.");
    (void)cpn::parse_external_checker_spec(json_text);
    return parse_strict_json(json_text, max_bytes);
}

CheckerVisualDraft CheckerModel::visual_from_json(std::string_view json_text, std::size_t max_bytes) {
    (void)validate_json(json_text, max_bytes);
    auto const spec = cpn::parse_external_checker_spec(json_text);
    CheckerVisualDraft draft;
    draft.executable = spec.command.executable;
    draft.arguments = spec.command.arguments;
    draft.wall_timeout_ms = static_cast<std::uint64_t>(spec.limits.wall_timeout.count());
    draft.accepted_exit_codes = spec.accepted_exit_codes;
    draft.wrong_answer_exit_codes = spec.wrong_answer_exit_codes;
    draft.inherit_environment = spec.inherit_environment;
    return draft;
}

std::string CheckerModel::apply_visual_to_json(std::string_view base_json_text, CheckerVisualDraft const& draft, std::size_t max_bytes) {
    auto root = validate_json(base_json_text, max_bytes);
    root["command"] = Json{
        {"executable", draft.executable},
        {"arguments", draft.arguments}
    };
    if (!root.contains("limits") || !root.at("limits").is_object()) root["limits"] = Json::object();
    root["limits"]["wall_timeout_ms"] = draft.wall_timeout_ms;
    root["accepted_exit_codes"] = draft.accepted_exit_codes;
    root["wrong_answer_exit_codes"] = draft.wrong_answer_exit_codes;
    root["inherit_environment"] = draft.inherit_environment;
    auto formatted = root.dump(2);
    (void)validate_json(formatted, max_bytes);
    return formatted;
}

std::string CheckerModel::fingerprint(CheckerSettings const& settings, std::size_t max_bytes) {
    if (!settings.enabled) return "disabled";
    try {
        auto parsed = validate_json(settings.json_text, max_bytes);
        return "enabled:" + cpn::sha256(parsed.dump());
    } catch (...) {
        return "invalid:" + cpn::sha256(settings.json_text);
    }
}

std::vector<std::string> CheckerModel::parse_arguments_lines(std::string_view text) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        auto line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty()) result.emplace_back(line);
        if (end == text.size()) break;
        start = end + 1;
    }
    return result;
}

std::string CheckerModel::format_arguments_lines(std::vector<std::string> const& arguments) {
    std::string out;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        if (i) out += '\n';
        out += arguments[i];
    }
    return out;
}

std::vector<int> CheckerModel::parse_exit_codes(std::string_view text) {
    std::vector<int> result;
    std::size_t start = 0;
    while (start <= text.size()) {
        auto comma = text.find(',', start);
        if (comma == std::string_view::npos) comma = text.size();
        auto token = trim(text.substr(start, comma - start));
        if (!token.empty()) {
            std::size_t pos = 0;
            int value = std::stoi(token, &pos);
            if (pos != token.size() || value < 0 || value > 255)
                throw std::invalid_argument("Exit code must be an integer between 0 and 255.");
            if (std::find(result.begin(), result.end(), value) == result.end()) result.push_back(value);
        }
        if (comma == text.size()) break;
        start = comma + 1;
    }
    return result;
}

std::string CheckerModel::format_exit_codes(std::vector<int> const& codes) {
    std::string out;
    for (std::size_t i = 0; i < codes.size(); ++i) {
        if (i) out += ", ";
        out += std::to_string(codes[i]);
    }
    return out;
}
}
