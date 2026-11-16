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
            if ((args.ModificationType() & text_flags) != 0) self->notify();
        }
    });
    availability_ = control_.CommittedSnapshotAvailabilityChanged(winrt::auto_revoke, [weak](auto const&, bool) {
        if (auto self = weak.lock(); self && !self->replacing_) self->notify();
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
void WinUIEditAdapter::update_read_only() const {
    if (!editor_) return;
    if (tracking_available() && committed_available()) {
        try { document_exhausted_ = document_exhausted_ || RevisionPolicy::exhausted(revision()); }
        catch (winrt::hresult_error const&) { document_exhausted_ = true; }
    }
    const bool readonly = workspace_read_only_ || document_exhausted_;
    if (editor_.ReadOnly() != readonly) editor_.ReadOnly(readonly);
}
void WinUIEditAdapter::set_workspace_read_only(bool readonly) {
    workspace_read_only_ = readonly; update_read_only();
}
void WinUIEditAdapter::notify() const {
    // Apply the real revision limit synchronously at native committed availability, before another edit.
    if (!replacing_) update_read_only();
    if (changed_) changed_();
}
void WinUIEditAdapter::replace_document(std::string_view utf8) {
    if (!control_.IsCommittedSnapshotTrackingAvailable()) winrt::throw_hresult(E_FAIL);
    if (!control_.IsCommittedSnapshotAvailable()) winrt::throw_hresult(E_PENDING);
    FileStore::validate_utf8(utf8);
    std::string loaded_baseline{utf8};
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
    replacing_ = true; document_exhausted_ = false;
    try {
        editor_.ReadOnly(false);
        editor_.ClearAll();
        if (!utf8.empty()) {
            editor_.AddTextFromBuffer(static_cast<std::int64_t>(utf8.size()), data);
        }
        editor_.EmptyUndoBuffer();
        editor_.SetSavePoint();
        editor_.CurrentPos(0);
        editor_.Anchor(0);
        document_id_ = text;
        native_revision_base_ = control_.CommittedRevision();
        document_revision_base_ = 1;
        saved_.record_written(std::move(loaded_baseline));
        saved_.match_native_savepoint();
    } catch (...) {
        replacing_ = false;
        throw;
    }
    replacing_ = false;
    notify();
}
void WinUIEditAdapter::restore_document(cpn::SessionDocument const& record, std::int64_t caret, std::int64_t anchor) {
    replace_document(record.source_text);
    document_id_ = std::wstring{winrt::to_hstring(record.document_id)};
    document_revision_base_ = record.revision;
    if (record.save_point) {
        saved_.restore_digest(record.save_point->source_digest);
        if (!saved_.compare_committed(record.revision, record.source_text)) { editor_.SetSavePoint(); saved_.match_native_savepoint(); }
    } else saved_.mark_unwritten();
    editor_.CurrentPos(caret); editor_.Anchor(anchor); notify();
}
void WinUIEditAdapter::set_language(std::wstring_view language) {
    control_.HighlightingLanguage(winrt::hstring{language});
}
void WinUIEditAdapter::set_tab_width(std::uint32_t width) {
    editor_.TabWidth(static_cast<std::int32_t>(width));
}
bool WinUIEditAdapter::mark_saved(EditorSnapshot written_snapshot) {
    if (written_snapshot.document_id != document_id_) winrt::throw_hresult(E_INVALIDARG);
    saved_.record_written(std::move(written_snapshot.utf8));
    if (!committed_available() || revision() != written_snapshot.revision) { notify(); return false; }
    editor_.SetSavePoint(); saved_.match_native_savepoint(); notify(); return true;
}
std::uint64_t WinUIEditAdapter::revision() const {
    try { return RevisionPolicy::document(control_.CommittedRevision(), native_revision_base_, document_revision_base_); }
    catch (std::overflow_error const&) { winrt::throw_hresult(E_BOUNDS); }

}
bool WinUIEditAdapter::committed_available() const { return control_.IsCommittedSnapshotAvailable(); }
bool WinUIEditAdapter::tracking_available() const { return control_.IsCommittedSnapshotTrackingAvailable(); }
bool WinUIEditAdapter::composition_active() const { return control_.IsCompositionActive(); }
void WinUIEditAdapter::focus() { control_.Focus(winrt::Microsoft::UI::Xaml::FocusState::Programmatic); }
bool WinUIEditAdapter::dirty() const {
    // A preedit or tracking failure must never permit a destructive action to assume a clean document.
    if (!tracking_available() || !committed_available()) return true;
    if (saved_.never_written()) return true;
    if (saved_.native_matches_disk()) return editor_.Modify();
    const auto current_revision = revision();
    if (auto cached = saved_.cached_dirty(current_revision)) return *cached;
    EditorSnapshot current;
    try { current = snapshot(); }
    catch (winrt::hresult_error const& error) {
        if (error.code() == E_PENDING || error.code() == E_FAIL) return true;
        throw;
    }
    const bool changed = saved_.compare_committed(current.revision, current.utf8);
    if (!changed) { editor_.SetSavePoint(); saved_.match_native_savepoint(); }
    return changed;
}
std::int64_t WinUIEditAdapter::length() const { return editor_.Length(); }
std::int64_t WinUIEditAdapter::caret_byte() const { return editor_.CurrentPos(); }
std::int64_t WinUIEditAdapter::anchor_byte() const { return editor_.Anchor(); }
EditorSnapshot WinUIEditAdapter::snapshot() const {
    std::uint64_t native_revision{};
    std::int64_t caret{}, anchor{};
    const auto bytes = control_.ReadCommittedText(native_revision, caret, anchor);
    std::uint64_t revision{};
    try { revision = RevisionPolicy::document(native_revision, native_revision_base_, document_revision_base_); }
    catch (std::overflow_error const&) { winrt::throw_hresult(E_BOUNDS); }
    std::string utf8;
    if (bytes.Length()) utf8.assign(reinterpret_cast<char const*>(bytes.data()), bytes.Length());
    FileStore::validate_utf8(utf8);
    return {document_id_, revision, std::move(utf8), caret, anchor};
}
void WinUIEditAdapter::dispose() noexcept {
    changed_ = {};
    availability_.revoke();
    modified_.revoke();
    reached_.revoke();
    left_.revoke();
    updated_.revoke();
    editor_ = nullptr;
    control_ = nullptr;
}
}
