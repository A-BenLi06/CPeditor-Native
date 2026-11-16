#pragma once
#include "pch.h"

namespace cpn::windows {
struct EditorProfile {
    std::wstring id;
    std::wstring label_resource;
    std::wstring highlighting_language;
    std::wstring default_extension;
    std::vector<std::wstring> extensions;
};
struct EditorConfiguration {
    std::wstring default_profile;
    std::wstring fallback_profile;
    std::uint32_t max_document_bytes{};
    std::uint32_t tab_width{};
    std::vector<EditorProfile> profiles;
    static EditorConfiguration parse(winrt::hstring const& json);
    [[nodiscard]] const EditorProfile& by_id(std::wstring_view id) const;
    [[nodiscard]] const EditorProfile& for_path(std::filesystem::path const& path) const;
};
}
