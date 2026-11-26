#pragma once
#include "pch.h"
#include "EditorAssist.h"
#include "SaveBaseline.h"
#include "RevisionPolicy.h"
#include "cpn/session.hpp"

namespace cpn::windows {
struct EditorSnapshot {
    std::wstring document_id;
    std::uint64_t revision{};
    std::string utf8;
    std::int64_t caret_byte{};
    std::int64_t anchor_byte{};
};

// Platform-local adapter: no WinUI/Scintilla object enters the competition Core.
class WinUIEditAdapter final : public std::enable_shared_from_this<WinUIEditAdapter> {
public:
    static std::shared_ptr<WinUIEditAdapter> create(
        winrt::WinUIEditor::CodeEditorControl const&, std::function<void()> changed);
    ~WinUIEditAdapter();
    void replace_document(std::string_view utf8);
    void restore_document(cpn::SessionDocument const&, std::int64_t caret_byte, std::int64_t anchor_byte);
    void set_language(std::wstring_view language);
    void set_language_id(std::string language_id);
    void set_tab_width(std::uint32_t width);
    void set_word_wrap(bool enabled);
    void set_assist_enabled(bool enabled);
    void set_assist_config(AssistConfig config);
    void set_workspace_read_only(bool);
    [[nodiscard]] bool mark_saved(EditorSnapshot written_snapshot);
    void focus();
    void undo();
    void redo();
    [[nodiscard]] bool can_undo() const;
    [[nodiscard]] bool can_redo() const;
    void dispose() noexcept;
    [[nodiscard]] bool dirty() const;
    [[nodiscard]] std::uint64_t revision() const;
    [[nodiscard]] std::wstring const& document_id() const noexcept { return document_id_; }
    [[nodiscard]] bool committed_available() const;
    [[nodiscard]] bool tracking_available() const;
    [[nodiscard]] bool composition_active() const;
    [[nodiscard]] std::int64_t length() const;
    [[nodiscard]] std::int64_t caret_byte() const;
    [[nodiscard]] std::int64_t anchor_byte() const;
    [[nodiscard]] std::int64_t line_number() const;
    [[nodiscard]] std::int64_t column_number() const;
    [[nodiscard]] EditorSnapshot snapshot() const;
private:
    explicit WinUIEditAdapter(winrt::WinUIEditor::CodeEditorControl const&, std::function<void()>);
    void subscribe();
    void notify() const;
    void update_read_only() const;
    [[nodiscard]] std::string read_range_utf8(std::int64_t start, std::int64_t end) const;
    void handle_preview_key_down(winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);
    void handle_char_added(winrt::WinUIEditor::CharAddedEventArgs const&);

    winrt::WinUIEditor::CodeEditorControl control_{nullptr};
    winrt::WinUIEditor::Editor editor_{nullptr};
    winrt::WinUIEditor::Editor::Modified_revoker modified_;
    winrt::WinUIEditor::Editor::SavePointReached_revoker reached_;
    winrt::WinUIEditor::Editor::SavePointLeft_revoker left_;
    winrt::WinUIEditor::Editor::UpdateUI_revoker updated_;
    winrt::WinUIEditor::Editor::CharAdded_revoker char_added_;
    winrt::Microsoft::UI::Xaml::UIElement::PreviewKeyDown_revoker preview_key_down_;
    winrt::WinUIEditor::CodeEditorControl::CommittedSnapshotAvailabilityChanged_revoker availability_;
    bool replacing_{false};
    bool assisting_{false};
    bool workspace_read_only_{};
    bool assist_enabled_{true};
    std::uint32_t tab_width_{4};
    std::string language_id_{"cpp"};
    AssistConfig assist_config_;
    std::optional<std::string> pending_wrap_selection_;
    std::int64_t pending_wrap_pos_{-1};
    mutable bool document_exhausted_{};
    std::wstring document_id_;
    std::uint64_t native_revision_base_{0};
    std::uint64_t document_revision_base_{1};
    mutable SaveBaseline saved_;
    std::function<void()> changed_;
};
}
