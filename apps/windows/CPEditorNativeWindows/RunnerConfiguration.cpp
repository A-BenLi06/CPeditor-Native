#include "pch.h"
#include "RunnerConfiguration.h"
#include "cpn/api.h"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace cpn::windows {
namespace {
using Json = nlohmann::json;
std::uint64_t bound(Json const& value, std::uint64_t maximum) {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 1))
        throw std::invalid_argument("Invalid native runner limit.");
    const auto number = value.get<std::uint64_t>();
    if (!number || number > maximum) throw std::invalid_argument("Native runner limit exceeds its bound.");
    return number;
}
}
RunnerConfiguration RunnerConfiguration::parse(std::string_view text) {
    auto const json = Json::parse(text);
    if (!json.at("schemaVersion").is_number_integer() || json.at("schemaVersion") != 1) throw std::invalid_argument("Unsupported native runner configuration.");
    RunnerConfiguration result;
    auto const& limits = json.at("limits");
    result.maximum_documents = static_cast<std::size_t>(bound(limits.at("maximumDocuments"), 64));
    result.maximum_tests = static_cast<std::size_t>(bound(limits.at("maximumTests"), 4096));
    result.display_output_bytes = static_cast<std::size_t>(bound(limits.at("displayOutputBytes"), 1024 * 1024));
    result.options.maximum_commands = static_cast<std::size_t>(bound(limits.at("maximumCommands"), 256));
    result.options.maximum_drain_events = static_cast<std::uint32_t>(bound(limits.at("maximumDrainEvents"), 256));
    result.options.event_poll_interval = std::chrono::milliseconds{bound(limits.at("eventPollMilliseconds"), 1000)};
    auto& options = result.options.engine;
    options.struct_size = sizeof(options); options.abi_version = CPN_ABI_VERSION;
    options.max_workers = static_cast<std::uint32_t>(bound(limits.at("maximumWorkers"), 32));
    options.max_jobs = static_cast<std::uint32_t>(bound(limits.at("maximumJobs"), 256));
    options.max_buffers = static_cast<std::uint32_t>(bound(limits.at("maximumBuffers"), 256));
    options.max_request_bytes = bound(limits.at("maximumRequestBytes"), 64 * 1024 * 1024);
    options.max_retained_output_bytes = bound(limits.at("maximumRetainedOutputBytes"), 256 * 1024 * 1024);
    result.default_comparison = json.at("defaultComparison").get<std::string>();
    result.default_toolchains = json.at("defaultToolchains").get<decltype(result.default_toolchains)>();
    result.state_labels = json.at("stateLabels").get<decltype(result.state_labels)>();
    result.verdict_labels = json.at("verdictLabels").get<decltype(result.verdict_labels)>();
    result.build_labels = json.at("buildLabels").get<decltype(result.build_labels)>();
    std::set<std::string> toolchain_ids;
    for (auto const& choice : json.at("toolchains")) {
        ToolchainChoice item{choice.at("id").get<std::string>(), choice.at("file").get<std::string>(),
                             choice.at("labelResource").get<std::string>(), {}};
        if (item.id.empty() || item.label_resource.empty() || item.file.empty() ||
            item.file.find_first_of("/\\:") != std::string::npos || item.file == "." || item.file == ".." ||
            !toolchain_ids.insert(item.id).second)
            throw std::invalid_argument("Invalid or duplicate native toolchain choice.");
        result.toolchains.push_back(std::move(item));
    }
    if (result.toolchains.empty()) throw std::invalid_argument("No native toolchain choices.");
    for (auto const& [language, id] : result.default_toolchains)
        if (language.empty() || !toolchain_ids.contains(id)) throw std::invalid_argument("Invalid language/toolchain mapping.");
    std::set<std::string> comparison_ids;
    for (auto const& choice : json.at("comparisons")) {
        ComparisonChoice item{choice.at("id").get<std::string>(), choice.at("labelResource").get<std::string>()};
        if (item.id.empty() || item.label_resource.empty() || !comparison_ids.insert(item.id).second)
            throw std::invalid_argument("Invalid native comparison choice.");
        result.comparisons.push_back(std::move(item));
    }
    if (!comparison_ids.contains(result.default_comparison)) throw std::invalid_argument("Invalid default comparison policy.");
    return result;
}
std::string const& RunnerConfiguration::default_toolchain(std::string const& language) const {
    const auto found = default_toolchains.find(language);
    if (found == default_toolchains.end()) throw std::invalid_argument("Missing default toolchain for editor language.");
    return found->second;
}
bool RunnerConfiguration::is_compiled_toolchain(nlohmann::json const& profile) {
    return profile.is_object() && profile.contains("build") && !profile.at("build").is_null();
}
nlohmann::json RunnerConfiguration::validate_toolchain_json(std::string_view json_text) {
    cpn_error error{};
    error.struct_size = sizeof(error);
    auto status = cpn_validate_toolchain_json(
        cpn_bytes{reinterpret_cast<std::uint8_t const*>(json_text.data()), json_text.size()}, &error);
    if (status != CPN_OK) throw std::invalid_argument(error.message);
    return Json::parse(json_text);
}
ToolchainVisualDraft RunnerConfiguration::visual_from_json(nlohmann::json const& profile) {
    auto validated = validate_toolchain_json(profile.dump());
    ToolchainVisualDraft draft;
    draft.has_build = is_compiled_toolchain(validated);
    if (draft.has_build) {
        auto const& build = validated.at("build");
        draft.build_executable = build.at("executable").get<std::string>();
        draft.build_arguments = build.at("arguments").get<std::vector<std::string>>();
        if (validated.contains("build_limits") && validated.at("build_limits").is_object() &&
            validated.at("build_limits").contains("wall_timeout_ms"))
            draft.build_wall_timeout_ms = validated.at("build_limits").at("wall_timeout_ms").get<std::uint64_t>();
    }
    auto const& run = validated.at("run");
    draft.run_executable = run.at("executable").get<std::string>();
    draft.run_arguments = run.at("arguments").get<std::vector<std::string>>();
    if (validated.contains("run_limits") && validated.at("run_limits").is_object() &&
        validated.at("run_limits").contains("wall_timeout_ms"))
        draft.run_wall_timeout_ms = validated.at("run_limits").at("wall_timeout_ms").get<std::uint64_t>();
    if (validated.contains("inherit_environment"))
        draft.inherit_environment = validated.at("inherit_environment").get<bool>();
    return draft;
}
nlohmann::json RunnerConfiguration::apply_visual_to_json(nlohmann::json const& base_profile, ToolchainVisualDraft const& draft) {
    auto next = validate_toolchain_json(base_profile.dump());
    if (draft.has_build) {
        next["build"] = Json{{"executable", draft.build_executable}, {"arguments", draft.build_arguments}};
        if (!next.contains("build_limits") || !next.at("build_limits").is_object()) next["build_limits"] = Json::object();
        next["build_limits"]["wall_timeout_ms"] = draft.build_wall_timeout_ms;
    } else {
        next["build"] = nullptr;
    }
    next["run"] = Json{{"executable", draft.run_executable}, {"arguments", draft.run_arguments}};
    if (!next.contains("run_limits") || !next.at("run_limits").is_object()) next["run_limits"] = Json::object();
    next["run_limits"]["wall_timeout_ms"] = draft.run_wall_timeout_ms;
    next["inherit_environment"] = draft.inherit_environment;
    return validate_toolchain_json(next.dump());
}
}
