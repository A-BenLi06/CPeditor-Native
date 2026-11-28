#include "pch.h"
#include "MainWindow.xaml.h"
#include "TestTextChange.h"
#include "cpn/digest.hpp"
#include <objbase.h>
#include <algorithm>
#include <utility>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
using cpn::windows::CheckerModel;
using cpn::windows::FileStore;
using cpn::windows::UiStrings;

namespace {
std::string new_test_id() {
    GUID value{};
    check_hresult(CoCreateGuid(&value));
    wchar_t text[40]{};
    if (!StringFromGUID2(value, text, static_cast<int>(std::size(text)))) throw_hresult(E_UNEXPECTED);
    return to_string(hstring{text});
}
bool is_checked(CheckBox const& box) {
    auto const value = box.IsChecked();
    return value && value.Value();
}
void set_checked(CheckBox const& box, bool value) {
    box.IsChecked(box_value(value).as<IReference<bool>>());
}
bool is_binary_bytes(std::string_view bytes) {
    if (bytes.find('\0') != std::string_view::npos) return true;
    try { FileStore::validate_utf8(bytes); return false; } catch (...) { return true; }
}
Style lookup_style(wchar_t const* key) {
    return Application::Current().Resources().Lookup(box_value(key)).as<Style>();
}
Brush lookup_brush(wchar_t const* key) {
    return Application::Current().Resources().Lookup(box_value(key)).as<Brush>();
}
Button make_compact_icon_button(wchar_t const* glyph, hstring const& tip, int size = 24) {
    Button btn;
    btn.Style(lookup_style(L"FluentIconButtonStyle"));
    btn.Width(size);
    btn.Height(size);
    btn.Padding(Thickness{0});
    FontIcon icon;
    icon.Glyph(glyph);
    icon.FontSize(12);
    btn.Content(icon);
    ToolTipService::SetToolTip(btn, box_value(tip));
    return btn;
}
Border make_column_header(hstring const& title, TextBlock& bytes_label, FrameworkElement const& right_control) {
    Grid header_grid;
    header_grid.Padding(Thickness{10, 4, 6, 4});
    ColumnDefinition c0, c1;
    c0.Width(GridLength{1, GridUnitType::Star});
    c1.Width(GridLength{0, GridUnitType::Auto});
    header_grid.ColumnDefinitions().Append(c0);
    header_grid.ColumnDefinitions().Append(c1);

    StackPanel left;
    left.Orientation(Orientation::Horizontal);
    left.Spacing(6);
    left.VerticalAlignment(VerticalAlignment::Center);
    TextBlock label;
    label.Text(title);
    label.FontSize(11);
    label.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
    bytes_label.FontSize(11);
    bytes_label.Opacity(0.68);
    bytes_label.VerticalAlignment(VerticalAlignment::Center);
    left.Children().Append(label);
    left.Children().Append(bytes_label);
    Grid::SetColumn(left, 0);
    header_grid.Children().Append(left);

    if (right_control) {
        Grid::SetColumn(right_control, 1);
        header_grid.Children().Append(right_control);
    }

    Border bar;
    bar.Child(header_grid);
    bar.BorderThickness(Thickness{0, 0, 0, 1});
    bar.BorderBrush(lookup_brush(L"CardStrokeColorDefaultBrush"));
    bar.Background(lookup_brush(L"LayerFillColorDefaultBrush"));
    return bar;
}
}

