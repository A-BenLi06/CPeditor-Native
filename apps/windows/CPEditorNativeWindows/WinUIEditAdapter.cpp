#include "pch.h"
#include "WinUIEditAdapter.h"
#include "FileStore.h"
#include <objbase.h>

namespace cpn::windows {
std::shared_ptr<WinUIEditAdapter> WinUIEditAdapter::create(
    winrt::WinUIEditor::CodeEditorControl const& control, std::function<void()> changed) {
    auto result = std::shared_ptr<WinUIEditAdapter>(new WinUIEditAdapter(control, std::move(changed)));
    result->subscribe();
    return result;
}
WinUIEditAdapter::WinUIEditAdapter(winrt::WinUIEditor::CodeEditorControl const& control,
                                 std::function<void()> changed)
    : control_(control), editor_(control.Editor()), changed_(std::move(changed)) {
    control_.ApplyDefaultsToDocument();
    editor_.CodePage(CP_UTF8);
}
WinUIEditAdapter::~WinUIEditAdapter() { dispose(); }
void WinUIEditAdapter::subscribe() {
    const auto weak = weak_from_this();
    modified_ = editor_.Modified(winrt::auto_revoke, [weak](auto const&, winrt::WinUIEditor::ModifiedEventArgs const& args) {
        if (auto self = weak.lock(); self && !self->replacing_) {
            constexpr auto text_flags = static_cast<int>(winrt::WinUIEditor::ModificationFlags::InsertText) |
                                        static_cast<int>(winrt::WinUIEditor::ModificationFlags::DeleteText);
            if ((args.ModificationType() & text_flags) != 0) {
                if (self->revision_ == std::numeric_limits<std::uint64_t>::max()) {
                    winrt::throw_hresult(E_BOUNDS);
                }
                ++self->revision_;
                self->notify();
            }
        }
    });
    reached_ = editor_.SavePointReached(winrt::auto_revoke, [weak](auto const&, auto const&) {
        if (auto self = weak.lock(); self && !self->replacing_) self->notify();
    });
    left_ = editor_.SavePointLeft(winrt::auto_revoke, [weak](auto const&, auto const&) {
        if (auto self = weak.lock(); self && !self->replacing_) self->notify();
    });
    updated_ = editor_.UpdateUI(winrt::auto_revoke, [weak](auto const&, auto const&) {
        if (auto self = weak.lock(); self && !self->replacing_) self->notify();
    });
}
void WinUIEditAdapter::notify() const { if (changed_) changed_(); }
void WinUIEditAdapter::replace_document(std::string_view utf8) {
    FileStore::validate_utf8(utf8);
    if (utf8.size() >= std::numeric_limits<std::uint32_t>::max()) winrt::throw_hresult(E_BOUNDS);
    GUID identity{};
    winrt::check_hresult(CoCreateGuid(&identity));
    wchar_t text[40]{};
    if (StringFromGUID2(identity, text, static_cast<int>(std::size(text))) == 0) {
        winrt::throw_hresult(E_UNEXPECTED);
    }
    winrt::Windows::Storage::Streams::Buffer data{static_cast<std::uint32_t>(utf8.size())};
    data.Length(static_cast<std::uint32_t>(utf8.size()));
    if (!utf8.empty()) std::memcpy(data.data(), utf8.data(), utf8.size());
    replacing_ = true;
    try {
        editor_.ClearAll();
        if (!utf8.empty()) {
            editor_.AddTextFromBuffer(static_cast<std::int64_t>(utf8.size()), data);
        }
        editor_.EmptyUndoBuffer();
        editor_.SetSavePoint();
        editor_.CurrentPos(0);
        editor_.Anchor(0);
        document_id_ = text;
        revision_ = 1;
    } catch (...) {
        replacing_ = false;
        throw;
    }
    replacing_ = false;
    notify();
}
void WinUIEditAdapter::set_language(std::wstring_view language) {
    control_.HighlightingLanguage(winrt::hstring{language});
}
void WinUIEditAdapter::set_tab_width(std::uint32_t width) {
    editor_.TabWidth(static_cast<std::int32_t>(width));
}
void WinUIEditAdapter::mark_saved() { editor_.SetSavePoint(); notify(); }
void WinUIEditAdapter::focus() { control_.Focus(winrt::Microsoft::UI::Xaml::FocusState::Programmatic); }
bool WinUIEditAdapter::dirty() const { return editor_.Modify(); }
std::int64_t WinUIEditAdapter::length() const { return editor_.Length(); }
std::int64_t WinUIEditAdapter::caret_byte() const { return editor_.CurrentPos(); }
std::int64_t WinUIEditAdapter::anchor_byte() const { return editor_.Anchor(); }
EditorSnapshot WinUIEditAdapter::snapshot() const {
    const auto length = editor_.Length();
    if (length < 0 || length >= std::numeric_limits<std::uint32_t>::max()) winrt::throw_hresult(E_BOUNDS);
    winrt::Windows::Storage::Streams::Buffer bytes{static_cast<std::uint32_t>(length + 1)};
    bytes.Length(static_cast<std::uint32_t>(length + 1));
    const auto copied = editor_.GetTextWriteBuffer(length + 1, bytes);
    if (copied != length) winrt::throw_hresult(E_UNEXPECTED);
    std::string utf8{reinterpret_cast<char const*>(bytes.data()), static_cast<std::size_t>(length)};
    FileStore::validate_utf8(utf8);
    return {document_id_, revision_, std::move(utf8),
            editor_.CurrentPos(), editor_.Anchor()};
}
void WinUIEditAdapter::dispose() noexcept {
    changed_ = {};
    modified_.revoke();
    reached_.revoke();
    left_.revoke();
    updated_.revoke();
    editor_ = nullptr;
    control_ = nullptr;
}
}
