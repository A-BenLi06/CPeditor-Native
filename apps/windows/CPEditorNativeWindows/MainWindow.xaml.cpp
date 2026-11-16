#include "pch.h"
#include "MainWindow.xaml.h"
#include "SnapshotJson.h"
#include "TestTextChange.h"
#include "cpn/batch.hpp"
#include "cpn/digest.hpp"
#include <objbase.h>
#include <shobjidl.h>
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
using cpn::windows::FileStore;
using cpn::windows::UiStrings;
namespace {
std::string identity() {
    GUID value{}; check_hresult(CoCreateGuid(&value));
    wchar_t text[40]{};
    if (!StringFromGUID2(value, text, static_cast<int>(std::size(text)))) throw_hresult(E_UNEXPECTED);
    return to_string(hstring{text});
}
bool checked(CheckBox const& box) { const auto value = box.IsChecked(); return value && value.Value(); }
void set_checked(CheckBox const& box, bool value) { box.IsChecked(box_value(value).as<IReference<bool>>()); }
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
    AppWindow().Resize(Windows::Graphics::SizeInt32{1280, 820});
    closing_ = AppWindow().Closing(auto_revoke, {this, &MainWindow::OnClosing});
    const auto weak = get_weak();
    closed_ = Closed(auto_revoke, [weak](auto const&, auto const&) {
        if (auto self = weak.get()) {
            self->shutting_down_ = true;
            if (self->recovery_timer_) self->recovery_timer_.Stop();
            self->recovery_tick_.revoke();
            self->closing_.revoke(); self->closed_.revoke();
            for (auto const& document : self->documents_) {
                document->loaded.revoke();
                if (document->adapter) document->adapter->dispose();
            }
            self->documents_.clear();
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
        configuration_ = cpn::windows::EditorConfiguration::parse(co_await FileIO::ReadTextAsync(file));
        file = co_await StorageFile::GetFileFromApplicationUriAsync(Uri{L"ms-appx:///runner-settings.json"});
        runner_configuration_ = cpn::windows::RunnerConfiguration::parse(to_string(co_await FileIO::ReadTextAsync(file)));
        file = co_await StorageFile::GetFileFromApplicationUriAsync(Uri{L"ms-appx:///recovery-settings.json"});
        recovery_configuration_ = cpn::windows::RecoveryConfiguration::parse(to_string(co_await FileIO::ReadTextAsync(file)));
        file = co_await StorageFile::GetFileFromApplicationUriAsync(Uri{L"ms-appx:///companion-settings.json"});
        companion_configuration_ = cpn::windows::CompanionConfiguration::parse(to_string(co_await FileIO::ReadTextAsync(file)));
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
        co_await resume_background();
        try {
            try {
                if (auto location = cpn::native_file_store()->read_optional(location_file, recovery_config.maximum_location_bytes))
                    recovery_file = cpn::windows::RecoveryModel::parse_location(*location, recovery_config);
            } catch (...) { location_failure = std::current_exception(); }
            std::filesystem::create_directories(recovery_file.parent_path());
            loaded = cpn::windows::RecoveryModel::load_response(worker->load(cpn::path_to_utf8(recovery_file)).get());
            if (loaded.status == cpn::SessionLoadStatus::loaded) {
                // The baseline describes the real persisted state BEFORE native
                // normalization. Startup only spends a revision if capture changes it.
                loaded_content = cpn::windows::RecoveryModel::logical_content(*loaded.session);
                if (cpn::windows::RevisionPolicy::exhausted(loaded.session->revision)) frozen = *loaded.session;
                prepared = cpn::windows::RecoveryModel::prepare(std::move(*loaded.session), recovery_config, runner_defaults, profiles, default_profile);
            }
        } catch (...) { failure = std::current_exception(); }
        co_await ui;
        recovery_paths_ = recovery_configuration_.paths(recovery_file);
        if (prepared && prepared->toolchains) {
            try {
                // Validate every resource before applying any recovered native state.
                for (auto const& choice : *prepared->toolchains)
                    if (strings_.get(to_hstring(choice.label_resource)).empty()) throw std::invalid_argument("Unknown restored toolchain label resource.");
                runner_configuration_.toolchains = std::move(*prepared->toolchains);
            } catch (...) { failure = std::current_exception(); prepared.reset(); }
        }
        for (auto const& choice : runner_configuration_.toolchains) {
            ComboBoxItem item; item.Content(box_value(strings_.get(to_hstring(choice.label_resource)))); Toolchains().Items().Append(item);
        }
        for (auto const& choice : runner_configuration_.comparisons) {
            ComboBoxItem item; item.Content(box_value(strings_.get(to_hstring(choice.label_resource)))); Comparisons().Items().Append(item);
        }
        for (auto const& profile : configuration_.profiles) {
            (void)runner_configuration_.default_toolchain(to_string(hstring{profile.id}));
            ComboBoxItem item; item.Content(box_value(strings_.get(profile.label_resource))); Profiles().Items().Append(item);
        }
        initialized_ = true; workspace_id_ = prepared ? prepared->session.workspace_id : identity();
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
        recovery_tick_ = recovery_timer_.Tick(auto_revoke, [weak](auto const&, auto const&) { if (auto self = weak.get()) self->SaveRecoveryAsync(); });
        recovery_initialized_ = true; InitializeCompanionStorage(); StartRunner(); ScheduleRecoverySave(true); UpdateRecoveryControls();
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
fire_and_forget MainWindow::CloseRunnerBackground(std::shared_ptr<cpn::windows::CoreRunner> runner) {
    co_await resume_background(); runner->close();
}
fire_and_forget MainWindow::CloseSessionBackground(std::shared_ptr<cpn::windows::SessionWorker> worker) {
    co_await resume_background(); worker->close();
}
fire_and_forget MainWindow::CloseCompanionBackground(std::shared_ptr<cpn::windows::CompanionWorker> worker) {
    co_await resume_background(); worker->close();
}
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
std::shared_ptr<MainWindow::Document> MainWindow::CurrentDocument() {
    const auto selected = Tabs().SelectedItem();
    for (auto const& document : documents_) if (selected == document->tab) return document;
    return {};
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
    document->path = record.original_path.value_or(std::filesystem::path{});
    document->profile = std::wstring{to_hstring(record.language_id.value_or(to_string(hstring{configuration_.default_profile})))};
    document->toolchain = record.toolchain_profile_id.value_or(runner_configuration_.default_toolchain(to_string(hstring{document->profile})));
    document->comparison = prefs.comparison; document->utf8_bom = prefs.utf8_bom;
    document->test_revision = prefs.test_revision; document->restore_preferences = prefs;
    document->preserved_preferences = record.editor_preferences; document->save_point = record.save_point;
    document->control.IsEnabled(false); document->control.Visibility(Visibility::Collapsed);
    document->test_view.Visibility(Visibility::Collapsed);
    document->test_view.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    document->test_panel.Margin(Thickness{12, 0, 12, 12}); document->test_panel.Spacing(12);
    document->run_status.TextWrapping(TextWrapping::Wrap);
    document->run_details.TextWrapping(TextWrapping::Wrap); document->run_details.IsTextSelectionEnabled(true);
    document->test_panel.Children().Append(document->run_status); document->test_panel.Children().Append(document->run_details);
    document->test_view.Content(document->test_panel);
    for (auto const& test : record.tests) AddTest(document, test);
    document->initial_record = std::move(record);
    const auto weak = get_weak(); const std::weak_ptr<Document> weak_document = document;
    document->loaded = document->control.Loaded(auto_revoke, [weak, weak_document](auto const&, auto const&) {
        if (auto self = weak.get()) if (auto value = weak_document.lock()) self->InitializeDocument(std::move(value));
    });
    const auto previous_tab = Tabs().SelectedItem();
    documents_.reserve(documents_.size() + 1);
    try {
        documents_.push_back(document);
        EditorDeck().Children().Append(document->control); TestDeck().Children().Append(document->test_view);
        Tabs().TabItems().Append(document->tab); Tabs().SelectedItem(document->tab); SelectDocument(document);
    } catch (...) {
        const auto failure = std::current_exception(); document->closed = true; document->loaded.revoke();
        if (document->adapter) document->adapter->dispose();
        std::uint32_t index{};
        try { if (EditorDeck().Children().IndexOf(document->control, index)) EditorDeck().Children().RemoveAt(index); } catch (...) {}
        try { if (TestDeck().Children().IndexOf(document->test_view, index)) TestDeck().Children().RemoveAt(index); } catch (...) {}
        try { if (Tabs().TabItems().IndexOf(document->tab, index)) Tabs().TabItems().RemoveAt(index); } catch (...) {}
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
        document->adapter->set_tab_width(configuration_.tab_width);
        document->adapter->set_language(configuration_.by_id(document->profile).highlighting_language);
        if (!document->initial_record) throw_hresult(E_UNEXPECTED);
        document->adapter->restore_document(*document->initial_record,
            document->restore_preferences.caret_byte, document->restore_preferences.anchor_byte);
        document->adapter->set_workspace_read_only(!WorkspaceMutable());
        document->initial_record.reset();
        document->control.IsEnabled(true);
        document->loaded.revoke(); UpdateDocument(document); UpdateControls();
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
    if (!selected) { UpdateControls(); return; }
    selecting_document_ = true;
    try {
        select_id(Profiles(), configuration_.profiles, selected->profile);
        select_id(Toolchains(), runner_configuration_.toolchains, selected->toolchain);
        select_id(Comparisons(), runner_configuration_.comparisons, selected->comparison);
    } catch (...) { selecting_document_ = false; throw; }
    selecting_document_ = false;
    UpdateDocument(selected); UpdateControls();
}
void MainWindow::Tabs_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&) {
    if (initialized_) { SelectDocument(CurrentDocument()); ScheduleRecoverySave(); }
}
void MainWindow::Profile_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&) {
    auto document = CurrentDocument(); const auto index = Profiles().SelectedIndex();
    if (selecting_document_ || !document || index < 0 || !TestsMutable(document)) return;
    document->profile = configuration_.profiles.at(static_cast<std::size_t>(index)).id;
    document->toolchain = runner_configuration_.default_toolchain(to_string(hstring{document->profile}));
    if (document->adapter) document->adapter->set_language(configuration_.by_id(document->profile).highlighting_language);
    selecting_document_ = true;
    try { select_id(Toolchains(), runner_configuration_.toolchains, document->toolchain); }
    catch (...) { selecting_document_ = false; throw; }
    selecting_document_ = false;
    TestsChanged(document);
}
void MainWindow::Toolchain_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&) {
    auto document = CurrentDocument(); const auto index = Toolchains().SelectedIndex();
    if (selecting_document_ || !document || index < 0 || !TestsMutable(document)) return;
    document->toolchain = runner_configuration_.toolchains.at(static_cast<std::size_t>(index)).id; TestsChanged(document);
}
void MainWindow::Comparison_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&) {
    auto document = CurrentDocument(); const auto index = Comparisons().SelectedIndex();
    if (selecting_document_ || !document || index < 0 || !TestsMutable(document)) return;
    document->comparison = runner_configuration_.comparisons.at(static_cast<std::size_t>(index)).id; TestsChanged(document);
}
void MainWindow::AddTest(std::shared_ptr<Document> const& document, std::optional<cpn::TestCase> restored) {
    if (!restored && !TestsMutable(document)) return;
    if (document->tests.size() >= runner_configuration_.maximum_tests) throw hresult_error{E_BOUNDS, strings_.get(L"TestLimit")};
    auto test = std::make_shared<TestView>(); test->id = restored ? restored->id : identity(); test->panel.Spacing(6);
    test->input_bytes = restored ? restored->input : std::string{};
    test->expected_bytes = restored && restored->expected ? *restored->expected : std::string{};
    const auto is_binary = [](std::string_view bytes) {
        if (bytes.find('\0') != std::string_view::npos) return true;
        try { FileStore::validate_utf8(bytes); return false; } catch (...) { return true; }
    };
    test->input_binary = is_binary(test->input_bytes); test->expected_binary = is_binary(test->expected_bytes);
    test->enabled_value = !restored || restored->enabled; test->has_expected_value = restored && restored->expected.has_value();
    set_checked(test->enabled, test->enabled_value); test->enabled.Content(box_value(strings_.get(L"TestEnabled")));
    set_checked(test->has_expected, test->has_expected_value); test->has_expected.Content(box_value(strings_.get(L"HasExpected")));
    test->input.AcceptsReturn(true); test->input.TextWrapping(TextWrapping::Wrap); test->input.MinHeight(72);
    test->input.Header(box_value(strings_.get(test->input_binary ? L"BinaryTestInput" : L"TestInput")));
    test->input.Text(test->input_binary ? RenderBytes(test->input_bytes) : to_hstring(test->input_bytes)); test->input.IsReadOnly(test->input_binary);
    test->expected.AcceptsReturn(true); test->expected.TextWrapping(TextWrapping::Wrap); test->expected.MinHeight(72);
    test->expected.Header(box_value(strings_.get(test->expected_binary ? L"BinaryTestExpected" : L"TestExpected")));
    test->expected.Text(test->expected_binary ? RenderBytes(test->expected_bytes) : to_hstring(test->expected_bytes)); test->expected.IsReadOnly(test->expected_binary);
    test->result.TextWrapping(TextWrapping::Wrap); test->result.IsTextSelectionEnabled(true);
    test->result.Text(strings_.get(L"VerdictNotRun"));
    test->remove.Content(box_value(strings_.get(L"RemoveTest")));
    test->panel.Children().Append(test->enabled); test->panel.Children().Append(test->input);
    test->panel.Children().Append(test->has_expected); test->panel.Children().Append(test->expected);
    test->panel.Children().Append(test->result); test->panel.Children().Append(test->remove);
    const auto weak = get_weak(); const std::weak_ptr<Document> weak_document = document; const std::weak_ptr<TestView> weak_test = test;
    const auto before_text = [weak, weak_document, weak_test](bool input, auto const& args) {
        bool allowed = false;
        if (auto self = weak.get()) if (auto value = weak_document.lock()) if (auto item = weak_test.lock())
            allowed = !(input ? item->input_binary : item->expected_binary) && self->TestsMutable(value);
        args.Cancel(!allowed);
    };
    test->input.BeforeTextChanging([before_text](auto const&, auto const& args) { before_text(true, args); });
    test->expected.BeforeTextChanging([before_text](auto const&, auto const& args) { before_text(false, args); });
    test->input.TextChanging([weak, weak_document, weak_test](auto const&, auto const&) {
        if (auto self = weak.get()) if (auto value = weak_document.lock()) if (auto item = weak_test.lock())
            self->RecordTestText(value, item, true);
    });
    test->expected.TextChanging([weak, weak_document, weak_test](auto const&, auto const&) {
        if (auto self = weak.get()) if (auto value = weak_document.lock()) if (auto item = weak_test.lock())
            self->RecordTestText(value, item, false);
    });
    const auto changed = [weak, weak_document, weak_test](auto const&, auto const&) {
        if (auto self = weak.get()) if (auto value = weak_document.lock()) if (auto item = weak_test.lock()) {
            const bool enabled = checked(item->enabled), has_expected = checked(item->has_expected);
            if (enabled == item->enabled_value && has_expected == item->has_expected_value) return;
            if (!self->TestsMutable(value)) {
                set_checked(item->enabled, item->enabled_value); set_checked(item->has_expected, item->has_expected_value);
                return;
            }
            item->enabled_value = enabled; item->has_expected_value = has_expected; self->TestsChanged(value);
        }
    };
    test->enabled.Checked(changed); test->enabled.Unchecked(changed);
    test->has_expected.Checked(changed); test->has_expected.Unchecked(changed);
    test->remove.Click([weak, weak_document, weak_test](auto const&, auto const&) {
        if (auto self = weak.get()) if (auto value = weak_document.lock()) if (auto item = weak_test.lock(); item && self->TestsMutable(value)) {
            std::uint32_t index{};
            if (value->test_panel.Children().IndexOf(item->panel, index)) value->test_panel.Children().RemoveAt(index);
            std::erase(value->tests, item); self->TestsChanged(value);
        }
    });
    document->tests.push_back(test); document->test_panel.Children().Append(test->panel);
    if (!restored) TestsChanged(document);
}
void MainWindow::TestsChanged(std::shared_ptr<Document> const& document) {
    if (document->closed || !TestsMutable(document)) return;
    document->test_revision = cpn::windows::RevisionPolicy::next(document->test_revision); UpdateDocument(document); UpdateControls(); ScheduleRecoverySave();
}
void MainWindow::RecordTestText(std::shared_ptr<Document> const& document, std::shared_ptr<TestView> const& test, bool input) {
    // TextChanging runs synchronously with the new Text; never update the visual tree here.
    try {
        auto& bytes = input ? test->input_bytes : test->expected_bytes;
        const auto& box = input ? test->input : test->expected;
        const auto result = cpn::windows::TestTextChange::record(bytes, document->test_revision, to_string(box.Text()),
            !(input ? test->input_binary : test->expected_binary) && TestsMutable(document));
        if (result == cpn::windows::TestTextChange::Result::unchanged) return;
        if (result == cpn::windows::TestTextChange::Result::rejected) document->test_text_tracking_failed = true;
    } catch (...) { document->test_text_tracking_failed = true; }
    recovery_pending_ = true;
    try { ScheduleDocumentChanged(document); } catch (...) { /* The authoritative model remains guarded. */ }
}
fire_and_forget MainWindow::AddTest_Click(IInspectable const&, RoutedEventArgs const&) {
    auto lifetime = get_strong(); hstring error;
    try { if (auto document = CurrentDocument()) AddTest(document); } catch (...) { error = CurrentError(); }
    if (!error.empty()) co_await ShowErrorAsync(L"TestError", error);
}
void MainWindow::UpdateDocument(std::shared_ptr<Document> const& document) {
    const auto name = document->path.empty() ? std::wstring{strings_.get(UiStrings::untitled)} : document->path.filename().wstring();
    const bool available = document->adapter && document->adapter->committed_available();
    const bool dirty = document->adapter ? (!available || document->adapter->dirty()) :
        (document->initial_record && (!document->save_point || cpn::sha256(document->initial_record->source_text) != document->save_point->source_digest));
    document->tab.Header(box_value(hstring{name + (dirty ? std::wstring{strings_.get(UiStrings::unsaved_suffix)} : L"")}));
    auto const state = runner_configuration_.state_labels.find(document->run_state);
    auto state_text = state == runner_configuration_.state_labels.end() ? strings_.get(L"RunUnknown") : strings_.get(to_hstring(state->second));
    std::wstring run_status{state_text};
    if (document->run_capability != cpn::windows::CompanionRunCapability::ordinary)
        run_status += L"\n" + std::wstring{strings_.get(document->run_capability == cpn::windows::CompanionRunCapability::unsupported ? L"CompanionRunUnsupported" : L"CompanionRunUnavailable")};
    if (document->cancel_requested) run_status += L"  " + std::wstring{strings_.get(L"CancellationRequested")};
    if (document->result) {
        const bool stale = !available || document->result->revision != document->adapter->revision() ||
                           document->result_test_revision != document->test_revision;
        run_status += L"\n" + UiStrings::format(strings_.get(L"ResultRevisionFormat"), {std::to_wstring(document->result->revision)});
        if (stale) run_status += L"  " + std::wstring{strings_.get(L"ResultStale")};
    }
    document->run_status.Text(hstring{run_status});
    if (CurrentDocument() != document) return;
    Title(hstring{UiStrings::format(strings_.get(UiStrings::window_title_format), {
        std::wstring{strings_.get(UiStrings::app_title)}, name, dirty ? std::wstring{strings_.get(UiStrings::unsaved_suffix)} : L""})});
    hstring status;
    if (!document->adapter) status = strings_.get(L"EditorInitializing");
    else if (document->test_text_tracking_failed) status = strings_.get(L"TestTextTrackingFailed");
    else if (!document->adapter->tracking_available()) status = strings_.get(L"SnapshotTrackingFailed");
    else if (!available) status = strings_.get(L"CompositionOrNativeBusy");
    else status = hstring{UiStrings::format(strings_.get(UiStrings::status_format), {
        std::wstring{strings_.get(configuration_.by_id(document->profile).label_resource)}, std::to_wstring(document->adapter->revision()),
        std::to_wstring(document->adapter->length()), std::to_wstring(document->adapter->caret_byte()), std::to_wstring(document->adapter->anchor_byte())})};
    if (persistence_gate_.final_active()) status = hstring{std::wstring{status} + L"  " + std::wstring{strings_.get(L"FinalRevisionCommitting")}};
    else if (!WorkspaceMutable()) status = hstring{std::wstring{status} + L"  " + std::wstring{strings_.get(L"WorkspaceRevisionExhausted")}};
    else if ((available && cpn::windows::RevisionPolicy::exhausted(document->adapter->revision())) || cpn::windows::RevisionPolicy::exhausted(document->test_revision))
        status = hstring{std::wstring{status} + L"  " + std::wstring{strings_.get(L"DocumentRevisionExhausted")}};
    if (!document->notice.empty()) status = hstring{std::wstring{status} + L"  " + document->notice};
    Status().Text(status);
}
bool MainWindow::WorkspaceRevisionAvailable() const noexcept {
    return !cpn::windows::RevisionPolicy::exhausted(recovery_revision_);
}
bool MainWindow::WorkspaceMutable() const noexcept {
    return WorkspaceRevisionAvailable() && !persistence_gate_.final_active();
}
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
    const auto document = CurrentDocument(); const bool editor_ready = document && document->adapter;
    Commands().IsEnabled(initialized_ && !busy_ && !shutting_down_ && !persistence_gate_.final_active());
    const bool metadata_mutable = initialized_ && !shutting_down_ && document && TestsMutable(document);
    Profiles().IsEnabled(metadata_mutable); Toolchains().IsEnabled(metadata_mutable); Comparisons().IsEnabled(metadata_mutable);
    NewButton().IsEnabled(WorkspaceMutable()); OpenButton().IsEnabled(WorkspaceMutable());
    SaveButton().IsEnabled(WorkspaceMutable()); SaveAsButton().IsEnabled(WorkspaceMutable());
    ExportButton().IsEnabled(editor_ready);
    for (auto const& value : documents_) {
        value->tab.IsClosable(WorkspaceMutable());
        if (value->adapter) value->adapter->set_workspace_read_only(!WorkspaceMutable());
        const bool mutable_tests = TestsMutable(value);
        for (auto const& test : value->tests) {
            test->input.IsReadOnly(test->input_binary || !mutable_tests); test->expected.IsReadOnly(test->expected_binary || !mutable_tests);
            test->enabled.IsEnabled(mutable_tests); test->has_expected.IsEnabled(mutable_tests); test->remove.IsEnabled(mutable_tests);
        }
    }
    RunButton().IsEnabled(initialized_ && core_ready_ && !shutting_down_ && !persistence_gate_.final_active() && editor_ready &&
        !document->test_text_tracking_failed && !document->run_active && document->run_capability == cpn::windows::CompanionRunCapability::ordinary);
    CancelButton().IsEnabled(!shutting_down_ && document && document->run_active && !document->cancel_requested);
    AddTestButton().IsEnabled(metadata_mutable);
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
        request.document_id = to_string(hstring{snapshot.document_id}); request.revision = snapshot.revision;
        request.source = std::move(snapshot.utf8);
        if (!document->path.empty()) request.original_path = to_string(hstring{document->path.wstring()});
        runner_->submit(std::move(request)); document->pending_run.reset(); document->run_state = "Queued";
    } catch (...) {
        document->pending_run.reset(); document->run_active = false; document->run_token.clear(); document->run_state = "Failed"; throw;
    }
}
fire_and_forget MainWindow::Run_Click(IInspectable const&, RoutedEventArgs const&) {
    auto lifetime = get_strong(); auto document = CurrentDocument(); hstring error;
    if (!document || !document->adapter || !core_ready_ || shutting_down_ || document->run_active || persistence_gate_.final_active()) co_return;
    try {
        if (document->run_capability != cpn::windows::CompanionRunCapability::ordinary)
            throw hresult_error{E_NOTIMPL, strings_.get(document->run_capability == cpn::windows::CompanionRunCapability::unsupported ? L"CompanionRunUnsupported" : L"CompanionRunUnavailable")};
        if (!document->adapter->tracking_available()) throw hresult_error{E_FAIL, strings_.get(L"SnapshotTrackingFailed")};
        if (document->test_text_tracking_failed) throw hresult_error{E_FAIL, strings_.get(L"TestTextTrackingFailed")};
        cpn::windows::RunRequest request; request.token = identity(); request.comparison = document->comparison;
        auto const choice = std::find_if(runner_configuration_.toolchains.begin(), runner_configuration_.toolchains.end(),
            [&](auto const& value) { return value.id == document->toolchain; });
        if (choice == runner_configuration_.toolchains.end()) throw_hresult(E_INVALIDARG);
        request.toolchain = choice->profile;
        for (auto const& test : document->tests) request.tests.push_back({test->id, test->input_bytes,
            test->has_expected_value ? std::optional<std::string>{test->expected_bytes} : std::nullopt, test->enabled_value});
        document->run_token = request.token; document->pending_run = std::move(request);
        document->run_active = true; document->cancel_requested = false; document->last_sequence = 0;
        document->run_test_revision = document->test_revision; document->run_state = "AwaitingCommittedText"; document->notice.clear();
        SubmitPendingRun(document); UpdateDocument(document); UpdateControls();
    } catch (...) { error = CurrentError(); CancelRun(document); document->run_state = "Failed"; UpdateDocument(document); UpdateControls(); }
    if (!error.empty()) co_await ShowErrorAsync(L"RunError", error);
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
            document->result = std::move(event.result); document->result_test_revision = document->run_test_revision; RenderResult(document);
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
void MainWindow::RenderResult(std::shared_ptr<Document> const& document) {
    if (!document->result) return;
    auto const& output = *document->result;
    const auto build_label = runner_configuration_.build_labels.find(output.build_outcome);
    const auto build_text = build_label == runner_configuration_.build_labels.end() ? strings_.get(L"RunUnknown") : strings_.get(to_hstring(build_label->second));
    auto build_details = UiStrings::format(strings_.get(L"BuildOutputFormat"), {std::wstring{build_text},
        std::wstring{RenderBytes(output.build_output)}, std::wstring{RenderBytes(output.build_error)}, std::wstring{to_hstring(output.error)}});
    if (output.build_stdout_truncated || output.build_stderr_truncated) build_details += L"\n" + std::wstring{strings_.get(L"CoreOutputTruncated")};
    document->run_details.Text(hstring{build_details});
    for (auto const& test : document->tests) {
        const auto result = std::find_if(output.tests.begin(), output.tests.end(), [&](auto const& value) { return value.id == test->id; });
        if (result == output.tests.end()) { test->result.Text(strings_.get(L"VerdictNotRun")); continue; }
        auto const label = runner_configuration_.verdict_labels.find(result->verdict);
        auto const verdict = label == runner_configuration_.verdict_labels.end() ? strings_.get(L"RunUnknown") : strings_.get(to_hstring(label->second));
        auto text = UiStrings::format(strings_.get(L"TestResultFormat"), {std::wstring{verdict}, std::to_wstring(result->elapsed_ms),
            std::wstring{RenderBytes(result->standard_output)}, std::wstring{RenderBytes(result->standard_error)}, std::wstring{to_hstring(result->detail)}});
        if (result->stdout_truncated || result->stderr_truncated) text += L"\n" + std::wstring{strings_.get(L"CoreOutputTruncated")};
        test->result.Text(hstring{text});
    }
}
void MainWindow::UpdateRecoveryControls() {
    RecoveryStatus().Text(hstring{recovery_notice_});
    RetryRecoveryButton().IsEnabled(recovery_initialized_ && recovery_retry_allowed_ && !busy_ && !shutting_down_ && !recovery_saving_ && !recovery_copying_);
    SaveRecoveryAsButton().IsEnabled(recovery_initialized_ && !busy_ && !shutting_down_ && !recovery_copying_);
}
void MainWindow::ScheduleRecoverySave(bool immediate) {
    recovery_pending_ = true;
    if (!recovery_initialized_ || recovery_blocked_ || shutting_down_ || recovery_copying_ || recovery_saving_ || recovery_flushing_) return;
    if (immediate) { SaveRecoveryAsync(); return; }
    // This timer merges writes. Only the editor's committed barrier decides whether text may be captured.
    if (!recovery_timer_.IsRunning()) recovery_timer_.Start();
}
std::optional<cpn::WorkspaceSession> MainWindow::CaptureWorkspace() {
    cpn::WorkspaceSession session; session.workspace_id = workspace_id_; session.revision = recovery_revision_;
    if (!WorkspaceRevisionAvailable()) session.selected_document_id = recovery_selected_baseline_;
    session.editor_preferences = workspace_preserved_preferences_;
    bool preferences_limited = !cpn::windows::RecoveryModel::merge_preferences(session.editor_preferences,
        cpn::windows::RecoveryModel::workspace_preferences(runner_configuration_.toolchains, recovery_configuration_.workspace_namespace));
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
            // An unloaded/failed editor remains disabled and its immutable record is authoritative.
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
            // No await separates capability freeze from this owned candidate capture.
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
            // A logical match is insufficient: the persisted file may have been changed externally.
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
        recovery_notice_ = std::wstring{strings_.get(recovery_preferences_limited_ ? L"RecoveryPreferencesLimited" : L"RecoverySaved")};
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
    // Retrying never adopts an externally changed file's digest. A conflict must be resolved by a new recovery location.
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
            recovery_blocked_ = false; recovery_retry_allowed_ = true; recovery_notice_ = std::wstring{strings_.get(L"RecoverySaved")};
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
        // A presentation failure keeps the original diagnostic visible without failing the event handler.
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
    const auto name = document->path.empty() ? std::wstring{strings_.get(UiStrings::untitled)} : document->path.filename().wstring();
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
        // The user's Save As selection authorizes this target; normal Save retains the prior disk token.
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
        // Keep an actual completed write's token/baseline observable rather than discarding its result.
        recovery_blocked_ = true; recovery_notice_ = std::wstring{strings_.get(L"PersistentOperationUnavailable")};
        throw hresult_error{E_FAIL, strings_.get(L"PersistentOperationUnavailable")};
    }
    if (result.status == cpn::windows::SourceSaveResult::Status::durability_unknown)
        throw hresult_error{E_FAIL, hstring{std::wstring{strings_.get(L"SourceDurabilityUnknown")} + L" " + std::wstring{to_hstring(result.detail)}}};
    co_return true;
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
        case Command::Export: co_await ExportSnapshotAsync(document); break;
        }
    } catch (...) { error = CurrentError(); }
    if (!error.empty()) co_await ShowErrorAsync(command == Command::Open ? UiStrings::error_open : UiStrings::error_save, error);
    SetBusy(false);
}
void MainWindow::RemoveDocument(std::shared_ptr<Document> const& document) {
    if (!WorkspaceMutable()) throw hresult_error{E_PENDING, strings_.get(L"PersistentOperationUnavailable")};
    CancelRun(document); document->closed = true; document->loaded.revoke();
    if (document->adapter) document->adapter->dispose(); document->adapter.reset();
    std::uint32_t index{};
    if (EditorDeck().Children().IndexOf(document->control, index)) EditorDeck().Children().RemoveAt(index);
    if (TestDeck().Children().IndexOf(document->test_view, index)) TestDeck().Children().RemoveAt(index);
    std::erase(documents_, document);
    ScheduleRecoverySave();
    if (Tabs().TabItems().IndexOf(document->tab, index)) Tabs().TabItems().RemoveAt(index);
    if (documents_.empty()) AddDocument({}, {});
    else { if (!CurrentDocument()) Tabs().SelectedItem(documents_.front()->tab); SelectDocument(CurrentDocument()); }
}
fire_and_forget MainWindow::Tabs_CloseRequested(TabView const&, TabViewTabCloseRequestedEventArgs const& args) {
    auto lifetime = get_strong(); if (busy_ || shutting_down_ || !WorkspaceMutable()) co_return;
    std::shared_ptr<Document> document;
    for (auto const& value : documents_) if (value->tab == args.Tab()) { document = value; break; }
    if (!document) co_return; SetBusy(true); hstring error;
    try { if (co_await ConfirmDiscardAsync(document)) RemoveDocument(document); } catch (...) { error = CurrentError(); }
    if (!error.empty()) co_await ShowErrorAsync(UiStrings::error_save, error); SetBusy(false);
}
fire_and_forget MainWindow::OnClosing(Microsoft::UI::Windowing::AppWindow const&, Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args) {
    auto lifetime = get_strong(); if (closing_allowed_) co_return; args.Cancel(true);
    // Pending Open/Save owns live documents. Its completion must run before another close attempt.
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
                // Check persistence after the potentially slow runner shutdown, before closing the Session worker.
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
fire_and_forget MainWindow::Export_Click(IInspectable const&, RoutedEventArgs const&) { co_await ExecuteAsync(Command::Export); }
}
