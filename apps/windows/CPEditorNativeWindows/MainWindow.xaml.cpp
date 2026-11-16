#include "pch.h"
#include "MainWindow.xaml.h"
#include "FileStore.h"
#include "SnapshotJson.h"
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

namespace winrt::CPEditorNativeWindows::implementation {
MainWindow::MainWindow() {
    InitializeComponent();
    Title(strings_.get(UiStrings::app_title));
    AppWindow().Resize(Windows::Graphics::SizeInt32{1100, 760});
    closing_ = AppWindow().Closing(auto_revoke, {this, &MainWindow::OnClosing});
    const auto weak = get_weak();
    closed_ = Closed(auto_revoke, [weak](auto const&, auto const&) {
        if (auto self = weak.get()) {
            self->closing_.revoke();
            self->closed_.revoke();
            if (self->adapter_) self->adapter_->dispose();
            self->adapter_.reset();
        }
    });
}

HWND MainWindow::WindowHandle() {
    HWND handle{};
    check_hresult(this->as<IWindowNative>()->get_WindowHandle(&handle));
    return handle;
}

hstring MainWindow::CurrentError() {
    try { throw; }
    catch (hresult_error const& error) { return error.message(); }
    catch (std::exception const& error) { return to_hstring(error.what()); }
    catch (...) { return to_hstring(HRESULT_FROM_WIN32(ERROR_UNHANDLED_EXCEPTION)); }
}

fire_and_forget MainWindow::Editor_Loaded(IInspectable const&, RoutedEventArgs const&) {
    auto lifetime = get_strong();
    if (initialized_ || busy_) co_return;
    SetBusy(true);
    hstring error;
    try {
        const auto file = co_await StorageFile::GetFileFromApplicationUriAsync(
            Windows::Foundation::Uri{L"ms-appx:///editor-profiles.json"});
        configuration_ = cpn::windows::EditorConfiguration::parse(co_await FileIO::ReadTextAsync(file));
        const auto weak = get_weak();
        adapter_ = cpn::windows::WinUIEditAdapter::create(CodeEditor(), [weak] {
            if (auto self = weak.get()) self->UpdateStatus();
        });
        adapter_->set_tab_width(configuration_.tab_width);
        for (auto const& profile : configuration_.profiles) {
            ComboBoxItem item;
            item.Content(box_value(strings_.get(profile.label_resource)));
            Profiles().Items().Append(item);
        }
        SelectProfile(configuration_.default_profile);
        adapter_->replace_document({});
        initialized_ = true;
    } catch (...) { error = CurrentError(); }
    if (!error.empty()) {
        Status().Text(strings_.get(UiStrings::initialization_failed));
        co_await ShowErrorAsync(UiStrings::error_config, error);
    }
    SetBusy(false);
}

void MainWindow::SetBusy(bool value) {
    busy_ = value;
    const bool enabled = initialized_ && !busy_;
    Commands().IsEnabled(enabled);
    Profiles().IsEnabled(enabled);
    CodeEditor().IsEnabled(enabled);
    if (enabled && adapter_) adapter_->focus();
}

cpn::windows::EditorProfile const& MainWindow::CurrentProfile() const {
    const auto index = Profiles().SelectedIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= configuration_.profiles.size()) {
        throw_hresult(E_UNEXPECTED);
    }
    return configuration_.profiles[static_cast<std::size_t>(index)];
}

void MainWindow::SelectProfile(std::wstring_view id) {
    for (std::size_t index = 0; index < configuration_.profiles.size(); ++index) {
        if (configuration_.profiles[index].id == id) {
            Profiles().SelectedIndex(static_cast<std::int32_t>(index));
            return;
        }
    }
    throw_hresult(E_INVALIDARG);
}

void MainWindow::Profile_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&) {
    if (!adapter_ || Profiles().SelectedIndex() < 0) return;
    adapter_->set_language(CurrentProfile().highlighting_language);
    UpdateStatus();
}

