#include "pch.h"
#include "MainWindow.xaml.h"
#include "SnapshotJson.h"
#include "TestTextChange.h"
#include "cpn/batch.hpp"
#include "cpn/companion_inbox.hpp"
#include "cpn/digest.hpp"
#include <objbase.h>
#include <shobjidl.h>
#include <algorithm>
#include <sstream>
#include <utility>
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Storage;
using namespace Windows::Storage::Pickers;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Input;
using namespace Microsoft::UI::Xaml::Media;
using cpn::windows::CheckerConfiguration;
using cpn::windows::CheckerModel;
using cpn::windows::FileStore;
using cpn::windows::RunnerConfiguration;
using cpn::windows::ToolchainDiscovery;
using cpn::windows::ToolchainDiscoveryConfiguration;
using cpn::windows::UiStrings;

namespace {
std::string identity() {
    GUID value{}; check_hresult(CoCreateGuid(&value));
    wchar_t text[40]{};
    if (!StringFromGUID2(value, text, static_cast<int>(std::size(text)))) throw_hresult(E_UNEXPECTED);
    return to_string(hstring{text});
}
std::string sanitize_id(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if (std::isalnum(c) || c == '-' || c == '_') out.push_back(static_cast<char>(c));
        else out.push_back('_');
    }
    if (out.empty()) out = "doc";
    if (out.size() > 64) out.resize(64);
    return out;
}
void select_id(ComboBox const& box, auto const& choices, auto const& id) {
    for (std::size_t index = 0; index < choices.size(); ++index)
        if (choices[index].id == id) { box.SelectedIndex(static_cast<std::int32_t>(index)); return; }
    throw_hresult(E_INVALIDARG);
}
}