namespace winrt::CPEditorNativeWindows::implementation {
SolidColorBrush MainWindow::VerdictBrush(std::string_view verdict) {
    bool const dark = Root().ActualTheme() == ElementTheme::Dark;
    if (verdict == "AC" || verdict == "Accepted" || verdict == "Succeeded") {
        return SolidColorBrush{dark ? Windows::UI::Color{255, 108, 203, 95} : Windows::UI::Color{255, 16, 124, 16}};
    }
    if (verdict == "WA" || verdict == "CompilationError" || verdict == "Failed") {
        return SolidColorBrush{dark ? Windows::UI::Color{255, 255, 153, 164} : Windows::UI::Color{255, 196, 43, 28}};
    }
    if (verdict == "TLE" || verdict == "RE" || verdict == "OLE" || verdict == "CheckerError" || verdict == "InfrastructureError") {
        return SolidColorBrush{dark ? Windows::UI::Color{255, 252, 225, 0} : Windows::UI::Color{255, 157, 93, 0}};
    }
    if (verdict == "Cancelled") {
        return SolidColorBrush{dark ? Windows::UI::Color{255, 170, 170, 170} : Windows::UI::Color{255, 100, 100, 100}};
    }
    return SolidColorBrush{dark ? Windows::UI::Color{255, 150, 150, 150} : Windows::UI::Color{255, 110, 110, 110}};
}

SolidColorBrush MainWindow::VerdictBackgroundBrush(std::string_view verdict) {
    bool const dark = Root().ActualTheme() == ElementTheme::Dark;
    if (verdict == "AC" || verdict == "Accepted" || verdict == "Succeeded") {
        return SolidColorBrush{dark ? Windows::UI::Color{56, 108, 203, 95} : Windows::UI::Color{32, 16, 124, 16}};
    }
    if (verdict == "WA" || verdict == "CompilationError" || verdict == "Failed") {
        return SolidColorBrush{dark ? Windows::UI::Color{56, 255, 100, 110} : Windows::UI::Color{32, 196, 43, 28}};
    }
    if (verdict == "TLE" || verdict == "RE" || verdict == "OLE" || verdict == "CheckerError" || verdict == "InfrastructureError") {
        return SolidColorBrush{dark ? Windows::UI::Color{56, 252, 200, 0} : Windows::UI::Color{32, 202, 80, 16}};
    }
    return SolidColorBrush{dark ? Windows::UI::Color{36, 255, 255, 255} : Windows::UI::Color{20, 0, 0, 0}};
}

void MainWindow::CopyTextToClipboard(hstring const& text) {
    Windows::ApplicationModel::DataTransfer::DataPackage pkg;
    pkg.SetText(text);
    Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(pkg);
    Status().Text(strings_.get(L"CopiedNotice"));
}

void MainWindow::UpdateCheckerMenuState(std::shared_ptr<Document> const& document) {
    updating_ui_ = true;
    MenuUseExternalCheckerItem().IsChecked(checker_settings_.enabled);
    std::string const cmp = document ? document->comparison : runner_configuration_.default_comparison;
    MenuCompareTrailingItem().IsChecked(cmp == "trailing");
    MenuCompareStrictItem().IsChecked(cmp == "strict");
    MenuCompareByteItem().IsChecked(cmp == "byte");
    updating_ui_ = false;

    if (checker_settings_.enabled) {
        CheckerMenuIcon().Glyph(L"\xE83D");
        std::wstring name;
        try {
            auto const json_str = checker_settings_.json_text.empty()
                ? checker_configuration_.default_checker.dump()
                : checker_settings_.json_text;
            auto const draft = CheckerModel::visual_from_json(json_str, checker_configuration_.max_checker_json_bytes);
            name = std::filesystem::path{to_hstring(draft.executable).c_str()}.filename().wstring();
        } catch (...) {}
        if (name.empty()) name = std::wstring{strings_.get(L"CheckerToggleText")};
        CheckerMenuLabel().Text(hstring{UiStrings::format(strings_.get(L"CheckerModeExternalFormat"), {name})});
    } else {
        CheckerMenuIcon().Glyph(L"\xE9E9");
        hstring cmp_text = strings_.get(L"ComparisonTrailing");
        for (auto const& c : runner_configuration_.comparisons) {
            if (c.id == cmp) { cmp_text = strings_.get(to_hstring(c.label_resource)); break; }
        }
        CheckerMenuLabel().Text(cmp_text);
    }
}

void MainWindow::AddTest(std::shared_ptr<Document> const& document, std::optional<cpn::TestCase> restored) {
    if (!restored && !TestsMutable(document)) return;
    if (document->tests.size() >= runner_configuration_.maximum_tests)
        throw hresult_error{E_BOUNDS, strings_.get(L"TestLimit")};

    auto test = std::make_shared<TestView>();
    test->id = restored ? restored->id : new_test_id();
    test->input_bytes = restored ? restored->input : std::string{};
    test->expected_bytes = restored && restored->expected ? *restored->expected : std::string{};
    test->input_binary = is_binary_bytes(test->input_bytes);
    test->expected_binary = is_binary_bytes(test->expected_bytes);
    test->enabled_value = !restored || restored->enabled;
    test->has_expected_value = restored && restored->expected.has_value();

    // Fluent Card Chrome with inset 3px rounded semantic stripe
    Border outer;
    outer.CornerRadius(CornerRadius{8});
    outer.BorderThickness(Thickness{1});
    outer.BorderBrush(lookup_brush(L"CardStrokeColorDefaultBrush"));
    outer.Background(lookup_brush(L"CardBackgroundFillColorDefaultBrush"));

    Grid root_grid;
    ColumnDefinition stripe_col, body_col;
    stripe_col.Width(GridLength{8, GridUnitType::Pixel});
    body_col.Width(GridLength{1, GridUnitType::Star});
    root_grid.ColumnDefinitions().Append(stripe_col);
    root_grid.ColumnDefinitions().Append(body_col);

    Border stripe;
    stripe.Width(3);
    stripe.CornerRadius(CornerRadius{2});
    stripe.Margin(Thickness{5, 10, 0, 10});
    stripe.HorizontalAlignment(HorizontalAlignment::Left);
    stripe.Background(SolidColorBrush{Windows::UI::Color{0, 0, 0, 0}});
    Grid::SetColumn(stripe, 0);
    root_grid.Children().Append(stripe);

    StackPanel section_body;
    section_body.Padding(Thickness{10, 10, 12, 12});
    section_body.Spacing(8);
    Grid::SetColumn(section_body, 1);
    root_grid.Children().Append(section_body);
    outer.Child(root_grid);

    // Header row: CheckBox + Sample #N + Verdict Pill + Elapsed + Icon Actions
    Grid header_row;
    ColumnDefinition hc0, hc1;
    hc0.Width(GridLength{1, GridUnitType::Star});
    hc1.Width(GridLength{0, GridUnitType::Auto});
    header_row.ColumnDefinitions().Append(hc0);
    header_row.ColumnDefinitions().Append(hc1);

    StackPanel left_hdr;
    left_hdr.Orientation(Orientation::Horizontal);
    left_hdr.Spacing(8);
    left_hdr.VerticalAlignment(VerticalAlignment::Center);

    CheckBox enabled_box;
    enabled_box.MinWidth(0);
    enabled_box.Padding(Thickness{0});
    enabled_box.VerticalAlignment(VerticalAlignment::Center);
    ToolTipService::SetToolTip(enabled_box, box_value(strings_.get(L"TestEnabled")));
    set_checked(enabled_box, test->enabled_value);

    TextBlock title;
    title.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
    title.FontSize(13);
    title.VerticalAlignment(VerticalAlignment::Center);

    Border verdict_badge;
    verdict_badge.CornerRadius(CornerRadius{10});
    verdict_badge.Padding(Thickness{8, 2, 8, 2});
    verdict_badge.Background(VerdictBackgroundBrush("NotRun"));
    TextBlock verdict_text;
    verdict_text.FontSize(11);
    verdict_text.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
    verdict_text.Text(strings_.get(L"VerdictNotRun"));
    verdict_text.Foreground(VerdictBrush("NotRun"));
    verdict_badge.Child(verdict_text);

    TextBlock elapsed_text;
    elapsed_text.FontSize(11);
    elapsed_text.Opacity(0.72);
    elapsed_text.VerticalAlignment(VerticalAlignment::Center);

    left_hdr.Children().Append(enabled_box);
    left_hdr.Children().Append(title);
    left_hdr.Children().Append(verdict_badge);
    left_hdr.Children().Append(elapsed_text);
    Grid::SetColumn(left_hdr, 0);
    header_row.Children().Append(left_hdr);

    StackPanel right_hdr;
    right_hdr.Orientation(Orientation::Horizontal);
    right_hdr.Spacing(2);
    right_hdr.VerticalAlignment(VerticalAlignment::Center);

    auto use_output_btn = make_compact_icon_button(L"\xE8AB", strings_.get(L"UseOutputTip"), 26);
    use_output_btn.IsEnabled(false);
    auto dup_btn = make_compact_icon_button(L"\xE8C8", strings_.get(L"DuplicateSampleTip"), 26);
    auto remove_btn = make_compact_icon_button(L"\xE74D", strings_.get(L"RemoveSampleTip"), 26);

    right_hdr.Children().Append(use_output_btn);
    right_hdr.Children().Append(dup_btn);
    right_hdr.Children().Append(remove_btn);
    Grid::SetColumn(right_hdr, 1);
    header_row.Children().Append(right_hdr);
    section_body.Children().Append(header_row);

    // 3-column horizontal-scroll grid: Input -> Output -> Expected Output
    ScrollViewer hscroll;
    hscroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Auto);
    hscroll.VerticalScrollBarVisibility(ScrollBarVisibility::Disabled);

