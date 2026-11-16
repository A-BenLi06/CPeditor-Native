#pragma once
#include "CoreRunner.h"
#include <map>
#include <string>
#include <vector>

namespace cpn::windows {
struct ToolchainChoice { std::string id; std::string file; std::string label_resource; nlohmann::json profile; };
struct ComparisonChoice { std::string id; std::string label_resource; };
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
};
}