namespace winrt::CPEditorNativeWindows::implementation {
MainWindow::MainWindow() {
    InitializeComponent();
    recovery_idle_ = winrt::handle{CreateEventW(nullptr, true, true, nullptr)};
    if (!recovery_idle_) throw_last_error();
    Title(strings_.get(UiStrings::app_title));
    AppWindow().Resize(Windows::Graphics::SizeInt32{1360, 860});

    CompileBtnLabel().Text(strings_.get(L"CompileBtnText"));
    RunOnlyBtnLabel().Text(strings_.get(L"RunOnlyBtnText"));
    CompileAndRunBtnLabel().Text(strings_.get(L"CompileAndRunBtnText"));
    StopBtnLabel().Text(strings_.get(L"StopBtnText"));
    CompanionBtnLabel().Text(L"");
    CompanionBtnLabel().Visibility(Visibility::Collapsed);
    TestCasesHeaderText().Text(strings_.get(L"TestCasesTitle"));
    OutdatedBadgeText().Text(strings_.get(L"OutdatedBadge"));
    CheckerPrefixText().Text(strings_.get(L"CheckerModePrefix"));
    CompanionPaneTitle().Text(strings_.get(L"CompanionInboxTitle"));
    CompanionFilterAllItem().Text(strings_.get(L"CompanionFilterAllLabel"));
    CompanionFilterPendingItem().Text(strings_.get(L"CompanionPending"));
    CompanionFilterAppliedItem().Text(strings_.get(L"CompanionApplied"));

    MenuUseExternalCheckerItem().Text(strings_.get(L"UseExternalCheckerMenu"));
    MenuCheckerSettingsItem().Text(strings_.get(L"ConfigureCheckerMenu"));
    MenuCompareTrailingItem().Text(strings_.get(L"ComparisonTrailing"));
    MenuCompareStrictItem().Text(strings_.get(L"ComparisonStrict"));
    MenuCompareByteItem().Text(strings_.get(L"ComparisonByte"));
    MenuEnableAllSamplesItem().Text(strings_.get(L"EnableAllSamples"));
    MenuDisableAllSamplesItem().Text(strings_.get(L"DisableAllSamples"));
    MenuRemoveDisabledSamplesItem().Text(strings_.get(L"RemoveDisabledSamples"));
    UseAllOutputsItem().Text(strings_.get(L"UseAllOutputsAsExpected"));
    DeleteAllSamplesItem().Text(strings_.get(L"DeleteAllSamples"));
    StatusWrapButton().Content(box_value(strings_.get(L"StatusBarWrapOff")));
    SummaryBadgeText().Text(L"0/0");

    ToolTipService::SetToolTip(TopCompileButton(), box_value(strings_.get(L"TopCompileTip")));
    ToolTipService::SetToolTip(TopRunButton(), box_value(strings_.get(L"TopRunTip")));
    ToolTipService::SetToolTip(TopStopButton(), box_value(strings_.get(L"TopStopTip")));
    ToolTipService::SetToolTip(CompanionToggleButton(), box_value(strings_.get(L"CompanionTip")));
    ToolTipService::SetToolTip(SettingsButton(), box_value(strings_.get(L"SettingsTip")));
    ToolTipService::SetToolTip(InspectorToggleButton(), box_value(strings_.get(L"ToggleInspectorTip")));
    ToolTipService::SetToolTip(AddTestButton(), box_value(strings_.get(L"AddSampleTip")));
    ToolTipService::SetToolTip(MoreSamplesButton(), box_value(strings_.get(L"MoreSamplesTip")));

    KeyboardAccelerator settings_acc;
    settings_acc.Key(static_cast<Windows::System::VirtualKey>(188)); // VK_OEM_COMMA
    settings_acc.Modifiers(Windows::System::VirtualKeyModifiers::Control);
    MenuSettingsItem().KeyboardAccelerators().Append(settings_acc);

    const auto weak = get_weak();
    closing_token_ = AppWindow().Closing([weak](auto const& sender, Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args) {
        if (auto self = weak.get()) self->OnClosing(sender, args);
    });
    closed_ = Closed(auto_revoke, [weak](auto const&, auto const&) {
        if (auto self = weak.get()) {
            self->shutting_down_ = true;
            if (self->recovery_timer_) {
                if (self->recovery_tick_token_) self->recovery_timer_.Tick(std::exchange(self->recovery_tick_token_, {}));
                self->recovery_timer_.Stop();
            }
            if (self->closing_token_) {
                try { self->AppWindow().Closing(std::exchange(self->closing_token_, {})); } catch (...) {}
            }
            self->closed_.revoke();
            for (auto const& document : self->documents_) {
                document->loaded.revoke();
                if (document->adapter) document->adapter->dispose();
            }
            self->documents_.clear();
            self->CleanupWorkspaceArtifacts();
            auto runner = std::exchange(self->runner_, {});
            if (runner) CloseRunnerBackground(std::move(runner));
            auto session = std::exchange(self->session_worker_, {});
            if (session) CloseSessionBackground(std::move(session));
            auto companion = std::exchange(self->companion_worker_, {});
            if (companion) CloseCompanionBackground(std::move(companion));
        }
    });
}
HWND MainWindow::WindowHandle() {
    HWND handle{}; check_hresult(get_strong().as<IWindowNative>()->get_WindowHandle(&handle)); return handle;
}
hstring MainWindow::CurrentError() {
    try { throw; }
    catch (hresult_error const& error) { return error.message(); }
    catch (std::exception const& error) { return to_hstring(error.what()); }
    catch (...) { return hresult_error{HRESULT_FROM_WIN32(ERROR_UNHANDLED_EXCEPTION)}.message(); }
}

fire_and_forget MainWindow::Root_Loaded(IInspectable const&, RoutedEventArgs const&) {
    auto lifetime = get_strong();
    if (initialized_ || initializing_) co_return;
    initializing_ = true; SetBusy(true);
    hstring error;
    try {
        auto file = co_await StorageFile::GetFileFromApplicationUriAsync(Uri{L"ms-appx:///editor-profiles.json"});
        auto const editor_profiles_text = co_await FileIO::ReadTextAsync(file);
        configuration_ = cpn::windows::EditorConfiguration::parse(editor_profiles_text);
        assist_config_ = cpn::windows::AssistConfig::parse_from_editor_profiles(to_string(editor_profiles_text));

        file = co_await StorageFile::GetFileFromApplicationUriAsync(Uri{L"ms-appx:///runner-settings.json"});
        runner_configuration_ = RunnerConfiguration::parse(to_string(co_await FileIO::ReadTextAsync(file)));
        file = co_await StorageFile::GetFileFromApplicationUriAsync(Uri{L"ms-appx:///recovery-settings.json"});
        recovery_configuration_ = cpn::windows::RecoveryConfiguration::parse(to_string(co_await FileIO::ReadTextAsync(file)));
        file = co_await StorageFile::GetFileFromApplicationUriAsync(Uri{L"ms-appx:///companion-settings.json"});
        companion_configuration_ = cpn::windows::CompanionConfiguration::parse(to_string(co_await FileIO::ReadTextAsync(file)));
        file = co_await StorageFile::GetFileFromApplicationUriAsync(Uri{L"ms-appx:///checker-settings.json"});
        checker_configuration_ = CheckerConfiguration::parse(to_string(co_await FileIO::ReadTextAsync(file)));
        file = co_await StorageFile::GetFileFromApplicationUriAsync(Uri{L"ms-appx:///toolchain-discovery.json"});
        discovery_configuration_ = ToolchainDiscoveryConfiguration::parse(to_string(co_await FileIO::ReadTextAsync(file)));

        checker_settings_.enabled = false;
        checker_settings_.json_text = checker_configuration_.default_checker.dump(2);
        ui_preferences_.tab_width = configuration_.tab_width;
        ui_preferences_.auto_pair = assist_config_.auto_pair_default;

        for (auto const& [status, resource] : companion_configuration_.status_labels) { (void)status; (void)strings_.get(to_hstring(resource)); }
        for (auto const& [issue, resource] : companion_configuration_.issue_labels) { (void)issue; (void)strings_.get(to_hstring(resource)); }
        companion_worker_ = std::make_shared<cpn::windows::CompanionWorker>(companion_configuration_.worker);
        for (auto& choice : runner_configuration_.toolchains) {
            file = co_await StorageFile::GetFileFromApplicationUriAsync(Uri{L"ms-appx:///Toolchains/" + to_hstring(choice.file)});
            choice.profile = nlohmann::json::parse(to_string(co_await FileIO::ReadTextAsync(file)));
            const auto profile = cpn::parse_toolchain_profile(choice.profile.dump());
            if (profile.id != choice.id) throw std::invalid_argument("Toolchain catalog identity mismatch.");
        }

        const std::filesystem::path local{std::wstring{ApplicationData::Current().LocalFolder().Path()}};
        recovery_location_file_ = local / cpn::path_from_utf8(recovery_configuration_.location_filename);
        auto recovery_file = local / cpn::path_from_utf8(recovery_configuration_.directory) / cpn::path_from_utf8(recovery_configuration_.filename);
        source_lock_directory_ = local / cpn::path_from_utf8(recovery_configuration_.source_lock_directory);
        source_store_ = std::make_shared<cpn::windows::SourceStore>(cpn::windows::SourceStore::Options{
            source_lock_directory_, configuration_.max_document_bytes, recovery_configuration_.lock_timeout});
        session_worker_ = std::make_shared<cpn::windows::SessionWorker>(recovery_configuration_.options);
        auto worker = session_worker_; auto recovery_config = recovery_configuration_; auto runner_defaults = runner_configuration_;
        std::vector<std::string> profiles;
        for (auto const& profile : configuration_.profiles) profiles.push_back(to_string(hstring{profile.id}));
        const auto default_profile = to_string(hstring{configuration_.default_profile});
        const auto location_file = recovery_location_file_;
        apartment_context ui; std::exception_ptr failure, location_failure;
        cpn::SessionLoadResult loaded; std::optional<cpn::windows::PreparedRecovery> prepared;
        std::optional<cpn::WorkspaceSession> frozen;
        std::optional<std::string> loaded_content;
        auto default_toolchains = runner_configuration_.toolchains;
        auto discovery_cfg = discovery_configuration_;
        std::vector<cpn::windows::ToolchainProbeResult> initial_probes;

        co_await resume_background();
        try {
            initial_probes = ToolchainDiscovery::probe_all(default_toolchains, discovery_cfg);
            (void)ToolchainDiscovery::apply_discovered_defaults(default_toolchains, initial_probes);
            initial_probes = ToolchainDiscovery::probe_all(default_toolchains, discovery_cfg);
            try {
                if (auto location = cpn::native_file_store()->read_optional(location_file, recovery_config.maximum_location_bytes))
                    recovery_file = cpn::windows::RecoveryModel::parse_location(*location, recovery_config);
            } catch (...) { location_failure = std::current_exception(); }
            std::filesystem::create_directories(recovery_file.parent_path());
            loaded = cpn::windows::RecoveryModel::load_response(worker->load(cpn::path_to_utf8(recovery_file)).get());
            if (loaded.status == cpn::SessionLoadStatus::loaded) {
                loaded_content = cpn::windows::RecoveryModel::logical_content(*loaded.session);
                if (cpn::windows::RevisionPolicy::exhausted(loaded.session->revision)) frozen = *loaded.session;
                prepared = cpn::windows::RecoveryModel::prepare(std::move(*loaded.session), recovery_config, runner_defaults, profiles, default_profile);
            }
        } catch (...) { failure = std::current_exception(); }
        co_await ui;

        runner_configuration_.toolchains = std::move(default_toolchains);
        toolchain_probes_ = std::move(initial_probes);
        recovery_paths_ = recovery_configuration_.paths(recovery_file);
        if (prepared) {
            if (prepared->toolchains) {
                try {
                    for (auto const& choice : *prepared->toolchains)
                        if (strings_.get(to_hstring(choice.label_resource)).empty()) throw std::invalid_argument("Unknown restored toolchain label resource.");
                    runner_configuration_.toolchains = std::move(*prepared->toolchains);
                    (void)ToolchainDiscovery::apply_discovered_defaults(runner_configuration_.toolchains, toolchain_probes_);
                    toolchain_probes_ = ToolchainDiscovery::probe_all(runner_configuration_.toolchains, discovery_configuration_);
                } catch (...) { failure = std::current_exception(); prepared.reset(); }
            }
            if (prepared) {
                ui_preferences_ = prepared->ui_preferences;
                if (ui_preferences_.checker) checker_settings_ = *ui_preferences_.checker;
            }
        }
        RebuildToolchainComboBox();
        for (auto const& profile : configuration_.profiles) {
            (void)runner_configuration_.default_toolchain(to_string(hstring{profile.id}));
            ComboBoxItem item; item.Content(box_value(strings_.get(profile.label_resource))); Profiles().Items().Append(item);
        }

        initialized_ = true; workspace_id_ = prepared ? prepared->session.workspace_id : identity();
        artifact_root_directory_ = std::filesystem::temp_directory_path() / L"CPEditorNative" / L"artifacts" / cpn::path_from_utf8(sanitize_id(workspace_id_));
        recovery_disk_digest_ = loaded.disk_digest;
        if (prepared) {
            recovery_revision_ = prepared->session.revision;
            recovery_saved_content_ = std::move(loaded_content);
            recovery_frozen_session_ = std::move(frozen); recovery_selected_baseline_ = prepared->session.selected_document_id;
            workspace_preserved_preferences_ = std::move(prepared->session.editor_preferences);
            auto selected = prepared->session.selected_document_id;
            std::shared_ptr<Document> selected_document;
            for (std::size_t index = 0; index < prepared->session.documents.size(); ++index) {
                const bool selected_row = selected && prepared->session.documents[index].document_id == *selected;
                AddRestoredDocument(std::move(prepared->session.documents[index]), prepared->documents[index]);
                if (selected_row) selected_document = documents_.back();
            }
            if (selected_document) Tabs().SelectedItem(selected_document->tab);
        }
        if (documents_.empty() && WorkspaceMutable()) AddDocument({}, {});
        if (failure || location_failure || (loaded.status != cpn::SessionLoadStatus::loaded && loaded.status != cpn::SessionLoadStatus::missing)) {
            recovery_blocked_ = true; recovery_retry_allowed_ = false;
            recovery_notice_ = std::wstring{strings_.get(L"RecoveryPreserved")};
            try { if (failure) std::rethrow_exception(failure); if (location_failure) std::rethrow_exception(location_failure); }
            catch (...) { error = CurrentError(); }
            if (error.empty()) error = to_hstring(loaded.detail);
            recovery_notice_ += L" " + std::wstring{error};
        }
        recovery_timer_ = DispatcherQueue().CreateTimer(); recovery_timer_.IsRepeating(false);
        recovery_timer_.Interval(recovery_configuration_.autosave_interval);
        const auto weak = get_weak();
        recovery_tick_token_ = recovery_timer_.Tick([weak](auto const&, auto const&) { if (auto self = weak.get()) self->SaveRecoveryAsync(); });
        recovery_initialized_ = true;
        ApplyWorkspaceUiPreferences();
        SetViewMode(ViewMode::Split);
        InitializeCompanionStorage();
        StartRunner();
        ScheduleRecoverySave(true);
        UpdateRecoveryControls();
    } catch (...) { error = CurrentError(); }
    initializing_ = false; SetBusy(false);
    if (!error.empty()) co_await ShowErrorAsync(L"RecoveryError", error);
    if (companion_available_) co_await CompanionAsync(CompanionCommand::Refresh);
}

void MainWindow::StartRunner() {
    core_ready_ = false; delivery_failure_reported_ = false;
    if (runner_generation_ == std::numeric_limits<std::uint64_t>::max()) throw_hresult(E_BOUNDS);
    const auto generation = ++runner_generation_;
    const auto weak = get_weak(); const auto queue = DispatcherQueue();
    runner_ = std::make_shared<cpn::windows::CoreRunner>(runner_configuration_.options, [weak, queue, generation](cpn::windows::RunEvent event) {
        auto packet = std::make_shared<cpn::windows::RunEvent>(std::move(event));
        if (!queue.TryEnqueue([weak, packet, generation] {
            if (auto self = weak.get(); self && !self->shutting_down_ && self->runner_generation_ == generation)
                self->ReceiveCoreEvent(std::move(*packet));
        })) throw std::runtime_error("The UI dispatcher is closed.");
    });
    UpdateControls();
}
fire_and_forget MainWindow::CloseRunnerBackground(std::shared_ptr<cpn::windows::CoreRunner> runner) { co_await resume_background(); runner->close(); }
fire_and_forget MainWindow::CloseSessionBackground(std::shared_ptr<cpn::windows::SessionWorker> worker) { co_await resume_background(); worker->close(); }
fire_and_forget MainWindow::CloseCompanionBackground(std::shared_ptr<cpn::windows::CompanionWorker> worker) { co_await resume_background(); worker->close(); }

void MainWindow::SetBusy(bool value) {
    busy_ = value; UpdateControls();
    if (!value && recovery_pending_) ScheduleRecoverySave();
}
MainWindow::PersistentOperation::PersistentOperation(MainWindow& value) : owner(value), generation(0) {
    if (!owner.WorkspaceMutable()) throw hresult_error{E_PENDING, owner.strings_.get(L"PersistentOperationUnavailable")};
    generation = owner.persistence_gate_.begin_operation();
}
void MainWindow::PersistentOperation::finish() noexcept { if (active) { owner.persistence_gate_.end_operation(); active = false; } }
MainWindow::PersistentOperation::~PersistentOperation() { finish(); }
MainWindow::FinalRevisionLease::~FinalRevisionLease() noexcept {
    if (capability.acquired()) {
        capability.release();
        try { owner.UpdateControls(); } catch (...) {}
    }
}

std::filesystem::path MainWindow::ManagedArtifactPath(std::shared_ptr<Document> const& document, std::string const& artifact_name) const {
    auto doc_id = document->adapter ? to_string(hstring{document->adapter->document_id()})
        : (document->initial_record ? document->initial_record->document_id : std::string{"doc"});
    auto dir = artifact_root_directory_ / cpn::path_from_utf8(sanitize_id(doc_id));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / cpn::path_from_utf8(artifact_name.empty() ? std::string{"solution.exe"} : artifact_name);
}
void MainWindow::CleanupDocumentArtifacts(std::string const& document_id) const noexcept {
    if (artifact_root_directory_.empty() || document_id.empty()) return;
    std::error_code ec;
    std::filesystem::remove_all(artifact_root_directory_ / cpn::path_from_utf8(sanitize_id(document_id)), ec);
}
void MainWindow::CleanupWorkspaceArtifacts() const noexcept {
    if (artifact_root_directory_.empty()) return;
    std::error_code ec;
    std::filesystem::remove_all(artifact_root_directory_, ec);
}

bool MainWindow::IsResultCurrent(std::shared_ptr<Document> const& document) const {
    if (!document || !document->result || !document->adapter || !document->adapter->committed_available()) return false;
    auto const* tc = FindToolchain(document->toolchain);
    std::string const current_tc = tc ? tc->profile.dump() : std::string{};
    return document->result->revision == document->adapter->revision() &&
           document->result_test_revision == document->test_revision &&
           document->result_toolchain_dump == current_tc &&
           document->result_comparison == document->comparison &&
           document->result_checker_fingerprint == CheckerModel::fingerprint(checker_settings_, checker_configuration_.max_checker_json_bytes);
}

std::wstring MainWindow::ComputeTabTitle(std::shared_ptr<Document> const& document) const {
    if (!document->path.empty()) return document->path.filename().wstring();
    if (document->companion_title && !document->companion_title->empty())
        return std::wstring{to_hstring(*document->companion_title)};
    std::size_t untitled_index = 0;
    for (auto const& row : documents_) {
        if (row->path.empty() && (!row->companion_title || row->companion_title->empty())) {
            ++untitled_index;
            if (row == document) break;
        }
    }
    if (untitled_index <= 1) return std::wstring{strings_.get(UiStrings::untitled)};
    return UiStrings::format(strings_.get(UiStrings::untitled_numbered_format), {std::to_wstring(untitled_index)});
}

std::shared_ptr<MainWindow::Document> MainWindow::CurrentDocument() {
    const auto selected = Tabs().SelectedItem();
    for (auto const& document : documents_) if (selected == document->tab) return document;
    return {};
}

void MainWindow::SetViewMode(ViewMode mode) {
    view_mode_ = mode;
    auto const total_width = WorkspaceSplit().ActualWidth();
    if (mode == ViewMode::EditorOnly) {
        LeftColumn().MinWidth(0); RightColumn().MinWidth(0);
        LeftColumn().Width(GridLength{1, GridUnitType::Star});
        SplitterColumn().Width(GridLength{0, GridUnitType::Pixel});
        RightColumn().Width(GridLength{0, GridUnitType::Pixel});
        SplitHandle().Visibility(Visibility::Collapsed);
    } else if (mode == ViewMode::TestsOnly) {
        // Keep EditorDeck mounted at 0px width so WinUIEdit Scintilla & Undo history survive mode switches.
        LeftColumn().MinWidth(0); RightColumn().MinWidth(360);
        LeftColumn().Width(GridLength{0, GridUnitType::Pixel});
        SplitterColumn().Width(GridLength{0, GridUnitType::Pixel});
        RightColumn().Width(GridLength{1, GridUnitType::Star});
        SplitHandle().Visibility(Visibility::Collapsed);
    } else {
        LeftColumn().MinWidth(300); RightColumn().MinWidth(360);
        SplitterColumn().Width(GridLength{7, GridUnitType::Pixel});
        SplitHandle().Visibility(Visibility::Visible);
        double const desired = split_left_width_ > 0.0 ? split_left_width_ : 510.0;
        double const max_right = total_width > 667.0 ? std::min(720.0, total_width - 307.0) : 510.0;
        double const clamped = std::clamp(desired, 360.0, std::max(360.0, max_right));
        LeftColumn().Width(GridLength{1, GridUnitType::Star});
        RightColumn().Width(GridLength{clamped, GridUnitType::Pixel});
    }
    updating_ui_ = true;
    InspectorToggleButton().IsChecked(mode != ViewMode::EditorOnly);
    updating_ui_ = false;
}

void MainWindow::ViewEditorOnly_Click(IInspectable const&, RoutedEventArgs const&) { SetViewMode(ViewMode::EditorOnly); }
void MainWindow::ViewSplit_Click(IInspectable const&, RoutedEventArgs const&) { SetViewMode(ViewMode::Split); }
void MainWindow::ViewTestsOnly_Click(IInspectable const&, RoutedEventArgs const&) { SetViewMode(ViewMode::TestsOnly); }
void MainWindow::ToggleTestPanel_Click(IInspectable const&, RoutedEventArgs const&) {
    SetViewMode(view_mode_ == ViewMode::EditorOnly ? ViewMode::Split : ViewMode::EditorOnly);
}
void MainWindow::InspectorToggle_Click(IInspectable const&, RoutedEventArgs const&) {
    if (updating_ui_) return;
    SetViewMode(view_mode_ == ViewMode::Split ? ViewMode::EditorOnly : ViewMode::Split);
}
void MainWindow::ToggleCompanionPane_Click(IInspectable const&, RoutedEventArgs const&) {
    companion_pane_open_ = !companion_pane_open_;
    CompanionToggleButton().IsChecked(companion_pane_open_);
    TestsPane().Visibility(companion_pane_open_ ? Visibility::Collapsed : Visibility::Visible);
    CompanionPane().Visibility(companion_pane_open_ ? Visibility::Visible : Visibility::Collapsed);
    if (companion_pane_open_ && view_mode_ == ViewMode::EditorOnly) SetViewMode(ViewMode::Split);
}

void MainWindow::SplitHandle_PointerPressed(IInspectable const&, PointerRoutedEventArgs const& args) {
    if (view_mode_ != ViewMode::Split) return;
    split_drag_active_ = SplitHandle().CapturePointer(args.Pointer());
    if (split_drag_active_) {
        split_drag_start_x_ = args.GetCurrentPoint(WorkspaceSplit()).Position().X;
        split_drag_start_left_width_ = RightColumn().ActualWidth();
        SplitLine().Background(Application::Current().Resources().Lookup(box_value(L"AccentFillColorDefaultBrush")).as<Brush>());
        SplitPill().Background(Application::Current().Resources().Lookup(box_value(L"AccentFillColorDefaultBrush")).as<Brush>());
        SplitPill().Opacity(1.0);
        args.Handled(true);
    }
}
void MainWindow::SplitHandle_PointerMoved(IInspectable const&, PointerRoutedEventArgs const& args) {
    if (!split_drag_active_ || view_mode_ != ViewMode::Split) return;
    double const total = WorkspaceSplit().ActualWidth();
    double const max_right = total > 667.0 ? std::min(720.0, total - 307.0) : 360.0;
    double const delta = args.GetCurrentPoint(WorkspaceSplit()).Position().X - split_drag_start_x_;
    double const next = std::clamp(split_drag_start_left_width_ - delta, 360.0, std::max(360.0, max_right));
    split_left_width_ = next;
    LeftColumn().Width(GridLength{1, GridUnitType::Star});
    RightColumn().Width(GridLength{next, GridUnitType::Pixel});
    args.Handled(true);
}
void MainWindow::SplitHandle_PointerReleased(IInspectable const&, PointerRoutedEventArgs const& args) {
    if (split_drag_active_) {
        split_drag_active_ = false;
        SplitHandle().ReleasePointerCapture(args.Pointer());
        SplitLine().Background(Application::Current().Resources().Lookup(box_value(L"CardStrokeColorDefaultBrush")).as<Brush>());
        SplitPill().Background(Application::Current().Resources().Lookup(box_value(L"ControlStrongStrokeColorDefaultBrush")).as<Brush>());
        SplitPill().Opacity(0.0);
        args.Handled(true);
    }
}
void MainWindow::SplitHandle_PointerCaptureLost(IInspectable const&, PointerRoutedEventArgs const&) {
    split_drag_active_ = false;
    SplitLine().Background(Application::Current().Resources().Lookup(box_value(L"CardStrokeColorDefaultBrush")).as<Brush>());
    SplitPill().Background(Application::Current().Resources().Lookup(box_value(L"ControlStrongStrokeColorDefaultBrush")).as<Brush>());
    SplitPill().Opacity(0.0);
}
void MainWindow::SplitHandle_PointerEntered(IInspectable const&, PointerRoutedEventArgs const&) {
    if (!split_drag_active_) {
        SplitLine().Background(Application::Current().Resources().Lookup(box_value(L"ControlStrongStrokeColorDefaultBrush")).as<Brush>());
        SplitPill().Opacity(0.65);
    }
}
void MainWindow::SplitHandle_PointerExited(IInspectable const&, PointerRoutedEventArgs const&) {
    if (!split_drag_active_) {
        SplitLine().Background(Application::Current().Resources().Lookup(box_value(L"CardStrokeColorDefaultBrush")).as<Brush>());
        SplitPill().Opacity(0.0);
    }
}

void MainWindow::AddDocument(std::filesystem::path path, cpn::windows::LoadedDocument loaded, std::optional<std::string> disk_digest) {
    if (!WorkspaceMutable()) throw hresult_error{E_PENDING, strings_.get(L"PersistentOperationUnavailable")};
    cpn::SessionDocument record; record.document_id = identity(); record.revision = 1;
    record.source_text = std::move(loaded.utf8);
    const auto profile = path.empty() ? configuration_.default_profile : configuration_.for_path(path).id;
    record.language_id = to_string(hstring{profile});
    record.toolchain_profile_id = runner_configuration_.default_toolchain(*record.language_id);
    if (!path.empty()) {
        record.original_path = std::move(path);
        record.save_point = cpn::DocumentSavePoint{record.revision, cpn::sha256(record.source_text), std::move(disk_digest)};
    }
    cpn::windows::DocumentPreferences prefs;
    prefs.utf8_bom = loaded.utf8_bom; prefs.comparison = runner_configuration_.default_comparison;
    record.tests.push_back({identity(), {}, {}, true});
    AddRestoredDocument(std::move(record), std::move(prefs));
}

void MainWindow::AddRestoredDocument(cpn::SessionDocument record, cpn::windows::DocumentPreferences prefs) {
    if (documents_.size() >= runner_configuration_.maximum_documents)
        throw hresult_error{E_BOUNDS, strings_.get(L"DocumentLimit")};
    auto document = std::make_shared<Document>();
    document->run_capability = cpn::windows::CompanionModel::run_capability(record);
    try {
        if (auto prov = cpn::read_companion_document_provenance(record)) {
            for (auto const& item : companion_page_.items) {
                if (item.import_id == prov->identity.import_id) {
                    document->companion_title = item.candidate.name;
                    break;
                }
            }
        }
    } catch (...) {}

    document->tab = TabViewItem{};
    StackPanel tab_hdr;
    tab_hdr.Orientation(Orientation::Horizontal);
    tab_hdr.Spacing(6);
    TextBlock tab_title;
    tab_title.FontSize(12);
    tab_title.TextTrimming(TextTrimming::CharacterEllipsis);
    tab_title.VerticalAlignment(VerticalAlignment::Center);
    Border tab_badge;
    tab_badge.CornerRadius(CornerRadius{9});
    tab_badge.Padding(Thickness{6, 1, 6, 1});
    tab_badge.Visibility(Visibility::Collapsed);
    TextBlock tab_badge_text;
    tab_badge_text.FontSize(10);
    tab_badge_text.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
    tab_badge.Child(tab_badge_text);
    Border tab_dirty_dot;
    tab_dirty_dot.Width(6);
    tab_dirty_dot.Height(6);
    tab_dirty_dot.CornerRadius(CornerRadius{3});
    tab_dirty_dot.VerticalAlignment(VerticalAlignment::Center);
    tab_dirty_dot.Background(Application::Current().Resources().Lookup(box_value(L"SystemFillColorCautionBrush")).as<Brush>());
    tab_dirty_dot.Visibility(Visibility::Collapsed);
    tab_hdr.Children().Append(tab_title);
    tab_hdr.Children().Append(tab_badge);
    tab_hdr.Children().Append(tab_dirty_dot);
    document->tab.Header(tab_hdr);
    document->tab_title = tab_title;
    document->tab_badge = tab_badge;
    document->tab_badge_text = tab_badge_text;
    document->tab_dirty_dot = tab_dirty_dot;

    document->control = winrt::WinUIEditor::CodeEditorControl{};
    document->test_view = ScrollViewer{};
    document->test_panel = StackPanel{};
    document->path = record.original_path.value_or(std::filesystem::path{});
    document->profile = std::wstring{to_hstring(record.language_id.value_or(to_string(hstring{configuration_.default_profile})))};
    document->toolchain = record.toolchain_profile_id.value_or(runner_configuration_.default_toolchain(to_string(hstring{document->profile})));
    document->comparison = prefs.comparison; document->utf8_bom = prefs.utf8_bom;
    document->test_revision = prefs.test_revision; document->restore_preferences = prefs;
    document->preserved_preferences = record.editor_preferences; document->save_point = record.save_point;
    document->control.IsEnabled(false); document->control.Visibility(Visibility::Collapsed);
    document->test_view.Visibility(Visibility::Collapsed);
    document->test_view.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    document->test_panel.Padding(Thickness{12, 10, 12, 14});
    document->test_panel.Spacing(10);

    // Per-document Fluent Compiler Output Card (inside scrollable test_panel)
    Border c_banner;
    c_banner.CornerRadius(CornerRadius{8});
    c_banner.BorderThickness(Thickness{1});
    c_banner.BorderBrush(Application::Current().Resources().Lookup(box_value(L"CardStrokeColorDefaultBrush")).as<Brush>());
    c_banner.Background(Application::Current().Resources().Lookup(box_value(L"CardBackgroundFillColorDefaultBrush")).as<Brush>());
    c_banner.Visibility(Visibility::Collapsed);
    Grid c_grid;
    ColumnDefinition cc0, cc1;
    cc0.Width(GridLength{8, GridUnitType::Pixel});
    cc1.Width(GridLength{1, GridUnitType::Star});
    c_grid.ColumnDefinitions().Append(cc0);
    c_grid.ColumnDefinitions().Append(cc1);
    Border c_accent;
    c_accent.Width(3);
    c_accent.CornerRadius(CornerRadius{2});
    c_accent.Margin(Thickness{5, 10, 0, 10});
    c_accent.HorizontalAlignment(HorizontalAlignment::Left);
    Grid::SetColumn(c_accent, 0);
    c_grid.Children().Append(c_accent);
    StackPanel c_body;
    c_body.Padding(Thickness{10, 10, 12, 10});
    c_body.Spacing(6);
    Grid::SetColumn(c_body, 1);
    c_grid.Children().Append(c_body);
    c_banner.Child(c_grid);

    Grid c_hdr_grid;
    ColumnDefinition ch0, ch1;
    ch0.Width(GridLength{1, GridUnitType::Star});
    ch1.Width(GridLength{0, GridUnitType::Auto});
    c_hdr_grid.ColumnDefinitions().Append(ch0);
    c_hdr_grid.ColumnDefinitions().Append(ch1);
    Button c_toggle;
    c_toggle.Style(Application::Current().Resources().Lookup(box_value(L"FluentIconButtonStyle")).as<Style>());
    c_toggle.HorizontalAlignment(HorizontalAlignment::Stretch);
    c_toggle.HorizontalContentAlignment(HorizontalAlignment::Left);
    c_toggle.Padding(Thickness{0});
    StackPanel c_toggle_row;
    c_toggle_row.Orientation(Orientation::Horizontal);
    c_toggle_row.Spacing(8);
    FontIcon c_chevron;
    c_chevron.Glyph(L"\xE70D");
    c_chevron.FontSize(11);
    TextBlock c_header;
    c_header.FontSize(12);
    c_header.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
    c_toggle_row.Children().Append(c_chevron);
    c_toggle_row.Children().Append(c_header);
    c_toggle.Content(c_toggle_row);
    Grid::SetColumn(c_toggle, 0);
    c_hdr_grid.Children().Append(c_toggle);

    Button c_copy;
    c_copy.Style(Application::Current().Resources().Lookup(box_value(L"FluentIconButtonStyle")).as<Style>());
    c_copy.Width(24);
    c_copy.Height(24);
    c_copy.Padding(Thickness{0});
    FontIcon c_copy_icon;
    c_copy_icon.Glyph(L"\xE8C8");
    c_copy_icon.FontSize(12);
    c_copy.Content(c_copy_icon);
    ToolTipService::SetToolTip(c_copy, box_value(strings_.get(L"CopyTextTip")));
    Grid::SetColumn(c_copy, 1);
    c_hdr_grid.Children().Append(c_copy);
    c_body.Children().Append(c_hdr_grid);

    ScrollViewer c_scroll;
    c_scroll.MaxHeight(180);
    c_scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    c_scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Auto);
    c_scroll.Visibility(Visibility::Collapsed);
    TextBlock c_out_text;
    c_out_text.FontFamily(FontFamily{L"Cascadia Mono, Consolas, Courier New"});
    c_out_text.FontSize(12);
    c_out_text.TextWrapping(TextWrapping::NoWrap);
    c_out_text.IsTextSelectionEnabled(true);
    c_scroll.Content(c_out_text);
    c_body.Children().Append(c_scroll);

