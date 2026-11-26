#include "pch.h"
#include "ToolchainDiscovery.h"
#include "cpn/files.hpp"
#include "cpn/process.hpp"
#include <algorithm>
#include <cstdlib>
#include <set>
#include <sstream>
#include <stdexcept>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cpn::windows {
namespace {
using Json = nlohmann::json;

void require_fields(Json const& object, std::initializer_list<std::string_view> allowed) {
    if (!object.is_object()) throw std::invalid_argument("Expected discovery JSON object.");
    for (auto const& [key, entry] : object.items()) {
        (void)entry;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
            throw std::invalid_argument("Unknown discovery configuration field: " + key);
    }
}

bool is_windows_store_stub(std::filesystem::path const& candidate) {
#if defined(_WIN32)
    auto text = cpn::path_to_utf8(candidate);
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.find("\\microsoft\\windowsapps\\") != std::string::npos) {
        std::error_code ec;
        auto size = std::filesystem::file_size(candidate, ec);
        if (ec || size == 0) return true;
    }
#else
    (void)candidate;
#endif
    return false;
}

bool is_usable_executable(std::filesystem::path const& candidate) {
    if (candidate.empty()) return false;
    std::error_code ec;
    if (!std::filesystem::exists(candidate, ec) || ec) return false;
    if (std::filesystem::is_directory(candidate, ec) || ec) return false;
    if (is_windows_store_stub(candidate)) return false;
    return true;
}

std::vector<std::filesystem::path> split_path_env() {
    std::vector<std::filesystem::path> dirs;
#if defined(_WIN32)
    DWORD needed = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (needed > 1) {
        std::wstring buffer(needed, L'\0');
        DWORD copied = GetEnvironmentVariableW(L"PATH", buffer.data(), needed);
        buffer.resize(copied);
        std::size_t start = 0;
        while (start <= buffer.size()) {
            auto sep = buffer.find(L';', start);
            if (sep == std::wstring::npos) sep = buffer.size();
            auto part = buffer.substr(start, sep - start);
            if (!part.empty()) dirs.emplace_back(part);
            if (sep == buffer.size()) break;
            start = sep + 1;
        }
    }
#else
    if (char const* raw = std::getenv("PATH")) {
        std::string_view view{raw};
        std::size_t start = 0;
        while (start <= view.size()) {
            auto sep = view.find(':', start);
            if (sep == std::string_view::npos) sep = view.size();
            auto part = view.substr(start, sep - start);
            if (!part.empty()) dirs.push_back(cpn::path_from_utf8(part));
            if (sep == view.size()) break;
            start = sep + 1;
        }
    }
#endif
    return dirs;
}

std::optional<std::filesystem::path> search_executable(
    std::string_view executable_name, std::vector<std::string> const& extra_dirs) {
    if (executable_name.empty()) return std::nullopt;
    auto raw_path = cpn::path_from_utf8(executable_name);
    if (raw_path.is_absolute() && is_usable_executable(raw_path)) return raw_path;

    std::vector<std::filesystem::path> candidates;
    for (auto const& dir : split_path_env()) candidates.push_back(dir);
    for (auto const& dir : extra_dirs) {
        auto expanded = ToolchainDiscovery::expand_env_vars(dir);
        if (!expanded.empty()) candidates.push_back(cpn::path_from_utf8(expanded));
    }

    auto filename = raw_path.filename();
    for (auto const& dir : candidates) {
        auto full = dir / filename;
        if (is_usable_executable(full)) return full;
#if defined(_WIN32)
        if (!filename.has_extension()) {
            auto exe = full;
            exe += L".exe";
            if (is_usable_executable(exe)) return exe;
        }
#endif
    }
    return std::nullopt;
}

