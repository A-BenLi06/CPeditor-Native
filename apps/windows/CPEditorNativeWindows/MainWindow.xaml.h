#pragma once
#include "MainWindow.g.h"
#include "EditorProfiles.h"
#include "FileStore.h"
#include "RunnerConfiguration.h"
#include "UiStrings.h"
#include "WinUIEditAdapter.h"
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
        std::string initial_text;
        std::wstring profile;
        std::string toolchain;
        std::string comparison;
        bool utf8_bom{};
        bool dispatch_pending{};
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
    };
    enum class Command { New, Open, Save, SaveAs, Export };
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
    void ScheduleDocumentChanged(std::weak_ptr<Document> const&);
    void ReceiveCoreEvent(cpn::windows::RunEvent);
    void StartRunner();
    void RenderResult(std::shared_ptr<Document> const&);
    void AddDocument(std::filesystem::path, cpn::windows::LoadedDocument);
    void RemoveDocument(std::shared_ptr<Document> const&);
    void AddTest(std::shared_ptr<Document> const&);
    void TestsChanged(std::shared_ptr<Document> const&);
    void SubmitPendingRun(std::shared_ptr<Document> const&);
    void CancelRun(std::shared_ptr<Document> const&);
    void SelectDocument(std::shared_ptr<Document> const&);
    void UpdateDocument(std::shared_ptr<Document> const&);
    void UpdateControls();
    void SetBusy(bool);
    [[nodiscard]] winrt::hstring RenderBytes(std::string_view) const;
    [[nodiscard]] HWND WindowHandle();
    [[nodiscard]] std::shared_ptr<Document> CurrentDocument() const;
    [[nodiscard]] static winrt::hstring CurrentError();
    cpn::windows::UiStrings strings_;
    cpn::windows::EditorConfiguration configuration_;
    cpn::windows::RunnerConfiguration runner_configuration_;
    std::vector<std::shared_ptr<Document>> documents_;
    std::shared_ptr<cpn::windows::CoreRunner> runner_;
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
