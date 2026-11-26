#include "pch.h"
#include "WinUIEditAdapter.h"
#include "FileStore.h"
#include <objbase.h>

namespace cpn::windows {
namespace {
bool key_down(winrt::Windows::System::VirtualKey key) {
    using winrt::Microsoft::UI::Input::InputKeyboardSource;
    using winrt::Windows::UI::Core::CoreVirtualKeyStates;
    return (InputKeyboardSource::GetKeyStateForCurrentThread(key) & CoreVirtualKeyStates::Down) == CoreVirtualKeyStates::Down;
}
}

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
    editor_.UseTabs(false);
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
    preview_key_down_ = control_.PreviewKeyDown(winrt::auto_revoke, [weak](auto const&, winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
        if (auto self = weak.lock()) self->handle_preview_key_down(args);
    });
    char_added_ = editor_.CharAdded(winrt::auto_revoke, [weak](auto const&, winrt::WinUIEditor::CharAddedEventArgs const& args) {
        if (auto self = weak.lock()) self->handle_char_added(args);
    });
}
std::string WinUIEditAdapter::read_range_utf8(std::int64_t start, std::int64_t end) const {
    if (!editor_ || start >= end || start < 0) return {};
    editor_.TargetStart(start);
    editor_.TargetEnd(end);
    return winrt::to_string(editor_.GetTargetText());
}
void WinUIEditAdapter::handle_preview_key_down(winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
    pending_wrap_selection_.reset();
    pending_wrap_pos_ = -1;
    if (!assist_enabled_ || replacing_ || assisting_ || !editor_ || !control_) return;
    if (control_.IsCompositionActive() || editor_.ReadOnly()) return;
    if (key_down(winrt::Windows::System::VirtualKey::Control) ||
        key_down(winrt::Windows::System::VirtualKey::Menu) ||
        key_down(winrt::Windows::System::VirtualKey::LeftWindows) ||
        key_down(winrt::Windows::System::VirtualKey::RightWindows)) return;

    auto const key = args.Key();
    bool const shift = key_down(winrt::Windows::System::VirtualKey::Shift);
    auto const sel_start = editor_.SelectionStart();
    auto const sel_end = editor_.SelectionEnd();

    if (key == winrt::Windows::System::VirtualKey::Tab) {
        auto const first_line = editor_.LineFromPosition(sel_start);
        auto const last_line = editor_.LineFromPosition(sel_end);
        auto const block_start = editor_.PositionFromLine(first_line);
        auto const block_end = editor_.GetLineEndPosition(last_line);
        auto const text = read_range_utf8(block_start, block_end);
        auto const local_start = static_cast<std::size_t>(sel_start - block_start);
        auto const local_end = static_cast<std::size_t>(sel_end - block_start);
        if (auto plan = EditorAssist::plan_tab_indent(text, local_start, local_end, shift, tab_width_)) {
            assisting_ = true;
            try {
                editor_.BeginUndoAction();
                editor_.TargetStart(block_start + static_cast<std::int64_t>(plan->replace_start));
                editor_.TargetEnd(block_start + static_cast<std::int64_t>(plan->replace_start + plan->replace_length));
                auto const replacement = winrt::to_hstring(plan->replacement);
                editor_.ReplaceTarget(-1, replacement);
                editor_.SetSel(block_start + static_cast<std::int64_t>(plan->new_sel_start),
                               block_start + static_cast<std::int64_t>(plan->new_sel_end));
                editor_.EndUndoAction();
                args.Handled(true);
            } catch (...) {
                try { editor_.EndUndoAction(); } catch (...) {}
            }
            assisting_ = false;
        } else if (shift) {
            args.Handled(true);
        }
        return;
    }

    if (key == winrt::Windows::System::VirtualKey::Back && sel_start == sel_end && sel_start > 0) {
        auto const line = editor_.LineFromPosition(sel_start);
        auto const line_start = editor_.PositionFromLine(line);
        auto const line_end = editor_.GetLineEndPosition(line);
        auto const before = read_range_utf8(line_start, sel_start);
        auto const after = read_range_utf8(sel_start, line_end);
        if (auto plan = EditorAssist::plan_backspace(before, after, assist_config_)) {
            assisting_ = true;
            try {
                editor_.BeginUndoAction();
                editor_.DeleteRange(sel_start - static_cast<std::int64_t>(plan->erase_before),
                                    static_cast<std::int64_t>(plan->erase_before + plan->erase_after));
                editor_.EndUndoAction();
                args.Handled(true);
            } catch (...) {
                try { editor_.EndUndoAction(); } catch (...) {}
            }
            assisting_ = false;
        }
        return;
    }

    if (key == winrt::Windows::System::VirtualKey::Enter && !shift) {
        auto const line = editor_.LineFromPosition(sel_start);
        auto const line_start = editor_.PositionFromLine(line);
        auto const line_end = editor_.GetLineEndPosition(line);
        auto const before = read_range_utf8(line_start, sel_start);
        auto const after = read_range_utf8(sel_end, line_end);
        std::string_view eol = "\r\n";
        auto const eol_mode = editor_.EOLMode();
        if (eol_mode == winrt::WinUIEditor::EndOfLine::Lf) eol = "\n";
        else if (eol_mode == winrt::WinUIEditor::EndOfLine::Cr) eol = "\r";
        if (auto plan = EditorAssist::plan_newline(before, after, eol, tab_width_)) {
            assisting_ = true;
            try {
                editor_.BeginUndoAction();
                if (sel_start != sel_end) {
                    editor_.DeleteRange(sel_start, sel_end - sel_start);
                }
                if (plan->erase_after > 0) {
                    editor_.DeleteRange(sel_start, static_cast<std::int64_t>(plan->erase_after));
                }
                auto const text = winrt::to_hstring(plan->insert_text);
                editor_.InsertText(sel_start, text);
                auto const new_caret = sel_start + static_cast<std::int64_t>(plan->caret_offset_in_insert);
                editor_.SetSel(new_caret, new_caret);
                editor_.EndUndoAction();
                args.Handled(true);
            } catch (...) {
                try { editor_.EndUndoAction(); } catch (...) {}
            }
            assisting_ = false;
        }
        return;
    }

    if (sel_start != sel_end && (sel_end - sel_start) <= 65536) {
        pending_wrap_selection_ = read_range_utf8(sel_start, sel_end);
        pending_wrap_pos_ = sel_start;
    }
}