#if defined(_WIN32)
std::optional<std::filesystem::path> read_kits_root_10() {
    for (auto const* subkey : {
        L"SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots",
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows Kits\\Installed Roots"
    }) {
        wchar_t buffer[4096]{};
        DWORD size = sizeof(buffer);
        DWORD type = 0;
        LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE, subkey, L"KitsRoot10",
                                      RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, &type, buffer, &size);
        if (status != ERROR_SUCCESS) {
            size = sizeof(buffer);
            status = RegGetValueW(HKEY_LOCAL_MACHINE, subkey, L"KitsRoot10",
                                  RRF_RT_REG_SZ | RRF_SUBKEY_WOW6432KEY, &type, buffer, &size);
        }
        if (status == ERROR_SUCCESS && buffer[0] != L'\0') {
            std::filesystem::path root{buffer};
            std::error_code ec;
            if (std::filesystem::exists(root / L"Include", ec)) return root;
        }
    }
    return std::nullopt;
}

std::vector<int> parse_dotted_version(std::wstring const& name) {
    std::vector<int> parts;
    std::size_t start = 0;
    while (start <= name.size()) {
        auto dot = name.find(L'.', start);
        if (dot == std::wstring::npos) dot = name.size();
        auto piece = name.substr(start, dot - start);
        if (piece.empty()) return {};
        try {
            parts.push_back(std::stoi(piece));
        } catch (...) {
            return {};
        }
        if (dot == name.size()) break;
        start = dot + 1;
    }
    return parts;
}

std::optional<std::filesystem::path> find_latest_subdir(
    std::filesystem::path const& parent,
    std::function<bool(std::filesystem::path const&)> const& predicate) {
    std::error_code ec;
    if (!std::filesystem::exists(parent, ec) || !std::filesystem::is_directory(parent, ec))
        return std::nullopt;
    std::optional<std::pair<std::vector<int>, std::filesystem::path>> best;
    for (auto const& entry : std::filesystem::directory_iterator(parent, ec)) {
        if (!entry.is_directory(ec)) continue;
        auto ver = parse_dotted_version(entry.path().filename().wstring());
        if (ver.empty()) continue;
        if (predicate && !predicate(entry.path())) continue;
        if (!best || best->first < ver) best = std::make_pair(std::move(ver), entry.path());
    }
    return best ? std::optional<std::filesystem::path>{best->second} : std::nullopt;
}

std::optional<std::filesystem::path> discover_vs_installation(ToolchainDiscoveryConfiguration const& config) {
    for (auto const& raw : config.vswhere_paths) {
        auto expanded = cpn::path_from_utf8(ToolchainDiscovery::expand_env_vars(raw));
        if (!is_usable_executable(expanded)) continue;
        cpn::ProcessSpec spec;
        spec.executable = expanded;
        spec.arguments = {
            "-latest", "-products", "*",
            "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-property", "installationPath"
        };
        spec.limits.wall_timeout = std::chrono::milliseconds{4000};
        spec.limits.stdout_bytes = 64 * 1024;
        spec.limits.stderr_bytes = 16 * 1024;
        spec.limits.combined_bytes = 80 * 1024;
        auto res = cpn::run_process(spec);
        if (res.termination == cpn::ProcessTermination::exited && res.exit_code == 0) {
            std::string out = res.standard_output;
            while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' ')) out.pop_back();
            if (!out.empty()) {
                auto vs_root = cpn::path_from_utf8(out);
                std::error_code ec;
                if (std::filesystem::exists(vs_root, ec)) return vs_root;
            }
        }
    }
    return std::nullopt;
}

std::string get_env_utf8(wchar_t const* name) {
    DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed <= 1) return {};
    std::wstring buf(needed, L'\0');
    DWORD copied = GetEnvironmentVariableW(name, buf.data(), needed);
    buf.resize(copied);
    return cpn::path_to_utf8(std::filesystem::path{buf});
}