    document->compiler_banner = c_banner;
    document->compiler_accent_bar = c_accent;
    document->compiler_chevron = c_chevron;
    document->compiler_header = c_header;
    document->compiler_copy = c_copy;
    document->compiler_scroll = c_scroll;
    document->compiler_output_text = c_out_text;
    document->test_panel.Children().Append(c_banner);

    // Empty samples Fluent guide card
    Border empty_card;
    empty_card.Padding(Thickness{20});
    empty_card.CornerRadius(CornerRadius{8});
    empty_card.BorderThickness(Thickness{1});
    empty_card.BorderBrush(Application::Current().Resources().Lookup(box_value(L"CardStrokeColorDefaultBrush")).as<Brush>());
    empty_card.Background(Application::Current().Resources().Lookup(box_value(L"CardBackgroundFillColorDefaultBrush")).as<Brush>());
    StackPanel empty_stack;
    empty_stack.Spacing(8);
    empty_stack.HorizontalAlignment(HorizontalAlignment::Center);
    FontIcon empty_icon;
    empty_icon.Glyph(L"\xE8FD");
    empty_icon.FontSize(22);
    empty_icon.Opacity(0.55);
    empty_icon.HorizontalAlignment(HorizontalAlignment::Center);
    TextBlock empty_title;
    empty_title.Text(strings_.get(L"NoSamplesTitle"));
    empty_title.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
    empty_title.HorizontalAlignment(HorizontalAlignment::Center);
    TextBlock empty_hint;
    empty_hint.Text(strings_.get(L"NoSamplesHint"));
    empty_hint.FontSize(12);
    empty_hint.Opacity(0.75);
    empty_hint.TextWrapping(TextWrapping::Wrap);
    empty_hint.TextAlignment(TextAlignment::Center);
    Button empty_add_btn;
    empty_add_btn.Content(box_value(strings_.get(L"AddSampleTip")));
    empty_add_btn.HorizontalAlignment(HorizontalAlignment::Center);
    empty_add_btn.CornerRadius(CornerRadius{6});
    empty_stack.Children().Append(empty_icon);
    empty_stack.Children().Append(empty_title);
    empty_stack.Children().Append(empty_hint);
    empty_stack.Children().Append(empty_add_btn);
    empty_card.Child(empty_stack);
    document->empty_tests_panel = empty_card;
    document->empty_add_test_btn = empty_add_btn;
    document->test_panel.Children().Append(empty_card);

    document->test_view.Content(document->test_panel);
    for (auto const& test : record.tests) AddTest(document, test);
    RefreshTestHeaders(document);
    document->initial_record = std::move(record);
    const auto weak = get_weak(); const std::weak_ptr<Document> weak_document = document;
    c_toggle.Click([weak, weak_document](auto const&, auto const&) {
        if (auto self = weak.get()) if (auto doc = weak_document.lock()) {
            doc->compiler_expanded = !doc->compiler_expanded;
            self->RenderCompilerBanner(doc);
        }
    });
    c_copy.Click([weak, weak_document](auto const&, auto const&) {
        if (auto self = weak.get()) if (auto doc = weak_document.lock())
            self->CopyTextToClipboard(doc->compiler_output_text.Text());
    });
    empty_add_btn.Click([weak, weak_document](auto const&, auto const&) {
        if (auto self = weak.get()) if (auto doc = weak_document.lock()) self->AddTest(doc);
    });
    document->loaded = document->control.Loaded(auto_revoke, [weak, weak_document](auto const&, auto const&) {
        if (auto self = weak.get()) if (auto value = weak_document.lock()) self->InitializeDocument(std::move(value));
    });
    const auto previous_tab = Tabs().SelectedItem();
    documents_.reserve(documents_.size() + 1);
    mutating_tabs_ = true;
    try {
        documents_.push_back(document);
        EditorDeck().Children().Append(document->control); TestDeck().Children().Append(document->test_view);
        Tabs().TabItems().Append(document->tab); Tabs().SelectedItem(document->tab); mutating_tabs_ = false; SelectDocument(document);
    } catch (...) {
        mutating_tabs_ = false;
        const auto failure = std::current_exception(); document->closed = true; document->loaded.revoke();
        if (document->adapter) document->adapter->dispose();
        std::uint32_t index{};
        try { if (EditorDeck().Children().IndexOf(document->control, index)) EditorDeck().Children().RemoveAt(index); } catch (...) {}
        try { if (TestDeck().Children().IndexOf(document->test_view, index)) TestDeck().Children().RemoveAt(index); } catch (...) {}
        mutating_tabs_ = true;
        try { if (Tabs().TabItems().IndexOf(document->tab, index)) Tabs().TabItems().RemoveAt(index); } catch (...) {}
        mutating_tabs_ = false;
        std::erase(documents_, document);
        try { Tabs().SelectedItem(previous_tab); SelectDocument(CurrentDocument()); } catch (...) {}
        std::rethrow_exception(failure);
    }
    ScheduleRecoverySave();
}

