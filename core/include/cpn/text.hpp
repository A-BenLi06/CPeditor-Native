// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace cpn {

struct Utf8Scalar {
    char32_t value;
    std::size_t offset;
    std::size_t length;
};
struct Utf8Decode {
    std::vector<Utf8Scalar> scalars;
    std::optional<std::size_t> error_offset;
};
[[nodiscard]] Utf8Decode decode_utf8(std::string_view);

enum class PositionEncoding { utf8, utf16, utf32 };
struct TextPosition {
    std::size_t line;
    std::size_t character;
    bool operator==(const TextPosition&) const = default;
};

class TextIndex {
public:
    [[nodiscard]] static std::optional<TextIndex> make(std::string_view);
    [[nodiscard]] std::optional<std::size_t> byte_offset(TextPosition, PositionEncoding) const;
    [[nodiscard]] std::optional<TextPosition> position(std::size_t, PositionEncoding) const;
    [[nodiscard]] std::size_t line_count() const noexcept { return lines_.size(); }
private:
    struct Boundary { std::size_t byte, utf16, utf32; };
    struct Line { std::size_t start, end; std::vector<Boundary> boundaries; };
    std::vector<Line> lines_;
};

} // namespace cpn
