#pragma once
#include "pch.h"
#include "WinUIEditAdapter.h"

namespace cpn::windows {
struct SnapshotJson {
    [[nodiscard]] static std::string encode(EditorSnapshot const&, std::filesystem::path const&,
                                             std::wstring_view profile);
};
}
