#pragma once
#include "CoreRunner.h"
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace cpn::windows {
struct ToolchainChoice { std::string id; std::string file; std::string label_resource; nlohmann::json profile; };
struct ComparisonChoice { std::string id; std::string label_resource; };
struct ToolchainVisualDraft {
    bool has_build{false};
    std::string build_executable;
    std::vector<std::string> build_arguments;
    std::uint64_t build_wall_timeout_ms{30000};
    std::string run_executable;
    std::vector<std::string> run_arguments;
    std::uint64_t run_wall_timeout_ms{2000};
    bool inherit_environment{true};
};
struct RunnerConfiguration {
    CoreRunner::Options options;
    std::size_t maximum_documents{};
    std::size_t maximum_tests{};
    std::size_t display_output_bytes{};
    std::string default_comparison;
    std::map<std::string, std::string> default_toolchains;
    std::map<std::string, std::string> state_labels;
    std::map<std::string, std::string> verdict_labels;
    std::map<std::string, std::string> build_labels;
    std::vector<ToolchainChoice> toolchains;
    std::vector<ComparisonChoice> comparisons;
    [[nodiscard]] static RunnerConfiguration parse(std::string_view);
    [[nodiscard]] std::string const& default_toolchain(std::string const& language) const;
    [[nodiscard]] static bool is_compiled_toolchain(nlohmann::json const& profile);
    [[nodiscard]] static nlohmann::json validate_toolchain_json(std::string_view json_text);
    [[nodiscard]] static ToolchainVisualDraft visual_from_json(nlohmann::json const& profile);
    [[nodiscard]] static nlohmann::json apply_visual_to_json(nlohmann::json const& base_profile, ToolchainVisualDraft const&);
};
}