    Grid tri_grid;
    tri_grid.ColumnSpacing(8);
    for (int i = 0; i < 3; ++i) {
        ColumnDefinition cd;
        cd.Width(GridLength{1, GridUnitType::Star});
        cd.MinWidth(220);
        tri_grid.ColumnDefinitions().Append(cd);
    }

    auto make_cell_shell = [](UIElement const& hdr, TextBox const& editor) {
        Border shell;
        shell.CornerRadius(CornerRadius{6});
        shell.BorderThickness(Thickness{1});
        shell.BorderBrush(lookup_brush(L"CardStrokeColorDefaultBrush"));
        Grid g;
        RowDefinition r0, r1;
        r0.Height(GridLength{0, GridUnitType::Auto});
        r1.Height(GridLength{1, GridUnitType::Star});
        g.RowDefinitions().Append(r0);
        g.RowDefinitions().Append(r1);
        Grid::SetRow(hdr.as<FrameworkElement>(), 0);
        Grid::SetRow(editor, 1);
        g.Children().Append(hdr);
        g.Children().Append(editor);
        shell.Child(g);
        return shell;
    };

    auto const mono_font = FontFamily{L"Cascadia Mono, Consolas, Courier New"};
    auto const copy_tip = strings_.get(L"CopyTextTip");

