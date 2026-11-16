// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/batch.hpp"
#include "cpn/text.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>
namespace cpn {
namespace {
using Json = nlohmann::json;
bool valid_text(std::string_view value) {
    return value.find('\0') == std::string_view::npos && !decode_utf8(value).error_offset;
}
void require_text(std::string_view value, const char* kind, bool nonempty = false) {
    if ((nonempty && value.empty()) || !valid_text(value)) throw std::runtime_error(kind);
}
void fields(const Json& object, std::initializer_list<std::string_view> names) {
    if (!object.is_object()) throw std::runtime_error("Expected profile object");
    for (const auto& [key, value] : object.items()) {
        (void)value;
        if (std::find(names.begin(), names.end(), key) == names.end())
            throw std::runtime_error("Unknown profile field: " + key);
    }
}
std::string string_field(const Json& object, const char* key, bool required = false) {
    const auto found = object.find(key);
    if (found == object.end()) {
        if (required) throw std::runtime_error(std::string("Missing profile field: ") + key);
        return {};
    }
    if (!found->is_string()) throw std::runtime_error(std::string("Expected string field: ") + key);
    return found->get<std::string>();
}
std::int64_t integer_field(const Json& object, const char* key, std::int64_t fallback) {
    const auto found = object.find(key);
    if (found == object.end()) return fallback;
    if (!found->is_number_integer() ||
        (found->is_number_unsigned() && found->get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
        throw std::runtime_error(std::string("Expected representable integer field: ") + key);
    return found->get<std::int64_t>();
}
ProcessLimits parse_limits(const Json& object) {
    fields(object, {"spawn_timeout_ms", "wall_timeout_ms", "terminate_grace_ms", "drain_timeout_ms",
                    "stdout_bytes", "stderr_bytes", "combined_bytes"});
    ProcessLimits result;
    result.spawn_timeout = std::chrono::milliseconds(integer_field(object, "spawn_timeout_ms", result.spawn_timeout.count()));
    result.wall_timeout = std::chrono::milliseconds(integer_field(object, "wall_timeout_ms", result.wall_timeout.count()));
    result.terminate_grace = std::chrono::milliseconds(integer_field(object, "terminate_grace_ms", result.terminate_grace.count()));
    result.drain_timeout = std::chrono::milliseconds(integer_field(object, "drain_timeout_ms", result.drain_timeout.count()));
    const auto stdout_bytes = integer_field(object, "stdout_bytes", static_cast<std::int64_t>(result.stdout_bytes));
    const auto stderr_bytes = integer_field(object, "stderr_bytes", static_cast<std::int64_t>(result.stderr_bytes));
    const auto combined_bytes = integer_field(object, "combined_bytes", static_cast<std::int64_t>(result.combined_bytes));
    if (stdout_bytes <= 0 || stderr_bytes <= 0 || combined_bytes <= 0) throw std::runtime_error("Invalid process output limits");
    result.stdout_bytes = static_cast<std::uint64_t>(stdout_bytes);
    result.stderr_bytes = static_cast<std::uint64_t>(stderr_bytes);
    result.combined_bytes = static_cast<std::uint64_t>(combined_bytes);
    return result;
}
CommandTemplate parse_command(const Json& object) {
    fields(object, {"executable", "arguments"});
    CommandTemplate result;
    result.executable = string_field(object, "executable", true);
    const auto& arguments = object.at("arguments");
    if (!arguments.is_array()) throw std::runtime_error("Command arguments must be an array");
    for (const auto& value : arguments) {
        if (!value.is_string()) throw std::runtime_error("Command arguments must be strings");
        result.arguments.push_back(value.get<std::string>());
    }
    return result;
}
std::vector<std::pair<std::string, std::string>> parse_pairs(const Json& object, const char* key) {
    std::vector<std::pair<std::string, std::string>> result;
    const auto found = object.find(key);
    if (found == object.end()) return result;
    if (!found->is_array()) throw std::runtime_error(std::string("Expected pair array: ") + key);
    for (const auto& pair : *found) {
        fields(pair, {"name", "value"});
        result.emplace_back(string_field(pair, "name", true), string_field(pair, "value", true));
    }
    return result;
}
std::string ascii_fold(std::string value) {
    for (auto& byte : value) if (byte >= 'a' && byte <= 'z') byte = static_cast<char>(byte - 'a' + 'A');
    return value;
}
void filename(const std::string& name) {
    require_text(name, "Invalid UTF-8 profile filename", true);
    if (name == "." || name == ".." || name.back() == '.' || name.back() == ' ' ||
        name.find_first_of("<>:\"/\\|?*") != std::string::npos ||
        std::any_of(name.begin(), name.end(), [](unsigned char byte) { return byte < 32; }))
        throw std::runtime_error("Profile filename must be one portable relative filename");
    auto stem = ascii_fold(name.substr(0, name.find('.')));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" || stem == "CONIN$" || stem == "CONOUT$" ||
        ((stem.starts_with("COM") || stem.starts_with("LPT")) &&
         ((stem.size() == 4 && stem[3] >= '1' && stem[3] <= '9') ||
          stem.substr(3) == "¹" || stem.substr(3) == "²" || stem.substr(3) == "³")))
        throw std::runtime_error("Reserved Windows device filename");
}
bool variable_name(std::string_view name) {
    const auto alpha = [](unsigned char byte) { return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || byte == '_'; };
    if (name.empty() || !alpha(static_cast<unsigned char>(name.front()))) return false;
    return std::all_of(name.begin() + 1, name.end(), [&](unsigned char byte) { return alpha(byte) || (byte >= '0' && byte <= '9'); });
}
void template_text(std::string_view text, const std::set<std::string>& variables) {
    require_text(text, "Invalid UTF-8 command text");
    for (std::size_t begin = text.find("${"); begin != std::string_view::npos;) {
        const auto end = text.find('}', begin + 2);
        if (end == std::string_view::npos) throw std::runtime_error("Unclosed command placeholder");
        const auto key = std::string(text.substr(begin + 2, end - begin - 2));
        if (!variables.contains(key)) throw std::runtime_error("Unknown command placeholder: " + key);
        begin = text.find("${", end + 1);
    }
}
}
void validate_toolchain_profile(const ToolchainProfile& profile) {
    require_text(profile.id, "Invalid toolchain profile ID", true);
    require_text(profile.language_id, "Invalid language ID", true);
    filename(profile.source_filename);
    if (profile.build) {
        filename(profile.artifact_name);
        if (ascii_fold(profile.artifact_name) == ascii_fold(profile.source_filename))
            throw std::runtime_error("Artifact must not overwrite source snapshot");
    } else if (!profile.artifact_name.empty()) filename(profile.artifact_name);
    const auto validate_limits = [](const ProcessLimits& limit) {
        using Clock = std::chrono::steady_clock;
        auto latest_start = Clock::time_point::max();
        const auto representable = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::duration::max());
        for (const auto duration : {limit.spawn_timeout, limit.wall_timeout, limit.terminate_grace, limit.drain_timeout}) {
            if (duration.count() < 0 || duration > representable) throw std::runtime_error("Invalid process duration limits");
            const auto delta = std::chrono::duration_cast<Clock::duration>(duration);
            if (latest_start.time_since_epoch() < delta) throw std::runtime_error("Process duration sum overflows");
            latest_start -= delta;
        }
        if (limit.spawn_timeout.count() <= 0 || limit.wall_timeout.count() <= 0 || limit.drain_timeout.count() <= 0 ||
            Clock::now() > latest_start || !limit.stdout_bytes || !limit.stderr_bytes || !limit.combined_bytes)
            throw std::runtime_error("Invalid process limits");
    };
    validate_limits(profile.build_limits); validate_limits(profile.run_limits);
    std::set<std::string> environment_names;
    for (const auto& [name, value] : profile.environment) {
        require_text(name, "Invalid environment name", true); require_text(value, "Invalid environment value");
        if (name.find('=') != std::string::npos || !environment_names.insert(ascii_fold(name)).second)
            throw std::runtime_error("Invalid or duplicate process environment name");
    }
    std::set<std::string> variables{"source", "artifact", "source_dir", "job_dir"};
    for (const auto& [name, value] : profile.variables) {
        if (!variable_name(name) || !variables.insert(name).second) throw std::runtime_error("Invalid, duplicate or reserved profile variable");
        require_text(value, "Invalid profile variable value");
    }
    const auto command_valid = [&](const CommandTemplate& command) {
        require_text(command.executable, "Missing or invalid command executable", true);
        template_text(command.executable, variables);
        for (const auto& argument : command.arguments) template_text(argument, variables);
    };
    command_valid(profile.run); if (profile.build) command_valid(*profile.build);
}
ToolchainProfile parse_toolchain_profile(std::string_view text) {
    if (text.size() > 1024 * 1024) throw std::runtime_error("Profile exceeds byte limit");
    std::vector<std::set<std::string>> object_keys;
    const auto json = Json::parse(text, [&](int depth, Json::parse_event_t event, Json& parsed) {
        if (depth > 64) throw std::runtime_error("Profile exceeds nesting limit");
        if (event == Json::parse_event_t::object_start) object_keys.emplace_back();
        else if (event == Json::parse_event_t::object_end) object_keys.pop_back();
        else if (event == Json::parse_event_t::key && !object_keys.back().insert(parsed.get<std::string>()).second)
            throw std::runtime_error("Duplicate profile field");
        return true;
    });
    fields(json, {"schema_version", "id", "language_id", "source_filename", "artifact_name", "build", "run",
                  "build_limits", "run_limits", "inherit_environment", "environment", "variables"});
    const auto& version = json.at("schema_version");
    if (!version.is_number_integer() || version != 1) throw std::runtime_error("Unsupported toolchain schema version");
    ToolchainProfile result;
    result.id = string_field(json, "id", true); result.language_id = string_field(json, "language_id", true);
    result.source_filename = string_field(json, "source_filename", true); result.artifact_name = string_field(json, "artifact_name");
    result.run = parse_command(json.at("run"));
    if (const auto found = json.find("build"); found != json.end() && !found->is_null()) result.build = parse_command(*found);
    if (const auto found = json.find("build_limits"); found != json.end()) result.build_limits = parse_limits(*found);
    if (const auto found = json.find("run_limits"); found != json.end()) result.run_limits = parse_limits(*found);
    if (const auto found = json.find("inherit_environment"); found != json.end()) {
        if (!found->is_boolean()) throw std::runtime_error("inherit_environment must be a boolean");
        result.inherit_environment = found->get<bool>();
    }
    result.environment = parse_pairs(json, "environment"); result.variables = parse_pairs(json, "variables");
    validate_toolchain_profile(result);
    return result;
}
ToolchainProfile load_toolchain_profile(const std::filesystem::path& path, const FileStore& files) {
    return parse_toolchain_profile(files.read(path, 1024 * 1024));
}
}