void MainWindow::UpdateStatus() {
    if (!adapter_ || Profiles().SelectedIndex() < 0) return;
    const auto name = document_path_.empty() ? std::wstring{strings_.get(UiStrings::untitled)} :
                                             document_path_.filename().wstring();
    const auto suffix = adapter_->dirty() ? std::wstring{strings_.get(UiStrings::unsaved_suffix)} : L"";
    Title(hstring{UiStrings::format(strings_.get(UiStrings::window_title_format), {
        std::wstring{strings_.get(UiStrings::app_title)}, name, suffix})});
    Status().Text(hstring{UiStrings::format(strings_.get(UiStrings::status_format), {
        std::wstring{strings_.get(CurrentProfile().label_resource)},
        std::to_wstring(adapter_->revision()), std::to_wstring(adapter_->length()),
        std::to_wstring(adapter_->caret_byte()), std::to_wstring(adapter_->anchor_byte())})});
}

IAsyncAction MainWindow::ShowErrorAsync(std::wstring_view action, hstring details) {
    ContentDialog dialog;
    dialog.XamlRoot(Root().XamlRoot());
    dialog.Title(box_value(strings_.get(UiStrings::error_title)));
    dialog.Content(box_value(hstring{UiStrings::format(strings_.get(UiStrings::error_details), {
        std::wstring{strings_.get(action)}, std::wstring{details}})}));
    dialog.CloseButtonText(strings_.get(UiStrings::close_button));
    co_await dialog.ShowAsync();
}

IAsyncOperation<bool> MainWindow::ConfirmDiscardAsync() {
    if (!adapter_ || !adapter_->dirty()) co_return true;
    ContentDialog dialog;
    dialog.XamlRoot(Root().XamlRoot());
    dialog.Title(box_value(strings_.get(UiStrings::confirm_title)));
    const auto name = document_path_.empty() ? std::wstring{strings_.get(UiStrings::untitled)} :
                                             document_path_.filename().wstring();
    dialog.Content(box_value(hstring{UiStrings::format(strings_.get(UiStrings::confirm_content), {name})}));
    dialog.PrimaryButtonText(strings_.get(UiStrings::save_button));
    dialog.SecondaryButtonText(strings_.get(UiStrings::discard_button));
    dialog.CloseButtonText(strings_.get(UiStrings::cancel_button));
    dialog.DefaultButton(ContentDialogButton::Primary);
    const auto result = co_await dialog.ShowAsync();
    if (result == ContentDialogResult::Primary) co_return co_await SaveDocumentAsync(false);
    co_return result == ContentDialogResult::Secondary;
}

IAsyncAction MainWindow::OpenDocumentAsync() {
    FileOpenPicker picker;
    check_hresult(picker.as<IInitializeWithWindow>()->Initialize(WindowHandle()));
    picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary);
    picker.FileTypeFilter().Append(L"*");
    const auto file = co_await picker.PickSingleFileAsync();
    if (!file) co_return;
    const std::filesystem::path path{std::wstring{file.Path()}};
    const auto limit = configuration_.max_document_bytes;
    apartment_context ui;
    cpn::windows::LoadedDocument loaded;
    std::exception_ptr failure;
    co_await resume_background();
    try { loaded = FileStore::read(path, limit); }
    catch (...) { failure = std::current_exception(); }
    co_await ui;
    if (failure) std::rethrow_exception(failure);
    // File I/O finishes before changing any current document identity or dirty state.
    SelectProfile(configuration_.for_path(path).id);
    adapter_->replace_document(loaded.utf8);
    document_path_ = path;
    utf8_bom_ = loaded.utf8_bom;
    UpdateStatus();
}

IAsyncOperation<bool> MainWindow::SaveDocumentAsync(bool save_as) {
    auto path = document_path_;
    if (save_as || path.empty()) {
        FileSavePicker picker;
        check_hresult(picker.as<IInitializeWithWindow>()->Initialize(WindowHandle()));
        picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary);
        picker.SuggestedFileName(path.empty() ? strings_.get(UiStrings::untitled) :
                                               hstring{path.stem().wstring()});
        picker.FileTypeChoices().Insert(strings_.get(CurrentProfile().label_resource),
            single_threaded_vector<hstring>({hstring{CurrentProfile().default_extension}}));
        const auto file = co_await picker.PickSaveFileAsync();
        if (!file) co_return false;
        path = std::filesystem::path{std::wstring{file.Path()}};
    }
    // ExecuteAsync disables the editor during this transaction, keeping savepoint and bytes aligned.
    auto snapshot = adapter_->snapshot();
    if (utf8_bom_) snapshot.utf8.insert(0, "\xEF\xBB\xBF");
    apartment_context ui;
    std::exception_ptr failure;
    co_await resume_background();
    try { FileStore::write_atomic(path, snapshot.utf8); }
    catch (...) { failure = std::current_exception(); }
    co_await ui;
    if (failure) std::rethrow_exception(failure);
    document_path_ = path;
    adapter_->mark_saved();
    UpdateStatus();
    co_return true;
}