fire_and_forget MainWindow::InitializeDocument(std::shared_ptr<Document> document) {
    auto lifetime = get_strong();
    if (document->adapter || shutting_down_) co_return;
    hstring error;
    try {
        const auto weak = get_weak(); const std::weak_ptr<Document> weak_document = document;
        document->adapter = cpn::windows::WinUIEditAdapter::create(document->control, [weak, weak_document] {
            if (auto self = weak.get()) self->ScheduleDocumentChanged(weak_document);
        });
        document->adapter->set_tab_width(ui_preferences_.tab_width);
        document->adapter->set_word_wrap(ui_preferences_.word_wrap);
        document->adapter->set_assist_enabled(ui_preferences_.auto_pair);
        document->adapter->set_assist_config(assist_config_);
        document->adapter->set_language(configuration_.by_id(document->profile).highlighting_language);
        document->adapter->set_language_id(to_string(hstring{document->profile}));
        if (!document->initial_record) throw_hresult(E_UNEXPECTED);
        document->adapter->restore_document(*document->initial_record,
            document->restore_preferences.caret_byte, document->restore_preferences.anchor_byte);
        document->adapter->set_workspace_read_only(!WorkspaceMutable());
        document->initial_record.reset();
        document->control.IsEnabled(true);
        document->loaded.revoke();
        UpdateDocument(document);
        UpdateControls();
    } catch (...) {
        error = CurrentError();
        if (document->adapter) document->adapter->dispose();
        document->adapter.reset(); document->notice = std::wstring{strings_.get(UiStrings::initialization_failed)};
        UpdateControls();
    }
    if (!error.empty()) co_await ShowErrorAsync(UiStrings::error_config, error);
}

void MainWindow::ScheduleDocumentChanged(std::weak_ptr<Document> const& weak_document) {
    auto document = weak_document.lock();
    if (!document || document->closed || document->dispatch_pending || shutting_down_) return;
    document->dispatch_pending = true;
    const auto weak = get_weak();
    if (!DispatcherQueue().TryEnqueue([weak, weak_document] {
        if (auto self = weak.get()) if (auto value = weak_document.lock()) self->DocumentChanged(std::move(value));
    })) document->dispatch_pending = false;
}

fire_and_forget MainWindow::DocumentChanged(std::shared_ptr<Document> document) {
    auto lifetime = get_strong(); document->dispatch_pending = false;
    if (shutting_down_ || document->closed || !document->adapter) co_return;
    hstring error;
    try {
        bool auto_enabled_expected = false;
        for (auto const& test : document->tests) {
            if (!test->expected_bytes.empty() && !test->has_expected_value && TestsMutable(document)) {
                test->has_expected_value = true;
                updating_ui_ = true;
                test->has_expected.IsChecked(box_value(true).as<IReference<bool>>());
                updating_ui_ = false;
                auto_enabled_expected = true;
            }
        }
        RefreshTestHeaders(document);
        if (auto_enabled_expected) TestsChanged(document);

        if (document->pending_run) {
            if (!document->adapter->tracking_available()) {
                CancelRun(document); document->run_state = "Failed";
                error = strings_.get(L"SnapshotTrackingFailed");
            } else if (document->adapter->committed_available()) SubmitPendingRun(document);
        }
        UpdateDocument(document); UpdateControls(); ScheduleRecoverySave();
    } catch (...) {
        error = CurrentError();
        if (document->pending_run) { CancelRun(document); document->run_state = "Failed"; UpdateDocument(document); }
    }
    if (!error.empty()) co_await ShowErrorAsync(L"RunError", error);
}

void MainWindow::SelectDocument(std::shared_ptr<Document> const& selected) {
    for (auto const& document : documents_) {
        const auto visibility = document == selected ? Visibility::Visible : Visibility::Collapsed;
        document->control.Visibility(visibility); document->test_view.Visibility(visibility);
    }
    if (!selected) {
        UpdateCheckerMenuState({});
        UpdateControls();
        return;
    }
    selecting_document_ = true;
    try {
        select_id(Profiles(), configuration_.profiles, selected->profile);
        select_id(Toolchains(), runner_configuration_.toolchains, selected->toolchain);
    } catch (...) { selecting_document_ = false; throw; }
    selecting_document_ = false;
    UpdateCheckerMenuState(selected);
    UpdateDocument(selected);
    RenderCompilerBanner(selected);
    UpdateSummaryBadge(selected);
    UpdateControls();
}

void MainWindow::Tabs_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&) {
    if (initialized_) { SelectDocument(CurrentDocument()); ScheduleRecoverySave(); }
}
void MainWindow::Tabs_TabItemsChanged(TabView const& sender, Windows::Foundation::Collections::IVectorChangedEventArgs const&) {
    if (mutating_tabs_ || shutting_down_ || !initialized_ || !WorkspaceMutable()) return;
    auto const items = sender.TabItems();
    if (items.Size() != documents_.size()) return;
    std::vector<std::shared_ptr<Document>> reordered;
    reordered.reserve(documents_.size());
    bool order_changed = false;
    for (std::uint32_t i = 0; i < items.Size(); ++i) {
        auto tab_item = items.GetAt(i);
        auto it = std::find_if(documents_.begin(), documents_.end(), [&](auto const& d) { return d->tab == tab_item; });
        if (it == documents_.end()) return;
        if (static_cast<std::size_t>(std::distance(documents_.begin(), it)) != i) order_changed = true;
        reordered.push_back(*it);
    }
    if (!order_changed) return;
    documents_ = std::move(reordered);
    for (auto const& doc : documents_) UpdateDocument(doc);
    ScheduleRecoverySave();
}
fire_and_forget MainWindow::Tabs_AddTabButtonClick(TabView const&, IInspectable const&) {
    co_await ExecuteAsync(Command::New);
}
void MainWindow::Profile_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&) {
    auto document = CurrentDocument(); const auto index = Profiles().SelectedIndex();
    if (selecting_document_ || !document || index < 0 || !TestsMutable(document)) return;
    document->profile = configuration_.profiles.at(static_cast<std::size_t>(index)).id;
    document->toolchain = runner_configuration_.default_toolchain(to_string(hstring{document->profile}));
    if (document->adapter) {
        document->adapter->set_language(configuration_.by_id(document->profile).highlighting_language);
        document->adapter->set_language_id(to_string(hstring{document->profile}));
    }
    selecting_document_ = true;
    try { select_id(Toolchains(), runner_configuration_.toolchains, document->toolchain); }
    catch (...) { selecting_document_ = false; throw; }
    selecting_document_ = false;
    TestsChanged(document);
}
void MainWindow::Toolchain_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&) {
    auto document = CurrentDocument(); const auto index = Toolchains().SelectedIndex();
    if (selecting_document_ || !document || index < 0 || !TestsMutable(document)) return;
    document->toolchain = runner_configuration_.toolchains.at(static_cast<std::size_t>(index)).id;
    TestsChanged(document);
}
void MainWindow::TestsChanged(std::shared_ptr<Document> const& document) {
    if (document->closed || !TestsMutable(document)) return;
    document->test_revision = cpn::windows::RevisionPolicy::next(document->test_revision);
    UpdateDocument(document);
    UpdateControls();
    ScheduleRecoverySave();
}
void MainWindow::RecordTestText(std::shared_ptr<Document> const& document, std::shared_ptr<TestView> const& test, bool input) {
    try {
        auto& bytes = input ? test->input_bytes : test->expected_bytes;
        const auto& box = input ? test->input : test->expected;
        const auto result = cpn::windows::TestTextChange::record(bytes, document->test_revision, to_string(box.Text()),
            !(input ? test->input_binary : test->expected_binary) && TestsMutable(document));
        if (result == cpn::windows::TestTextChange::Result::unchanged) return;
        if (result == cpn::windows::TestTextChange::Result::rejected) document->test_text_tracking_failed = true;
    } catch (...) { document->test_text_tracking_failed = true; }
    recovery_pending_ = true;
    try { ScheduleDocumentChanged(document); } catch (...) {}
}
fire_and_forget MainWindow::AddTest_Click(IInspectable const&, RoutedEventArgs const&) {
    auto lifetime = get_strong(); hstring error;
    try { if (auto document = CurrentDocument()) AddTest(document); } catch (...) { error = CurrentError(); }
    if (!error.empty()) co_await ShowErrorAsync(L"TestError", error);
}

void MainWindow::UpdateDocument(std::shared_ptr<Document> const& document) {
    const auto name = ComputeTabTitle(document);
    const bool available = document->adapter && document->adapter->committed_available();
    const bool dirty = document->adapter ? (!available || document->adapter->dirty()) :
        (document->initial_record && (!document->save_point || cpn::sha256(document->initial_record->source_text) != document->save_point->source_digest));
    if (document->tab_title) {
        document->tab_title.Text(hstring{name});
    }
    if (document->tab_dirty_dot) {
        document->tab_dirty_dot.Visibility(dirty ? Visibility::Visible : Visibility::Collapsed);
    }
    UpdateSummaryBadge(document);
    if (document->result) {
        bool const current = IsResultCurrent(document);
        for (auto const& test : document->tests) {
            test->use_output.IsEnabled(current && test->last_output_adoptable && !test->expected_binary && TestsMutable(document));
        }
    }

    if (CurrentDocument() != document) return;
    Title(hstring{UiStrings::format(strings_.get(UiStrings::window_title_format), {
        std::wstring{strings_.get(UiStrings::app_title)}, name, dirty ? std::wstring{strings_.get(UiStrings::unsaved_suffix)} : L""})});

    StatusLanguageText().Text(strings_.get(configuration_.by_id(document->profile).label_resource));
    StatusEncodingText().Text(strings_.get(document->utf8_bom ? L"StatusBarEncodingBom" : L"StatusBarEncodingUtf8"));
    if (document->adapter) {
        StatusCursorText().Text(hstring{UiStrings::format(strings_.get(L"StatusBarLnColFormat"), {
            std::to_wstring(document->adapter->line_number()), std::to_wstring(document->adapter->column_number())})});
        StatusBytesText().Text(hstring{UiStrings::format(strings_.get(L"StatusBarBytesFormat"), {
            std::to_wstring(document->adapter->length())})});
    } else {
        StatusCursorText().Text(L"Ln 1, Col 1");
        StatusBytesText().Text(L"0 B");
    }

    std::wstring notice_parts;
    if (!document->adapter) notice_parts = std::wstring{strings_.get(L"EditorInitializing")};
    else if (document->test_text_tracking_failed) notice_parts = std::wstring{strings_.get(L"TestTextTrackingFailed")};
    else if (!document->adapter->tracking_available()) notice_parts = std::wstring{strings_.get(L"SnapshotTrackingFailed")};
    else if (!available) notice_parts = std::wstring{strings_.get(L"CompositionOrNativeBusy")};
    else if (document->run_capability != cpn::windows::CompanionRunCapability::ordinary)
        notice_parts = std::wstring{strings_.get(document->run_capability == cpn::windows::CompanionRunCapability::unsupported ? L"CompanionRunUnsupported" : L"CompanionRunUnavailable")};
    else if (document->cancel_requested) notice_parts = std::wstring{strings_.get(L"CancellationRequested")};
    else if (persistence_gate_.final_active()) notice_parts = std::wstring{strings_.get(L"FinalRevisionCommitting")};
    else if (!WorkspaceMutable()) notice_parts = std::wstring{strings_.get(L"WorkspaceRevisionExhausted")};
    else if ((available && cpn::windows::RevisionPolicy::exhausted(document->adapter->revision())) || cpn::windows::RevisionPolicy::exhausted(document->test_revision))
        notice_parts = std::wstring{strings_.get(L"DocumentRevisionExhausted")};
    if (!document->notice.empty()) {
        if (!notice_parts.empty()) notice_parts += L"  ";
        notice_parts += document->notice;
    }
    Status().Text(hstring{notice_parts});
}

bool MainWindow::WorkspaceRevisionAvailable() const noexcept { return !cpn::windows::RevisionPolicy::exhausted(recovery_revision_); }
bool MainWindow::WorkspaceMutable() const noexcept { return WorkspaceRevisionAvailable() && !persistence_gate_.final_active(); }
bool MainWindow::WorkspaceCommittedAvailable() const {
    for (auto const& document : documents_) {
        if (document->test_text_tracking_failed) throw hresult_error{E_FAIL, strings_.get(L"TestTextTrackingFailed")};
        if (document->adapter) {
            if (!document->adapter->tracking_available()) throw hresult_error{E_FAIL, strings_.get(L"SnapshotTrackingFailed")};
            if (!document->adapter->committed_available()) return false;
        }
    }
    return true;
}
bool MainWindow::TryBeginFinalRevision(bool owned_busy, FinalRevisionLease& lease) {
    if (!cpn::windows::RevisionPolicy::final_revision_pending(recovery_revision_)) return true;
    if (!lease.capability.try_acquire(busy_ && !recovery_flushing_ && !owned_busy)) return false;
    UpdateControls(); return true;
}
bool MainWindow::TestsMutable(std::shared_ptr<Document> const& document) const noexcept {
    return document && !document->closed && !document->test_text_tracking_failed && WorkspaceMutable() &&
        cpn::windows::RevisionPolicy::tests_mutable(recovery_revision_, document->test_revision);
}
void MainWindow::FreezePersistedNavigation(cpn::windows::PreparedRecovery const& persisted) {
    recovery_selected_baseline_ = persisted.session.selected_document_id;
    for (std::size_t index = 0; index < persisted.session.documents.size(); ++index) {
        const auto& record = persisted.session.documents[index];
        for (auto const& document : documents_) {
            const auto id = document->adapter ? to_string(hstring{document->adapter->document_id()}) :
                (document->initial_record ? document->initial_record->document_id : std::string{});
            if (id == record.document_id) {
                document->restore_preferences.caret_byte = persisted.documents[index].caret_byte;
                document->restore_preferences.anchor_byte = persisted.documents[index].anchor_byte;
            }
        }
    }
}

