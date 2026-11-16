#pragma once
#include "pch.h"
#include "SaveBaseline.h"

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
    void set_language(std::wstring_view language);
    void set_tab_width(std::uint32_t width);
    [[nodiscard]] bool mark_saved(EditorSnapshot written_snapshot);
    void focus();
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
    [[nodiscard]] EditorSnapshot snapshot() const;
private:
    explicit WinUIEditAdapter(winrt::WinUIEditor::CodeEditorControl const&, std::function<void()>);
    void subscribe();
    void notify() const;
    winrt::WinUIEditor::CodeEditorControl control_{nullptr};
    winrt::WinUIEditor::Editor editor_{nullptr};
    winrt::WinUIEditor::Editor::Modified_revoker modified_;
    winrt::WinUIEditor::Editor::SavePointReached_revoker reached_;
    winrt::WinUIEditor::Editor::SavePointLeft_revoker left_;
    winrt::WinUIEditor::Editor::UpdateUI_revoker updated_;
    winrt::WinUIEditor::CodeEditorControl::CommittedSnapshotAvailabilityChanged_revoker availability_;
    bool replacing_{false};
    std::wstring document_id_;
    std::uint64_t native_revision_base_{0};
    mutable SaveBaseline saved_;
    std::function<void()> changed_;
};
}