void WinUIEditAdapter::handle_char_added(winrt::WinUIEditor::CharAddedEventArgs const& args) {
    auto wrap_selection = std::exchange(pending_wrap_selection_, std::nullopt);
    auto wrap_pos = std::exchange(pending_wrap_pos_, -1);
    if (!assist_enabled_ || replacing_ || assisting_ || !editor_ || !control_) return;
    if (control_.IsCompositionActive() || editor_.ReadOnly()) return;

    auto const ch_int = args.Ch();
    if (ch_int < 0x20 || ch_int > 0x7E) return;
    char const typed = static_cast<char>(ch_int);

    auto const pos = editor_.CurrentPos();
    auto const sel_start = editor_.SelectionStart();
    auto const sel_end = editor_.SelectionEnd();
    if (sel_start != sel_end || pos <= 0) return;

    if (wrap_selection && !wrap_selection->empty() && pos == wrap_pos + 1) {
        if (auto plan = EditorAssist::plan_char_input("", "", *wrap_selection, typed, language_id_, assist_config_)) {
            assisting_ = true;
            try {
                editor_.BeginUndoAction();
                // Replace the single typed opener with the full wrapped string in one undoable step.
                auto const suffix = plan->insert_text.substr(1);
                editor_.InsertText(pos, winrt::to_hstring(suffix));
                auto const caret = wrap_pos + static_cast<std::int64_t>(plan->caret_offset_in_insert);
                editor_.SetSel(caret, caret);
                editor_.EndUndoAction();
            } catch (...) {
                try { editor_.EndUndoAction(); } catch (...) {}
            }
            assisting_ = false;
        }
        return;
    }

    auto const current_line = editor_.LineFromPosition(pos);
    auto const window_line = std::max<std::int64_t>(0, current_line - 64);
    auto const window_start = editor_.PositionFromLine(window_line);
    auto const line_end = editor_.GetLineEndPosition(current_line);
    auto const before = read_range_utf8(window_start, pos - 1);
    auto const after = read_range_utf8(pos, line_end);
    if (auto plan = EditorAssist::plan_char_input(before, after, "", typed, language_id_, assist_config_)) {
        assisting_ = true;
        try {
            editor_.BeginUndoAction();
            if (plan->erase_after > 0) {
                editor_.DeleteRange(pos, static_cast<std::int64_t>(plan->erase_after));
                editor_.SetSel(pos, pos);
            } else if (plan->insert_text.size() > 1) {
                editor_.InsertText(pos, winrt::to_hstring(plan->insert_text.substr(1)));
                editor_.SetSel(pos, pos);
            }
            editor_.EndUndoAction();
        } catch (...) {
            try { editor_.EndUndoAction(); } catch (...) {}
        }
        assisting_ = false;
    }
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
    pending_wrap_selection_.reset(); pending_wrap_pos_ = -1;
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
void WinUIEditAdapter::set_language_id(std::string language_id) {
    language_id_ = std::move(language_id);
}
void WinUIEditAdapter::set_tab_width(std::uint32_t width) {
    tab_width_ = std::max<std::uint32_t>(1, width);
    editor_.TabWidth(static_cast<std::int32_t>(tab_width_));
    editor_.Indent(static_cast<std::int32_t>(tab_width_));
    editor_.UseTabs(false);
}
void WinUIEditAdapter::set_word_wrap(bool enabled) {
    if (editor_) editor_.WrapMode(enabled ? winrt::WinUIEditor::Wrap::Word : winrt::WinUIEditor::Wrap::None);
}
void WinUIEditAdapter::set_assist_enabled(bool enabled) { assist_enabled_ = enabled; }
void WinUIEditAdapter::set_assist_config(AssistConfig config) { assist_config_ = std::move(config); }
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
void WinUIEditAdapter::undo() { if (editor_ && !editor_.ReadOnly() && editor_.CanUndo()) editor_.Undo(); }
void WinUIEditAdapter::redo() { if (editor_ && !editor_.ReadOnly() && editor_.CanRedo()) editor_.Redo(); }
bool WinUIEditAdapter::can_undo() const { return editor_ && editor_.CanUndo(); }
bool WinUIEditAdapter::can_redo() const { return editor_ && editor_.CanRedo(); }
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
std::int64_t WinUIEditAdapter::line_number() const {
    if (!editor_) return 1;
    return editor_.LineFromPosition(editor_.CurrentPos()) + 1;
}
std::int64_t WinUIEditAdapter::column_number() const {
    if (!editor_) return 1;
    return editor_.GetColumn(editor_.CurrentPos()) + 1;
}
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
    char_added_.revoke();
    preview_key_down_.revoke();
    editor_ = nullptr;
    control_ = nullptr;
}
}