void MainWindow::UpdateControls() {
    if (runner_ && runner_->delivery_failed() && !shutting_down_ && !delivery_failure_reported_) {
        core_ready_ = false; delivery_failure_reported_ = true;
        for (auto const& value : documents_) {
            if (value->run_active) value->run_state = "Failed";
            value->pending_run.reset(); value->run_active = false; value->run_token.clear(); value->cancel_requested = false;
            value->notice = std::wstring{strings_.get(L"CoreDeliveryFailed")}; UpdateDocument(value);
        }
    }
    const auto document = CurrentDocument();
    const bool editor_ready = document && document->adapter;
    const bool file_ops_ready = initialized_ && !busy_ && !shutting_down_ && !persistence_gate_.final_active();
    const bool metadata_mutable = initialized_ && !shutting_down_ && document && TestsMutable(document);

    Profiles().IsEnabled(metadata_mutable);
    Toolchains().IsEnabled(metadata_mutable);
    Tabs().CanReorderTabs(WorkspaceMutable() && !busy_ && !shutting_down_);

    bool const running = !shutting_down_ && document && document->run_active;
    bool const checker_ctrl_ok = initialized_ && WorkspaceMutable() && !shutting_down_ && !running;
    MenuUseExternalCheckerItem().IsEnabled(checker_ctrl_ok);
    MenuCheckerSettingsItem().IsEnabled(checker_ctrl_ok);
    bool const policy_ok = !checker_settings_.enabled && metadata_mutable && !running;
    MenuCompareTrailingItem().IsEnabled(policy_ok);
    MenuCompareStrictItem().IsEnabled(policy_ok);
    MenuCompareByteItem().IsEnabled(policy_ok);
    CheckerMenuButton().IsEnabled(initialized_ && !shutting_down_ && !running);
    UpdateCheckerMenuState(document);

    MenuNewItem().IsEnabled(file_ops_ready && WorkspaceMutable());
    MenuOpenItem().IsEnabled(file_ops_ready && WorkspaceMutable());
    MenuSaveItem().IsEnabled(file_ops_ready && WorkspaceMutable() && editor_ready);
    MenuSaveAsItem().IsEnabled(file_ops_ready && WorkspaceMutable() && editor_ready);
    MenuSaveAllItem().IsEnabled(file_ops_ready && WorkspaceMutable());
    MenuCloseTabItem().IsEnabled(file_ops_ready && WorkspaceMutable() && document != nullptr);
    MenuCloseSavedItem().IsEnabled(file_ops_ready && WorkspaceMutable());
    MenuExportSnapshotItem().IsEnabled(file_ops_ready && editor_ready);
    MenuUndoItem().IsEnabled(editor_ready && WorkspaceMutable() && document->adapter->can_undo());
    MenuRedoItem().IsEnabled(editor_ready && WorkspaceMutable() && document->adapter->can_redo());
    MenuWordWrapItem().IsEnabled(initialized_ && WorkspaceMutable());
    MenuAutoPairItem().IsEnabled(initialized_ && WorkspaceMutable());
    MenuSettingsItem().IsEnabled(initialized_ && !shutting_down_);

    for (auto const& value : documents_) {
        value->tab.IsClosable(WorkspaceMutable());
        if (value->adapter) value->adapter->set_workspace_read_only(!WorkspaceMutable());
        const bool mutable_tests = TestsMutable(value);
        for (auto const& test : value->tests) {
            test->input.IsReadOnly(test->input_binary || !mutable_tests);
            test->expected.IsReadOnly(test->expected_binary || !mutable_tests);
            test->enabled.IsEnabled(mutable_tests);
            test->has_expected.IsEnabled(mutable_tests);
            test->duplicate_btn.IsEnabled(mutable_tests);
            test->remove.IsEnabled(mutable_tests);
        }
    }

    auto const* tc = document ? FindToolchain(document->toolchain) : nullptr;
    bool const compiled_tc = tc && RunnerConfiguration::is_compiled_toolchain(tc->profile);
    if (tc) {
        auto const* probe = FindProbe(tc->id);
        if (probe && !probe->available) {
            ToolchainAvailabilityText().Text(strings_.get(L"ToolchainMissingHint"));
            ToolchainAvailabilityText().Visibility(Visibility::Visible);
        } else {
            ToolchainAvailabilityText().Visibility(Visibility::Collapsed);
        }
    } else {
        ToolchainAvailabilityText().Visibility(Visibility::Collapsed);
    }

    bool const can_run_base = initialized_ && core_ready_ && !shutting_down_ && !persistence_gate_.final_active() &&
                              editor_ready && !document->test_text_tracking_failed && !document->run_active &&
                              document->run_capability == cpn::windows::CompanionRunCapability::ordinary;
    bool const has_artifact = compiled_tc && document && document->compiled_artifact &&
                              document->compiled_artifact->toolchain_id == document->toolchain &&
                              tc && document->compiled_artifact->toolchain_dump == tc->profile.dump() &&
                              std::filesystem::is_regular_file(document->compiled_artifact->path);

    bool const running_active = !shutting_down_ && document && document->run_active;
    bool const stop_enabled = running_active && !document->cancel_requested;

    TopCompileButton().Visibility(compiled_tc && !running_active ? Visibility::Visible : Visibility::Collapsed);
    TopCompileButton().IsEnabled(can_run_base && compiled_tc);
    TopRunButton().Visibility(!running_active ? Visibility::Visible : Visibility::Collapsed);
    TopRunButton().IsEnabled(can_run_base);
    TopStopButton().Visibility(running_active ? Visibility::Visible : Visibility::Collapsed);
    TopStopButton().IsEnabled(stop_enabled);

    if (running_active) {
        CompileButton().Visibility(Visibility::Collapsed);
        RunOnlyButton().Visibility(Visibility::Collapsed);
        RunButton().Visibility(Visibility::Collapsed);
        CancelButton().Visibility(Visibility::Visible);
        CancelButton().IsEnabled(stop_enabled);
    } else if (compiled_tc) {
        CancelButton().Visibility(Visibility::Collapsed);
        CompileButton().Visibility(Visibility::Visible);
        CompileButton().IsEnabled(can_run_base);
        RunOnlyButton().Visibility(Visibility::Visible);
        RunOnlyButton().IsEnabled(can_run_base && has_artifact);
        RunButton().Visibility(Visibility::Visible);
        RunButton().IsEnabled(can_run_base);
        Grid::SetColumn(RunButton(), 2);
        Grid::SetColumnSpan(RunButton(), 1);
        CompileAndRunBtnLabel().Text(strings_.get(L"CompileAndRunBtnText"));
    } else {
        CancelButton().Visibility(Visibility::Collapsed);
        CompileButton().Visibility(Visibility::Collapsed);
        RunOnlyButton().Visibility(Visibility::Collapsed);
        RunButton().Visibility(Visibility::Visible);
        RunButton().IsEnabled(can_run_base);
        Grid::SetColumn(RunButton(), 0);
        Grid::SetColumnSpan(RunButton(), 3);
        CompileAndRunBtnLabel().Text(strings_.get(L"RunOnlyBtnText"));
    }

    MenuCompileItem().IsEnabled(can_run_base && compiled_tc);
    MenuRunOnlyItem().IsEnabled(can_run_base && (!compiled_tc || has_artifact));
    MenuCompileAndRunItem().IsEnabled(can_run_base);
    MenuStopItem().IsEnabled(stop_enabled);
    MenuAddSampleItem().IsEnabled(metadata_mutable);
    AddTestButton().IsEnabled(metadata_mutable);
    if (document && document->empty_add_test_btn) document->empty_add_test_btn.IsEnabled(metadata_mutable);
    bool const has_tests = document && !document->tests.empty();
    bool const has_disabled = document && std::any_of(document->tests.begin(), document->tests.end(),
        [](auto const& t) { return !t->enabled_value; });
    MenuEnableAllSamplesItem().IsEnabled(metadata_mutable && has_tests);
    MenuDisableAllSamplesItem().IsEnabled(metadata_mutable && has_tests);
    MenuRemoveDisabledSamplesItem().IsEnabled(metadata_mutable && has_disabled);
    UseAllOutputsItem().IsEnabled(metadata_mutable && has_tests && document && IsResultCurrent(document));
    DeleteAllSamplesItem().IsEnabled(metadata_mutable && has_tests);
    MoreSamplesButton().IsEnabled(metadata_mutable && has_tests);

    UpdateRecoveryControls();
    UpdateCompanionControls();
}

void MainWindow::SubmitPendingRun(std::shared_ptr<Document> const& document) {
    if (!document->pending_run || !document->adapter) return;
    if (document->run_capability != cpn::windows::CompanionRunCapability::ordinary)
        throw hresult_error{E_NOTIMPL, strings_.get(document->run_capability == cpn::windows::CompanionRunCapability::unsupported ? L"CompanionRunUnsupported" : L"CompanionRunUnavailable")};
    if (document->test_text_tracking_failed) throw hresult_error{E_FAIL, strings_.get(L"TestTextTrackingFailed")};
    if (!document->adapter->tracking_available()) throw hresult_error{E_FAIL, strings_.get(L"SnapshotTrackingFailed")};
    if (!document->adapter->committed_available()) return;
    try {
        auto snapshot = document->adapter->snapshot();
        auto request = std::move(*document->pending_run);
        request.document_id = to_string(hstring{snapshot.document_id});
        request.revision = snapshot.revision;
        request.source = std::move(snapshot.utf8);
        if (!document->path.empty()) request.original_path = to_string(hstring{document->path.wstring()});
        runner_->submit(std::move(request));
        document->pending_run.reset();
        document->run_state = "Queued";
    } catch (...) {
        document->pending_run.reset(); document->run_active = false; document->run_token.clear(); document->run_state = "Failed"; throw;
    }
}

void MainWindow::StartRunWithMode(std::shared_ptr<Document> const& document, std::string const& mode) {
    if (!document || !document->adapter || !core_ready_ || shutting_down_ || document->run_active || persistence_gate_.final_active()) return;
    hstring error;
    try {
        if (document->run_capability != cpn::windows::CompanionRunCapability::ordinary)
            throw hresult_error{E_NOTIMPL, strings_.get(document->run_capability == cpn::windows::CompanionRunCapability::unsupported ? L"CompanionRunUnsupported" : L"CompanionRunUnavailable")};
        if (!document->adapter->tracking_available()) throw hresult_error{E_FAIL, strings_.get(L"SnapshotTrackingFailed")};
        if (document->test_text_tracking_failed) throw hresult_error{E_FAIL, strings_.get(L"TestTextTrackingFailed")};
        auto const* choice = FindToolchain(document->toolchain);
        if (!choice) throw_hresult(E_INVALIDARG);

        bool const compiled_tc = RunnerConfiguration::is_compiled_toolchain(choice->profile);
        std::string effective_mode = mode;
        if (!compiled_tc) {
            if (effective_mode == "compile_only") return;
            effective_mode = "compile_and_run";
        }

        if (view_mode_ == ViewMode::EditorOnly) SetViewMode(ViewMode::Split);
        if (companion_pane_open_) ToggleCompanionPane_Click(nullptr, nullptr);

        cpn::windows::RunRequest request;
        request.token = identity();
        request.comparison = document->comparison;
        request.toolchain = choice->profile;
        request.mode = effective_mode;
        if (compiled_tc) {
            if (effective_mode == "run_only") {
                if (!document->compiled_artifact || document->compiled_artifact->toolchain_id != document->toolchain ||
                    document->compiled_artifact->toolchain_dump != choice->profile.dump() ||
                    !std::filesystem::is_regular_file(document->compiled_artifact->path)) {
                    throw hresult_error{E_FAIL, strings_.get(L"RunMissingArtifact")};
                }
                request.artifact_path = cpn::path_to_utf8(document->compiled_artifact->path);
                document->pending_artifact_path.reset();
                if (document->adapter->committed_available() && document->compiled_artifact->revision != document->adapter->revision()) {
                    document->notice = std::wstring{strings_.get(L"RunBuildStale")};
                } else {
                    document->notice.clear();
                }
            } else {
                auto const artifact_name = choice->profile.value("artifact_name", std::string{"solution.exe"});
                auto const target = ManagedArtifactPath(document, artifact_name);
                request.artifact_path = cpn::path_to_utf8(target);
                document->pending_artifact_path = target;
                document->notice.clear();
            }
        } else {
            document->pending_artifact_path.reset();
            document->notice.clear();
        }

        if (checker_settings_.enabled && effective_mode != "compile_only") {
            auto const json_str = checker_settings_.json_text.empty()
                ? checker_configuration_.default_checker.dump()
                : checker_settings_.json_text;
            request.external_checker = CheckerModel::validate_json(json_str, checker_configuration_.max_checker_json_bytes);
        }

        for (auto const& test : document->tests) {
            request.tests.push_back({test->id, test->input_bytes,
                test->has_expected_value ? std::optional<std::string>{test->expected_bytes} : std::nullopt,
                test->enabled_value});
        }

        document->run_token = request.token;
        document->run_mode = effective_mode;
        document->run_toolchain_dump = choice->profile.dump();
        document->run_comparison = document->comparison;
        document->run_checker_fingerprint = CheckerModel::fingerprint(checker_settings_, checker_configuration_.max_checker_json_bytes);
        document->pending_run = std::move(request);
        document->run_active = true;
        document->cancel_requested = false;
        document->last_sequence = 0;
        document->run_test_revision = document->test_revision;
        document->run_state = "AwaitingCommittedText";
        SubmitPendingRun(document);
        UpdateDocument(document);
        UpdateControls();
    } catch (...) {
        error = CurrentError();
        CancelRun(document);
        document->run_state = "Failed";
        UpdateDocument(document);
        UpdateControls();
    }
    if (!error.empty()) {
        auto show = ShowErrorAsync(L"RunError", error);
        (void)show;
    }
}

fire_and_forget MainWindow::Compile_Click(IInspectable const&, RoutedEventArgs const&) {
    auto lifetime = get_strong();
    if (auto doc = CurrentDocument()) StartRunWithMode(doc, "compile_only");
    co_return;
}
fire_and_forget MainWindow::RunOnly_Click(IInspectable const&, RoutedEventArgs const&) {
    auto lifetime = get_strong();
    if (auto doc = CurrentDocument()) StartRunWithMode(doc, "run_only");
    co_return;
}
fire_and_forget MainWindow::Run_Click(IInspectable const&, RoutedEventArgs const&) {
    auto lifetime = get_strong();
    if (auto doc = CurrentDocument()) StartRunWithMode(doc, "compile_and_run");
    co_return;
}

