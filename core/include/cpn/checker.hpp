// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <optional>
#include <stop_token>
#include <string_view>

namespace cpn {

enum class ComparisonMode { cp_editor_strict, cp_editor_ignore_trailing_spaces, byte_exact };
enum class ComparisonVerdict { accepted, wrong_answer, encoding_error, cancelled };
struct ComparisonResult {
    ComparisonVerdict verdict;
    std::optional<std::size_t> invalid_output_offset;
    std::optional<std::size_t> invalid_expected_offset;
};

[[nodiscard]] ComparisonResult compare_outputs(std::string_view output,
                                              std::string_view expected,
                                              ComparisonMode,
                                              std::stop_token = {});
} // namespace cpn
