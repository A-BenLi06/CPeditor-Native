// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/checker.hpp"
#include "cpn/text.hpp"
#include <algorithm>
#include <vector>

namespace cpn {
namespace {
// Unicode White_Space property, matching modern QChar::isSpace semantics.
// This excludes BOM, zero-width space and Mongolian vowel separator.
bool is_space(char32_t value) {
    return (value >= 0x09 && value <= 0x0d) || value == 0x20 || value == 0x85 ||
           value == 0xa0 || value == 0x1680 || (value >= 0x2000 && value <= 0x200a) ||
           value == 0x2028 || value == 0x2029 || value == 0x202f || value == 0x205f || value == 0x3000;
}
std::vector<char32_t> normalized(const std::vector<Utf8Scalar>& input) {
    std::vector<char32_t> result;
    result.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        auto value = input[i].value;
        if (value == U'\r') {
            if (i + 1 < input.size() && input[i + 1].value == U'\n') ++i;
            value = U'\n';
        }
        result.push_back(value);
    }
    return result;
}
std::vector<std::vector<char32_t>> trimmed_lines(const std::vector<char32_t>& text) {
    std::vector<std::vector<char32_t>> lines(1);
    for (const auto scalar : text) {
        if (scalar == U'\n') lines.emplace_back();
        else lines.back().push_back(scalar);
    }
    for (auto& line : lines)
        while (!line.empty() && is_space(line.back())) line.pop_back();
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    return lines;
}
} // namespace
ComparisonResult compare_outputs(std::string_view output, std::string_view expected, ComparisonMode mode) {
    if (mode == ComparisonMode::byte_exact)
        return {output == expected ? ComparisonVerdict::accepted : ComparisonVerdict::wrong_answer, {}, {}};
    const auto out = decode_utf8(output), ans = decode_utf8(expected);
    if (out.error_offset || ans.error_offset)
        return {ComparisonVerdict::encoding_error, out.error_offset, ans.error_offset};
    const auto lhs = normalized(out.scalars), rhs = normalized(ans.scalars);
    const bool equal = mode == ComparisonMode::cp_editor_strict ? lhs == rhs :
                       trimmed_lines(lhs) == trimmed_lines(rhs);
    return {equal ? ComparisonVerdict::accepted : ComparisonVerdict::wrong_answer, {}, {}};
}
} // namespace cpn