void MainWindow::CancelRun(std::shared_ptr<Document> const& document) {
    if (!document->run_active) return;
    if (document->pending_run) {
        document->pending_run.reset(); document->run_active = false; document->run_token.clear(); document->run_state = "Cancelled";
    } else { runner_->cancel(document->run_token); document->cancel_requested = true; }
    UpdateDocument(document); UpdateControls();
}
fire_and_forget MainWindow::Cancel_Click(IInspectable const&, RoutedEventArgs const&) {
    auto lifetime = get_strong(); hstring error;
    try { if (auto document = CurrentDocument()) CancelRun(document); } catch (...) { error = CurrentError(); }
    if (!error.empty()) co_await ShowErrorAsync(L"RunError", error);
}

void MainWindow::ReceiveCoreEvent(cpn::windows::RunEvent event) {
    if (event.kind == cpn::windows::RunEvent::Kind::ready) { core_ready_ = true; UpdateControls(); return; }
    if (event.kind == cpn::windows::RunEvent::Kind::engine_error) {
        core_ready_ = false;
        for (auto const& document : documents_) {
            document->pending_run.reset(); document->run_active = false; document->run_state = "Failed";
            document->run_token.clear(); document->notice = std::wstring{to_hstring(event.error)}; UpdateDocument(document);
        }
        UpdateControls(); return;
    }
    for (auto const& document : documents_) if (document->run_token == event.token) {
        if (event.sequence && event.sequence <= document->last_sequence) return;
        document->last_sequence = event.sequence; document->run_state = event.state;
        if (!event.error.empty()) document->notice = std::wstring{to_hstring(event.error)};
        if (event.result) {
            if (event.result->build_outcome == "Succeeded" && document->pending_artifact_path &&
                std::filesystem::is_regular_file(*document->pending_artifact_path)) {
                document->compiled_artifact = CompiledArtifact{
                    event.result->revision,
                    event.result->source_digest,
                    document->toolchain,
                    document->run_toolchain_dump,
                    *document->pending_artifact_path
                };
            }
            document->pending_artifact_path.reset();
            document->result = std::move(event.result);
            document->result_test_revision = document->run_test_revision;
            document->result_toolchain_dump = document->run_toolchain_dump;
            document->result_comparison = document->run_comparison;
            document->result_checker_fingerprint = document->run_checker_fingerprint;
            document->result_mode = document->run_mode;
            document->compiler_expanded = document->result->build_outcome == "CompilationError" || document->result->build_outcome == "Failed";
            RenderResult(document);
        }
        if (event.terminal) { document->run_active = false; document->cancel_requested = false; document->run_token.clear(); }
        UpdateDocument(document); UpdateControls(); return;
    }
}

hstring MainWindow::RenderBytes(std::string_view value) const {
    const auto length = std::min(value.size(), runner_configuration_.display_output_bytes);
    auto const visible = value.substr(0, length); std::wstring result;
    bool binary = visible.find('\0') != std::string_view::npos;
    try { FileStore::validate_utf8(visible); } catch (...) { binary = true; }
    if (binary) {
        result = std::wstring{strings_.get(L"OutputHex")}; result += L"\n";
        constexpr wchar_t digits[] = L"0123456789ABCDEF";
        for (unsigned char byte : visible) { result += digits[byte >> 4]; result += digits[byte & 15]; result += L' '; }
    } else result = std::wstring{to_hstring(visible)};
    if (length < value.size()) result += L"\n" + UiStrings::format(strings_.get(L"DisplayOutputTruncated"), {std::to_wstring(value.size())});
    return hstring{result};
}

void MainWindow::Undo_Click(IInspectable const&, RoutedEventArgs const&) {
    if (auto doc = CurrentDocument(); doc && doc->adapter && WorkspaceMutable()) {
        doc->adapter->undo();
        UpdateControls();
    }
}
void MainWindow::Redo_Click(IInspectable const&, RoutedEventArgs const&) {
    if (auto doc = CurrentDocument(); doc && doc->adapter && WorkspaceMutable()) {
        doc->adapter->redo();
        UpdateControls();
    }
}
void MainWindow::ToggleWordWrap_Click(IInspectable const&, RoutedEventArgs const&) {
    if (!initialized_ || !WorkspaceMutable()) return;
    ui_preferences_.word_wrap = !ui_preferences_.word_wrap;
    ApplyWorkspaceUiPreferences();
    ScheduleRecoverySave();
}
void MainWindow::ToggleAutoPair_Click(IInspectable const&, RoutedEventArgs const&) {
    if (!initialized_ || !WorkspaceMutable()) return;
    ui_preferences_.auto_pair = !ui_preferences_.auto_pair;
    ApplyWorkspaceUiPreferences();
    ScheduleRecoverySave();
}

void MainWindow::UpdateRecoveryControls() {
    bool const show_bar = !recovery_notice_.empty() &&
        (recovery_blocked_ || recovery_preferences_limited_ || !WorkspaceMutable());
    RecoveryInfoBar().IsOpen(show_bar);
    RecoveryInfoBar().Message(hstring{recovery_notice_});
    RecoveryInfoBar().Severity(recovery_blocked_ ? InfoBarSeverity::Error : InfoBarSeverity::Warning);
    bool const retry_ok = recovery_initialized_ && recovery_retry_allowed_ && !busy_ && !shutting_down_ && !recovery_saving_ && !recovery_copying_;
    bool const save_as_ok = recovery_initialized_ && !busy_ && !shutting_down_ && !recovery_copying_;
    RetryRecoveryButton().IsEnabled(retry_ok);
    SaveRecoveryAsButton().IsEnabled(save_as_ok);
    MenuRetryRecoveryItem().IsEnabled(retry_ok);
    MenuSaveRecoveryAsItem().IsEnabled(save_as_ok);
}

void MainWindow::ScheduleRecoverySave(bool immediate) {
    recovery_pending_ = true;
    if (!recovery_initialized_ || recovery_blocked_ || shutting_down_ || recovery_copying_ || recovery_saving_ || recovery_flushing_) return;
    if (immediate) { SaveRecoveryAsync(); return; }
    if (!recovery_timer_.IsRunning()) recovery_timer_.Start();
}

std::optional<cpn::WorkspaceSession> MainWindow::CaptureWorkspace() {
    cpn::WorkspaceSession session; session.workspace_id = workspace_id_; session.revision = recovery_revision_;
    if (!WorkspaceRevisionAvailable()) session.selected_document_id = recovery_selected_baseline_;
    session.editor_preferences = workspace_preserved_preferences_;
    ui_preferences_.checker = checker_settings_;
    bool preferences_limited = !cpn::windows::RecoveryModel::merge_preferences(session.editor_preferences,
        cpn::windows::RecoveryModel::workspace_preferences(runner_configuration_.toolchains, ui_preferences_, recovery_configuration_.workspace_namespace));
    const auto selected = CurrentDocument();
    for (auto const& document : documents_) {
        if (document->test_text_tracking_failed) throw hresult_error{E_FAIL, strings_.get(L"TestTextTrackingFailed")};
        cpn::SessionDocument record; auto prefs = document->restore_preferences;
        if (document->adapter) {
            if (!document->adapter->tracking_available()) throw hresult_error{E_FAIL, strings_.get(L"SnapshotTrackingFailed")};
            if (!document->adapter->committed_available()) return {};
            auto snapshot = document->adapter->snapshot(); record.document_id = to_string(hstring{snapshot.document_id});
            record.revision = snapshot.revision; record.source_text = std::move(snapshot.utf8);
            if (WorkspaceRevisionAvailable()) { prefs.caret_byte = snapshot.caret_byte; prefs.anchor_byte = snapshot.anchor_byte; }
        } else {
            if (!document->initial_record || document->control.IsEnabled()) throw hresult_error{E_FAIL, strings_.get(L"SnapshotTrackingFailed")};
            record.document_id = document->initial_record->document_id; record.revision = document->initial_record->revision;
            record.source_text = document->initial_record->source_text;
        }
        if (WorkspaceRevisionAvailable() && document == selected) session.selected_document_id = record.document_id;
        if (!document->path.empty()) record.original_path = document->path;
        record.language_id = to_string(hstring{document->profile}); record.toolchain_profile_id = document->toolchain;
        record.save_point = document->save_point;
        for (auto const& test : document->tests) record.tests.push_back({test->id, test->input_bytes,
            test->has_expected_value ? std::optional<std::string>{test->expected_bytes} : std::nullopt, test->enabled_value});
        record.editor_preferences = document->preserved_preferences;
        prefs.utf8_bom = document->utf8_bom; prefs.comparison = document->comparison; prefs.test_revision = document->test_revision;
        if (!cpn::windows::RecoveryModel::merge_preferences(record.editor_preferences,
            cpn::windows::RecoveryModel::document_preferences(prefs, recovery_configuration_.document_namespace))) preferences_limited = true;
        session.documents.push_back(std::move(record));
    }
    recovery_preferences_limited_ = preferences_limited;
    return session;
}

fire_and_forget MainWindow::SaveRecoveryAsync() {
    auto lifetime = get_strong();
    if (!recovery_initialized_ || recovery_blocked_ || recovery_saving_ || recovery_copying_ || shutting_down_) co_return;
    FinalRevisionLease final_lease{*this};
    if (cpn::windows::RevisionPolicy::final_revision_pending(recovery_revision_) &&
        (persistence_gate_.operation_active() || (busy_ && !recovery_flushing_))) {
        recovery_pending_ = true; recovery_notice_ = std::wstring{strings_.get(L"FinalRevisionAwaitingOperations")};
        UpdateRecoveryControls();
        if (!recovery_flushing_ && recovery_timer_ && !recovery_timer_.IsRunning()) recovery_timer_.Start();
        co_return;
    }
    std::optional<cpn::WorkspaceSession> candidate; hstring error;
    try {
        if (WorkspaceCommittedAvailable()) {
            if (!TryBeginFinalRevision(false, final_lease)) throw hresult_error{E_PENDING, strings_.get(L"FinalRevisionAwaitingOperations")};
            candidate = CaptureWorkspace();
        }
    } catch (...) { error = CurrentError(); }
    if (!candidate) {
        recovery_pending_ = true;
        recovery_notice_ = std::wstring{strings_.get(error.empty() ? L"RecoveryAwaitingCommittedText" : L"RecoveryPreserved")};
        if (!error.empty()) { recovery_notice_ += L" " + std::wstring{error}; recovery_blocked_ = true; }
        UpdateRecoveryControls(); if (!recovery_blocked_ && !recovery_flushing_ && recovery_timer_ && !recovery_timer_.IsRunning()) recovery_timer_.Start();
        co_return;
    }
    recovery_pending_ = false; recovery_saving_ = true; check_bool(ResetEvent(recovery_idle_.get()));
    recovery_notice_ = std::wstring{strings_.get(L"RecoverySaving")}; UpdateRecoveryControls();
    const auto paths = recovery_paths_; const auto expected = recovery_disk_digest_; const auto previous = recovery_saved_content_;
    const auto timeout = recovery_configuration_.lock_timeout; auto worker = session_worker_;
    const bool force_write = std::exchange(recovery_force_write_, false); const auto exact_frozen = recovery_frozen_session_;
    const auto config = recovery_configuration_; const auto defaults = runner_configuration_;
    std::vector<std::string> profiles;
    for (auto const& profile : configuration_.profiles) profiles.push_back(to_string(hstring{profile.id}));
    const auto default_profile = to_string(hstring{configuration_.default_profile});
    auto next_revision = candidate->revision; std::string content; cpn::SessionSaveResult result; bool unchanged{};
    std::optional<cpn::windows::PreparedRecovery> frozen_navigation;
    std::exception_ptr failure; apartment_context ui;
    co_await resume_background();
    try {
        auto logical = *candidate; logical.revision = 0; content = cpn::serialize_workspace_session(logical);
        const bool same = previous && *previous == content;
        if (same && !force_write) {
            const auto observed = cpn::windows::RecoveryModel::load_response(worker->load(cpn::path_to_utf8(paths.recovery_file)).get());
            unchanged = observed.status == cpn::SessionLoadStatus::loaded && observed.disk_digest == expected;
            if (!unchanged) result = {observed.status == cpn::SessionLoadStatus::invalid || observed.status == cpn::SessionLoadStatus::unsupported_schema ?
                cpn::SessionSaveStatus::invalid_existing : cpn::SessionSaveStatus::conflict, observed.disk_digest, false, false,
                observed.detail.empty() ? "The persisted recovery file no longer matches this workspace." : observed.detail};
        } else {
            candidate->revision = cpn::windows::RevisionPolicy::persistence_revision(candidate->revision, !same);
            next_revision = candidate->revision;
            if (same && exact_frozen) *candidate = *exact_frozen;
            if (cpn::windows::RevisionPolicy::exhausted(next_revision))
                frozen_navigation = cpn::windows::RecoveryModel::prepare(*candidate, config, defaults, profiles, default_profile);
            result = cpn::windows::RecoveryModel::save_response(worker->save(
                cpn::windows::RecoveryModel::save_request(paths, *candidate, expected, timeout)).get());
        }
    } catch (...) { failure = std::current_exception(); }
    co_await ui;
    if (!unchanged && result.recovery_replaced) {
        recovery_disk_digest_ = std::move(result.disk_digest); recovery_revision_ = next_revision;
        recovery_saved_content_ = std::move(content);
        if (frozen_navigation) {
            recovery_frozen_session_ = std::move(*candidate); FreezePersistedNavigation(*frozen_navigation);
        }
    }
    UpdateControls();
    if (failure || (!unchanged && result.status != cpn::SessionSaveStatus::saved)) {
        recovery_blocked_ = true; recovery_pending_ = true;
        if (result.status == cpn::SessionSaveStatus::invalid_existing) recovery_retry_allowed_ = false;
        recovery_notice_ = std::wstring{strings_.get(L"RecoveryPreserved")};
        try { if (failure) std::rethrow_exception(failure); } catch (...) { error = CurrentError(); }
        recovery_notice_ += L" " + std::wstring{error.empty() ? to_hstring(result.detail) : error};
    } else {
        recovery_notice_ = recovery_preferences_limited_ ? std::wstring{strings_.get(L"RecoveryPreferencesLimited")} : std::wstring{};
    }
    if (final_lease.capability.acquired()) { final_lease.capability.release(); UpdateControls(); }
    recovery_saving_ = false; check_bool(SetEvent(recovery_idle_.get())); UpdateRecoveryControls();
    if (recovery_pending_ && !recovery_blocked_ && !shutting_down_ && !recovery_copying_ && !recovery_flushing_ && !recovery_timer_.IsRunning()) recovery_timer_.Start();
}

