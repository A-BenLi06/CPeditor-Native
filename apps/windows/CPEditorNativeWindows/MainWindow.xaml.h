#pragma once
#include "MainWindow.g.h"
#include "CheckerModel.h"
#include "CompanionModel.h"
#include "EditorAssist.h"
#include "EditorProfiles.h"
#include "FileStore.h"
#include "PersistenceGate.h"
#include "RecoveryModel.h"
#include "RunnerConfiguration.h"
#include "SourceStore.h"
#include "ToolchainDiscovery.h"
#include "UiStrings.h"
#include "WinUIEditAdapter.h"
#include <deque>
#include <optional>

namespace winrt::CPEditorNativeWindows::implementation {
struct MainWindow : MainWindowT<MainWindow> {
    MainWindow();
    winrt::fire_and_forget Root_Loaded(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget New_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget Open_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget Save_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget SaveAs_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget SaveAll_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget CloseTab_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget CloseSavedTabs_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget Export_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void Undo_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void Redo_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ToggleWordWrap_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ToggleAutoPair_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ViewEditorOnly_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ViewSplit_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ViewTestsOnly_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ToggleTestPanel_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ToggleCompanionPane_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget Compile_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget RunOnly_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget Run_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget Cancel_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget AddTest_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget UseAllOutputs_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget DeleteAllSamples_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget ConfigureChecker_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void CheckerToggle_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void CompilerToggle_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void CompilerCopy_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget Settings_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void RescanToolchains_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget RetryRecovery_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget SaveRecoveryAs_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void CompanionFilter_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget ImportCompanion_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget RefreshCompanion_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget MoreCompanion_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget ApplyCompanion_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget StartCompanion_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget StopCompanion_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void Companion_SelectionChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void Tabs_SelectionChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    winrt::fire_and_forget Tabs_AddTabButtonClick(Microsoft::UI::Xaml::Controls::TabView const&, Windows::Foundation::IInspectable const&);
    winrt::fire_and_forget Tabs_CloseRequested(Microsoft::UI::Xaml::Controls::TabView const&, Microsoft::UI::Xaml::Controls::TabViewTabCloseRequestedEventArgs const&);
    void Profile_SelectionChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void Toolchain_SelectionChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void Comparison_SelectionChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void SplitHandle_PointerPressed(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
    void SplitHandle_PointerMoved(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
    void SplitHandle_PointerReleased(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
    void SplitHandle_PointerCaptureLost(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
    void SplitHandle_PointerEntered(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
    void SplitHandle_PointerExited(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);

private:
    enum class ViewMode { EditorOnly, Split, TestsOnly };
    enum class CompanionFilter { All, Pending, Applied };
    enum class Command { New, Open, Save, SaveAs, SaveAll, Export };
    enum class CompanionCommand { Import, Refresh, More, Apply, Start, Stop };

    struct CompiledArtifact {
        std::uint64_t revision{};
        std::string source_digest;
        std::string toolchain_id;
        std::string toolchain_dump;
        std::filesystem::path path;
    };

    struct TestView {
        std::string id;
        Microsoft::UI::Xaml::Controls::Border container{nullptr};
        Microsoft::UI::Xaml::Controls::Border stripe{nullptr};
        Microsoft::UI::Xaml::Controls::TextBlock title{nullptr};
        Microsoft::UI::Xaml::Controls::CheckBox enabled{nullptr};
        Microsoft::UI::Xaml::Controls::Border verdict_badge{nullptr};
        Microsoft::UI::Xaml::Controls::TextBlock verdict_text{nullptr};
        Microsoft::UI::Xaml::Controls::TextBlock elapsed_text{nullptr};
        Microsoft::UI::Xaml::Controls::Button copy_output{nullptr};
        Microsoft::UI::Xaml::Controls::Button use_output{nullptr};
        Microsoft::UI::Xaml::Controls::Button duplicate_btn{nullptr};
        Microsoft::UI::Xaml::Controls::Button remove{nullptr};
        Microsoft::UI::Xaml::Controls::TextBlock input_bytes_label{nullptr};
        Microsoft::UI::Xaml::Controls::TextBlock output_bytes_label{nullptr};
        Microsoft::UI::Xaml::Controls::TextBlock expected_bytes_label{nullptr};
        Microsoft::UI::Xaml::Controls::TextBox input{nullptr};
        Microsoft::UI::Xaml::Controls::TextBox output_box{nullptr};
        Microsoft::UI::Xaml::Controls::TextBox expected{nullptr};
        Microsoft::UI::Xaml::Controls::CheckBox has_expected{nullptr};
        Microsoft::UI::Xaml::Controls::Border diagnostics_panel{nullptr};
        Microsoft::UI::Xaml::Controls::TextBlock diagnostics_text{nullptr};
        std::string input_bytes;
        std::string expected_bytes;
        std::string last_output_bytes;
        bool last_output_adoptable{};
        bool input_binary{};
        bool expected_binary{};
        bool enabled_value{true};
        bool has_expected_value{};
    };

    struct Document {
        Microsoft::UI::Xaml::Controls::TabViewItem tab{nullptr};
        Microsoft::UI::Xaml::Controls::TextBlock tab_title{nullptr};
        Microsoft::UI::Xaml::Controls::Border tab_badge{nullptr};
        Microsoft::UI::Xaml::Controls::TextBlock tab_badge_text{nullptr};
        winrt::WinUIEditor::CodeEditorControl control{nullptr};
        Microsoft::UI::Xaml::FrameworkElement::Loaded_revoker loaded;
        std::shared_ptr<cpn::windows::WinUIEditAdapter> adapter;
        Microsoft::UI::Xaml::Controls::ScrollViewer test_view{nullptr};
        Microsoft::UI::Xaml::Controls::StackPanel test_panel{nullptr};
        Microsoft::UI::Xaml::Controls::Border empty_tests_panel{nullptr};
        std::vector<std::shared_ptr<TestView>> tests;
        std::filesystem::path path;
        std::optional<std::string> companion_title;
        std::optional<cpn::SessionDocument> initial_record;
        std::vector<cpn::NativeEditorPreferences> preserved_preferences;
        std::optional<cpn::DocumentSavePoint> save_point;
        cpn::windows::DocumentPreferences restore_preferences;
        std::wstring profile;
        std::string toolchain;
        std::string comparison;
        bool utf8_bom{};
        bool dispatch_pending{};
        bool test_text_tracking_failed{};
        bool closed{};
        bool run_active{};
        bool cancel_requested{};
        bool compiler_expanded{};
        std::string run_token;
        std::string run_mode{"compile_and_run"};
        std::string run_state{"Idle"};
        std::uint64_t last_sequence{};
        std::uint64_t test_revision{1};
        std::uint64_t run_test_revision{};
        std::uint64_t result_test_revision{};
        std::string run_toolchain_dump;
        std::string result_toolchain_dump;
        std::string run_comparison;
        std::string result_comparison;
        std::string run_checker_fingerprint;
        std::string result_checker_fingerprint;
        std::string result_mode;
        std::optional<std::filesystem::path> pending_artifact_path;
        std::optional<CompiledArtifact> compiled_artifact;
        std::optional<cpn::windows::RunRequest> pending_run;
        std::optional<cpn::windows::RunOutput> result;
        std::wstring notice;
        cpn::windows::CompanionRunCapability run_capability{cpn::windows::CompanionRunCapability::ordinary};
    };

    struct PersistentOperation {
        explicit PersistentOperation(MainWindow&);
        ~PersistentOperation();
        void finish() noexcept;
        PersistentOperation(PersistentOperation const&) = delete;
        PersistentOperation& operator=(PersistentOperation const&) = delete;
        MainWindow& owner;
        std::uint64_t generation;
        bool active{true};
    };
    struct FinalRevisionLease {
        explicit FinalRevisionLease(MainWindow& value) : owner(value), capability(value.persistence_gate_) {}
        ~FinalRevisionLease() noexcept;
        FinalRevisionLease(FinalRevisionLease const&) = delete;
        FinalRevisionLease& operator=(FinalRevisionLease const&) = delete;
        MainWindow& owner;
        cpn::windows::FinalPersistenceLease capability;
    };

    Windows::Foundation::IAsyncAction CompanionAsync(CompanionCommand);
    Windows::Foundation::IAsyncOperation<bool> EnsureCompanionStorageAsync();
    Windows::Foundation::IAsyncAction ReadCompanionPageAsync(bool reset);
    Windows::Foundation::IAsyncAction ApplyCompanionAsync(cpn::CompanionInboxItem);
    void InitializeCompanionStorage();
    void ValidateCompanionStorage() const;
    void RenderCompanionItems();
    void UpdateCompanionControls();
    [[nodiscard]] cpn::CompanionInboxItem const* SelectedCompanionItem();
    [[nodiscard]] winrt::hstring CompanionStatus(std::string const&, std::string const& detail = {}) const;
    void InstallCompanionDelta(cpn::windows::CompanionDelta const&, cpn::WorkspaceSession const& base);

    Windows::Foundation::IAsyncAction ExecuteAsync(Command);
    Windows::Foundation::IAsyncOperation<bool> ConfirmDiscardAsync(std::shared_ptr<Document>);
    Windows::Foundation::IAsyncOperation<bool> SaveDocumentAsync(std::shared_ptr<Document>, bool);
    Windows::Foundation::IAsyncAction SaveAllDocumentsAsync();
    Windows::Foundation::IAsyncAction CloseDocumentTabAsync(std::shared_ptr<Document>);
    Windows::Foundation::IAsyncAction CloseSavedTabsAsync();
    Windows::Foundation::IAsyncAction OpenDocumentAsync();
    Windows::Foundation::IAsyncAction ExportSnapshotAsync(std::shared_ptr<Document>);
    Windows::Foundation::IAsyncAction ShowSettingsDialogAsync(int initial_tab = 0);
    Windows::Foundation::IAsyncAction ShowErrorAsync(std::wstring_view, winrt::hstring);
    Windows::Foundation::IAsyncOperation<Microsoft::UI::Xaml::Controls::ContentDialogResult> ShowNativeDialogAsync(Microsoft::UI::Xaml::Controls::ContentDialog);
    winrt::fire_and_forget OnClosing(Microsoft::UI::Windowing::AppWindow const&, Microsoft::UI::Windowing::AppWindowClosingEventArgs const&);
    winrt::fire_and_forget InitializeDocument(std::shared_ptr<Document>);
    winrt::fire_and_forget DocumentChanged(std::shared_ptr<Document>);
    static winrt::fire_and_forget CloseRunnerBackground(std::shared_ptr<cpn::windows::CoreRunner>);
    static winrt::fire_and_forget CloseSessionBackground(std::shared_ptr<cpn::windows::SessionWorker>);
    static winrt::fire_and_forget CloseCompanionBackground(std::shared_ptr<cpn::windows::CompanionWorker>);

    void ScheduleDocumentChanged(std::weak_ptr<Document> const&);
    void ReceiveCoreEvent(cpn::windows::RunEvent);
    void StartRunner();
    void StartRunWithMode(std::shared_ptr<Document> const&, std::string const& mode);
    void RenderResult(std::shared_ptr<Document> const&);
    void RenderCompilerBanner(std::shared_ptr<Document> const&);
    void UpdateSummaryBadge(std::shared_ptr<Document> const&);
    void RefreshTestHeaders(std::shared_ptr<Document> const&);
    void AdoptOutputAsExpected(std::shared_ptr<Document> const&, std::shared_ptr<TestView> const&);
    void CopyTextToClipboard(winrt::hstring const&);
    void AddDocument(std::filesystem::path, cpn::windows::LoadedDocument, std::optional<std::string> disk_digest = {});
    void AddRestoredDocument(cpn::SessionDocument, cpn::windows::DocumentPreferences);
    void RemoveDocument(std::shared_ptr<Document> const&);
    void AddTest(std::shared_ptr<Document> const&, std::optional<cpn::TestCase> restored = {});
    void TestsChanged(std::shared_ptr<Document> const&);
    void RecordTestText(std::shared_ptr<Document> const&, std::shared_ptr<TestView> const&, bool input);
    void SubmitPendingRun(std::shared_ptr<Document> const&);
    void CancelRun(std::shared_ptr<Document> const&);
    void SelectDocument(std::shared_ptr<Document> const&);
    void UpdateDocument(std::shared_ptr<Document> const&);
    void UpdateControls();
    void SetViewMode(ViewMode mode);
    void ApplyWorkspaceUiPreferences();
    void RescanToolchains();
    void RebuildToolchainComboBox();
    [[nodiscard]] cpn::windows::ToolchainChoice const* FindToolchain(std::string const& id) const;
    [[nodiscard]] cpn::windows::ToolchainProbeResult const* FindProbe(std::string const& id) const;
    [[nodiscard]] std::filesystem::path ManagedArtifactPath(std::shared_ptr<Document> const&, std::string const& artifact_name) const;
    void CleanupDocumentArtifacts(std::string const& document_id) const noexcept;
    void CleanupWorkspaceArtifacts() const noexcept;
    [[nodiscard]] bool IsResultCurrent(std::shared_ptr<Document> const&) const;
    [[nodiscard]] std::wstring ComputeTabTitle(std::shared_ptr<Document> const&) const;
    [[nodiscard]] Microsoft::UI::Xaml::Media::SolidColorBrush VerdictBrush(std::string_view verdict);
    [[nodiscard]] Microsoft::UI::Xaml::Media::SolidColorBrush VerdictBackgroundBrush(std::string_view verdict);
    [[nodiscard]] bool WorkspaceMutable() const noexcept;
    [[nodiscard]] bool WorkspaceRevisionAvailable() const noexcept;
    [[nodiscard]] bool TryBeginFinalRevision(bool owned_busy, FinalRevisionLease&);
    [[nodiscard]] bool WorkspaceCommittedAvailable() const;
    [[nodiscard]] bool TestsMutable(std::shared_ptr<Document> const&) const noexcept;
    void FreezePersistedNavigation(cpn::windows::PreparedRecovery const&);
    void SetBusy(bool);
    void ScheduleRecoverySave(bool immediate = false);
    winrt::fire_and_forget SaveRecoveryAsync();
    Windows::Foundation::IAsyncOperation<bool> FlushRecoveryAsync();
    [[nodiscard]] std::optional<cpn::WorkspaceSession> CaptureWorkspace();
    void UpdateRecoveryControls();
    [[nodiscard]] std::optional<std::filesystem::path> PickSavePath(winrt::hstring label, std::filesystem::path filename, std::wstring extension);
    [[nodiscard]] winrt::hstring RenderBytes(std::string_view) const;
    [[nodiscard]] bool IsReservedSourcePath(std::filesystem::path const&) const;
    [[nodiscard]] HWND WindowHandle();
    [[nodiscard]] std::shared_ptr<Document> CurrentDocument();
    [[nodiscard]] static winrt::hstring CurrentError();

    cpn::windows::UiStrings strings_;
    cpn::windows::EditorConfiguration configuration_;
    cpn::windows::AssistConfig assist_config_;
    cpn::windows::RunnerConfiguration runner_configuration_;
    cpn::windows::CheckerConfiguration checker_configuration_;
    cpn::windows::ToolchainDiscoveryConfiguration discovery_configuration_;
    std::vector<cpn::windows::ToolchainProbeResult> toolchain_probes_;
    cpn::windows::WorkspaceUiPreferences ui_preferences_;
    cpn::windows::CheckerSettings checker_settings_;
    std::vector<std::shared_ptr<Document>> documents_;
    std::shared_ptr<cpn::windows::CoreRunner> runner_;
    cpn::windows::RecoveryConfiguration recovery_configuration_;
    cpn::windows::CompanionConfiguration companion_configuration_;
    std::shared_ptr<cpn::windows::CompanionWorker> companion_worker_;
    std::optional<cpn::windows::CompanionBinding> companion_binding_;
    cpn::windows::CompanionPagingState companion_page_;
    std::vector<std::size_t> companion_visible_indices_;
    CompanionFilter companion_filter_{CompanionFilter::All};
    bool companion_pane_open_{false};
    cpn::windows::CompanionListenerResponse companion_listener_;
    std::wstring companion_notice_;
    bool companion_available_{};
    bool companion_operating_{};
    std::shared_ptr<cpn::windows::SessionWorker> session_worker_;
    std::shared_ptr<cpn::windows::SourceStore> source_store_;
    cpn::SessionPaths recovery_paths_;
    std::filesystem::path recovery_location_file_;
    std::filesystem::path source_lock_directory_;
    std::filesystem::path artifact_root_directory_;
    std::string workspace_id_;
    std::uint64_t recovery_revision_{};
    cpn::windows::PersistenceGate persistence_gate_;
    std::optional<std::string> recovery_disk_digest_;
    std::optional<std::string> recovery_saved_content_;
    std::optional<cpn::WorkspaceSession> recovery_frozen_session_;
    std::optional<std::string> recovery_selected_baseline_;
    std::vector<cpn::NativeEditorPreferences> workspace_preserved_preferences_;
    bool recovery_initialized_{};
    bool recovery_pending_{};
    bool recovery_saving_{};
    bool recovery_blocked_{};
    bool recovery_retry_allowed_{true};
    bool recovery_copying_{};
    bool recovery_flushing_{};
    bool recovery_force_write_{};
    bool recovery_preferences_limited_{};
    std::wstring recovery_notice_;
    winrt::handle recovery_idle_;
    Microsoft::UI::Dispatching::DispatcherQueueTimer recovery_timer_{nullptr};
    winrt::event_token recovery_tick_token_{};
    struct DialogTicket { winrt::handle event; };
    std::deque<std::shared_ptr<DialogTicket>> dialogs_;
    ViewMode view_mode_{ViewMode::Split};
    double split_left_width_{0.0};
    bool split_drag_active_{};
    double split_drag_start_x_{};
    double split_drag_start_left_width_{};
    bool initialized_{};
    bool initializing_{};
    bool busy_{};
    bool selecting_document_{};
    bool updating_ui_{};
    bool core_ready_{};
    bool delivery_failure_reported_{};
    std::uint64_t runner_generation_{};
    bool shutting_down_{};
    bool closing_allowed_{};
    winrt::event_token closing_token_{};
    Microsoft::UI::Xaml::Window::Closed_revoker closed_;
};
}
namespace winrt::CPEditorNativeWindows::factory_implementation {
struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}
