#include "pch.h"
#include "MainWindow.xaml.h"
#include "SnapshotJson.h"
#include "cpn/batch.hpp"
#include <objbase.h>
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
    Title(strings_.get(UiStrings::app_title));
    AppWindow().Resize(Windows::Graphics::SizeInt32{1280, 820});
    closing_ = AppWindow().Closing(auto_revoke, {this, &MainWindow::OnClosing});
    const auto weak = get_weak();
    closed_ = Closed(auto_revoke, [weak](auto const&, auto const&) {
        if (auto self = weak.get()) {
            self->shutting_down_ = true;
            self->closing_.revoke(); self->closed_.revoke();
            for (auto const& document : self->documents_) {
                document->loaded.revoke();
                if (document->adapter) document->adapter->dispose();
            }
            self->documents_.clear();
            auto runner = std::exchange(self->runner_, {});
            if (runner) CloseRunnerBackground(std::move(runner));
        }
    });
}
HWND MainWindow::WindowHandle() {
    HWND handle{}; check_hresult(this->as<IWindowNative>()->get_WindowHandle(&handle)); return handle;
}
hstring MainWindow::CurrentError() {
    try { throw; }
    catch (hresult_error const& error) { return error.message(); }
    catch (std::exception const& error) { return to_hstring(error.what()); }
    catch (...) { return to_hstring(HRESULT_FROM_WIN32(ERROR_UNHANDLED_EXCEPTION)); }
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
        for (auto& choice : runner_configuration_.toolchains) {
            file = co_await StorageFile::GetFileFromApplicationUriAsync(Uri{L"ms-appx:///Toolchains/" + to_hstring(choice.file)});
            choice.profile = nlohmann::json::parse(to_string(co_await FileIO::ReadTextAsync(file)));
            const auto profile = cpn::parse_toolchain_profile(choice.profile.dump());
            if (profile.id != choice.id) throw std::invalid_argument("Toolchain catalog identity mismatch.");
            ComboBoxItem item; item.Content(box_value(strings_.get(to_hstring(choice.label_resource)))); Toolchains().Items().Append(item);
        }
        for (auto const& choice : runner_configuration_.comparisons) {
            ComboBoxItem item; item.Content(box_value(strings_.get(to_hstring(choice.label_resource)))); Comparisons().Items().Append(item);
        }
        for (auto const& profile : configuration_.profiles) {
            (void)runner_configuration_.default_toolchain(to_string(hstring{profile.id}));
            ComboBoxItem item; item.Content(box_value(strings_.get(profile.label_resource))); Profiles().Items().Append(item);
        }
        initialized_ = true;
        AddDocument({}, {});
        StartRunner();
    } catch (...) { error = CurrentError(); }
    initializing_ = false; SetBusy(false);
    if (!error.empty()) {
        Status().Text(strings_.get(UiStrings::initialization_failed));
        co_await ShowErrorAsync(UiStrings::error_config, error);
    }
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
void MainWindow::SetBusy(bool value) { busy_ = value; UpdateControls(); }
std::shared_ptr<MainWindow::Document> MainWindow::CurrentDocument() const {
    const auto selected = Tabs().SelectedItem();
    for (auto const& document : documents_) if (selected == document->tab) return document;
    return {};
}
void MainWindow::AddDocument(std::filesystem::path path, cpn::windows::LoadedDocument loaded) {
    if (documents_.size() >= runner_configuration_.maximum_documents)
        throw hresult_error{E_BOUNDS, strings_.get(L"DocumentLimit")};
    auto document = std::make_shared<Document>();
    document->path = std::move(path); document->utf8_bom = loaded.utf8_bom; document->initial_text = std::move(loaded.utf8);
    document->profile = document->path.empty() ? configuration_.default_profile : configuration_.for_path(document->path).id;
    document->toolchain = runner_configuration_.default_toolchain(to_string(hstring{document->profile}));
    document->comparison = runner_configuration_.default_comparison;
    document->control.IsEnabled(false);
    document->control.Visibility(Visibility::Collapsed);
    document->test_view.Visibility(Visibility::Collapsed);
    document->test_view.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    document->test_panel.Margin(Thickness{12, 0, 12, 12}); document->test_panel.Spacing(12);
    document->run_status.TextWrapping(TextWrapping::Wrap);
    document->run_details.TextWrapping(TextWrapping::Wrap); document->run_details.IsTextSelectionEnabled(true);
    document->test_panel.Children().Append(document->run_status); document->test_panel.Children().Append(document->run_details);
    document->test_view.Content(document->test_panel);
    const auto weak = get_weak(); const std::weak_ptr<Document> weak_document = document;
    document->loaded = document->control.Loaded(auto_revoke, [weak, weak_document](auto const&, auto const&) {
        if (auto self = weak.get()) if (auto value = weak_document.lock()) self->InitializeDocument(std::move(value));
    });
    documents_.push_back(document);
    EditorDeck().Children().Append(document->control);
    TestDeck().Children().Append(document->test_view);
    Tabs().TabItems().Append(document->tab);
    AddTest(document); document->test_revision = 1;
    Tabs().SelectedItem(document->tab);
    SelectDocument(document);
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
        document->adapter->replace_document(document->initial_text);
        std::string{}.swap(document->initial_text);
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
        UpdateDocument(document); UpdateControls();
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
    if (initialized_) SelectDocument(CurrentDocument());
}
void MainWindow::Profile_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&) {
    auto document = CurrentDocument(); const auto index = Profiles().SelectedIndex();
    if (selecting_document_ || !document || index < 0) return;
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
    if (selecting_document_ || !document || index < 0) return;
    document->toolchain = runner_configuration_.toolchains.at(static_cast<std::size_t>(index)).id; TestsChanged(document);
}
void MainWindow::Comparison_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&) {
    auto document = CurrentDocument(); const auto index = Comparisons().SelectedIndex();
    if (selecting_document_ || !document || index < 0) return;
    document->comparison = runner_configuration_.comparisons.at(static_cast<std::size_t>(index)).id; TestsChanged(document);
}
void MainWindow::AddTest(std::shared_ptr<Document> const& document) {
    if (document->tests.size() >= runner_configuration_.maximum_tests) throw hresult_error{E_BOUNDS, strings_.get(L"TestLimit")};
    auto test = std::make_shared<TestView>(); test->id = identity(); test->panel.Spacing(6);
    set_checked(test->enabled, true); test->enabled.Content(box_value(strings_.get(L"TestEnabled")));
    set_checked(test->has_expected, false); test->has_expected.Content(box_value(strings_.get(L"HasExpected")));
    test->input.AcceptsReturn(true); test->input.TextWrapping(TextWrapping::Wrap); test->input.MinHeight(72);
    test->input.Header(box_value(strings_.get(L"TestInput")));
    test->expected.AcceptsReturn(true); test->expected.TextWrapping(TextWrapping::Wrap); test->expected.MinHeight(72);
    test->expected.Header(box_value(strings_.get(L"TestExpected")));
    test->result.TextWrapping(TextWrapping::Wrap); test->result.IsTextSelectionEnabled(true);
    test->result.Text(strings_.get(L"VerdictNotRun"));
    Button remove; remove.Content(box_value(strings_.get(L"RemoveTest")));
    test->panel.Children().Append(test->enabled); test->panel.Children().Append(test->input);
    test->panel.Children().Append(test->has_expected); test->panel.Children().Append(test->expected);
    test->panel.Children().Append(test->result); test->panel.Children().Append(remove);
    const auto weak = get_weak(); const std::weak_ptr<Document> weak_document = document; const std::weak_ptr<TestView> weak_test = test;
    const auto changed = [weak, weak_document](auto const&, auto const&) {
        if (auto self = weak.get()) if (auto value = weak_document.lock()) self->TestsChanged(value);
    };
    test->input.TextChanged(changed); test->expected.TextChanged(changed);
    test->enabled.Checked(changed); test->enabled.Unchecked(changed);
    test->has_expected.Checked(changed); test->has_expected.Unchecked(changed);
    remove.Click([weak, weak_document, weak_test](auto const&, auto const&) {
        if (auto self = weak.get()) if (auto value = weak_document.lock()) if (auto item = weak_test.lock()) {
            std::uint32_t index{};
            if (value->test_panel.Children().IndexOf(item->panel, index)) value->test_panel.Children().RemoveAt(index);
            std::erase(value->tests, item); self->TestsChanged(value);
        }
    });
    document->tests.push_back(test); document->test_panel.Children().Append(test->panel); TestsChanged(document);
}
void MainWindow::TestsChanged(std::shared_ptr<Document> const& document) {
    if (document->test_revision == std::numeric_limits<std::uint64_t>::max()) throw_hresult(E_BOUNDS);
    if (document->closed) return;
    ++document->test_revision; UpdateDocument(document); UpdateControls();
}
fire_and_forget MainWindow::AddTest_Click(IInspectable const&, RoutedEventArgs const&) {
    auto lifetime = get_strong(); hstring error;
    try { if (auto document = CurrentDocument()) AddTest(document); } catch (...) { error = CurrentError(); }
    if (!error.empty()) co_await ShowErrorAsync(L"TestError", error);
}
void MainWindow::UpdateDocument(std::shared_ptr<Document> const& document) {
    const auto name = document->path.empty() ? std::wstring{strings_.get(UiStrings::untitled)} : document->path.filename().wstring();
    const bool available = document->adapter && document->adapter->committed_available();
    const bool dirty = document->adapter && (!available || document->adapter->dirty());
    document->tab.Header(box_value(hstring{name + (dirty ? std::wstring{strings_.get(UiStrings::unsaved_suffix)} : L"")}));
    auto const state = runner_configuration_.state_labels.find(document->run_state);
    auto state_text = state == runner_configuration_.state_labels.end() ? strings_.get(L"RunUnknown") : strings_.get(to_hstring(state->second));
    std::wstring run_status{state_text};
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
    else if (!document->adapter->tracking_available()) status = strings_.get(L"SnapshotTrackingFailed");
    else if (!available) status = strings_.get(L"CompositionOrNativeBusy");
    else status = hstring{UiStrings::format(strings_.get(UiStrings::status_format), {
        std::wstring{strings_.get(configuration_.by_id(document->profile).label_resource)}, std::to_wstring(document->adapter->revision()),
        std::to_wstring(document->adapter->length()), std::to_wstring(document->adapter->caret_byte()), std::to_wstring(document->adapter->anchor_byte())})};
    if (!document->notice.empty()) status = hstring{std::wstring{status} + L"  " + document->notice};
    Status().Text(status);
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
    Commands().IsEnabled(initialized_ && !busy_ && !shutting_down_);
    Profiles().IsEnabled(initialized_ && !shutting_down_ && static_cast<bool>(document));
    Toolchains().IsEnabled(initialized_ && !shutting_down_ && static_cast<bool>(document));
    Comparisons().IsEnabled(initialized_ && !shutting_down_ && static_cast<bool>(document));
    RunButton().IsEnabled(initialized_ && core_ready_ && !shutting_down_ && editor_ready && !document->run_active);
    CancelButton().IsEnabled(!shutting_down_ && document && document->run_active && !document->cancel_requested);
    AddTestButton().IsEnabled(initialized_ && !shutting_down_ && static_cast<bool>(document));
}
void MainWindow::SubmitPendingRun(std::shared_ptr<Document> const& document) {
    if (!document->pending_run || !document->adapter) return;
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
    if (!document || !document->adapter || !core_ready_ || shutting_down_ || document->run_active) co_return;
    try {
        if (!document->adapter->tracking_available()) throw hresult_error{E_FAIL, strings_.get(L"SnapshotTrackingFailed")};
        cpn::windows::RunRequest request; request.token = identity(); request.comparison = document->comparison;
        auto const choice = std::find_if(runner_configuration_.toolchains.begin(), runner_configuration_.toolchains.end(),
            [&](auto const& value) { return value.id == document->toolchain; });
        if (choice == runner_configuration_.toolchains.end()) throw_hresult(E_INVALIDARG);
        request.toolchain = choice->profile;
        for (auto const& test : document->tests) request.tests.push_back({test->id, to_string(test->input.Text()),
            checked(test->has_expected) ? std::optional<std::string>{to_string(test->expected.Text())} : std::nullopt, checked(test->enabled)});
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
    if (!document->adapter || (document->adapter->committed_available() && !document->adapter->dirty())) co_return true;
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
IAsyncAction MainWindow::OpenDocumentAsync() {
    FileOpenPicker picker; check_hresult(picker.as<IInitializeWithWindow>()->Initialize(WindowHandle()));
    picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary); picker.FileTypeFilter().Append(L"*");
    const auto file = co_await picker.PickSingleFileAsync(); if (!file) co_return;
    const std::filesystem::path path{std::wstring{file.Path()}};
    for (auto const& document : documents_) if (document->path == path) { Tabs().SelectedItem(document->tab); co_return; }
    const auto limit = configuration_.max_document_bytes; apartment_context ui;
    cpn::windows::LoadedDocument loaded; std::exception_ptr failure;
    co_await resume_background(); try { loaded = FileStore::read(path, limit); } catch (...) { failure = std::current_exception(); }
    co_await ui; if (failure) std::rethrow_exception(failure); AddDocument(path, std::move(loaded));
}
IAsyncOperation<bool> MainWindow::SaveDocumentAsync(std::shared_ptr<Document> document, bool save_as) {
    if (!document || !document->adapter) co_return false;
    auto path = document->path;
    if (save_as || path.empty()) {
        FileSavePicker picker; check_hresult(picker.as<IInitializeWithWindow>()->Initialize(WindowHandle()));
        picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary);
        picker.SuggestedFileName(path.empty() ? strings_.get(UiStrings::untitled) : hstring{path.stem().wstring()});
        auto const& profile = configuration_.by_id(document->profile);
        picker.FileTypeChoices().Insert(strings_.get(profile.label_resource), single_threaded_vector<hstring>({hstring{profile.default_extension}}));
        const auto file = co_await picker.PickSaveFileAsync(); if (!file) co_return false; path = std::filesystem::path{std::wstring{file.Path()}};
    }
    if (!document->adapter->tracking_available()) throw hresult_error{E_FAIL, strings_.get(L"SnapshotTrackingFailed")};
    if (!document->adapter->committed_available()) throw hresult_error{E_PENDING, strings_.get(L"FinishCompositionBeforeSaving")};
    auto snapshot = document->adapter->snapshot();
    if (document->utf8_bom) snapshot.utf8.insert(0, "\xEF\xBB\xBF");
    apartment_context ui; std::exception_ptr failure;
    co_await resume_background(); try { FileStore::write_atomic(path, snapshot.utf8); } catch (...) { failure = std::current_exception(); }
    co_await ui; if (failure) std::rethrow_exception(failure);
    document->path = path;
    if (document->utf8_bom) snapshot.utf8.erase(0, 3);
    if (!document->adapter->mark_saved(std::move(snapshot))) document->notice = std::wstring{strings_.get(L"SavedEarlierRevision")};
    else document->notice = std::wstring{strings_.get(L"Saved")};
    UpdateDocument(document); co_return true;
}
IAsyncAction MainWindow::ExportSnapshotAsync(std::shared_ptr<Document> document) {
    if (!document || !document->adapter) co_return;
    FileSavePicker picker; check_hresult(picker.as<IInitializeWithWindow>()->Initialize(WindowHandle()));
    picker.SuggestedFileName(document->path.empty() ? strings_.get(UiStrings::untitled) : hstring{document->path.stem().wstring()});
    picker.FileTypeChoices().Insert(strings_.get(UiStrings::snapshot_files), single_threaded_vector<hstring>({L".json"}));
    const auto file = co_await picker.PickSaveFileAsync(); if (!file) co_return;
    const std::filesystem::path path{std::wstring{file.Path()}};
    const auto bytes = cpn::windows::SnapshotJson::encode(document->adapter->snapshot(), document->path, document->profile);
    apartment_context ui; std::exception_ptr failure;
    co_await resume_background(); try { FileStore::write_atomic(path, bytes); } catch (...) { failure = std::current_exception(); }
    co_await ui; if (failure) std::rethrow_exception(failure);
}
IAsyncAction MainWindow::ExecuteAsync(Command command) {
    auto lifetime = get_strong(); if (!initialized_ || busy_ || shutting_down_) co_return;
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
    CancelRun(document); document->closed = true; document->loaded.revoke();
    if (document->adapter) document->adapter->dispose(); document->adapter.reset();
    std::uint32_t index{};
    if (EditorDeck().Children().IndexOf(document->control, index)) EditorDeck().Children().RemoveAt(index);
    if (TestDeck().Children().IndexOf(document->test_view, index)) TestDeck().Children().RemoveAt(index);
    std::erase(documents_, document);
    if (Tabs().TabItems().IndexOf(document->tab, index)) Tabs().TabItems().RemoveAt(index);
    if (documents_.empty()) AddDocument({}, {});
    else { if (!CurrentDocument()) Tabs().SelectedItem(documents_.front()->tab); SelectDocument(CurrentDocument()); }
}
fire_and_forget MainWindow::Tabs_CloseRequested(TabView const&, TabViewTabCloseRequestedEventArgs const& args) {
    auto lifetime = get_strong(); if (busy_ || shutting_down_) co_return;
    std::shared_ptr<Document> document;
    for (auto const& value : documents_) if (value->tab == args.Tab()) { document = value; break; }
    if (!document) co_return; SetBusy(true); hstring error;
    try { if (co_await ConfirmDiscardAsync(document)) RemoveDocument(document); } catch (...) { error = CurrentError(); }
    if (!error.empty()) co_await ShowErrorAsync(UiStrings::error_save, error); SetBusy(false);
}
fire_and_forget MainWindow::OnClosing(Microsoft::UI::Windowing::AppWindow const&, Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args) {
    auto lifetime = get_strong(); if (closing_allowed_) co_return; args.Cancel(true);
    if (busy_ || shutting_down_) co_return;
    SetBusy(true); hstring error; bool proceed = true; std::vector<std::uint64_t> approved;
    try {
        for (auto const& document : documents_) {
            if (!(co_await ConfirmDiscardAsync(document))) { proceed = false; break; }
            approved.push_back(document->adapter ? document->adapter->revision() : 0);
        }
        if (proceed) {
            for (auto const& document : documents_) CancelRun(document);
            shutting_down_ = true; core_ready_ = false; UpdateControls();
            auto runner = runner_; apartment_context ui;
            std::exception_ptr close_failure;
            co_await resume_background();
            try { if (runner) runner->close(); } catch (...) { close_failure = std::current_exception(); }
            co_await ui;
            if (close_failure) std::rethrow_exception(close_failure);
            for (std::size_t index = 0; index < documents_.size(); ++index) {
                auto const& document = documents_[index];
                if (document->adapter && (!document->adapter->committed_available() ||
                    (document->adapter->dirty() && document->adapter->revision() != approved[index]))) proceed = false;
                document->run_active = false; document->pending_run.reset(); document->run_token.clear(); document->cancel_requested = false;
            }
            if (!proceed) {
                shutting_down_ = false; StartRunner();
                if (auto document = CurrentDocument()) { document->notice = std::wstring{strings_.get(L"ChangedDuringClose")}; UpdateDocument(document); }
            }
        }
    } catch (...) { error = CurrentError(); proceed = false; shutting_down_ = false; }
    SetBusy(false);
    if (!error.empty()) co_await ShowErrorAsync(UiStrings::error_save, error);
    if (proceed) { closing_allowed_ = true; Close(); }
}
fire_and_forget MainWindow::New_Click(IInspectable const&, RoutedEventArgs const&) { co_await ExecuteAsync(Command::New); }
fire_and_forget MainWindow::Open_Click(IInspectable const&, RoutedEventArgs const&) { co_await ExecuteAsync(Command::Open); }
fire_and_forget MainWindow::Save_Click(IInspectable const&, RoutedEventArgs const&) { co_await ExecuteAsync(Command::Save); }
fire_and_forget MainWindow::SaveAs_Click(IInspectable const&, RoutedEventArgs const&) { co_await ExecuteAsync(Command::SaveAs); }
fire_and_forget MainWindow::Export_Click(IInspectable const&, RoutedEventArgs const&) { co_await ExecuteAsync(Command::Export); }
}
