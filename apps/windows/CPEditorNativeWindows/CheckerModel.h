#pragma once
#include <nlohmann/json.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cpn::windows {
struct CheckerConfiguration {
    std::size_t max_checker_json_bytes{1024 * 1024};
    nlohmann::json default_checker;
    [[nodiscard]] static CheckerConfiguration parse(std::string_view);
};

struct CheckerSettings {
    bool enabled{false};
    std::string json_text;
    [[nodiscard]] bool operator==(CheckerSettings const&) const noexcept = default;
};

struct CheckerVisualDraft {
    std::string executable;
    std::vector<std::string> arguments;
    std::uint64_t wall_timeout_ms{2000};
    std::vector<int> accepted_exit_codes{0};
    std::vector<int> wrong_answer_exit_codes{1, 2};
    bool inherit_environment{true};
};

struct CheckerModel {
    [[nodiscard]] static nlohmann::json validate_json(std::string_view json_text, std::size_t max_bytes = 1024 * 1024);
    [[nodiscard]] static CheckerVisualDraft visual_from_json(std::string_view json_text, std::size_t max_bytes = 1024 * 1024);
    [[nodiscard]] static std::string apply_visual_to_json(std::string_view base_json_text, CheckerVisualDraft const&, std::size_t max_bytes = 1024 * 1024);
    [[nodiscard]] static std::string fingerprint(CheckerSettings const&, std::size_t max_bytes = 1024 * 1024);
    [[nodiscard]] static std::vector<std::string> parse_arguments_lines(std::string_view);
    [[nodiscard]] static std::string format_arguments_lines(std::vector<std::string> const&);
    [[nodiscard]] static std::vector<int> parse_exit_codes(std::string_view);
    [[nodiscard]] static std::string format_exit_codes(std::vector<int> const&);
};
}
