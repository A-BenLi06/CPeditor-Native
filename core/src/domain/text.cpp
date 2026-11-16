// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/text.hpp"

#include <algorithm>
#include <cstdint>

namespace cpn {
Utf8Decode decode_utf8(std::string_view input, std::stop_token stop) {
    Utf8Decode result;
    std::size_t next_check = 0;
    for (std::size_t i = 0; i < input.size();) {
        if (i >= next_check) {
            if (stop.stop_requested()) { result.cancelled = true; return result; }
            next_check = i + std::min<std::size_t>(4096, input.size() - i);
        }
        const auto first = static_cast<unsigned char>(input[i]);
        std::size_t length = 0;
        char32_t value = 0;
        if (first <= 0x7f) { length = 1; value = first; }
        else if (first >= 0xc2 && first <= 0xdf) { length = 2; value = first & 0x1f; }
        else if (first >= 0xe0 && first <= 0xef) { length = 3; value = first & 0x0f; }
        else if (first >= 0xf0 && first <= 0xf4) { length = 4; value = first & 0x07; }
        else { result.error_offset = i; return result; }
        if (length > input.size() - i) { result.error_offset = i; return result; }
        for (std::size_t j = 1; j < length; ++j) {
            const auto next = static_cast<unsigned char>(input[i + j]);
            if ((next & 0xc0) != 0x80) { result.error_offset = i + j; return result; }
            value = (value << 6) | (next & 0x3f);
        }
        if ((length == 2 && value < 0x80) || (length == 3 && value < 0x800) ||
            (length == 4 && value < 0x10000) || value > 0x10ffff ||
            (value >= 0xd800 && value <= 0xdfff)) {
            result.error_offset = i;
            return result;
        }
        result.scalars.push_back({value, i, length});
        i += length;
    }
    result.cancelled = stop.stop_requested();
    return result;
}

std::optional<TextIndex> TextIndex::make(std::string_view text) {
    const auto decoded = decode_utf8(text);
    if (decoded.error_offset) return std::nullopt;
    TextIndex index;
    Line current{0, 0, {{0, 0, 0}}};
    std::size_t utf16 = 0, utf32 = 0;
    for (std::size_t i = 0; i < decoded.scalars.size(); ++i) {
        const auto scalar = decoded.scalars[i];
        if (scalar.value == U'\r' || scalar.value == U'\n') {
            current.end = scalar.offset;
            index.lines_.push_back(std::move(current));
            auto next_start = scalar.offset + scalar.length;
            if (scalar.value == U'\r' && i + 1 < decoded.scalars.size() &&
                decoded.scalars[i + 1].value == U'\n') {
                const auto lf = decoded.scalars[++i];
                next_start = lf.offset + lf.length;
            }
            current = Line{next_start, next_start, {{next_start, 0, 0}}};
            utf16 = 0; utf32 = 0;
        } else {
            utf16 += scalar.value > 0xffff ? 2 : 1;
            ++utf32;
            current.boundaries.push_back({scalar.offset + scalar.length, utf16, utf32});
        }
    }
    current.end = text.size();
    index.lines_.push_back(std::move(current));
    return index;
}

std::optional<std::size_t> TextIndex::byte_offset(TextPosition pos, PositionEncoding encoding) const {
    if (pos.line >= lines_.size()) return std::nullopt;
    const auto& line = lines_[pos.line];
    for (const auto& boundary : line.boundaries) {
        const auto column = encoding == PositionEncoding::utf8 ? boundary.byte - line.start :
                            encoding == PositionEncoding::utf16 ? boundary.utf16 : boundary.utf32;
        if (column == pos.character) return boundary.byte;
        if (column > pos.character) break;
    }
    return std::nullopt;
}

std::optional<TextPosition> TextIndex::position(std::size_t byte, PositionEncoding encoding) const {
    auto found = std::upper_bound(lines_.begin(), lines_.end(), byte,
        [](auto value, const Line& line) { return value < line.start; });
    if (found == lines_.begin()) return std::nullopt;
    --found;
    if (byte > found->end) return std::nullopt;
    const auto line_number = static_cast<std::size_t>(found - lines_.begin());
    for (const auto& boundary : found->boundaries) {
        if (boundary.byte != byte) continue;
        const auto column = encoding == PositionEncoding::utf8 ? byte - found->start :
                            encoding == PositionEncoding::utf16 ? boundary.utf16 : boundary.utf32;
        return TextPosition{line_number, column};
    }
    return std::nullopt;
}
} // namespace cpn
