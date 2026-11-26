#include "pch.h"
#include "EditorAssist.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace cpn::windows {
namespace {
using Json = nlohmann::json;

bool is_ident_or_digit(unsigned char c) {
    return std::isalnum(c) != 0 || c == '_' || c >= 0x80;
}

bool is_hex_digit_char(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

struct LexicalState {
    bool in_line_comment{};
    bool in_block_comment{};
    bool in_string{};
    bool in_char{};
};

LexicalState scan_lexical_state(std::string_view prefix, std::string_view language_id) {
    LexicalState state;
    bool const cpp_like = language_id == "cpp" || language_id == "c";
    bool const py_like = language_id == "python";
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        char const ch = prefix[i];
        char const next = (i + 1 < prefix.size()) ? prefix[i + 1] : '\0';
        if (state.in_line_comment) {
            if (ch == '\n' || ch == '\r') state.in_line_comment = false;
            continue;
        }
        if (state.in_block_comment) {
            if (cpp_like && ch == '*' && next == '/') {
                state.in_block_comment = false;
                ++i;
            }
            continue;
        }
        if (state.in_string) {
            if (ch == '\\' && i + 1 < prefix.size()) {
                ++i;
                continue;
            }
            if (ch == '"') state.in_string = false;
            else if (ch == '\n' || ch == '\r') state.in_string = false;
            continue;
        }
        if (state.in_char) {
            if (ch == '\\' && i + 1 < prefix.size()) {
                ++i;
                continue;
            }
            if (ch == '\'') state.in_char = false;
            else if (ch == '\n' || ch == '\r') state.in_char = false;
            continue;
        }
        if (cpp_like && ch == '/' && next == '/') {
            state.in_line_comment = true;
            ++i;
            continue;
        }
        if (cpp_like && ch == '/' && next == '*') {
            state.in_block_comment = true;
            ++i;
            continue;
        }
        if (py_like && ch == '#') {
            state.in_line_comment = true;
            continue;
        }
        if (ch == '"') {
            state.in_string = true;
            continue;
        }
        if (ch == '\'') {
            // C++14 digit separator: e.g. 1'000 or 0xFF'FF
            if (cpp_like && i > 0 && is_hex_digit_char(static_cast<unsigned char>(prefix[i - 1])) &&
                i + 1 < prefix.size() && is_hex_digit_char(static_cast<unsigned char>(next))) {
                continue;
            }
            state.in_char = true;
            continue;
        }
    }
    return state;
}

bool is_escaped(std::string_view before_caret) {
    std::size_t backslashes = 0;
    for (std::size_t i = before_caret.size(); i > 0 && before_caret[i - 1] == '\\'; --i) ++backslashes;
    return (backslashes % 2) == 1;
}

std::optional<char> matching_close(AssistConfig const& cfg, char open) {
    for (auto const& pair : cfg.pairs) if (pair.open == open) return pair.close;
    return std::nullopt;
}

bool is_closing_char(AssistConfig const& cfg, char close) {
    for (auto const& pair : cfg.pairs) if (pair.close == close) return true;
    return false;
}

bool is_balanced_context_for_close(
    std::string_view before_caret, char close, AssistConfig const& cfg) {
    char open = '\0';
    for (auto const& pair : cfg.pairs) {
        if (pair.close == close) { open = pair.open; break; }
    }
    if (!open) return false;
    if (open == close) {
        std::size_t count = 0;
        for (std::size_t i = 0; i < before_caret.size(); ++i) {
            if (before_caret[i] == '\\') { ++i; continue; }
            if (before_caret[i] == open) ++count;
        }
        return (count % 2) == 1;
    }
    int depth = 0;
    for (char ch : before_caret) {
        if (ch == open) ++depth;
        else if (ch == close && depth > 0) --depth;
    }
    return depth > 0;
}

std::string leading_whitespace(std::string_view line) {
    std::size_t end = 0;
    while (end < line.size() && (line[end] == ' ' || line[end] == '\t')) ++end;
    return std::string{line.substr(0, end)};
}

std::string last_line_of(std::string_view text) {
    auto pos = text.find_last_of("\r\n");
    if (pos == std::string_view::npos) return std::string{text};
    return std::string{text.substr(pos + 1)};
}
}

AssistConfig AssistConfig::parse_from_editor_profiles(std::string_view json_text) {
    auto const root = Json::parse(json_text);
    AssistConfig cfg;
    if (!root.contains("assist")) return cfg;
    auto const& assist = root.at("assist");
    if (assist.contains("autoPairDefault")) cfg.auto_pair_default = assist.at("autoPairDefault").get<bool>();
    if (assist.contains("pairs")) {
        cfg.pairs.clear();
        for (auto const& item : assist.at("pairs")) {
            auto open = item.at("open").get<std::string>();
            auto close = item.at("close").get<std::string>();
            if (open.size() != 1 || close.size() != 1)
                throw std::invalid_argument("Assist pair delimiter must be single ASCII byte.");
            cfg.pairs.push_back({open[0], close[0]});
        }
    }
    return cfg;
}