std::optional<ToolchainProbeResult> probe_msvc_runtime(ToolchainDiscoveryConfiguration const& config) {
    auto vs_root = discover_vs_installation(config);
    if (!vs_root) return std::nullopt;

    auto msvc_root = *vs_root / L"VC" / L"Tools" / L"MSVC";
    auto toolset_dir = find_latest_subdir(msvc_root, [](std::filesystem::path const& candidate) {
        return is_usable_executable(candidate / L"bin" / L"Hostx64" / L"x64" / L"cl.exe");
    });
    if (!toolset_dir) return std::nullopt;

    auto cl_path = *toolset_dir / L"bin" / L"Hostx64" / L"x64" / L"cl.exe";
    std::vector<std::string> include_dirs{cpn::path_to_utf8(*toolset_dir / L"include")};
    std::vector<std::string> lib_dirs{cpn::path_to_utf8(*toolset_dir / L"lib" / L"x64")};
    std::vector<std::string> path_dirs{cpn::path_to_utf8(*toolset_dir / L"bin" / L"Hostx64" / L"x64")};

    if (auto kits_root = read_kits_root_10()) {
        auto sdk_ver = find_latest_subdir(*kits_root / L"Include", [](std::filesystem::path const& candidate) {
            std::error_code ec;
            return std::filesystem::exists(candidate / L"ucrt" / L"corecrt.h", ec) &&
                   std::filesystem::exists(candidate / L"um" / L"windows.h", ec);
        });
        if (sdk_ver) {
            auto ver_name = sdk_ver->filename();
            include_dirs.push_back(cpn::path_to_utf8(*kits_root / L"Include" / ver_name / L"ucrt"));
            include_dirs.push_back(cpn::path_to_utf8(*kits_root / L"Include" / ver_name / L"um"));
            include_dirs.push_back(cpn::path_to_utf8(*kits_root / L"Include" / ver_name / L"shared"));
            lib_dirs.push_back(cpn::path_to_utf8(*kits_root / L"Lib" / ver_name / L"ucrt" / L"x64"));
            lib_dirs.push_back(cpn::path_to_utf8(*kits_root / L"Lib" / ver_name / L"um" / L"x64"));
            path_dirs.push_back(cpn::path_to_utf8(*kits_root / L"bin" / ver_name / L"x64"));
        }
    }

    auto join_semicolon = [](std::vector<std::string> const& parts) {
        std::string out;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i) out += ';';
            out += parts[i];
        }
        return out;
    };

    std::string path_env = join_semicolon(path_dirs);
    if (auto existing_path = get_env_utf8(L"PATH"); !existing_path.empty()) {
        path_env += ';';
        path_env += existing_path;
    }

    ToolchainProbeResult probe;
    probe.id = config.msvc_toolchain_id;
    probe.available = true;
    probe.resolved_executable = cpn::path_to_utf8(cl_path);
    probe.resolved_environment = {
        {"INCLUDE", join_semicolon(include_dirs)},
        {"LIB", join_semicolon(lib_dirs)},
        {"PATH", std::move(path_env)}
    };
    probe.detail = probe.resolved_executable;
    return probe;
}
#endif

std::string configured_executable(nlohmann::json const& profile) {
    if (profile.contains("build") && profile.at("build").is_object() && profile.at("build").contains("executable"))
        return profile.at("build").at("executable").get<std::string>();
    if (profile.contains("run") && profile.at("run").is_object() && profile.at("run").contains("executable"))
        return profile.at("run").at("executable").get<std::string>();
    return {};
}

bool has_env_pair(nlohmann::json const& profile, std::string_view key) {
    if (!profile.contains("environment") || !profile.at("environment").is_array()) return false;
    for (auto const& pair : profile.at("environment")) {
        if (pair.is_object() && pair.contains("name") && pair.at("name").is_string() &&
            pair.at("name").get_ref<std::string const&>() == key) return true;
    }
    return false;
}
}

