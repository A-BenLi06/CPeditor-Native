#include "pch.h"
#include "SnapshotJson.h"

namespace cpn::windows {
namespace {
    struct Fields {
        static constexpr auto format = L"format";
        static constexpr auto version = L"schemaVersion";
        static constexpr auto kind = L"snapshotKind";
        static constexpr auto document_id = L"documentId";
        static constexpr auto revision = L"revision";
        static constexpr auto byte_length = L"byteLength";
        static constexpr auto text = L"text";
        static constexpr auto path = L"sourcePath";
        static constexpr auto profile = L"profileId";
        static constexpr auto position_encoding = L"positionEncoding";
        static constexpr auto caret = L"caretByte";
        static constexpr auto anchor = L"anchorByte";
    };
}
std::string SnapshotJson::encode(EditorSnapshot const& snapshot,
                                std::filesystem::path const& path, std::wstring_view profile) {
    using namespace winrt::Windows::Data::Json;
    JsonObject result;
    const auto string = [&](auto key, winrt::hstring const& value) {
        result.SetNamedValue(key, JsonValue::CreateStringValue(value));
    };
    string(Fields::format, L"cpn.editor-snapshot");
    result.SetNamedValue(Fields::version, JsonValue::CreateNumberValue(1));
    string(Fields::kind, L"native-buffer");
    string(Fields::document_id, winrt::hstring{snapshot.document_id});
    string(Fields::revision, winrt::to_hstring(snapshot.revision));
    string(Fields::byte_length, winrt::to_hstring(snapshot.utf8.size()));
    string(Fields::text, winrt::to_hstring(snapshot.utf8));
    string(Fields::path, winrt::hstring{path.wstring()});
    string(Fields::profile, winrt::hstring{profile});
    string(Fields::position_encoding, L"utf-8");
    string(Fields::caret, winrt::to_hstring(snapshot.caret_byte));
    string(Fields::anchor, winrt::to_hstring(snapshot.anchor_byte));
    return winrt::to_string(result.Stringify());
}
}