std::optional<AssistEditPlan> EditorAssist::plan_char_input(
    std::string_view before_caret, std::string_view after_caret, std::string_view selection,
    char typed, std::string_view language_id, AssistConfig const& cfg) {
    auto const close = matching_close(cfg, typed);
    bool const closing = is_closing_char(cfg, typed);

    if (!selection.empty()) {
        if (close) {
            AssistEditPlan plan;
            plan.insert_text.reserve(selection.size() + 2);
            plan.insert_text.push_back(typed);
            plan.insert_text.append(selection);
            plan.insert_text.push_back(*close);
            plan.caret_offset_in_insert = plan.insert_text.size();
            return plan;
        }
        return std::nullopt;
    }

    auto const lexical = scan_lexical_state(before_caret, language_id);
    if (lexical.in_line_comment || lexical.in_block_comment) return std::nullopt;

    // Skip closing bracket or quote when already immediately after caret in balanced context.
    if (closing && !after_caret.empty() && after_caret.front() == typed && !is_escaped(before_caret)) {
        bool const is_quote = (typed == '"' || typed == '\'');
        if (is_quote) {
            if ((typed == '"' && lexical.in_string) || (typed == '\'' && lexical.in_char)) {
                AssistEditPlan plan;
                plan.erase_after = 1;
                plan.insert_text.push_back(typed);
                plan.caret_offset_in_insert = 1;
                return plan;
            }
        } else if (!lexical.in_string && !lexical.in_char && is_balanced_context_for_close(before_caret, typed, cfg)) {
            AssistEditPlan plan;
            plan.erase_after = 1;
            plan.insert_text.push_back(typed);
            plan.caret_offset_in_insert = 1;
            return plan;
        }
    }

    if (!close) return std::nullopt;
    if (lexical.in_string || lexical.in_char) return std::nullopt;

    // Avoid pairing single quote as C++ digit separator after hex digit (e.g. 1'000).
    if (typed == '\'' && (language_id == "cpp" || language_id == "c") && !before_caret.empty() &&
        is_hex_digit_char(static_cast<unsigned char>(before_caret.back()))) {
        return std::nullopt;
    }

    // Do not auto-pair quotes immediately before identifier/digit characters or if escaped.
    if (typed == '"' || typed == '\'') {
        if (is_escaped(before_caret)) return std::nullopt;
        if (!before_caret.empty() && is_ident_or_digit(static_cast<unsigned char>(before_caret.back())))
            return std::nullopt;
    }
    if (!after_caret.empty()) {
        unsigned char next = static_cast<unsigned char>(after_caret.front());
        if (is_ident_or_digit(next)) return std::nullopt;
        if (typed == '\'' && next == '\'') return std::nullopt;
        if (typed == '"' && next == '"') return std::nullopt;
    }

    AssistEditPlan plan;
    plan.insert_text.push_back(typed);
    plan.insert_text.push_back(*close);
    plan.caret_offset_in_insert = 1;
    return plan;
}

std::optional<AssistEditPlan> EditorAssist::plan_backspace(
    std::string_view before_caret, std::string_view after_caret, AssistConfig const& cfg) {
    if (before_caret.empty() || after_caret.empty()) return std::nullopt;
    char const left = before_caret.back();
    char const right = after_caret.front();
    for (auto const& pair : cfg.pairs) {
        if (pair.open == left && pair.close == right) {
            // Do not delete quote pair if left quote is escaped.
            if (left == right && is_escaped(before_caret.substr(0, before_caret.size() - 1))) return std::nullopt;
            AssistEditPlan plan;
            plan.erase_before = 1;
            plan.erase_after = 1;
            return plan;
        }
    }
    return std::nullopt;
}