IAsyncOperation<bool> MainWindow::FlushRecoveryAsync() {
    if (!recovery_initialized_ || recovery_blocked_ || recovery_copying_) co_return false;
    if (recovery_timer_) recovery_timer_.Stop();
    apartment_context ui;
    if (recovery_saving_) { co_await resume_on_signal(recovery_idle_.get()); co_await ui; }
    if (recovery_blocked_) co_return false;
    SaveRecoveryAsync();
    if (recovery_saving_) { co_await resume_on_signal(recovery_idle_.get()); co_await ui; }
    if (recovery_blocked_ || !recovery_saved_content_) co_return false;
    auto current = CaptureWorkspace(); if (!current) co_return false;
    current->revision = 0;
    co_return cpn::serialize_workspace_session(*current) == *recovery_saved_content_;
}

fire_and_forget MainWindow::RetryRecovery_Click(IInspectable const&, RoutedEventArgs const&) {
    auto lifetime = get_strong();
    if (busy_ || shutting_down_ || recovery_saving_ || recovery_copying_ || !recovery_initialized_ || !recovery_retry_allowed_) co_return;
    recovery_blocked_ = false; recovery_force_write_ = true; ScheduleRecoverySave(true);
}

fire_and_forget MainWindow::SaveRecoveryAs_Click(IInspectable const&, RoutedEventArgs const&) {
    auto lifetime = get_strong();
    if (busy_ || shutting_down_ || recovery_copying_ || !recovery_initialized_) co_return;
    SetBusy(true); recovery_copying_ = true; if (recovery_timer_) recovery_timer_.Stop();
    hstring error; apartment_context ui; FinalRevisionLease final_lease{*this};
    try {
        if (recovery_saving_) { co_await resume_on_signal(recovery_idle_.get()); co_await ui; }
        if (persistence_gate_.operation_active()) throw hresult_error{E_PENDING, strings_.get(L"FinalRevisionAwaitingOperations")};
        if (!WorkspaceCommittedAvailable()) throw hresult_error{E_PENDING, strings_.get(L"FinishCompositionBeforeSaving")};
        if (!TryBeginFinalRevision(true, final_lease)) throw hresult_error{E_PENDING, strings_.get(L"FinalRevisionAwaitingOperations")};
        auto candidate = CaptureWorkspace();
        if (!candidate) throw hresult_error{E_PENDING, strings_.get(L"FinishCompositionBeforeSaving")};
        auto selected = PickSavePath(strings_.get(L"RecoveryFiles"), recovery_paths_.recovery_file.filename(), recovery_paths_.recovery_file.extension().wstring());
        if (selected) {
            const auto destination = cpn::windows::SourceStore::resolve(*selected);
            if (IsReservedSourcePath(destination)) throw hresult_error{E_INVALIDARG, strings_.get(L"RecoveryChooseNewLocation")};
            auto const paths = recovery_configuration_.paths(destination); auto const config = recovery_configuration_;
            for (auto const* transaction_path : {&paths.recovery_file, &paths.backup_file, &paths.lock_file}) {
                if (IsReservedSourcePath(*transaction_path)) throw hresult_error{E_INVALIDARG, strings_.get(L"ReservedRecoveryPath")};
                const auto target = cpn::windows::SourceStore::resolve(*transaction_path);
                for (auto const& document : documents_) if (!document->path.empty()) {
                    const auto source = cpn::windows::SourceStore::resolve(document->path);
                    if (CompareStringOrdinal(target.c_str(), -1, source.c_str(), -1, TRUE) == CSTR_EQUAL ||
                        (std::filesystem::exists(target) && std::filesystem::exists(source) && std::filesystem::equivalent(target, source)))
                        throw hresult_error{E_INVALIDARG, strings_.get(L"ReservedRecoveryPath")};
                }
            }
            auto worker = session_worker_; const auto location_file = recovery_location_file_;
            const auto previous = recovery_saved_content_; const auto exact_frozen = recovery_frozen_session_;
            const auto defaults = runner_configuration_; std::vector<std::string> profiles;
            for (auto const& profile : configuration_.profiles) profiles.push_back(to_string(hstring{profile.id}));
            const auto default_profile = to_string(hstring{configuration_.default_profile});
            auto next_revision = candidate->revision; cpn::SessionSaveResult result; std::string content; std::exception_ptr failure;
            std::optional<cpn::windows::PreparedRecovery> frozen_navigation; bool location_confirmed = false;
            co_await resume_background();
            try {
                auto logical = *candidate; logical.revision = 0; content = cpn::serialize_workspace_session(logical);
                const bool same = previous && *previous == content;
                candidate->revision = cpn::windows::RevisionPolicy::persistence_revision(candidate->revision, !same);
                next_revision = candidate->revision;
                if (same && exact_frozen) *candidate = *exact_frozen;
                if (cpn::windows::RevisionPolicy::exhausted(next_revision))
                    frozen_navigation = cpn::windows::RecoveryModel::prepare(*candidate, config, defaults, profiles, default_profile);
                result = cpn::windows::RecoveryModel::save_response(worker->save(
                    cpn::windows::RecoveryModel::save_request(paths, *candidate, {}, config.lock_timeout)).get());
                if (result.status == cpn::SessionSaveStatus::saved) {
                    cpn::native_file_store()->write_atomic(location_file, cpn::windows::RecoveryModel::serialize_location(destination, config));
                    location_confirmed = true;
                }
            } catch (...) { failure = std::current_exception(); }
            co_await ui;
            if (result.recovery_replaced && cpn::windows::RevisionPolicy::exhausted(next_revision)) {
                recovery_revision_ = next_revision;
                recovery_saved_content_ = content;
                recovery_frozen_session_ = *candidate;
                if (frozen_navigation) FreezePersistedNavigation(*frozen_navigation);
            }
            if (location_confirmed) {
                recovery_paths_ = paths; recovery_revision_ = next_revision; recovery_disk_digest_ = std::move(result.disk_digest);
                recovery_saved_content_ = std::move(content); recovery_retry_allowed_ = true;
                if (frozen_navigation) {
                    recovery_frozen_session_ = std::move(*candidate); FreezePersistedNavigation(*frozen_navigation);
                }
                UpdateControls();
            }
            if (!location_confirmed) {
                recovery_blocked_ = true; recovery_retry_allowed_ = false;
                if (result.recovery_replaced)
                    recovery_notice_ = UiStrings::format(strings_.get(L"RecoveryCopyPreservedFormat"), {destination.wstring()});
                if (failure) std::rethrow_exception(failure);
            }
            if (result.status != cpn::SessionSaveStatus::saved) {
                recovery_blocked_ = true; recovery_retry_allowed_ = false;
                throw hresult_error{E_FAIL, hstring{std::wstring{strings_.get(L"RecoveryPreserved")} + L" " + std::wstring{to_hstring(result.detail)}}};
            }
            recovery_blocked_ = false; recovery_retry_allowed_ = true; recovery_notice_.clear();
            recovery_pending_ = true;
        }
    } catch (...) { error = CurrentError(); recovery_notice_ += L" " + std::wstring{error}; }
    if (final_lease.capability.acquired()) { final_lease.capability.release(); UpdateControls(); }
    recovery_copying_ = false; SetBusy(false); ScheduleRecoverySave();
    if (!error.empty()) co_await ShowErrorAsync(L"RecoveryError", error);
}

IAsyncAction MainWindow::ShowErrorAsync(std::wstring_view action, hstring details) {
    ContentDialog dialog; dialog.XamlRoot(Root().XamlRoot()); dialog.Title(box_value(strings_.get(UiStrings::error_title)));
    dialog.Content(box_value(hstring{UiStrings::format(strings_.get(UiStrings::error_details), {std::wstring{strings_.get(action)}, std::wstring{details}})}));
    dialog.CloseButtonText(strings_.get(UiStrings::close_button));
    try { co_await ShowNativeDialogAsync(dialog); }
    catch (...) {
        Status().Text(hstring{UiStrings::format(strings_.get(UiStrings::error_details), {std::wstring{strings_.get(action)}, std::wstring{details}})});
    }
}

IAsyncOperation<ContentDialogResult> MainWindow::ShowNativeDialogAsync(ContentDialog dialog) {
    auto lifetime = get_strong();
    auto ticket = std::make_shared<DialogTicket>();
    ticket->event = winrt::handle{CreateEventW(nullptr, false, false, nullptr)};
    if (!ticket->event) throw_last_error();
    dialogs_.push_back(ticket);
    if (dialogs_.size() == 1) check_bool(SetEvent(ticket->event.get()));
    apartment_context ui;
    co_await resume_on_signal(ticket->event.get());
    co_await ui;
    ContentDialogResult result = ContentDialogResult::None; std::exception_ptr failure;
    try { result = co_await dialog.ShowAsync(); } catch (...) { failure = std::current_exception(); }
    dialogs_.pop_front();
    if (!dialogs_.empty()) check_bool(SetEvent(dialogs_.front()->event.get()));
    if (failure) std::rethrow_exception(failure);
    co_return result;
}

IAsyncOperation<bool> MainWindow::ConfirmDiscardAsync(std::shared_ptr<Document> document) {
    if (document->adapter && !document->adapter->tracking_available()) throw hresult_error{E_FAIL, strings_.get(L"SnapshotTrackingFailed")};
    if (document->adapter && !document->adapter->committed_available()) throw hresult_error{E_PENDING, strings_.get(L"FinishCompositionBeforeSaving")};
    if (document->adapter && !document->adapter->dirty()) co_return true;
    if (!document->adapter && document->initial_record && document->save_point &&
        cpn::sha256(document->initial_record->source_text) == document->save_point->source_digest) co_return true;
    ContentDialog dialog; dialog.XamlRoot(Root().XamlRoot()); dialog.Title(box_value(strings_.get(UiStrings::confirm_title)));
    const auto name = ComputeTabTitle(document);
    dialog.Content(box_value(hstring{UiStrings::format(strings_.get(UiStrings::confirm_content), {name})}));
    dialog.PrimaryButtonText(strings_.get(UiStrings::save_button)); dialog.SecondaryButtonText(strings_.get(UiStrings::discard_button));
    dialog.CloseButtonText(strings_.get(UiStrings::cancel_button)); dialog.DefaultButton(ContentDialogButton::Primary);
    const auto result = co_await ShowNativeDialogAsync(dialog);
    if (result == ContentDialogResult::Primary) {
        if (!(co_await SaveDocumentAsync(document, false))) co_return false;
        co_return document->adapter->committed_available() && !document->adapter->dirty();
    }
    co_return result == ContentDialogResult::Secondary;
}

bool MainWindow::IsReservedSourcePath(std::filesystem::path const& path) const {
    const auto candidate = cpn::windows::SourceStore::resolve(path);
    if (!source_lock_directory_.empty()) {
        const auto lock_directory = std::filesystem::weakly_canonical(std::filesystem::absolute(source_lock_directory_));
        for (auto ancestor = candidate; !ancestor.empty();) {
            if (CompareStringOrdinal(ancestor.c_str(), -1, lock_directory.c_str(), -1, TRUE) == CSTR_EQUAL) return true;
            const auto parent = ancestor.parent_path(); if (parent == ancestor) break; ancestor = parent;
        }
    }
    for (auto const* reserved : {&recovery_paths_.recovery_file, &recovery_paths_.backup_file, &recovery_paths_.lock_file, &recovery_location_file_}) {
        if (reserved->empty()) continue;
        const auto target = cpn::windows::SourceStore::resolve(*reserved);
        if (CompareStringOrdinal(candidate.c_str(), -1, target.c_str(), -1, TRUE) == CSTR_EQUAL) return true;
        if (std::filesystem::exists(candidate) && std::filesystem::exists(target) && std::filesystem::equivalent(candidate, target)) return true;
    }
    if (companion_binding_) for (auto const* reserved : {&companion_binding_->paths.inbox_file, &companion_binding_->paths.backup_file, &companion_binding_->paths.lock_file}) {
        const auto target = cpn::windows::SourceStore::resolve(*reserved);
        if (CompareStringOrdinal(candidate.c_str(), -1, target.c_str(), -1, TRUE) == CSTR_EQUAL ||
            (std::filesystem::exists(candidate) && std::filesystem::exists(target) && std::filesystem::equivalent(candidate, target))) return true;
    }
    return false;
}

std::optional<std::filesystem::path> MainWindow::PickSavePath(hstring label, std::filesystem::path filename, std::wstring extension) {
    com_ptr<IFileSaveDialog> picker;
    check_hresult(CoCreateInstance(__uuidof(FileSaveDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(picker.put())));
    DWORD options{}; check_hresult(picker->GetOptions(&options));
    check_hresult(picker->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT));
    const auto filter = L"*" + extension;
    const COMDLG_FILTERSPEC spec{label.c_str(), filter.c_str()}; check_hresult(picker->SetFileTypes(1, &spec));
    if (!extension.empty() && extension.front() == L'.') check_hresult(picker->SetDefaultExtension(extension.c_str() + 1));
    if (!filename.empty()) check_hresult(picker->SetFileName(filename.filename().c_str()));
    const auto shown = picker->Show(WindowHandle());
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
    check_hresult(shown); com_ptr<IShellItem> result; check_hresult(picker->GetResult(result.put()));
    PWSTR raw{}; check_hresult(result->GetDisplayName(SIGDN_FILESYSPATH, &raw));
    const auto release = std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)>{raw, &CoTaskMemFree};
    return std::filesystem::path{raw};
}

