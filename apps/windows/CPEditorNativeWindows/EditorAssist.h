#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cpn::windows {
struct AssistPair {
    char open{};
    char close{};
};

struct AssistConfig {
    bool auto_pair_default{true};
    std::vector<AssistPair> pairs{{'(', ')'}, {'[', ']'}, {'{', '}'}, {'"', '"'}, {'\'', '\''}};
    [[nodiscard]] static AssistConfig parse_from_editor_profiles(std::string_view json_text);
};

struct AssistEditPlan {
    std::size_t erase_before{};
    std::size_t erase_after{};
    std::string insert_text;
    std::size_t caret_offset_in_insert{};
};

struct TabIndentPlan {
    std::size_t replace_start{};
    std::size_t replace_length{};
    std::string replacement;
    std::size_t new_sel_start{};
    std::size_t new_sel_end{};
};

struct EditorAssist {
    [[nodiscard]] static std::optional<AssistEditPlan> plan_char_input(
        std::string_view before_caret, std::string_view after_caret, std::string_view selection,
        char typed, std::string_view language_id, AssistConfig const& = {});
    [[nodiscard]] static std::optional<AssistEditPlan> plan_backspace(
        std::string_view before_caret, std::string_view after_caret, AssistConfig const& = {});
    [[nodiscard]] static std::optional<AssistEditPlan> plan_newline(
        std::string_view line_before_caret, std::string_view after_caret,
        std::string_view eol, std::uint32_t indent_width);
    [[nodiscard]] static std::optional<TabIndentPlan> plan_tab_indent(
        std::string_view text, std::size_t sel_start, std::size_t sel_end,
        bool unindent, std::uint32_t indent_width);
};
}