    // Column 0: Input
    TextBlock input_bytes_lbl;
    input_bytes_lbl.Text(hstring{UiStrings::format(strings_.get(L"BytesBadgeFormat"), {std::to_wstring(test->input_bytes.size())})});
    auto copy_in_btn = make_compact_icon_button(L"\xE8C8", copy_tip, 22);
    TextBox input_box;
    input_box.AcceptsReturn(true);
    input_box.TextWrapping(TextWrapping::NoWrap);
    input_box.FontFamily(mono_font);
    input_box.FontSize(12);
    input_box.MinHeight(112);
    input_box.MaxHeight(224);
    input_box.BorderThickness(Thickness{0});
    input_box.Text(test->input_binary ? RenderBytes(test->input_bytes) : to_hstring(test->input_bytes));
    input_box.IsReadOnly(test->input_binary);
    auto input_hdr = make_column_header(strings_.get(test->input_binary ? L"BinaryTestInput" : L"TestInput"), input_bytes_lbl, copy_in_btn);
    auto input_cell = make_cell_shell(input_hdr, input_box);
    Grid::SetColumn(input_cell, 0);
    tri_grid.Children().Append(input_cell);

    // Column 1: Output
    TextBlock output_bytes_lbl;
    output_bytes_lbl.Text(hstring{UiStrings::format(strings_.get(L"BytesBadgeFormat"), {L"0"})});
    auto copy_out_btn = make_compact_icon_button(L"\xE8C8", copy_tip, 22);
    TextBox output_box;
    output_box.AcceptsReturn(true);
    output_box.TextWrapping(TextWrapping::NoWrap);
    output_box.FontFamily(mono_font);
    output_box.FontSize(12);
    output_box.MinHeight(112);
    output_box.MaxHeight(224);
    output_box.BorderThickness(Thickness{0});
    output_box.IsReadOnly(true);
    auto output_hdr = make_column_header(strings_.get(L"TestOutput"), output_bytes_lbl, copy_out_btn);
    auto output_cell = make_cell_shell(output_hdr, output_box);
    Grid::SetColumn(output_cell, 1);
    tri_grid.Children().Append(output_cell);

    // Column 2: Expected Output
    TextBlock expected_bytes_lbl;
    expected_bytes_lbl.Text(hstring{UiStrings::format(strings_.get(L"BytesBadgeFormat"), {std::to_wstring(test->expected_bytes.size())})});
    StackPanel exp_actions;
    exp_actions.Orientation(Orientation::Horizontal);
    exp_actions.Spacing(4);
    exp_actions.VerticalAlignment(VerticalAlignment::Center);
    CheckBox has_expected_box;
    has_expected_box.Content(box_value(strings_.get(L"HasExpected")));
    has_expected_box.MinWidth(0);
    has_expected_box.FontSize(11);
    set_checked(has_expected_box, test->has_expected_value);
    auto copy_exp_btn = make_compact_icon_button(L"\xE8C8", copy_tip, 22);
    exp_actions.Children().Append(has_expected_box);
    exp_actions.Children().Append(copy_exp_btn);
    TextBox expected_box;
    expected_box.AcceptsReturn(true);
    expected_box.TextWrapping(TextWrapping::NoWrap);
    expected_box.FontFamily(mono_font);
    expected_box.FontSize(12);
    expected_box.MinHeight(112);
    expected_box.MaxHeight(224);
    expected_box.BorderThickness(Thickness{0});
    expected_box.Text(test->expected_binary ? RenderBytes(test->expected_bytes) : to_hstring(test->expected_bytes));
    expected_box.IsReadOnly(test->expected_binary);
    auto expected_hdr = make_column_header(strings_.get(test->expected_binary ? L"BinaryTestExpected" : L"TestExpected"), expected_bytes_lbl, exp_actions);
    auto expected_cell = make_cell_shell(expected_hdr, expected_box);
    Grid::SetColumn(expected_cell, 2);
    tri_grid.Children().Append(expected_cell);

    hscroll.Content(tri_grid);
    section_body.Children().Append(hscroll);

    // Gated inline diagnostics (hidden unless stderr/truncation/detail/checker present)
    Border diag_panel;
    diag_panel.Visibility(Visibility::Collapsed);
    diag_panel.BorderThickness(Thickness{2, 0, 0, 0});
    diag_panel.Padding(Thickness{8, 4, 4, 4});
    diag_panel.BorderBrush(VerdictBrush("CheckerError"));
    TextBlock diag_text;
    diag_text.FontFamily(mono_font);
    diag_text.FontSize(11);
    diag_text.TextWrapping(TextWrapping::Wrap);
    diag_text.IsTextSelectionEnabled(true);
    diag_panel.Child(diag_text);
    section_body.Children().Append(diag_panel);