IAsyncAction MainWindow::OpenDocumentAsync() {
    PersistentOperation operation{*this};
    FileOpenPicker picker; check_hresult(picker.as<IInitializeWithWindow>()->Initialize(WindowHandle()));
    picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary); picker.FileTypeFilter().Append(L"*");
    const auto file = co_await picker.PickSingleFileAsync(); if (!file) co_return;
    const auto path = cpn::windows::SourceStore::resolve(std::filesystem::path{std::wstring{file.Path()}});
    if (IsReservedSourcePath(path)) throw hresult_error{E_INVALIDARG, strings_.get(L"ReservedRecoveryPath")};
    for (auto const& document : documents_) if (document->path == path) { Tabs().SelectedItem(document->tab); co_return; }
    auto store = source_store_; apartment_context ui;
    cpn::windows::SourceContents loaded; std::exception_ptr failure;
    co_await resume_background(); try { loaded = store->read(path); } catch (...) { failure = std::current_exception(); }
    co_await ui; if (failure) std::rethrow_exception(failure);
    if (!WorkspaceMutable() || !persistence_gate_.accepts(operation.generation))
        throw hresult_error{E_PENDING, strings_.get(L"PersistentOperationUnavailable")};
    AddDocument(std::move(loaded.path), {std::move(loaded.utf8), loaded.utf8_bom}, std::move(loaded.disk_digest));
}

IAsyncOperation<bool> MainWindow::SaveDocumentAsync(std::shared_ptr<Document> document, bool save_as) {
    if (!document || !document->adapter) co_return false;
    PersistentOperation operation{*this};
    if (!WorkspaceMutable()) throw hresult_error{E_BOUNDS, strings_.get(L"WorkspaceRevisionExhausted")};
    if (!document->adapter->tracking_available()) throw hresult_error{E_FAIL, strings_.get(L"SnapshotTrackingFailed")};
    if (!document->adapter->committed_available()) throw hresult_error{E_PENDING, strings_.get(L"FinishCompositionBeforeSaving")};
    auto snapshot = document->adapter->snapshot();
    auto path = document->path;
    if (save_as || path.empty()) {
        auto const& profile = configuration_.by_id(document->profile);
        auto selected = PickSavePath(strings_.get(profile.label_resource),
            path.empty() ? std::filesystem::path{std::wstring{strings_.get(UiStrings::untitled)} + profile.default_extension} : path.filename(), profile.default_extension);
        if (!selected) co_return false; path = std::move(*selected);
    }
    path = cpn::windows::SourceStore::resolve(path);
    if (IsReservedSourcePath(path)) throw hresult_error{E_INVALIDARG, strings_.get(L"ReservedRecoveryPath")};
    auto store = source_store_;
    auto expected = document->save_point ? document->save_point->disk_digest : std::optional<std::string>{};
    const bool different_target = path != document->path;
    auto point = cpn::DocumentSavePoint{snapshot.revision, cpn::sha256(snapshot.utf8), {}};
    const auto bom = document->utf8_bom;
    apartment_context ui; std::exception_ptr failure; cpn::windows::SourceSaveResult result;
    co_await resume_background();
    try {
        if (different_target) expected = store->disk_digest(path);
        result = store->save(path, snapshot.utf8, bom, expected);
    } catch (...) { failure = std::current_exception(); }
    co_await ui; if (failure) std::rethrow_exception(failure);
    if (!result.replaced) throw hresult_error{E_FAIL, hstring{std::wstring{strings_.get(L"SourceSaveConflict")} + L" " + std::wstring{to_hstring(result.detail)}}};
    const bool admission_valid = WorkspaceMutable() && persistence_gate_.accepts(operation.generation);
    point.disk_digest = std::move(result.disk_digest);
    document->path = std::move(path); document->save_point = std::move(point);
    ScheduleRecoverySave(true);
    const bool current = document->adapter->mark_saved(std::move(snapshot));
    document->notice = std::wstring{strings_.get(current ? L"Saved" : L"SavedEarlierRevision")};
    UpdateDocument(document);
    if (!admission_valid) {
        recovery_blocked_ = true; recovery_notice_ = std::wstring{strings_.get(L"PersistentOperationUnavailable")};
        throw hresult_error{E_FAIL, strings_.get(L"PersistentOperationUnavailable")};
    }
    if (result.status == cpn::windows::SourceSaveResult::Status::durability_unknown)
        throw hresult_error{E_FAIL, hstring{std::wstring{strings_.get(L"SourceDurabilityUnknown")} + L" " + std::wstring{to_hstring(result.detail)}}};
    co_return true;
}

IAsyncAction MainWindow::SaveAllDocumentsAsync() {
    auto snapshot_docs = documents_;
    for (auto const& doc : snapshot_docs) {
        if (doc->closed || !doc->adapter) continue;
        if (doc->adapter->dirty()) {
            Tabs().SelectedItem(doc->tab);
            if (!(co_await SaveDocumentAsync(doc, false))) break;
        }
    }
}

IAsyncAction MainWindow::ExportSnapshotAsync(std::shared_ptr<Document> document) {
    if (!document || !document->adapter) co_return;
    if (!document->adapter->tracking_available()) throw hresult_error{E_FAIL, strings_.get(L"SnapshotTrackingFailed")};
    if (!document->adapter->committed_available()) throw hresult_error{E_PENDING, strings_.get(L"FinishCompositionBeforeSaving")};
    const auto snapshot = document->adapter->snapshot();
    const auto selected = PickSavePath(strings_.get(UiStrings::snapshot_files),
        document->path.empty() ? std::filesystem::path{std::wstring{strings_.get(UiStrings::untitled)}} : document->path.stem(), L".json");
    if (!selected) co_return; const auto path = cpn::windows::SourceStore::resolve(*selected);
    if (IsReservedSourcePath(path)) throw hresult_error{E_INVALIDARG, strings_.get(L"ReservedRecoveryPath")};
    const auto bytes = cpn::windows::SnapshotJson::encode(snapshot, document->path, document->profile);
    apartment_context ui; std::exception_ptr failure;
    co_await resume_background(); try { FileStore::write_atomic(path, bytes); } catch (...) { failure = std::current_exception(); }
    co_await ui; if (failure) std::rethrow_exception(failure);
}

IAsyncAction MainWindow::ExecuteAsync(Command command) {
    auto lifetime = get_strong(); if (!initialized_ || busy_ || shutting_down_ || persistence_gate_.final_active()) co_return;
    if (!WorkspaceMutable() && command != Command::Export) co_return;
    auto document = CurrentDocument(); SetBusy(true); hstring error;
    try {
        switch (command) {
        case Command::New: AddDocument({}, {}); break;
        case Command::Open: co_await OpenDocumentAsync(); break;
        case Command::Save: (void)(co_await SaveDocumentAsync(document, false)); break;
        case Command::SaveAs: (void)(co_await SaveDocumentAsync(document, true)); break;
        case Command::SaveAll: co_await SaveAllDocumentsAsync(); break;
        case Command::Export: co_await ExportSnapshotAsync(document); break;
        }
    } catch (...) { error = CurrentError(); }
    if (!error.empty()) co_await ShowErrorAsync(command == Command::Open ? UiStrings::error_open : UiStrings::error_save, error);
    SetBusy(false);
}

void MainWindow::RemoveDocument(std::shared_ptr<Document> const& document) {
    if (!WorkspaceMutable()) throw hresult_error{E_PENDING, strings_.get(L"PersistentOperationUnavailable")};
    auto doc_id = document->adapter ? to_string(hstring{document->adapter->document_id()})
        : (document->initial_record ? document->initial_record->document_id : std::string{});
    CancelRun(document); document->closed = true; document->loaded.revoke();
    if (document->adapter) document->adapter->dispose(); document->adapter.reset();
    CleanupDocumentArtifacts(doc_id);
    std::uint32_t index{};
    if (EditorDeck().Children().IndexOf(document->control, index)) EditorDeck().Children().RemoveAt(index);
    if (TestDeck().Children().IndexOf(document->test_view, index)) TestDeck().Children().RemoveAt(index);
    std::erase(documents_, document);
    ScheduleRecoverySave();
    mutating_tabs_ = true;
    if (Tabs().TabItems().IndexOf(document->tab, index)) Tabs().TabItems().RemoveAt(index);
    mutating_tabs_ = false;
    if (documents_.empty()) AddDocument({}, {});
    else { if (!CurrentDocument()) Tabs().SelectedItem(documents_.front()->tab); SelectDocument(CurrentDocument()); }
}

IAsyncAction MainWindow::CloseDocumentTabAsync(std::shared_ptr<Document> document) {
    auto lifetime = get_strong(); if (busy_ || shutting_down_ || !WorkspaceMutable() || !document) co_return;
    SetBusy(true); hstring error;
    try { if (co_await ConfirmDiscardAsync(document)) RemoveDocument(document); } catch (...) { error = CurrentError(); }
    if (!error.empty()) co_await ShowErrorAsync(UiStrings::error_save, error);
    SetBusy(false);
}

IAsyncAction MainWindow::CloseSavedTabsAsync() {
    auto lifetime = get_strong(); if (busy_ || shutting_down_ || !WorkspaceMutable()) co_return;
    SetBusy(true); hstring error;
    try {
        auto copy = documents_;
        for (auto const& doc : copy) {
            if (doc->closed || !doc->adapter) continue;
            if (doc->adapter->tracking_available() && doc->adapter->committed_available() && !doc->adapter->dirty()) {
                RemoveDocument(doc);
            }
        }
    } catch (...) { error = CurrentError(); }
    if (!error.empty()) co_await ShowErrorAsync(UiStrings::error_save, error);
    SetBusy(false);
}

fire_and_forget MainWindow::Tabs_CloseRequested(TabView const&, TabViewTabCloseRequestedEventArgs const& args) {
    std::shared_ptr<Document> document;
    for (auto const& value : documents_) if (value->tab == args.Tab()) { document = value; break; }
    if (document) co_await CloseDocumentTabAsync(std::move(document));
}
fire_and_forget MainWindow::CloseTab_Click(IInspectable const&, RoutedEventArgs const&) {
    if (auto doc = CurrentDocument()) co_await CloseDocumentTabAsync(std::move(doc));
}
fire_and_forget MainWindow::CloseSavedTabs_Click(IInspectable const&, RoutedEventArgs const&) {
    co_await CloseSavedTabsAsync();
}

fire_and_forget MainWindow::OnClosing(Microsoft::UI::Windowing::AppWindow const&, Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args) {
    auto lifetime = get_strong(); if (closing_allowed_) co_return; args.Cancel(true);
    if (busy_ || shutting_down_ || companion_operating_ || persistence_gate_.operation_active()) co_return;
    recovery_flushing_ = true;
    SetBusy(true); hstring error; bool proceed = false; bool workers_stopped = false;
    try {
        if (!(co_await FlushRecoveryAsync())) {
            error = strings_.get(recovery_blocked_ ? L"RecoveryCloseBlocked" : L"ChangedDuringClose");
        } else {
            const auto approved = *recovery_saved_content_;
            for (auto const& document : documents_) CancelRun(document);
            shutting_down_ = true; core_ready_ = false; UpdateControls();
            if (recovery_timer_) recovery_timer_.Stop();
            auto runner = runner_; auto worker = session_worker_; auto companion = companion_worker_; const auto expected = recovery_disk_digest_;
            const auto recovery_file = recovery_paths_.recovery_file;
            apartment_context ui; std::exception_ptr failure; cpn::SessionLoadResult persisted;
            co_await resume_background();
            try {
                if (companion) companion->close();
                if (runner) runner->close();
                persisted = cpn::windows::RecoveryModel::load_response(worker->load(cpn::path_to_utf8(recovery_file)).get());
            } catch (...) { failure = std::current_exception(); }
            try { if (worker) worker->close(); } catch (...) { if (!failure) failure = std::current_exception(); }
            co_await ui; workers_stopped = true;
            if (failure) std::rethrow_exception(failure);
            if (persisted.status != cpn::SessionLoadStatus::loaded || persisted.disk_digest != expected) {
                recovery_blocked_ = true;
                if (persisted.status == cpn::SessionLoadStatus::invalid || persisted.status == cpn::SessionLoadStatus::unsupported_schema) recovery_retry_allowed_ = false;
                error = strings_.get(L"RecoveryCloseBlocked"); recovery_notice_ = std::wstring{error};
            } else {
                auto current = CaptureWorkspace();
                if (current) { current->revision = 0; proceed = cpn::serialize_workspace_session(*current) == approved; }
                if (!proceed) error = strings_.get(L"ChangedDuringClose");
            }
        }
    } catch (...) { error = CurrentError(); }
    if (!proceed) {
        shutting_down_ = false; recovery_flushing_ = false;
        if (workers_stopped) {
            for (auto const& document : documents_) {
                document->run_active = false; document->pending_run.reset(); document->run_token.clear(); document->cancel_requested = false;
            }
            try {
                session_worker_ = std::make_shared<cpn::windows::SessionWorker>(recovery_configuration_.options);
                companion_worker_ = std::make_shared<cpn::windows::CompanionWorker>(companion_configuration_.worker);
                companion_listener_ = {}; UpdateCompanionControls();
                StartRunner();
            } catch (...) { error = CurrentError(); recovery_blocked_ = true; }
        }
        ScheduleRecoverySave();
    }
    SetBusy(false);
    if (!error.empty()) co_await ShowErrorAsync(L"RecoveryError", error);
    if (proceed) { closing_allowed_ = true; Close(); }
}
fire_and_forget MainWindow::New_Click(IInspectable const&, RoutedEventArgs const&) { co_await ExecuteAsync(Command::New); }
fire_and_forget MainWindow::Open_Click(IInspectable const&, RoutedEventArgs const&) { co_await ExecuteAsync(Command::Open); }
fire_and_forget MainWindow::Save_Click(IInspectable const&, RoutedEventArgs const&) { co_await ExecuteAsync(Command::Save); }
fire_and_forget MainWindow::SaveAs_Click(IInspectable const&, RoutedEventArgs const&) { co_await ExecuteAsync(Command::SaveAs); }
fire_and_forget MainWindow::SaveAll_Click(IInspectable const&, RoutedEventArgs const&) { co_await ExecuteAsync(Command::SaveAll); }
fire_and_forget MainWindow::Export_Click(IInspectable const&, RoutedEventArgs const&) { co_await ExecuteAsync(Command::Export); }
}
