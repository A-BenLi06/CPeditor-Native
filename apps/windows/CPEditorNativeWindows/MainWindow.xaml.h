#pragma once
#include "MainWindow.g.h"
#include "EditorProfiles.h"
#include "FileStore.h"
#include "RunnerConfiguration.h"
#include "UiStrings.h"
#include "WinUIEditAdapter.h"
#include "RecoveryModel.h"
#include "SourceStore.h"
#include "PersistenceGate.h"
#include "CompanionModel.h"
#include <deque>

namespace winrt::CPEditorNativeWindows::implementation {
struct MainWindow : MainWindowT<MainWindow> {
    MainWindow();
    winrt::fire_and_forget Root_Loaded(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget New_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget Open_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget Save_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget SaveAs_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget Export_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget Run_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget Cancel_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget AddTest_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget RetryRecovery_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget SaveRecoveryAs_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget ImportCompanion_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget RefreshCompanion_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget MoreCompanion_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget ApplyCompanion_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget StartCompanion_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget StopCompanion_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void Companion_SelectionChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void Tabs_SelectionChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    winrt::fire_and_forget Tabs_CloseRequested(Microsoft::UI::Xaml::Controls::TabView const&, Microsoft::UI::Xaml::Controls::TabViewTabCloseRequestedEventArgs const&);
    void Profile_SelectionChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void Toolchain_SelectionChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void Comparison_SelectionChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
private:
    struct TestView {
        std::string id;
        Microsoft::UI::Xaml::Controls::StackPanel panel;
        Microsoft::UI::Xaml::Controls::TextBox input;
        Microsoft::UI::Xaml::Controls::TextBox expected;
        Microsoft::UI::Xaml::Controls::CheckBox enabled;
        Microsoft::UI::Xaml::Controls::CheckBox has_expected;
        Microsoft::UI::Xaml::Controls::TextBlock result;
        Microsoft::UI::Xaml::Controls::Button remove;
        std::string input_bytes;
        std::string expected_bytes;
        bool input_binary{};
        bool expected_binary{};
        bool enabled_value{true};
        bool has_expected_value{};
    };
    struct Document {
        Microsoft::UI::Xaml::Controls::TabViewItem tab;
        winrt::WinUIEditor::CodeEditorControl control;
        Microsoft::UI::Xaml::FrameworkElement::Loaded_revoker loaded;
        std::shared_ptr<cpn::windows::WinUIEditAdapter> adapter;
        Microsoft::UI::Xaml::Controls::ScrollViewer test_view;
        Microsoft::UI::Xaml::Controls::StackPanel test_panel;
        Microsoft::UI::Xaml::Controls::TextBlock run_status;
        Microsoft::UI::Xaml::Controls::TextBlock run_details;
        std::vector<std::shared_ptr<TestView>> tests;
        std::filesystem::path path;
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
        std::string run_token;
        std::string run_state{"Idle"};
        std::uint64_t last_sequence{};
        std::uint64_t test_revision{1};
        std::uint64_t run_test_revision{};
        std::uint64_t result_test_revision{};
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
    enum class Command { New, Open, Save, SaveAs, Export };
    enum class CompanionCommand { Import, Refresh, More, Apply, Start, Stop };
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
    Windows::Foundation::IAsyncAction OpenDocumentAsync();
    Windows::Foundation::IAsyncAction ExportSnapshotAsync(std::shared_ptr<Document>);
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
    void RenderResult(std::shared_ptr<Document> const&);
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
    cpn::windows::RunnerConfiguration runner_configuration_;
    std::vector<std::shared_ptr<Document>> documents_;
    std::shared_ptr<cpn::windows::CoreRunner> runner_;
    cpn::windows::RecoveryConfiguration recovery_configuration_;
    cpn::windows::CompanionConfiguration companion_configuration_;
    std::shared_ptr<cpn::windows::CompanionWorker> companion_worker_;
    std::optional<cpn::windows::CompanionBinding> companion_binding_;
    cpn::windows::CompanionPagingState companion_page_;
    cpn::windows::CompanionListenerResponse companion_listener_;
    std::wstring companion_notice_;
    bool companion_available_{};
    bool companion_operating_{};
    std::shared_ptr<cpn::windows::SessionWorker> session_worker_;
    std::shared_ptr<cpn::windows::SourceStore> source_store_;
    cpn::SessionPaths recovery_paths_;
    std::filesystem::path recovery_location_file_;
    std::filesystem::path source_lock_directory_;
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
    Microsoft::UI::Dispatching::DispatcherQueueTimer::Tick_revoker recovery_tick_;
    struct DialogTicket { winrt::handle event; };
    std::deque<std::shared_ptr<DialogTicket>> dialogs_;
    bool initialized_{};
    bool initializing_{};
    bool busy_{};
    bool selecting_document_{};
    bool core_ready_{};
    bool delivery_failure_reported_{};
    std::uint64_t runner_generation_{};
    bool shutting_down_{};
    bool closing_allowed_{};
    Microsoft::UI::Windowing::AppWindow::Closing_revoker closing_;
    Microsoft::UI::Xaml::Window::Closed_revoker closed_;
};
}
namespace winrt::CPEditorNativeWindows::factory_implementation {
struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}
