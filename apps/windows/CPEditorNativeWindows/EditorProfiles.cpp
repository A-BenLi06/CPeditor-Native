#include "pch.h"
#include "EditorProfiles.h"
#include <cwctype>
#include <cmath>
#include <unordered_set>

namespace cpn::windows {
namespace {
struct Fields {
    static constexpr auto version = L"schemaVersion";
    static constexpr auto default_profile = L"defaultProfile";
    static constexpr auto fallback_profile = L"fallbackProfile";
    static constexpr auto max_document_bytes = L"maxDocumentBytes";
    static constexpr auto tab_width = L"tabWidth";
    static constexpr auto profiles = L"profiles";
    static constexpr auto id = L"id";
    static constexpr auto label_resource = L"labelResource";
    static constexpr auto highlighting_language = L"highlightingLanguage";
    static constexpr auto default_extension = L"defaultExtension";
    static constexpr auto extensions = L"extensions";
};
std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return value;
}
}

EditorConfiguration EditorConfiguration::parse(winrt::hstring const& json) {
    using namespace winrt::Windows::Data::Json;
    const auto object = JsonObject::Parse(json);
    if (object.GetNamedNumber(Fields::version) != 1) {
        throw std::invalid_argument("Unsupported editor configuration version.");
    }
    EditorConfiguration result;
    result.default_profile = object.GetNamedString(Fields::default_profile);
    result.fallback_profile = object.GetNamedString(Fields::fallback_profile);
    const auto limit = object.GetNamedNumber(Fields::max_document_bytes);
    const auto tabs = object.GetNamedNumber(Fields::tab_width);
    if (!(limit >= 1 && limit <= std::numeric_limits<std::int32_t>::max() - 1) ||
        limit != std::floor(limit) || !(tabs >= 1 && tabs <= 32) || tabs != std::floor(tabs)) {
        throw std::invalid_argument("Invalid editor limits.");
    }
    result.max_document_bytes = static_cast<std::uint32_t>(limit);
    result.tab_width = static_cast<std::uint32_t>(tabs);
    std::unordered_set<std::wstring> assigned_extensions;
    for (auto const& value : object.GetNamedArray(Fields::profiles)) {
        auto item = value.GetObject();
        EditorProfile profile{
            std::wstring{item.GetNamedString(Fields::id)},
            std::wstring{item.GetNamedString(Fields::label_resource)},
            std::wstring{item.GetNamedString(Fields::highlighting_language)},
            std::wstring{item.GetNamedString(Fields::default_extension)}, {}};
        if (profile.id.empty() || profile.label_resource.empty() ||
            profile.default_extension.size() < 2 || profile.default_extension.front() != L'.') {
            throw std::invalid_argument("Invalid editor profile.");
        }
        if (std::any_of(result.profiles.begin(), result.profiles.end(),
                        [&](auto const& existing) { return existing.id == profile.id; })) {
            throw std::invalid_argument("Duplicate editor profile ID.");
        }
        for (auto const& extension : item.GetNamedArray(Fields::extensions)) {
            const std::wstring text{extension.GetString()};
            if (text.size() < 2 || text.front() != L'.') {
                throw std::invalid_argument("Invalid editor extension.");
            }
            auto normalized = lower(text);
            if (!assigned_extensions.insert(normalized).second) {
                throw std::invalid_argument("Duplicate editor extension mapping.");
            }
            profile.extensions.push_back(std::move(normalized));
        }
        if (std::find(profile.extensions.begin(), profile.extensions.end(),
                      lower(profile.default_extension)) == profile.extensions.end()) {
            throw std::invalid_argument("Default extension is not in the profile mapping.");
        }
        result.profiles.push_back(std::move(profile));
    }
    (void)result.by_id(result.default_profile);
    (void)result.by_id(result.fallback_profile);
    return result;
}

const EditorProfile& EditorConfiguration::by_id(std::wstring_view id) const {
    const auto found = std::find_if(profiles.begin(), profiles.end(),
                                   [&](auto const& profile) { return profile.id == id; });
    if (found == profiles.end()) throw std::invalid_argument("Unknown editor profile ID.");
    return *found;
}

const EditorProfile& EditorConfiguration::for_path(std::filesystem::path const& path) const {
    const auto extension = lower(path.extension().wstring());
    for (auto const& profile : profiles) {
        if (std::find(profile.extensions.begin(), profile.extensions.end(), extension) !=
            profile.extensions.end()) return profile;
    }
    return by_id(fallback_profile);
}
}