ToolchainDiscoveryConfiguration ToolchainDiscoveryConfiguration::defaults() {
    ToolchainDiscoveryConfiguration cfg;
    cfg.msvc_toolchain_id = "cpp-msvc";
    cfg.vswhere_paths = {
        "%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer\\vswhere.exe",
        "%ProgramFiles%\\Microsoft Visual Studio\\Installer\\vswhere.exe"
    };
    cfg.entries = {
        {"cpp-gcc-windows", "g++.exe", {
            "C:\\msys64\\ucrt64\\bin", "C:\\msys64\\mingw64\\bin", "C:\\msys64\\clang64\\bin",
            "C:\\mingw64\\bin", "C:\\mingw32\\bin", "C:\\TDM-GCC-64\\bin",
            "%USERPROFILE%\\scoop\\shims", "%ProgramData%\\chocolatey\\bin"
        }},
        {"cpp-clang-windows", "clang++.exe", {
            "%ProgramFiles%\\LLVM\\bin", "%ProgramFiles(x86)%\\LLVM\\bin",
            "C:\\msys64\\clang64\\bin", "C:\\msys64\\ucrt64\\bin"
        }},
        {"python-windows", "python.exe", {
            "%LOCALAPPDATA%\\Programs\\Python\\Python314",
            "%LOCALAPPDATA%\\Programs\\Python\\Python313",
            "%LOCALAPPDATA%\\Programs\\Python\\Python312",
            "%LOCALAPPDATA%\\Programs\\Python\\Python311",
            "C:\\Python314", "C:\\Python313", "C:\\Python312", "C:\\Python311"
        }}
    };
    return cfg;
}

ToolchainDiscoveryConfiguration ToolchainDiscoveryConfiguration::parse(std::string_view input) {
    std::vector<std::set<std::string>> keys;
    auto const json = Json::parse(input, [&](int depth, Json::parse_event_t event, Json& parsed) {
        if (depth > 32) throw std::invalid_argument("Discovery JSON nesting exceeds limit.");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::object_end) keys.pop_back();
        else if (event == Json::parse_event_t::key && !keys.back().insert(parsed.get<std::string>()).second)
            throw std::invalid_argument("Duplicate discovery JSON field.");
        return true;
    });
    require_fields(json, {"schemaVersion", "msvcToolchainId", "vswherePaths", "entries"});
    if (!json.at("schemaVersion").is_number_integer() || json.at("schemaVersion") != 1)
        throw std::invalid_argument("Unsupported toolchain discovery schema version.");
    ToolchainDiscoveryConfiguration result;
    result.msvc_toolchain_id = json.at("msvcToolchainId").get<std::string>();
    result.vswhere_paths = json.at("vswherePaths").get<std::vector<std::string>>();
    for (auto const& item : json.at("entries")) {
        require_fields(item, {"id", "executable", "searchDirectories"});
        ToolchainDiscoveryEntry entry;
        entry.id = item.at("id").get<std::string>();
        entry.executable = item.at("executable").get<std::string>();
        entry.search_directories = item.at("searchDirectories").get<std::vector<std::string>>();
        if (entry.id.empty() || entry.executable.empty())
            throw std::invalid_argument("Invalid toolchain discovery entry.");
        result.entries.push_back(std::move(entry));
    }
    return result;
}

std::string ToolchainDiscovery::expand_env_vars(std::string_view input) {
#if defined(_WIN32)
    if (input.empty()) return {};
    auto wide = cpn::path_from_utf8(input).wstring();
    DWORD needed = ExpandEnvironmentStringsW(wide.c_str(), nullptr, 0);
    if (needed == 0) return std::string{input};
    std::wstring expanded(needed, L'\0');
    DWORD copied = ExpandEnvironmentStringsW(wide.c_str(), expanded.data(), needed);
    if (copied == 0) return std::string{input};
    if (!expanded.empty() && expanded.back() == L'\0') expanded.pop_back();
    while (!expanded.empty() && expanded.back() == L'\0') expanded.pop_back();
    return cpn::path_to_utf8(std::filesystem::path{expanded});
#else
    return std::string{input};
#endif
}

