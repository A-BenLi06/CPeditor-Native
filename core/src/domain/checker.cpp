// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/checker.hpp"
#include "cpn/text.hpp"
#include <algorithm>
#include <vector>

namespace cpn {
namespace {
constexpr std::size_t cancellation_batch = 4096;
struct Cancelled {};
class CancellationChecks {
public:
    explicit CancellationChecks(std::stop_token stop) : stop_(stop) { check_now(); }
    void step() {
        if (++steps_ == cancellation_batch) { steps_ = 0; check_now(); }
    }
    void check_now() const { if (stop_.stop_requested()) throw Cancelled{}; }
private:
    std::stop_token stop_;
    std::size_t steps_{0};
};
// Unicode White_Space property, matching modern QChar::isSpace semantics.
// This excludes BOM, zero-width space and Mongolian vowel separator.
bool is_space(char32_t value) {
    return (value >= 0x09 && value <= 0x0d) || value == 0x20 || value == 0x85 ||
           value == 0xa0 || value == 0x1680 || (value >= 0x2000 && value <= 0x200a) ||
           value == 0x2028 || value == 0x2029 || value == 0x202f || value == 0x205f || value == 0x3000;
}
std::vector<char32_t> normalized(const std::vector<Utf8Scalar>& input, CancellationChecks& checks) {
    std::vector<char32_t> result;
    result.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        checks.step();
        auto value = input[i].value;
        if (value == U'\r') {
            if (i + 1 < input.size() && input[i + 1].value == U'\n') ++i;
            value = U'\n';
        }
        result.push_back(value);
    }
    checks.check_now();
    return result;
}
std::vector<std::vector<char32_t>> trimmed_lines(const std::vector<char32_t>& text, CancellationChecks& checks) {
    std::vector<std::vector<char32_t>> lines(1);
    for (const auto scalar : text) {
        checks.step();
        if (scalar == U'\n') lines.emplace_back();
        else lines.back().push_back(scalar);
    }
    for (auto& line : lines) {
        checks.step();
        while (!line.empty() && is_space(line.back())) { checks.step(); line.pop_back(); }
    }
    while (!lines.empty() && lines.back().empty()) { checks.step(); lines.pop_back(); }
    checks.check_now();
    return lines;
}
bool equal_scalars(const std::vector<char32_t>& left, const std::vector<char32_t>& right, CancellationChecks& checks) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        checks.step();
        if (left[i] != right[i]) return false;
    }
    return true;
}
bool equal_lines(const std::vector<std::vector<char32_t>>& left, const std::vector<std::vector<char32_t>>& right, CancellationChecks& checks) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        checks.step();
        if (!equal_scalars(left[i], right[i], checks)) return false;
    }
    return true;
}
bool equal_bytes(std::string_view left, std::string_view right, CancellationChecks& checks) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size();) {
        checks.check_now();
        const auto count = std::min(cancellation_batch, left.size() - i);
        if (left.substr(i, count) != right.substr(i, count)) return false;
        i += count;
    }
    return true;
}
} // namespace
ComparisonResult compare_outputs(std::string_view output, std::string_view expected, ComparisonMode mode, std::stop_token stop) {
    try {
        CancellationChecks checks(stop);
        bool equal = false;
        if (mode == ComparisonMode::byte_exact) equal = equal_bytes(output, expected, checks);
        else {
            const auto out = decode_utf8(output, stop);
            if (out.cancelled) throw Cancelled{};
            const auto ans = decode_utf8(expected, stop);
            if (ans.cancelled) throw Cancelled{};
            checks.check_now();
            if (out.error_offset || ans.error_offset)
                return {ComparisonVerdict::encoding_error, out.error_offset, ans.error_offset};
            const auto lhs = normalized(out.scalars, checks), rhs = normalized(ans.scalars, checks);
            if (mode == ComparisonMode::cp_editor_strict) equal = equal_scalars(lhs, rhs, checks);
            else equal = equal_lines(trimmed_lines(lhs, checks), trimmed_lines(rhs, checks), checks);
        }
        checks.check_now();
        return {equal ? ComparisonVerdict::accepted : ComparisonVerdict::wrong_answer, {}, {}};
    } catch (const Cancelled&) {
        return {ComparisonVerdict::cancelled, {}, {}};
    }
}
} // namespace cpn