IAsyncAction MainWindow::ExportSnapshotAsync() {
    FileSavePicker picker;
    check_hresult(picker.as<IInitializeWithWindow>()->Initialize(WindowHandle()));
    picker.SuggestedFileName(document_path_.empty() ? strings_.get(UiStrings::untitled) :
                                                    hstring{document_path_.stem().wstring()});
    picker.FileTypeChoices().Insert(strings_.get(UiStrings::snapshot_files),
                                   single_threaded_vector<hstring>({L".json"}));
    const auto file = co_await picker.PickSaveFileAsync();
    if (!file) co_return;
    const std::filesystem::path path{std::wstring{file.Path()}};
    const auto bytes = cpn::windows::SnapshotJson::encode(adapter_->snapshot(),
                                                       document_path_, CurrentProfile().id);
    apartment_context ui;
    std::exception_ptr failure;
    co_await resume_background();
    try { FileStore::write_atomic(path, bytes); }
    catch (...) { failure = std::current_exception(); }
    co_await ui;
    if (failure) std::rethrow_exception(failure);
}

IAsyncAction MainWindow::ExecuteAsync(Command command) {
    auto lifetime = get_strong();
    if (!initialized_ || busy_) co_return;
    SetBusy(true);
    hstring error;
    try {
        switch (command) {
        case Command::New:
            if (co_await ConfirmDiscardAsync()) {
                SelectProfile(configuration_.default_profile);
                adapter_->replace_document({});
                document_path_.clear();
                utf8_bom_ = false;
                UpdateStatus();
            }
            break;
        case Command::Open:
            if (co_await ConfirmDiscardAsync()) co_await OpenDocumentAsync();
            break;
        case Command::Save: (void)(co_await SaveDocumentAsync(false)); break;
        case Command::SaveAs: (void)(co_await SaveDocumentAsync(true)); break;
        case Command::Export: co_await ExportSnapshotAsync(); break;
        }
    } catch (...) { error = CurrentError(); }
    if (!error.empty()) {
        co_await ShowErrorAsync(command == Command::Open ? UiStrings::error_open : UiStrings::error_save,
                               error);
    }
    SetBusy(false);
}

fire_and_forget MainWindow::OnClosing(Microsoft::UI::Windowing::AppWindow const&,
                                      Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args) {
    auto lifetime = get_strong();
    if (closing_allowed_) co_return;
    if (busy_) { args.Cancel(true); co_return; }
    if (!adapter_ || !adapter_->dirty()) co_return;
    args.Cancel(true);
    SetBusy(true);
    bool proceed = false;
    hstring error;
    try { proceed = co_await ConfirmDiscardAsync(); }
    catch (...) { error = CurrentError(); }
    if (!error.empty()) co_await ShowErrorAsync(UiStrings::error_save, error);
    SetBusy(false);
    if (proceed) {
        closing_allowed_ = true;
        Close();
    }
}

fire_and_forget MainWindow::New_Click(IInspectable const&, RoutedEventArgs const&) {
    co_await ExecuteAsync(Command::New);
}
fire_and_forget MainWindow::Open_Click(IInspectable const&, RoutedEventArgs const&) {
    co_await ExecuteAsync(Command::Open);
}
fire_and_forget MainWindow::Save_Click(IInspectable const&, RoutedEventArgs const&) {
    co_await ExecuteAsync(Command::Save);
}
fire_and_forget MainWindow::SaveAs_Click(IInspectable const&, RoutedEventArgs const&) {
    co_await ExecuteAsync(Command::SaveAs);
}
fire_and_forget MainWindow::Export_Click(IInspectable const&, RoutedEventArgs const&) {
    co_await ExecuteAsync(Command::Export);
}
}