    test->container = outer;
    test->stripe = stripe;
    test->title = title;
    test->enabled = enabled_box;
    test->verdict_badge = verdict_badge;
    test->verdict_text = verdict_text;
    test->elapsed_text = elapsed_text;
    test->copy_output = copy_out_btn;
    test->use_output = use_output_btn;
    test->duplicate_btn = dup_btn;
    test->remove = remove_btn;
    test->input_bytes_label = input_bytes_lbl;
    test->output_bytes_label = output_bytes_lbl;
    test->expected_bytes_label = expected_bytes_lbl;
    test->input = input_box;
    test->output_box = output_box;
    test->expected = expected_box;
    test->has_expected = has_expected_box;
    test->diagnostics_panel = diag_panel;
    test->diagnostics_text = diag_text;

    const auto weak = get_weak();
    const std::weak_ptr<Document> weak_document = document;
    const std::weak_ptr<TestView> weak_test = test;

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
        if (auto self = weak.get()) {
            if (self->updating_ui_) return;
            if (auto value = weak_document.lock()) if (auto item = weak_test.lock()) {
                const bool enabled = is_checked(item->enabled), has_expected = is_checked(item->has_expected);
                if (enabled == item->enabled_value && has_expected == item->has_expected_value) return;
                if (!self->TestsMutable(value)) {
                    self->updating_ui_ = true;
                    set_checked(item->enabled, item->enabled_value);
                    set_checked(item->has_expected, item->has_expected_value);
                    self->updating_ui_ = false;
                    return;
                }
                item->enabled_value = enabled;
                item->has_expected_value = has_expected;
                self->TestsChanged(value);
            }
        }
    };
    test->enabled.Checked(changed);
    test->enabled.Unchecked(changed);
    test->has_expected.Checked(changed);
    test->has_expected.Unchecked(changed);

    copy_in_btn.Click([weak, weak_test](auto const&, auto const&) {
        if (auto self = weak.get()) if (auto item = weak_test.lock()) self->CopyTextToClipboard(item->input.Text());
    });
    copy_out_btn.Click([weak, weak_test](auto const&, auto const&) {
        if (auto self = weak.get()) if (auto item = weak_test.lock()) self->CopyTextToClipboard(item->output_box.Text());
    });
    copy_exp_btn.Click([weak, weak_test](auto const&, auto const&) {
        if (auto self = weak.get()) if (auto item = weak_test.lock()) self->CopyTextToClipboard(item->expected.Text());
    });
    use_output_btn.Click([weak, weak_document, weak_test](auto const&, auto const&) {
        if (auto self = weak.get()) if (auto value = weak_document.lock()) if (auto item = weak_test.lock())
            self->AdoptOutputAsExpected(value, item);
    });
    dup_btn.Click([weak, weak_document, weak_test](auto const&, auto const&) {
        if (auto self = weak.get()) if (auto value = weak_document.lock()) if (auto item = weak_test.lock()) {
            if (!self->TestsMutable(value)) return;
            try {
                cpn::TestCase copy;
                copy.id = new_test_id();
                copy.input = item->input_bytes;
                if (item->has_expected_value) copy.expected = item->expected_bytes;
                copy.enabled = item->enabled_value;
                self->AddTest(value, std::move(copy));
                self->TestsChanged(value);
            } catch (...) {}
        }
    });
    remove_btn.Click([weak, weak_document, weak_test](auto const&, auto const&) {
        if (auto self = weak.get()) if (auto value = weak_document.lock()) if (auto item = weak_test.lock(); item && self->TestsMutable(value)) {
            std::uint32_t index{};
            if (value->test_panel.Children().IndexOf(item->container, index)) value->test_panel.Children().RemoveAt(index);
            std::erase(value->tests, item);
            self->RefreshTestHeaders(value);
            self->TestsChanged(value);
        }
    });

    document->tests.push_back(test);
    document->test_panel.Children().Append(test->container);
    RefreshTestHeaders(document);
    if (!restored) TestsChanged(document);
}

void MainWindow::RefreshTestHeaders(std::shared_ptr<Document> const& document) {
    for (std::size_t i = 0; i < document->tests.size(); ++i) {
        auto const& test = document->tests[i];
        test->title.Text(hstring{UiStrings::format(strings_.get(L"SampleHeaderFormat"), {std::to_wstring(i + 1)})});
        test->input_bytes_label.Text(hstring{UiStrings::format(strings_.get(L"BytesBadgeFormat"), {std::to_wstring(test->input_bytes.size())})});
        test->expected_bytes_label.Text(hstring{UiStrings::format(strings_.get(L"BytesBadgeFormat"), {std::to_wstring(test->expected_bytes.size())})});
    }
    if (document->empty_tests_panel) {
        document->empty_tests_panel.Visibility(document->tests.empty() ? Visibility::Visible : Visibility::Collapsed);
    }
}

