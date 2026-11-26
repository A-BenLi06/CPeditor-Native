#pragma once
#include "RunnerConfiguration.h"
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cpn::windows {
struct ToolchainDiscoveryEntry {
    std::string id;
    std::string executable;
    std::vector<std::string> search_directories;
};

struct ToolchainDiscoveryConfiguration {
    std::string msvc_toolchain_id{"cpp-msvc"};
    std::vector<std::string> vswhere_paths;
    std::vector<ToolchainDiscoveryEntry> entries;
    [[nodiscard]] static ToolchainDiscoveryConfiguration parse(std::string_view);
    [[nodiscard]] static ToolchainDiscoveryConfiguration defaults();
};

struct ToolchainProbeResult {
    std::string id;
    bool available{};
    std::string resolved_executable;
    std::vector<std::pair<std::string, std::string>> resolved_environment;
    std::string detail;
};

struct ToolchainDiscovery {
    [[nodiscard]] static std::vector<ToolchainProbeResult> probe_all(
        std::vector<ToolchainChoice> const&, ToolchainDiscoveryConfiguration const&);
    [[nodiscard]] static ToolchainProbeResult probe_one(
        ToolchainChoice const&, ToolchainDiscoveryConfiguration const&);
    static bool apply_discovered_defaults(
        std::vector<ToolchainChoice>&, std::vector<ToolchainProbeResult> const&);
    [[nodiscard]] static std::string expand_env_vars(std::string_view);
};
}