ToolchainProbeResult ToolchainDiscovery::probe_one(
    ToolchainChoice const& choice, ToolchainDiscoveryConfiguration const& config) {
    ToolchainProbeResult result;
    result.id = choice.id;
    auto configured = configured_executable(choice.profile);
    if (!configured.empty()) {
        auto path = cpn::path_from_utf8(configured);
        if (path.is_absolute() && is_usable_executable(path)) {
            if (choice.id != config.msvc_toolchain_id ||
                (has_env_pair(choice.profile, "INCLUDE") && has_env_pair(choice.profile, "LIB"))) {
                result.available = true;
                result.resolved_executable = configured;
                result.detail = configured;
                return result;
            }
        }
    }

    if (choice.id == config.msvc_toolchain_id) {
#if defined(_WIN32)
        if (auto msvc = probe_msvc_runtime(config)) return *msvc;
        if (auto found = search_executable("cl.exe", {})) {
            result.available = true;
            result.resolved_executable = cpn::path_to_utf8(*found);
            result.detail = result.resolved_executable;
            return result;
        }
#endif
        result.available = false;
        result.detail = "MSVC cl.exe was not found on this machine.";
        return result;
    }

    std::vector<std::string> extra_dirs;
    std::string default_exe = configured;
    for (auto const& entry : config.entries) {
        if (entry.id == choice.id) {
            extra_dirs = entry.search_directories;
            if (default_exe.empty() || cpn::path_from_utf8(default_exe).is_absolute())
                default_exe = entry.executable;
            break;
        }
    }
#if defined(_WIN32)
    if (choice.id == "cpp-clang-windows") {
        if (auto vs_root = discover_vs_installation(config)) {
            extra_dirs.push_back(cpn::path_to_utf8(*vs_root / L"VC" / L"Tools" / L"Llvm" / L"x64" / L"bin"));
            extra_dirs.push_back(cpn::path_to_utf8(*vs_root / L"VC" / L"Tools" / L"Llvm" / L"bin"));
        }
    }
#endif
    if (auto found = search_executable(configured.empty() ? default_exe : configured, extra_dirs)) {
        result.available = true;
        result.resolved_executable = cpn::path_to_utf8(*found);
        result.detail = result.resolved_executable;
        return result;
    }
    if (!default_exe.empty() && default_exe != configured) {
        if (auto found = search_executable(default_exe, extra_dirs)) {
            result.available = true;
            result.resolved_executable = cpn::path_to_utf8(*found);
            result.detail = result.resolved_executable;
            return result;
        }
    }
    result.available = false;
    result.detail = (default_exe.empty() ? configured : default_exe) + " was not found.";
    return result;
}

std::vector<ToolchainProbeResult> ToolchainDiscovery::probe_all(
    std::vector<ToolchainChoice> const& choices, ToolchainDiscoveryConfiguration const& config) {
    std::vector<ToolchainProbeResult> results;
    results.reserve(choices.size());
    for (auto const& choice : choices) results.push_back(probe_one(choice, config));
    return results;
}

bool ToolchainDiscovery::apply_discovered_defaults(
    std::vector<ToolchainChoice>& choices, std::vector<ToolchainProbeResult> const& probes) {
    bool changed = false;
    for (auto& choice : choices) {
        auto it = std::find_if(probes.begin(), probes.end(), [&](auto const& p) { return p.id == choice.id; });
        if (it == probes.end() || !it->available || it->resolved_executable.empty()) continue;

        auto current = configured_executable(choice.profile);
        auto current_path = cpn::path_from_utf8(current);
        bool current_valid = current_path.is_absolute() && is_usable_executable(current_path);
        if (!current_valid) {
            if (choice.profile.contains("build") && choice.profile.at("build").is_object()) {
                choice.profile["build"]["executable"] = it->resolved_executable;
                changed = true;
            } else if (choice.profile.contains("run") && choice.profile.at("run").is_object()) {
                choice.profile["run"]["executable"] = it->resolved_executable;
                changed = true;
            }
        }

        if (!it->resolved_environment.empty() &&
            (!has_env_pair(choice.profile, "INCLUDE") || !has_env_pair(choice.profile, "LIB"))) {
            Json env = Json::array();
            for (auto const& [k, v] : it->resolved_environment) env.push_back({{"name", k}, {"value", v}});
            choice.profile["environment"] = std::move(env);
            changed = true;
        }
    }
    return changed;
}
}