void MainWindow::AdoptOutputAsExpected(std::shared_ptr<Document> const& document, std::shared_ptr<TestView> const& test) {
    if (!TestsMutable(document) || !test->last_output_adoptable || test->expected_binary) return;
    test->expected_bytes = test->last_output_bytes;
    test->has_expected_value = true;
    updating_ui_ = true;
    test->expected.Text(to_hstring(test->expected_bytes));
    set_checked(test->has_expected, true);
    updating_ui_ = false;
    RefreshTestHeaders(document);
    TestsChanged(document);
}

void MainWindow::EnableAllSamples_Click(IInspectable const&, RoutedEventArgs const&) {
    auto document = CurrentDocument();
    if (!document || !TestsMutable(document) || document->tests.empty()) return;
    updating_ui_ = true;
    for (auto const& test : document->tests) {
        test->enabled_value = true;
        set_checked(test->enabled, true);
    }
    updating_ui_ = false;
    TestsChanged(document);
}

void MainWindow::DisableAllSamples_Click(IInspectable const&, RoutedEventArgs const&) {
    auto document = CurrentDocument();
    if (!document || !TestsMutable(document) || document->tests.empty()) return;
    updating_ui_ = true;
    for (auto const& test : document->tests) {
        test->enabled_value = false;
        set_checked(test->enabled, false);
    }
    updating_ui_ = false;
    TestsChanged(document);
}