std::optional<AssistEditPlan> EditorAssist::plan_newline(
    std::string_view line_before_caret, std::string_view after_caret,
    std::string_view eol, std::uint32_t indent_width) {
    if (eol.empty()) eol = "\n";
    auto const line = last_line_of(line_before_caret);
    auto const base_indent = leading_whitespace(line);

    std::string_view trim_end = line;
    while (!trim_end.empty() && (trim_end.back() == ' ' || trim_end.back() == '\t')) trim_end.remove_suffix(1);
    auto trim_after = after_caret;
    while (!trim_after.empty() && (trim_after.front() == ' ' || trim_after.front() == '\t')) trim_after.remove_prefix(1);

    std::string const one_indent(std::max<std::uint32_t>(1, indent_width), ' ');
    if (!trim_end.empty() && trim_end.back() == '{') {
        if (!trim_after.empty() && trim_after.front() == '}') {
            std::size_t const spaces_before_close = after_caret.size() - trim_after.size();
            AssistEditPlan plan;
            plan.erase_after = spaces_before_close;
            plan.insert_text = std::string{eol} + base_indent + one_indent;
            plan.caret_offset_in_insert = plan.insert_text.size();
            plan.insert_text += std::string{eol} + base_indent;
            return plan;
        }
        AssistEditPlan plan;
        plan.insert_text = std::string{eol} + base_indent + one_indent;
        plan.caret_offset_in_insert = plan.insert_text.size();
        return plan;
    }

    if (base_indent.empty()) return std::nullopt;
    AssistEditPlan plan;
    plan.insert_text = std::string{eol} + base_indent;
    plan.caret_offset_in_insert = plan.insert_text.size();
    return plan;
}

std::optional<TabIndentPlan> EditorAssist::plan_tab_indent(
    std::string_view text, std::size_t sel_start, std::size_t sel_end,
    bool unindent, std::uint32_t indent_width) {
    if (sel_start > sel_end || sel_end > text.size()) return std::nullopt;
    std::size_t const width = std::max<std::uint32_t>(1, indent_width);
    std::string const indent_str(width, ' ');

    // Collapsed caret with Tab inserts spaces (without needing line-level logic unless Shift+Tab).
    if (sel_start == sel_end && !unindent) {
        TabIndentPlan plan;
        plan.replace_start = sel_start;
        plan.replace_length = 0;
        plan.replacement = indent_str;
        plan.new_sel_start = sel_start + width;
        plan.new_sel_end = sel_start + width;
        return plan;
    }

    // Find start of first affected line.
    std::size_t line_start = sel_start;
    while (line_start > 0 && text[line_start - 1] != '\n' && text[line_start - 1] != '\r') --line_start;

    // If selection ends exactly at start of a line (after newline) with non-empty selection, exclude that trailing empty line.
    std::size_t effective_end = sel_end;
    if (sel_end > sel_start && (text[sel_end - 1] == '\n' || text[sel_end - 1] == '\r')) {
        --effective_end;
    }
    std::size_t block_end = effective_end;
    while (block_end < text.size() && text[block_end] != '\n' && text[block_end] != '\r') ++block_end;

    std::string replacement;
    std::size_t new_start = sel_start;
    std::size_t new_end = sel_end;
    std::size_t cursor = line_start;
    std::size_t first_delta = 0;
    bool first_line = true;
    std::ptrdiff_t total_delta = 0;
    bool any_change = false;

    while (cursor <= block_end) {
        std::size_t line_end = cursor;
        while (line_end < block_end && text[line_end] != '\n' && text[line_end] != '\r') ++line_end;
        auto line = text.substr(cursor, line_end - cursor);

        if (!unindent) {
            replacement += indent_str;
            replacement += line;
            total_delta += static_cast<std::ptrdiff_t>(width);
            if (first_line) first_delta = width;
            any_change = true;
        } else {
            std::size_t remove = 0;
            if (!line.empty() && line.front() == '\t') {
                remove = 1;
            } else {
                while (remove < width && remove < line.size() && line[remove] == ' ') ++remove;
            }
            replacement += line.substr(remove);
            total_delta -= static_cast<std::ptrdiff_t>(remove);
            if (first_line) first_delta = remove;
            if (remove > 0) any_change = true;
        }
        first_line = false;

        if (line_end >= block_end) break;
        // Preserve exact line break (\r\n, \n, or \r)
        if (text[line_end] == '\r' && line_end + 1 < text.size() && text[line_end + 1] == '\n') {
            replacement += "\r\n";
            cursor = line_end + 2;
        } else {
            replacement.push_back(text[line_end]);
            cursor = line_end + 1;
        }
        if (cursor > block_end) break;
    }

    if (!any_change) return std::nullopt;
    if (!unindent) {
        new_start = sel_start + first_delta;
        new_end = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(sel_end) + total_delta);
    } else {
        std::size_t const start_offset_in_first = sel_start - line_start;
        new_start = line_start + (start_offset_in_first > first_delta ? start_offset_in_first - first_delta : 0);
        auto signed_end = static_cast<std::ptrdiff_t>(sel_end) + total_delta;
        new_end = static_cast<std::size_t>(std::max<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(new_start), signed_end));
    }

    return TabIndentPlan{line_start, block_end - line_start, std::move(replacement), new_start, new_end};
}
}
