#pragma once
#include "pch.h"

namespace cpn::windows {
struct UiStrings {
    static constexpr auto app_title = L"AppTitle";
    static constexpr auto window_title_format = L"WindowTitleFormat";
    static constexpr auto untitled = L"Untitled";
    static constexpr auto unsaved_suffix = L"UnsavedSuffix";
    static constexpr auto status_format = L"StatusFormat";
    static constexpr auto error_title = L"ErrorTitle";
    static constexpr auto error_open = L"OpenError";
    static constexpr auto error_save = L"SaveError";
    static constexpr auto error_config = L"ConfigurationError";
    static constexpr auto error_details = L"ErrorDetailsFormat";
    static constexpr auto close_button = L"Close";
    static constexpr auto confirm_title = L"UnsavedChangesTitle";
    static constexpr auto confirm_content = L"UnsavedChangesContent";
    static constexpr auto save_button = L"Save";
    static constexpr auto discard_button = L"Discard";
    static constexpr auto cancel_button = L"Cancel";
    static constexpr auto snapshot_files = L"SnapshotFiles";
    static constexpr auto initialization_failed = L"InitializationFailed";

    winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader loader;
    [[nodiscard]] winrt::hstring get(std::wstring_view key) const {
        return loader.GetString(winrt::hstring{key});
    }
    [[nodiscard]] static std::wstring format(std::wstring_view pattern,
                                             const std::vector<std::wstring>& values) {
        // Parse only the original resource, never placeholders inside inserted user values.
        std::wstring result;
        for (std::size_t offset = 0; offset < pattern.size();) {
            if (pattern[offset] == L'{') {
                const auto end = pattern.find(L'}', offset + 1);
                if (end != std::wstring_view::npos && end > offset + 1) {
                    std::size_t index = 0;
                    bool valid = true;
                    for (auto cursor = offset + 1; cursor < end; ++cursor) {
                        const auto digit = pattern[cursor];
                        if (digit < L'0' || digit > L'9' ||
                            index > (std::numeric_limits<std::size_t>::max() - 9) / 10) {
                            valid = false;
                            break;
                        }
                        index = index * 10 + static_cast<std::size_t>(digit - L'0');
                    }
                    if (valid && index < values.size()) {
                        result += values[index];
                        offset = end + 1;
                        continue;
                    }
                }
            }
            result += pattern[offset++];
        }
        return result;
    }
};
}