void MainWindow::RemoveDisabledSamples_Click(IInspectable const&, RoutedEventArgs const&) {
    auto document = CurrentDocument();
    if (!document || !TestsMutable(document)) return;
    bool removed = false;
    for (auto it = document->tests.begin(); it != document->tests.end();) {
        if (!(*it)->enabled_value) {
            std::uint32_t index{};
            if (document->test_panel.Children().IndexOf((*it)->container, index))
                document->test_panel.Children().RemoveAt(index);
            it = document->tests.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }
    if (removed) {
        RefreshTestHeaders(document);
        TestsChanged(document);
    }
}

fire_and_forget MainWindow::UseAllOutputs_Click(IInspectable const&, RoutedEventArgs const&) {
    auto lifetime = get_strong();
    auto document = CurrentDocument();
    if (!document || !TestsMutable(document) || !IsResultCurrent(document)) co_return;
    bool any = false;
    for (auto const& test : document->tests) {
        if (test->last_output_adoptable && !test->expected_binary) {
            test->expected_bytes = test->last_output_bytes;
            test->has_expected_value = true;
            updating_ui_ = true;
            test->expected.Text(to_hstring(test->expected_bytes));
            set_checked(test->has_expected, true);
            updating_ui_ = false;
            any = true;
        }
    }
    if (any) { RefreshTestHeaders(document); TestsChanged(document); }
}

fire_and_forget MainWindow::DeleteAllSamples_Click(IInspectable const&, RoutedEventArgs const&) {
    auto lifetime = get_strong();
    auto document = CurrentDocument();
    if (!document || !TestsMutable(document) || document->tests.empty()) co_return;
    for (auto const& test : document->tests) {
        std::uint32_t index{};
        if (document->test_panel.Children().IndexOf(test->container, index))
            document->test_panel.Children().RemoveAt(index);
    }
    document->tests.clear();
    RefreshTestHeaders(document);
    TestsChanged(document);
}

void MainWindow::RenderCompilerBanner(std::shared_ptr<Document> const& document) {
    if (!document || !document->compiler_banner) return;
    if (!document->result) {
        document->compiler_banner.Visibility(Visibility::Collapsed);
        return;
    }
    auto const& output = *document->result;
    bool const show_banner = output.build_outcome == "CompilationError" ||
                             output.build_outcome == "Failed" ||
                             !output.build_output.empty() ||
                             !output.build_error.empty() ||
                             !output.error.empty() ||
                             (output.build_outcome == "Succeeded" && document->result_mode == "compile_only");
    if (!show_banner) {
        document->compiler_banner.Visibility(Visibility::Collapsed);
        return;
    }
    document->compiler_banner.Visibility(Visibility::Visible);
    document->compiler_accent_bar.Background(VerdictBrush(output.build_outcome));

    std::wstring header;
    if (output.build_outcome == "Succeeded") {
        header = UiStrings::format(strings_.get(L"CompiledBannerFormat"), {std::to_wstring(output.build_elapsed_ms)});
    } else {
        auto const build_label = runner_configuration_.build_labels.find(output.build_outcome);
        auto const build_text = build_label == runner_configuration_.build_labels.end()
            ? strings_.get(L"RunUnknown") : strings_.get(to_hstring(build_label->second));
        header = std::wstring{strings_.get(L"CompilerOutputTitle")} + L" — " + std::wstring{build_text};
    }
    document->compiler_header.Text(hstring{header});
    document->compiler_header.Foreground(VerdictBrush(output.build_outcome));

    std::wstring body;
    if (!output.error.empty()) body += std::wstring{to_hstring(output.error)};
    if (!output.build_error.empty()) {
        if (!body.empty()) body += L"\n";
        body += std::wstring{RenderBytes(output.build_error)};
    }
    if (!output.build_output.empty()) {
        if (!body.empty()) body += L"\n";
        body += std::wstring{RenderBytes(output.build_output)};
    }
    if (output.build_stdout_truncated || output.build_stderr_truncated) {
        if (!body.empty()) body += L"\n";
        body += std::wstring{strings_.get(L"CoreOutputTruncated")};
    }
    document->compiler_output_text.Text(hstring{body});
    bool const expanded = document->compiler_expanded && !body.empty();
    document->compiler_scroll.Visibility(expanded ? Visibility::Visible : Visibility::Collapsed);
    document->compiler_chevron.Glyph(expanded ? L"\xE70D" : L"\xE76C");
    document->compiler_copy.Visibility(body.empty() ? Visibility::Collapsed : Visibility::Visible);
}

void MainWindow::UpdateSummaryBadge(std::shared_ptr<Document> const& document) {
    if (!document) return;
    std::wstring badge_text;
    std::string badge_verdict = "NotRun";
    bool const stale = !document->run_active && document->result && !IsResultCurrent(document);

    if (document->run_active) {
        auto const state = runner_configuration_.state_labels.find(document->run_state);
        badge_text = std::wstring{state == runner_configuration_.state_labels.end()
            ? strings_.get(L"RunUnknown") : strings_.get(to_hstring(state->second))};
    } else if (document->result) {
        auto const& output = *document->result;
        if (output.build_outcome == "CompilationError") {
            badge_verdict = "CompilationError";
            badge_text = L"CE";
        } else if (output.build_outcome == "Failed") {
            badge_verdict = "Failed";
            badge_text = std::wstring{strings_.get(L"BuildFailed")};
        } else if (document->result_mode == "compile_only" && output.build_outcome == "Succeeded") {
            badge_verdict = "Succeeded";
            badge_text = UiStrings::format(strings_.get(L"SummaryCompiledFormat"), {std::to_wstring(output.build_elapsed_ms)});
        } else {
            std::size_t total = 0, ac = 0;
            std::string first_non_ac;
            for (auto const& t : output.tests) {
                ++total;
                if (t.verdict == "AC") ++ac;
                else if (first_non_ac.empty()) first_non_ac = t.verdict;
            }
            if (total == 0) {
                badge_verdict = output.build_outcome == "Succeeded" ? "Succeeded" : "NotRun";
                badge_text = std::wstring{strings_.get(L"SummaryNoTests")};
            } else if (ac == total) {
                badge_verdict = "AC";
                badge_text = UiStrings::format(strings_.get(L"SummaryAllPassedFormat"), {std::to_wstring(ac), std::to_wstring(total)});
            } else {
                badge_verdict = first_non_ac.empty() ? "WA" : first_non_ac;
                badge_text = std::wstring{to_hstring(badge_verdict)} + L"  " + std::to_wstring(ac) + L"/" + std::to_wstring(total);
            }
        }
    } else {
        std::size_t enabled_count = 0;
        for (auto const& t : document->tests) if (t->enabled_value) ++enabled_count;
        badge_text = std::to_wstring(enabled_count) + L"/" + std::to_wstring(document->tests.size());
    }

    if (document->tab_badge && document->tab_badge_text) {
        if (document->run_active || document->result) {
            document->tab_badge.Visibility(Visibility::Visible);
            document->tab_badge.Background(VerdictBackgroundBrush(badge_verdict));
            document->tab_badge_text.Foreground(VerdictBrush(badge_verdict));
            document->tab_badge_text.Text(hstring{badge_text});
        } else {
            document->tab_badge.Visibility(Visibility::Collapsed);
        }
    }

    if (CurrentDocument() == document) {
        SummaryBadge().Background(VerdictBackgroundBrush(badge_verdict));
        SummaryBadgeText().Foreground(VerdictBrush(badge_verdict));
        SummaryBadgeText().Text(hstring{badge_text});
        OutdatedBadge().Visibility(stale ? Visibility::Visible : Visibility::Collapsed);
    }
}

void MainWindow::RenderResult(std::shared_ptr<Document> const& document) {
    if (!document->result) return;
    auto const& output = *document->result;
    bool const current = IsResultCurrent(document);

    for (auto const& test : document->tests) {
        const auto result = std::find_if(output.tests.begin(), output.tests.end(),
            [&](auto const& value) { return value.id == test->id; });
        if (result == output.tests.end()) {
            test->stripe.Background(SolidColorBrush{Windows::UI::Color{0, 0, 0, 0}});
            test->verdict_badge.Background(VerdictBackgroundBrush("NotRun"));
            test->verdict_text.Foreground(VerdictBrush("NotRun"));
            test->verdict_text.Text(strings_.get(L"VerdictNotRun"));
            test->elapsed_text.Text(L"");
            test->output_box.Text(L"");
            test->output_bytes_label.Text(hstring{UiStrings::format(strings_.get(L"BytesBadgeFormat"), {L"0"})});
            test->diagnostics_panel.Visibility(Visibility::Collapsed);
            test->last_output_bytes.clear();
            test->last_output_adoptable = false;
            continue;
        }

        auto const label = runner_configuration_.verdict_labels.find(result->verdict);
        auto const verdict = label == runner_configuration_.verdict_labels.end()
            ? strings_.get(L"RunUnknown") : strings_.get(to_hstring(label->second));
        test->stripe.Background(VerdictBrush(result->verdict));
        test->verdict_badge.Background(VerdictBackgroundBrush(result->verdict));
        test->verdict_text.Foreground(VerdictBrush(result->verdict));
        test->verdict_text.Text(verdict);
        test->elapsed_text.Text(hstring{UiStrings::format(strings_.get(L"SampleElapsedFormat"), {std::to_wstring(result->elapsed_ms)})});
        test->output_box.Text(RenderBytes(result->standard_output));
        test->output_bytes_label.Text(hstring{UiStrings::format(strings_.get(L"BytesBadgeFormat"), {std::to_wstring(result->stdout_total_bytes)})});

        test->last_output_bytes = result->standard_output;
        test->last_output_adoptable = result->termination == "Exited" &&
                                      result->exit_code == 0 &&
                                      !result->signal.has_value() &&
                                      !result->stdout_truncated &&
                                      !is_binary_bytes(result->standard_output);
        test->use_output.IsEnabled(current && test->last_output_adoptable && !test->expected_binary && TestsMutable(document));

        // Gated inline diagnostics section
        std::wstring diag;
        if (!result->standard_error.empty()) {
            diag += std::wstring{strings_.get(L"StderrLabel")} + L"\n" + std::wstring{RenderBytes(result->standard_error)};
        }
        if (result->stdout_truncated || result->stderr_truncated) {
            if (!diag.empty()) diag += L"\n";
            diag += std::wstring{strings_.get(L"CoreOutputTruncated")};
        }
        if (!result->detail.empty() && !result->checker) {
            if (!diag.empty()) diag += L"\n";
            diag += std::wstring{to_hstring(result->detail)};
        }
        if (result->checker) {
            auto const& chk = *result->checker;
            std::wstring suffix;
            if (!chk.process_attempted) suffix = std::wstring{strings_.get(L"CheckerNotAttempted")};
            else if (chk.exit_code) suffix = UiStrings::format(strings_.get(L"CheckerExitCodeFormat"), {std::to_wstring(*chk.exit_code)});
            if (!diag.empty()) diag += L"\n";
            diag += UiStrings::format(strings_.get(L"CheckerResultFormat"), {
                std::wstring{to_hstring(chk.verdict)}, std::to_wstring(chk.elapsed_ms), suffix});
            if (!chk.detail.empty()) diag += L"\n" + std::wstring{to_hstring(chk.detail)};
            if (!chk.standard_output.empty()) {
                diag += L"\n" + std::wstring{strings_.get(L"CheckerStdoutLabel")} + L"\n" + std::wstring{RenderBytes(chk.standard_output)};
            }
            if (!chk.standard_error.empty()) {
                diag += L"\n" + std::wstring{strings_.get(L"CheckerStderrLabel")} + L"\n" + std::wstring{RenderBytes(chk.standard_error)};
            }
            if (chk.stdout_truncated || chk.stderr_truncated) {
                diag += L"\n" + std::wstring{strings_.get(L"CoreOutputTruncated")};
            }
        }
        if (diag.empty()) {
            test->diagnostics_panel.Visibility(Visibility::Collapsed);
        } else {
            test->diagnostics_text.Text(hstring{diag});
            test->diagnostics_panel.BorderBrush(VerdictBrush(result->verdict));
            test->diagnostics_panel.Visibility(Visibility::Visible);
        }
    }
    RenderCompilerBanner(document);
    UpdateSummaryBadge(document);
}
}
