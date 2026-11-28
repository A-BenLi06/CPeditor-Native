#include "pch.h"
#include "MainWindow.xaml.h"
#include "cpn/companion.hpp"
#include "cpn/digest.hpp"
#include <set>
#include <sstream>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Storage::Pickers;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
using cpn::windows::CompanionModel;
using cpn::windows::CompanionWorker;
using cpn::windows::UiStrings;

namespace winrt::CPEditorNativeWindows::implementation {
namespace {
Border make_preview_card(hstring const& title, hstring const& input_label, hstring const& input_text,
                         hstring const& expected_label, hstring const& expected_text) {
    Border card;
    card.CornerRadius(CornerRadius{8});
    card.BorderThickness(Thickness{1});
    card.BorderBrush(Application::Current().Resources().Lookup(box_value(L"CardStrokeColorDefaultBrush")).as<Brush>());
    card.Background(Application::Current().Resources().Lookup(box_value(L"CardBackgroundFillColorDefaultBrush")).as<Brush>());
    card.Padding(Thickness{12, 10, 12, 12});

    StackPanel root;
    root.Spacing(6);
    TextBlock hdr;
    hdr.Text(title);
    hdr.FontSize(12);
    hdr.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
    root.Children().Append(hdr);

    Grid cols;
    cols.ColumnSpacing(8);
    ColumnDefinition c0, c1;
    c0.Width(GridLength{1, GridUnitType::Star});
    c1.Width(GridLength{1, GridUnitType::Star});
    cols.ColumnDefinitions().Append(c0);
    cols.ColumnDefinitions().Append(c1);

    auto const mono = FontFamily{L"Cascadia Mono, Consolas, Courier New"};
    auto make_box = [&](hstring const& lbl, hstring const& body) {
        StackPanel sp;
        sp.Spacing(2);
        TextBlock l;
        l.Text(lbl);
        l.FontSize(11);
        l.Opacity(0.7);
        TextBox b;
        b.Text(body);
        b.IsReadOnly(true);
        b.AcceptsReturn(true);
        b.TextWrapping(TextWrapping::NoWrap);
        b.FontFamily(mono);
        b.FontSize(11);
        b.MaxHeight(120);
        sp.Children().Append(l);
        sp.Children().Append(b);
        return sp;
    };
    auto left = make_box(input_label, input_text);
    auto right = make_box(expected_label, expected_text);
    Grid::SetColumn(left, 0);
    Grid::SetColumn(right, 1);
    cols.Children().Append(left);
    cols.Children().Append(right);
    root.Children().Append(cols);
    card.Child(root);
    return card;
}
}

void MainWindow::InitializeCompanionStorage() {
    try {
        auto restored = CompanionModel::binding(workspace_preserved_preferences_, companion_configuration_.storage_namespace);
        companion_binding_ = restored ? std::move(restored) : std::make_optional(cpn::windows::CompanionBinding{workspace_id_, companion_configuration_.paths(recovery_paths_.recovery_file)});
        ValidateCompanionStorage(); companion_available_ = true;
        companion_notice_ = std::wstring{strings_.get(L"CompanionReady")};
    } catch (...) {
        companion_available_ = false;
        companion_notice_ = std::wstring{strings_.get(L"CompanionStorageUnavailable")} + L" " + std::wstring{CurrentError()};
    }
    UpdateCompanionControls();
}
void MainWindow::ValidateCompanionStorage() const {
    if (!companion_binding_ || companion_binding_->workspace_id != workspace_id_) throw std::invalid_argument("Companion storage belongs to another workspace.");
    std::vector<std::filesystem::path> reserved{recovery_paths_.recovery_file, recovery_paths_.backup_file, recovery_paths_.lock_file, recovery_location_file_};
    for (auto const& document : documents_) if (!document->path.empty()) reserved.push_back(document->path);
    CompanionModel::validate_paths(companion_binding_->paths, reserved);
    if (!source_lock_directory_.empty()) {
        const auto directory = std::filesystem::weakly_canonical(std::filesystem::absolute(source_lock_directory_));
        for (auto const* storage : {&companion_binding_->paths.inbox_file, &companion_binding_->paths.backup_file, &companion_binding_->paths.lock_file}) {
            for (auto ancestor = cpn::windows::SourceStore::resolve(*storage); !ancestor.empty();) {
                if (CompareStringOrdinal(ancestor.c_str(), -1, directory.c_str(), -1, TRUE) == CSTR_EQUAL) throw std::invalid_argument("Companion storage is inside the source lock directory.");
                auto parent = ancestor.parent_path(); if (parent == ancestor) break; ancestor = std::move(parent);
            }
        }
    }
}
IAsyncOperation<bool> MainWindow::EnsureCompanionStorageAsync() {
    if (!companion_available_ || !companion_binding_ || shutting_down_) co_return false;
    if (recovery_saving_) {
        apartment_context ui;
        co_await resume_on_signal(recovery_idle_.get()); co_await ui;
        if (shutting_down_ || recovery_copying_) co_return false;
    }
    ValidateCompanionStorage();
    auto existing = CompanionModel::binding(workspace_preserved_preferences_, companion_configuration_.storage_namespace);
    if (existing && !CompanionModel::same_binding(*existing, *companion_binding_)) throw std::invalid_argument("Companion storage owner changed.");
    if (existing) {
        auto worker = session_worker_; const auto paths = recovery_paths_; const auto expected = recovery_disk_digest_;
        const auto binding = *companion_binding_; const auto ns = companion_configuration_.storage_namespace;
        cpn::SessionLoadResult observed; apartment_context ui; std::exception_ptr failure;
        co_await resume_background();
        try { observed = cpn::windows::RecoveryModel::load_response(worker->load(cpn::path_to_utf8(paths.recovery_file)).get()); }
        catch (...) { failure = std::current_exception(); }
        co_await ui; if (failure) std::rethrow_exception(failure);
        if (paths.recovery_file != recovery_paths_.recovery_file || expected != recovery_disk_digest_) {
            companion_notice_ = std::wstring{strings_.get(L"CompanionAwaitingRecovery")}; co_return false;
        }
        if (observed.status == cpn::SessionLoadStatus::loaded && observed.disk_digest == expected) {
            auto saved_owner = CompanionModel::binding(observed.session->editor_preferences, ns);
            if (saved_owner && CompanionModel::same_binding(*saved_owner, binding)) co_return true;
        } else if (observed.status != cpn::SessionLoadStatus::missing || expected) {
            throw hresult_error{E_FAIL, strings_.get(L"CompanionAwaitingRecovery")};
        }
    }
    if (!CompanionModel::can_create_storage_binding(recovery_revision_)) {
        companion_notice_ = std::wstring{strings_.get(L"CompanionStorageRevisionUnavailable")};
        co_return false;
    }
    if (!existing) {
        PersistentOperation operation{*this};
        auto next = workspace_preserved_preferences_;
        if (next.size() >= cpn::SessionBounds{}.max_preferences_per_owner) throw hresult_error{E_BOUNDS, strings_.get(L"CompanionStorageUnavailable")};
        next.push_back(CompanionModel::binding_preference(*companion_binding_, companion_configuration_.storage_namespace));
        workspace_preserved_preferences_.swap(next); ScheduleRecoverySave(); operation.finish();
    }
    if (!(co_await FlushRecoveryAsync())) {
        companion_notice_ = std::wstring{strings_.get(L"CompanionAwaitingRecovery")}; co_return false;
    }
    auto persisted = cpn::parse_workspace_session(*recovery_saved_content_);
    auto owner = CompanionModel::binding(persisted.editor_preferences, companion_configuration_.storage_namespace);
    co_return owner && CompanionModel::same_binding(*owner, *companion_binding_);
}
hstring MainWindow::CompanionStatus(std::string const& code, std::string const& detail) const {
    auto label = companion_configuration_.status_labels.find(code);
    auto text = label == companion_configuration_.status_labels.end() ?
        UiStrings::format(strings_.get(L"CompanionStatusFormat"), {std::wstring{to_hstring(code)}}) : std::wstring{strings_.get(to_hstring(label->second))};
    if (!detail.empty()) text += L" — " + std::wstring{to_hstring(detail)};
    return hstring{text};
}
cpn::CompanionInboxItem const* MainWindow::SelectedCompanionItem() {
    const auto index = CompanionItems().SelectedIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= companion_visible_indices_.size()) return nullptr;
    auto const real_index = companion_visible_indices_[static_cast<std::size_t>(index)];
    return real_index < companion_page_.items.size() ? &companion_page_.items[real_index] : nullptr;
}
void MainWindow::UpdateCompanionControls() {
    const bool ready = initialized_ && companion_available_ && companion_worker_ && !shutting_down_ && !busy_ && !companion_operating_ && !recovery_copying_ && !persistence_gate_.final_active();
    ImportCompanionButton().IsEnabled(ready && WorkspaceMutable());
    RefreshCompanionButton().IsEnabled(ready); MoreCompanionButton().IsEnabled(ready && companion_page_.has_more);
    StartCompanionButton().IsEnabled(ready && !companion_listener_.running);
    StopCompanionButton().IsEnabled(ready && companion_listener_.has_listener);
    auto item = SelectedCompanionItem();
    bool may_apply = WorkspaceMutable();
    if (!may_apply && item && companion_page_.inbox_id) {
        try {
            cpn::WorkspaceSession session; session.editor_preferences = workspace_preserved_preferences_;
            may_apply = cpn::find_import_application_receipt(session, *companion_page_.inbox_id, item->import_id).has_value();
        } catch (...) { may_apply = false; }
    }
    ApplyCompanionButton().IsEnabled(ready && may_apply && item && item->state == cpn::CompanionInboxState::pending);
    CompanionNotice().Text(hstring{companion_notice_});
    CompanionListenerStatus().Text(companion_listener_.running
        ? hstring{UiStrings::format(strings_.get(L"CompanionListenerFormat"), {std::to_wstring(companion_listener_.port)})}
        : CompanionStatus(companion_listener_.status.empty() ? "NotStarted" : companion_listener_.status,
            companion_listener_.failure_detail.empty() ? companion_listener_.detail : companion_listener_.failure_detail));

    std::size_t pending_count = 0;
    for (auto const& row : companion_page_.items) {
        if (row.state == cpn::CompanionInboxState::pending) ++pending_count;
    }
    if (pending_count > 0) {
        CompanionBtnLabel().Text(hstring{std::to_wstring(pending_count)});
        CompanionBtnLabel().Visibility(Visibility::Visible);
    } else {
        CompanionBtnLabel().Text(L"");
        CompanionBtnLabel().Visibility(Visibility::Collapsed);
    }

    if (companion_listener_.running) {
        StatusCompanionText().Text(hstring{UiStrings::format(strings_.get(L"StatusBarCompanionActiveFormat"), {std::to_wstring(companion_listener_.port)})});
        StatusCompanionText().Visibility(Visibility::Visible);
    } else {
        StatusCompanionText().Visibility(Visibility::Collapsed);
    }

    auto const target_item = companion_filter_ == CompanionFilter::Pending ? CompanionFilterPendingItem()
        : (companion_filter_ == CompanionFilter::Applied ? CompanionFilterAppliedItem() : CompanionFilterAllItem());
    if (CompanionFilterBar().SelectedItem() != target_item) {
        CompanionFilterBar().SelectedItem(target_item);
    }
}
void MainWindow::CompanionFilterBar_SelectionChanged(SelectorBar const& sender, SelectorBarSelectionChangedEventArgs const&) {
    auto const selected = sender.SelectedItem();
    if (selected == CompanionFilterPendingItem()) companion_filter_ = CompanionFilter::Pending;
    else if (selected == CompanionFilterAppliedItem()) companion_filter_ = CompanionFilter::Applied;
    else companion_filter_ = CompanionFilter::All;
    RenderCompanionItems();
}
void MainWindow::RenderCompanionItems() {
    std::optional<std::string> selected;
    if (auto item = SelectedCompanionItem()) selected = item->import_id;
    CompanionItems().Items().Clear();
    companion_visible_indices_.clear();
    std::int32_t selected_index = -1;
    for (std::size_t i = 0; i < companion_page_.items.size(); ++i) {
        auto const& item = companion_page_.items[i];
        if (companion_filter_ == CompanionFilter::Pending && item.state != cpn::CompanionInboxState::pending) continue;
        if (companion_filter_ == CompanionFilter::Applied && item.state != cpn::CompanionInboxState::applied) continue;

        auto const visible_pos = companion_visible_indices_.size();
        companion_visible_indices_.push_back(i);

        StackPanel row_panel;
        row_panel.Padding(Thickness{4, 4, 4, 4});
        row_panel.Spacing(2);
        TextBlock title_blk;
        title_blk.Text(to_hstring(CompanionModel::preview(item.candidate.name, companion_configuration_.preview_bytes)));
        title_blk.FontSize(13);
        title_blk.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
        title_blk.TextTrimming(TextTrimming::CharacterEllipsis);
        TextBlock sub_blk;
        auto const state = strings_.get(item.state == cpn::CompanionInboxState::pending ? L"CompanionPending" : L"CompanionApplied");
        sub_blk.Text(hstring{UiStrings::format(strings_.get(L"CompanionRowSubtitleFormat"), {
            std::wstring{state}, std::to_wstring(item.candidate.tests.size())})});
        sub_blk.FontSize(11);
        sub_blk.Opacity(0.75);
        row_panel.Children().Append(title_blk);
        row_panel.Children().Append(sub_blk);
        CompanionItems().Items().Append(row_panel);
        if (selected && item.import_id == *selected) selected_index = static_cast<std::int32_t>(visible_pos);
    }
    if (selected_index < 0 && !companion_visible_indices_.empty()) selected_index = 0;
    CompanionItems().SelectedIndex(selected_index);
    Companion_SelectionChanged(nullptr, nullptr);
    UpdateCompanionControls();
}
void MainWindow::Companion_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&) {
    CompanionPreviewPanel().Children().Clear();
    auto const* item = SelectedCompanionItem();
    if (!item) {
        CompanionDetailTitle().Text(L"");
        CompanionDetailMeta().Text(strings_.get(L"CompanionNoSelection"));
        UpdateCompanionControls();
        return;
    }
    auto const& candidate = item->candidate;
    const auto bounded = [&](std::string const& bytes) {
        return std::wstring{to_hstring(CompanionModel::preview(bytes, companion_configuration_.preview_bytes))};
    };
    CompanionDetailTitle().Text(hstring{bounded(candidate.name)});
    auto meta = UiStrings::format(strings_.get(L"CompanionMetadataFormat"), {
        bounded(candidate.group), bounded(candidate.url), std::to_wstring(candidate.tests.size()), bounded(item->import_id)});
    if (candidate.source_limits.time_limit_ms > 0 || candidate.source_limits.memory_limit_mb > 0) {
        meta += L"\n" + UiStrings::format(strings_.get(L"CompanionSourceLimitsFormat"), {
            std::to_wstring(candidate.source_limits.time_limit_ms), std::to_wstring(candidate.source_limits.memory_limit_mb)});
    }
    if (candidate.batch) {
        meta += L"\n" + UiStrings::format(strings_.get(L"CompanionBatchFormat"), {bounded(candidate.batch->id), std::to_wstring(candidate.batch->size)});
    }
    CompanionDetailMeta().Text(hstring{meta});

    for (auto const& issue : candidate.ordinary_stdio_issues) {
        InfoBar bar;
        bar.IsOpen(true);
        bar.IsClosable(false);
        bar.Severity(InfoBarSeverity::Warning);
        const auto label = companion_configuration_.issue_labels.find(cpn::companion_stdio_issue_name(issue.code));
        bar.Message(hstring{UiStrings::format(strings_.get(
            label == companion_configuration_.issue_labels.end() ? L"CompanionIssueUnknown" : to_hstring(label->second)),
            {bounded(issue.value)})});
        CompanionPreviewPanel().Children().Append(bar);
    }

    std::size_t index{};
    for (auto const& sample : candidate.tests) {
        if (index >= companion_configuration_.preview_samples) {
            TextBlock trunc;
            trunc.Text(strings_.get(L"CompanionPreviewTruncated"));
            trunc.FontSize(11);
            trunc.Opacity(0.75);
            CompanionPreviewPanel().Children().Append(trunc);
            break;
        }
        auto hdr = hstring{UiStrings::format(strings_.get(L"SampleHeaderFormat"), {std::to_wstring(++index)})};
        CompanionPreviewPanel().Children().Append(make_preview_card(
            hdr, strings_.get(L"TestInput"), hstring{bounded(sample.input)},
            strings_.get(L"TestExpected"), hstring{bounded(sample.output)}));
    }

    Expander payload_exp;
    payload_exp.Header(box_value(hstring{UiStrings::format(strings_.get(L"CompanionPayloadFormat"), {std::to_wstring(candidate.original_json.size())})}));
    payload_exp.HorizontalAlignment(HorizontalAlignment::Stretch);
    TextBox payload_box;
    payload_box.IsReadOnly(true);
    payload_box.AcceptsReturn(true);
    payload_box.TextWrapping(TextWrapping::Wrap);
    payload_box.FontFamily(FontFamily{L"Cascadia Mono, Consolas, Courier New"});
    payload_box.FontSize(11);
    payload_box.MaxHeight(160);
    payload_box.Text(hstring{bounded(candidate.original_json)});
    payload_exp.Content(payload_box);
    CompanionPreviewPanel().Children().Append(payload_exp);

    UpdateCompanionControls();
}
IAsyncAction MainWindow::ReadCompanionPageAsync(bool reset) {
    ValidateCompanionStorage();
    auto binding = *companion_binding_; auto config = companion_configuration_; auto worker = companion_worker_;
    const auto after = reset ? 0 : companion_page_.after;
    cpn::windows::CompanionPage page; cpn::windows::CompanionListenerResponse listener;
    apartment_context ui; std::exception_ptr failure;
    co_await resume_background();
    try {
        page = CompanionModel::page_response(worker->request(CompanionWorker::Operation::page, CompanionModel::page_request(binding.workspace_id, binding.paths, after, config)).get());
        listener = CompanionModel::listener_response(worker->request(CompanionWorker::Operation::listener_info, CompanionModel::version_request()).get());
    } catch (...) { failure = std::current_exception(); }
    co_await ui; if (failure) std::rethrow_exception(failure);
    if (shutting_down_) co_return;
    if (!companion_page_.install(std::move(page), workspace_id_, reset)) { co_await ReadCompanionPageAsync(true); co_return; }
    companion_listener_ = std::move(listener); RenderCompanionItems();
}
void MainWindow::InstallCompanionDelta(cpn::windows::CompanionDelta const& delta, cpn::WorkspaceSession const& base) {
    if (!WorkspaceMutable() || documents_.size() >= runner_configuration_.maximum_documents) throw hresult_error{E_BOUNDS, strings_.get(L"CompanionRevisionUnavailable")};
    auto current = CaptureWorkspace(); if (!current) throw hresult_error{E_PENDING, strings_.get(L"FinishCompositionBeforeSaving")};
    auto merged = CompanionModel::merge(*current, base, delta);
    std::vector<std::string> profiles; for (auto const& profile : configuration_.profiles) profiles.push_back(to_string(hstring{profile.id}));
    (void)cpn::windows::RecoveryModel::prepare(merged, recovery_configuration_, runner_configuration_, profiles, to_string(hstring{configuration_.default_profile}));
    auto preferences = std::move(merged.editor_preferences);
    AddRestoredDocument(delta.document, delta.preferences);
    workspace_preserved_preferences_.swap(preferences);
    ScheduleRecoverySave();
}
IAsyncAction MainWindow::ApplyCompanionAsync(cpn::CompanionInboxItem item) {
    if (!companion_page_.inbox_id) throw hresult_error{E_PENDING, strings_.get(L"CompanionRefreshRequired")};
    const auto inbox_id = *companion_page_.inbox_id;
    if (!(co_await EnsureCompanionStorageAsync())) co_return;
    std::unique_ptr<PersistentOperation> operation;
    if (WorkspaceMutable()) operation = std::make_unique<PersistentOperation>(*this);
    auto base = CaptureWorkspace(); if (!base) throw hresult_error{E_PENDING, strings_.get(L"FinishCompositionBeforeSaving")};
    auto prior_receipt = cpn::find_import_application_receipt(*base, inbox_id, item.import_id);
    if (!operation && !prior_receipt) throw hresult_error{E_BOUNDS, strings_.get(L"CompanionRevisionUnavailable")};
    cpn::ImportDocumentDefaults defaults; defaults.source_text = companion_configuration_.source_template;
    defaults.language_id = to_string(hstring{configuration_.default_profile}); defaults.toolchain_profile_id = runner_configuration_.default_toolchain(*defaults.language_id);
    cpn::windows::DocumentPreferences prefs; prefs.comparison = runner_configuration_.default_comparison;
    defaults.document_preferences.push_back(cpn::windows::RecoveryModel::document_preferences(prefs, recovery_configuration_.document_namespace));
    auto binding = *companion_binding_; auto config = companion_configuration_; auto recovery = recovery_configuration_; auto runner_defaults = runner_configuration_; auto worker = companion_worker_;
    std::vector<std::string> profiles; for (auto const& profile : configuration_.profiles) profiles.push_back(to_string(hstring{profile.id}));
    const auto default_profile = to_string(hstring{configuration_.default_profile});
    cpn::windows::CompanionPreparation prepared; std::optional<cpn::windows::CompanionDelta> delta;
    apartment_context ui; std::exception_ptr failure;
    co_await resume_background();
    try {
        prepared = CompanionModel::prepare_response(worker->request(CompanionWorker::Operation::prepare,
            CompanionModel::prepare_request(binding.workspace_id, binding.paths, item.import_id, *base, defaults)).get());
        if (prepared.status == "Prepared") delta = CompanionModel::delta(*base, prepared, inbox_id, item.import_id, recovery, runner_defaults, profiles, default_profile);
        else if (prepared.status == "AlreadyApplied" && (!prior_receipt || !prepared.receipt || !CompanionModel::same_receipt(*prior_receipt, *prepared.receipt) ||
            cpn::serialize_workspace_session(*prepared.session) != cpn::serialize_workspace_session(*base))) throw std::invalid_argument("Companion replay receipt differs from the live workspace.");
    } catch (...) { failure = std::current_exception(); }
    co_await ui; if (failure) std::rethrow_exception(failure);
    if (shutting_down_) co_return;
    if (delta) {
        if (!operation || !persistence_gate_.accepts(operation->generation)) throw hresult_error{E_PENDING, strings_.get(L"PersistentOperationUnavailable")};
        InstallCompanionDelta(*delta, *base);
    } else if (prepared.status != "AlreadyApplied") {
        companion_notice_ = std::wstring{CompanionStatus(prepared.status, prepared.detail)}; co_return;
    }
    if (operation) operation->finish();
    if (!(co_await FlushRecoveryAsync())) { companion_notice_ = std::wstring{strings_.get(L"CompanionAwaitingRecovery")}; co_return; }
    const auto paths = recovery_paths_;
    cpn::windows::CompanionWriteResponse marked; failure = {};
    co_await resume_background();
    try { marked = CompanionModel::write_response(worker->request(CompanionWorker::Operation::mark_applied, CompanionModel::mark_request(binding.workspace_id, binding.paths, item.import_id, paths, config)).get()); }
    catch (...) { failure = std::current_exception(); }
    co_await ui; if (failure) std::rethrow_exception(failure);
    companion_notice_ = std::wstring{CompanionStatus(marked.status, marked.detail)}; co_await ReadCompanionPageAsync(true);
}
IAsyncAction MainWindow::CompanionAsync(CompanionCommand command) {
    auto lifetime = get_strong();
    if (!initialized_ || !companion_available_ || !companion_worker_ || busy_ || shutting_down_ || companion_operating_ || recovery_copying_ || persistence_gate_.final_active()) co_return;
    companion_operating_ = true; UpdateCompanionControls(); hstring error;
    try {
        switch (command) {
        case CompanionCommand::Refresh: co_await ReadCompanionPageAsync(true); break;
        case CompanionCommand::More: if (companion_page_.has_more) co_await ReadCompanionPageAsync(false); break;
        case CompanionCommand::Apply: if (auto item = SelectedCompanionItem()) co_await ApplyCompanionAsync(*item); break;
        case CompanionCommand::Import: {
            if (!WorkspaceMutable() || !(co_await EnsureCompanionStorageAsync())) break;
            if (!WorkspaceMutable()) { companion_notice_ = std::wstring{strings_.get(L"CompanionRevisionUnavailable")}; break; }
            FileOpenPicker picker; check_hresult(picker.as<IInitializeWithWindow>()->Initialize(WindowHandle()));
            picker.SuggestedStartLocation(PickerLocationId::Downloads); picker.FileTypeFilter().Append(L".json");
            auto file = co_await picker.PickSingleFileAsync(); if (!file) break;
            const auto path = cpn::windows::SourceStore::resolve(std::filesystem::path{std::wstring{file.Path()}});
            if (IsReservedSourcePath(path)) throw hresult_error{E_INVALIDARG, strings_.get(L"ReservedRecoveryPath")};
            auto worker = companion_worker_; auto binding = *companion_binding_; auto config = companion_configuration_;
            cpn::windows::CompanionWriteResponse accepted; apartment_context ui; std::exception_ptr failure;
            co_await resume_background();
            try {
                auto bytes = cpn::native_file_store()->read(path, config.maximum_import_bytes);
                accepted = CompanionModel::write_response(worker->request(CompanionWorker::Operation::accept, CompanionModel::accept_request(binding.workspace_id, binding.paths, bytes, config)).get());
            } catch (...) { failure = std::current_exception(); }
            co_await ui; if (failure) std::rethrow_exception(failure);
            companion_notice_ = std::wstring{CompanionStatus(accepted.status, accepted.detail)}; co_await ReadCompanionPageAsync(true); break;
        }
        case CompanionCommand::Start:
        case CompanionCommand::Stop: {
            const bool start = command == CompanionCommand::Start;
            if (start && !(co_await EnsureCompanionStorageAsync())) break;
            auto worker = companion_worker_; auto binding = *companion_binding_; auto config = companion_configuration_;
            cpn::windows::CompanionListenerResponse result; apartment_context ui; std::exception_ptr failure;
            co_await resume_background();
            try { result = CompanionModel::listener_response(worker->request(start ? CompanionWorker::Operation::listener_start : CompanionWorker::Operation::listener_stop,
                start ? CompanionModel::listener_start_request(binding.workspace_id, binding.paths, config) : CompanionModel::version_request()).get()); }
            catch (...) { failure = std::current_exception(); }
            co_await ui; if (failure) std::rethrow_exception(failure);
            companion_listener_ = std::move(result); companion_notice_ = std::wstring{CompanionStatus(companion_listener_.status,
                companion_listener_.failure_detail.empty() ? companion_listener_.detail : companion_listener_.failure_detail)}; break;
        }
        }
    } catch (...) { error = CurrentError(); companion_notice_ = std::wstring{strings_.get(L"CompanionPreserved")} + L" " + std::wstring{error}; }
    companion_operating_ = false; UpdateCompanionControls();
    if (!error.empty()) co_await ShowErrorAsync(L"CompanionError", error);
}
fire_and_forget MainWindow::ImportCompanion_Click(IInspectable const&, RoutedEventArgs const&) { co_await CompanionAsync(CompanionCommand::Import); }
fire_and_forget MainWindow::RefreshCompanion_Click(IInspectable const&, RoutedEventArgs const&) { co_await CompanionAsync(CompanionCommand::Refresh); }
fire_and_forget MainWindow::MoreCompanion_Click(IInspectable const&, RoutedEventArgs const&) { co_await CompanionAsync(CompanionCommand::More); }
fire_and_forget MainWindow::ApplyCompanion_Click(IInspectable const&, RoutedEventArgs const&) { co_await CompanionAsync(CompanionCommand::Apply); }
fire_and_forget MainWindow::StartCompanion_Click(IInspectable const&, RoutedEventArgs const&) { co_await CompanionAsync(CompanionCommand::Start); }
fire_and_forget MainWindow::StopCompanion_Click(IInspectable const&, RoutedEventArgs const&) { co_await CompanionAsync(CompanionCommand::Stop); }
}
