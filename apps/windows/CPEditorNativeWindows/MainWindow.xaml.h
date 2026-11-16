#pragma once
#include "MainWindow.g.h"
#include "EditorProfiles.h"
#include "UiStrings.h"
#include "WinUIEditAdapter.h"

namespace winrt::CPEditorNativeWindows::implementation {
struct MainWindow : MainWindowT<MainWindow> {
    MainWindow();
    winrt::fire_and_forget Editor_Loaded(Windows::Foundation::IInspectable const&,
                                        Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget New_Click(Windows::Foundation::IInspectable const&,
                                    Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget Open_Click(Windows::Foundation::IInspectable const&,
                                     Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget Save_Click(Windows::Foundation::IInspectable const&,
                                     Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget SaveAs_Click(Windows::Foundation::IInspectable const&,
                                       Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget Export_Click(Windows::Foundation::IInspectable const&,
                                       Microsoft::UI::Xaml::RoutedEventArgs const&);
    void Profile_SelectionChanged(Windows::Foundation::IInspectable const&,
                                 Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
private:
    enum class Command { New, Open, Save, SaveAs, Export };
    Windows::Foundation::IAsyncAction ExecuteAsync(Command);
    Windows::Foundation::IAsyncOperation<bool> ConfirmDiscardAsync();
    Windows::Foundation::IAsyncOperation<bool> SaveDocumentAsync(bool save_as);
    Windows::Foundation::IAsyncAction OpenDocumentAsync();
    Windows::Foundation::IAsyncAction ExportSnapshotAsync();
    Windows::Foundation::IAsyncAction ShowErrorAsync(std::wstring_view, winrt::hstring);
    winrt::fire_and_forget OnClosing(Microsoft::UI::Windowing::AppWindow const&,
                                     Microsoft::UI::Windowing::AppWindowClosingEventArgs const&);
    void SetBusy(bool);
    void SelectProfile(std::wstring_view);
    void UpdateStatus();
    [[nodiscard]] HWND WindowHandle();
    [[nodiscard]] cpn::windows::EditorProfile const& CurrentProfile() const;
    [[nodiscard]] static winrt::hstring CurrentError();
    cpn::windows::UiStrings strings_;
    cpn::windows::EditorConfiguration configuration_;
    std::shared_ptr<cpn::windows::WinUIEditAdapter> adapter_;
    std::filesystem::path document_path_;
    bool utf8_bom_{false};
    bool initialized_{false};
    bool busy_{false};
    bool closing_allowed_{false};
    Microsoft::UI::Windowing::AppWindow::Closing_revoker closing_;
    Microsoft::UI::Xaml::Window::Closed_revoker closed_;
};
}
namespace winrt::CPEditorNativeWindows::factory_implementation {
struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}
