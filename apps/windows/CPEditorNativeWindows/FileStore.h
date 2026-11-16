#pragma once
#include "pch.h"

namespace cpn::windows {
struct LoadedDocument {
    std::string utf8;
    bool utf8_bom{false};
};
struct FileStore {
    [[nodiscard]] static LoadedDocument read(std::filesystem::path const&, std::uint32_t limit);
    static void write_atomic(std::filesystem::path const&, std::string_view bytes);
    static void validate_utf8(std::string_view bytes);
};
}
