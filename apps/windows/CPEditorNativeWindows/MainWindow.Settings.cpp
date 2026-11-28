#include "pch.h"
#include "MainWindow.xaml.h"
#include <algorithm>
#include <limits>
#include <utility>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
using cpn::windows::CheckerModel;
using cpn::windows::RunnerConfiguration;
using cpn::windows::ToolchainChoice;
using cpn::windows::ToolchainDiscovery;
using cpn::windows::UiStrings;

namespace winrt::CPEditorNativeWindows::implementation {
namespace {
void set_checked(CheckBox const& box, bool value) {
    box.IsChecked(box_value(value).as<IReference<bool>>());
}
std::uint64_t parse_positive_u64(hstring const& text) {
    auto s = to_string(text);
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    if (s.empty()) throw std::invalid_argument("Missing timeout.");
    std::size_t pos = 0;
    auto value = std::stoull(s, &pos);
    if (pos != s.size() || value == 0) throw std::invalid_argument("Invalid positive timeout.");
    return value;
}
Border make_settings_card(UIElement const& child) {
    Border card;
    card.CornerRadius(CornerRadius{8});
    card.BorderThickness(Thickness{1});
    card.BorderBrush(Application::Current().Resources().Lookup(box_value(L"CardStrokeColorDefaultBrush")).as<Brush>());
    card.Background(Application::Current().Resources().Lookup(box_value(L"CardBackgroundFillColorDefaultBrush")).as<Brush>());
    card.Padding(Thickness{16, 14, 16, 14});
    card.Child(child);
    return card;
}
}

void MainWindow::ApplyWorkspaceUiPreferences() {
    ElementTheme requested = ElementTheme::Default;
    if (ui_preferences_.theme == "light") requested = ElementTheme::Light;
    else if (ui_preferences_.theme == "dark") requested = ElementTheme::Dark;
    if (Root().RequestedTheme() != requested) Root().RequestedTheme(requested);

    for (auto const& doc : documents_) {
        if (doc->adapter) {
            doc->adapter->set_word_wrap(ui_preferences_.word_wrap);
            doc->adapter->set_assist_enabled(ui_preferences_.auto_pair);
            doc->adapter->set_tab_width(ui_preferences_.tab_width);
            doc->adapter->set_assist_config(assist_config_);
        }
    }
    updating_ui_ = true;
    MenuWordWrapItem().IsChecked(ui_preferences_.word_wrap);
    MenuAutoPairItem().IsChecked(ui_preferences_.auto_pair);
    updating_ui_ = false;
    StatusWrapButton().Content(box_value(strings_.get(ui_preferences_.word_wrap ? L"StatusBarWrapOn" : L"StatusBarWrapOff")));
    if (auto doc = CurrentDocument()) {
        UpdateDocument(doc);
        UpdateCheckerMenuState(doc);
        if (doc->result) RenderResult(doc);
    } else {
        UpdateCheckerMenuState({});
    }
}

void MainWindow::RescanToolchains() {
    toolchain_probes_ = ToolchainDiscovery::probe_all(runner_configuration_.toolchains, discovery_configuration_);
    if (ToolchainDiscovery::apply_discovered_defaults(runner_configuration_.toolchains, toolchain_probes_)) {
        toolchain_probes_ = ToolchainDiscovery::probe_all(runner_configuration_.toolchains, discovery_configuration_);
    }
    UpdateControls();
}

ToolchainChoice const* MainWindow::FindToolchain(std::string const& id) const {
    for (auto const& tc : runner_configuration_.toolchains) {
        if (tc.id == id) return &tc;
    }
    return nullptr;
}

cpn::windows::ToolchainProbeResult const* MainWindow::FindProbe(std::string const& id) const {
    for (auto const& p : toolchain_probes_) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

void MainWindow::RebuildToolchainComboBox() {
    auto doc = CurrentDocument();
    selecting_document_ = true;
    try {
        Toolchains().Items().Clear();
        std::int32_t selected_index = -1;
        for (std::size_t i = 0; i < runner_configuration_.toolchains.size(); ++i) {
            auto const& choice = runner_configuration_.toolchains[i];
            ComboBoxItem item;
            item.Content(box_value(strings_.get(to_hstring(choice.label_resource))));
            Toolchains().Items().Append(item);
            if (doc && doc->toolchain == choice.id) selected_index = static_cast<std::int32_t>(i);
        }
        if (selected_index >= 0) Toolchains().SelectedIndex(selected_index);
    } catch (...) {
        selecting_document_ = false;
        throw;
    }
    selecting_document_ = false;
}

IAsyncAction MainWindow::ShowSettingsDialogAsync(int initial_tab) {
    auto lifetime = get_strong();
    if (!initialized_ || shutting_down_) co_return;

    bool const can_mutate = WorkspaceMutable();
    auto const mono = FontFamily{L"Cascadia Mono, Consolas, Courier New"};
    const auto weak = get_weak();

    StackPanel shell;
    shell.MinWidth(680);
    shell.Spacing(12);

    SelectorBar nav_bar;
    SelectorBarItem nav_general, nav_editor, nav_run;
    nav_general.Text(strings_.get(L"SettingsTabGeneral"));
    nav_editor.Text(strings_.get(L"SettingsTabEditor"));
    nav_run.Text(strings_.get(L"SettingsTabRun"));
    nav_bar.Items().Append(nav_general);
    nav_bar.Items().Append(nav_editor);
    nav_bar.Items().Append(nav_run);
    shell.Children().Append(nav_bar);

    // Page 0: General
    StackPanel general_panel;
    general_panel.Spacing(10);

    if (!can_mutate) {
        InfoBar ro_bar;
        ro_bar.IsOpen(true);
        ro_bar.IsClosable(false);
        ro_bar.Severity(InfoBarSeverity::Informational);
        ro_bar.Message(strings_.get(L"SettingsReadOnlyHint"));
        general_panel.Children().Append(ro_bar);
    }

    StackPanel appearance_group;
    appearance_group.Spacing(12);
    ComboBox theme_combo;
    theme_combo.Header(box_value(strings_.get(L"SettingsThemeLabel")));
    theme_combo.MinWidth(240);
    theme_combo.IsEnabled(can_mutate);
    ComboBoxItem theme_sys, theme_light, theme_dark;
    theme_sys.Content(box_value(strings_.get(L"SettingsThemeSystem")));
    theme_light.Content(box_value(strings_.get(L"SettingsThemeLight")));
    theme_dark.Content(box_value(strings_.get(L"SettingsThemeDark")));
    theme_combo.Items().Append(theme_sys);
    theme_combo.Items().Append(theme_light);
    theme_combo.Items().Append(theme_dark);
    if (ui_preferences_.theme == "light") theme_combo.SelectedIndex(1);
    else if (ui_preferences_.theme == "dark") theme_combo.SelectedIndex(2);
    else theme_combo.SelectedIndex(0);

    theme_combo.SelectionChanged([weak, theme_combo](auto const&, auto const&) {
        if (auto self = weak.get(); self && self->WorkspaceMutable()) {
            auto idx = theme_combo.SelectedIndex();
            std::string next = idx == 1 ? "light" : (idx == 2 ? "dark" : "system");
            if (next != self->ui_preferences_.theme) {
                self->ui_preferences_.theme = std::move(next);
                self->ApplyWorkspaceUiPreferences();
                self->ScheduleRecoverySave();
            }
        }
    });
    appearance_group.Children().Append(theme_combo);

    TextBox recovery_path_box;
    recovery_path_box.Header(box_value(strings_.get(L"SettingsRecoveryLocation")));
    recovery_path_box.IsReadOnly(true);
    recovery_path_box.Text(hstring{recovery_paths_.recovery_file.wstring()});
    appearance_group.Children().Append(recovery_path_box);
    general_panel.Children().Append(make_settings_card(appearance_group));

    // Page 1: Editor
    StackPanel editor_panel;
    editor_panel.Spacing(10);
    StackPanel editor_group;
    editor_group.Spacing(12);

    ToggleSwitch wrap_switch;
    wrap_switch.Header(box_value(strings_.get(L"SettingsWordWrap")));
    wrap_switch.IsOn(ui_preferences_.word_wrap);
    wrap_switch.IsEnabled(can_mutate);
    wrap_switch.Toggled([weak, wrap_switch](auto const&, auto const&) {
        if (auto self = weak.get(); self && self->WorkspaceMutable()) {
            self->ui_preferences_.word_wrap = wrap_switch.IsOn();
            self->ApplyWorkspaceUiPreferences();
            self->ScheduleRecoverySave();
        }
    });
    editor_group.Children().Append(wrap_switch);

    ToggleSwitch assist_switch;
    assist_switch.Header(box_value(strings_.get(L"SettingsAutoPair")));
    assist_switch.IsOn(ui_preferences_.auto_pair);
    assist_switch.IsEnabled(can_mutate);
    assist_switch.Toggled([weak, assist_switch](auto const&, auto const&) {
        if (auto self = weak.get(); self && self->WorkspaceMutable()) {
            self->ui_preferences_.auto_pair = assist_switch.IsOn();
            self->ApplyWorkspaceUiPreferences();
            self->ScheduleRecoverySave();
        }
    });
    editor_group.Children().Append(assist_switch);

    ComboBox tab_width_combo;
    tab_width_combo.Header(box_value(strings_.get(L"SettingsTabWidth")));
    tab_width_combo.MinWidth(160);
    tab_width_combo.IsEnabled(can_mutate);
    for (int w : {2, 4, 8}) {
        ComboBoxItem item;
        item.Content(box_value(hstring{std::to_wstring(w)}));
        tab_width_combo.Items().Append(item);
    }
    tab_width_combo.SelectedIndex(ui_preferences_.tab_width == 2 ? 0 : (ui_preferences_.tab_width == 8 ? 2 : 1));
    tab_width_combo.SelectionChanged([weak, tab_width_combo](auto const&, auto const&) {
        if (auto self = weak.get(); self && self->WorkspaceMutable()) {
            constexpr std::uint32_t widths[]{2, 4, 8};
            auto idx = tab_width_combo.SelectedIndex();
            if (idx >= 0 && idx < 3) {
                self->ui_preferences_.tab_width = widths[idx];
                self->ApplyWorkspaceUiPreferences();
                self->ScheduleRecoverySave();
            }
        }
    });
    editor_group.Children().Append(tab_width_combo);
    editor_panel.Children().Append(make_settings_card(editor_group));

    // Page 2: Run & Toolchains
    ScrollViewer run_scroll;
    run_scroll.MaxHeight(520);
    run_scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    StackPanel run_panel;
    run_panel.Spacing(12);
    run_panel.Padding(Thickness{0, 2, 10, 4});

    // Toolchain Fluent Card
    StackPanel tc_card_body;
    tc_card_body.Spacing(10);

    TextBlock tc_title;
    tc_title.Text(strings_.get(L"SettingsToolchainSection"));
    tc_title.FontSize(14);
    tc_title.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
    tc_card_body.Children().Append(tc_title);

    Grid tc_top;
    ColumnDefinition tc_c0, tc_c1;
    tc_c0.Width(GridLength{1, GridUnitType::Star});
    tc_c1.Width(GridLength{0, GridUnitType::Auto});
    tc_top.ColumnDefinitions().Append(tc_c0);
    tc_top.ColumnDefinitions().Append(tc_c1);

    StackPanel tc_left;
    tc_left.Orientation(Orientation::Horizontal);
    tc_left.Spacing(8);
    ComboBox tc_picker;
    tc_picker.MinWidth(200);
    for (auto const& c : runner_configuration_.toolchains) {
        ComboBoxItem item;
        item.Content(box_value(strings_.get(to_hstring(c.label_resource))));
        tc_picker.Items().Append(item);
    }
    std::int32_t initial_tc = 0;
    if (auto doc = CurrentDocument()) {
        for (std::size_t i = 0; i < runner_configuration_.toolchains.size(); ++i) {
            if (runner_configuration_.toolchains[i].id == doc->toolchain) { initial_tc = static_cast<std::int32_t>(i); break; }
        }
    }
    tc_picker.SelectedIndex(initial_tc);

    Button rescan_btn;
    rescan_btn.Content(box_value(strings_.get(L"SettingsRescanBtn")));
    tc_left.Children().Append(tc_picker);
    tc_left.Children().Append(rescan_btn);
    Grid::SetColumn(tc_left, 0);
    tc_top.Children().Append(tc_left);

    SelectorBar tc_mode_bar;
    SelectorBarItem tc_visual_item, tc_json_item;
    tc_visual_item.Text(strings_.get(L"SettingsModeVisual"));
    tc_json_item.Text(strings_.get(L"SettingsModeJson"));
    tc_mode_bar.Items().Append(tc_visual_item);
    tc_mode_bar.Items().Append(tc_json_item);
    tc_mode_bar.SelectedItem(tc_visual_item);
    Grid::SetColumn(tc_mode_bar, 1);
    tc_top.Children().Append(tc_mode_bar);
    tc_card_body.Children().Append(tc_top);

    TextBlock tc_probe_status;
    tc_probe_status.FontSize(12);
    tc_probe_status.Foreground(Application::Current().Resources().Lookup(box_value(L"TextFillColorSecondaryBrush")).as<Brush>());
    tc_probe_status.TextWrapping(TextWrapping::Wrap);
    tc_card_body.Children().Append(tc_probe_status);

    StackPanel tc_visual_panel;
    tc_visual_panel.Spacing(8);
    TextBox tc_build_exe; tc_build_exe.Header(box_value(strings_.get(L"SettingsBuildExecutable")));
    TextBox tc_build_args; tc_build_args.Header(box_value(strings_.get(L"SettingsBuildArgs")));
    tc_build_args.AcceptsReturn(true); tc_build_args.FontFamily(mono); tc_build_args.MaxHeight(96);
    TextBox tc_build_timeout; tc_build_timeout.Header(box_value(strings_.get(L"SettingsBuildTimeout")));
    TextBox tc_run_exe; tc_run_exe.Header(box_value(strings_.get(L"SettingsRunExecutable")));
    TextBox tc_run_args; tc_run_args.Header(box_value(strings_.get(L"SettingsRunArgs")));
    tc_run_args.AcceptsReturn(true); tc_run_args.FontFamily(mono); tc_run_args.MaxHeight(72);
    TextBox tc_run_timeout; tc_run_timeout.Header(box_value(strings_.get(L"SettingsRunTimeout")));
    CheckBox tc_inherit_env; tc_inherit_env.Content(box_value(strings_.get(L"SettingsInheritEnv")));
    tc_visual_panel.Children().Append(tc_build_exe);
    tc_visual_panel.Children().Append(tc_build_args);
    tc_visual_panel.Children().Append(tc_build_timeout);
    tc_visual_panel.Children().Append(tc_run_exe);
    tc_visual_panel.Children().Append(tc_run_args);
    tc_visual_panel.Children().Append(tc_run_timeout);
    tc_visual_panel.Children().Append(tc_inherit_env);
    tc_card_body.Children().Append(tc_visual_panel);

    TextBox tc_json_box;
    tc_json_box.AcceptsReturn(true);
    tc_json_box.FontFamily(mono);
    tc_json_box.FontSize(12);
    tc_json_box.MinHeight(180);
    tc_json_box.MaxHeight(260);
    tc_json_box.Visibility(Visibility::Collapsed);
    tc_card_body.Children().Append(tc_json_box);

    TextBlock tc_feedback;
    tc_feedback.FontSize(12);
    tc_feedback.TextWrapping(TextWrapping::Wrap);
    tc_card_body.Children().Append(tc_feedback);

    Button tc_apply_btn;
    tc_apply_btn.Style(Application::Current().Resources().Lookup(box_value(L"AccentButtonStyle")).as<Style>());
    tc_apply_btn.Content(box_value(strings_.get(L"SettingsApplyToolchain")));
    tc_apply_btn.IsEnabled(can_mutate);
    tc_card_body.Children().Append(tc_apply_btn);
    run_panel.Children().Append(make_settings_card(tc_card_body));

    auto load_tc_fields = [weak, tc_picker, tc_probe_status, tc_build_exe, tc_build_args, tc_build_timeout,
                           tc_run_exe, tc_run_args, tc_run_timeout, tc_inherit_env, tc_json_box, tc_feedback, can_mutate]() {
        auto self = weak.get();
        if (!self) return;
        auto idx = tc_picker.SelectedIndex();
        if (idx < 0 || static_cast<std::size_t>(idx) >= self->runner_configuration_.toolchains.size()) return;
        auto const& tc = self->runner_configuration_.toolchains[static_cast<std::size_t>(idx)];
        tc_feedback.Text(L"");
        if (auto const* probe = self->FindProbe(tc.id)) {
            tc_probe_status.Text(hstring{UiStrings::format(
                self->strings_.get(probe->available ? L"ToolchainAvailableFormat" : L"ToolchainUnavailableFormat"),
                {std::wstring{to_hstring(probe->detail)}})});
        } else {
            tc_probe_status.Text(L"");
        }
        tc_json_box.Text(to_hstring(tc.profile.dump(2)));
        auto const draft = RunnerConfiguration::visual_from_json(tc.profile);
        auto const build_vis = draft.has_build ? Visibility::Visible : Visibility::Collapsed;
        tc_build_exe.Visibility(build_vis);
        tc_build_args.Visibility(build_vis);
        tc_build_timeout.Visibility(build_vis);
        tc_build_exe.Text(to_hstring(draft.build_executable));
        tc_build_args.Text(to_hstring(CheckerModel::format_arguments_lines(draft.build_arguments)));
        tc_build_timeout.Text(hstring{std::to_wstring(draft.build_wall_timeout_ms)});
        tc_run_exe.Text(to_hstring(draft.run_executable));
        tc_run_args.Text(to_hstring(CheckerModel::format_arguments_lines(draft.run_arguments)));
        tc_run_timeout.Text(hstring{std::to_wstring(draft.run_wall_timeout_ms)});
        set_checked(tc_inherit_env, draft.inherit_environment);
        tc_build_exe.IsReadOnly(!can_mutate); tc_build_args.IsReadOnly(!can_mutate); tc_build_timeout.IsReadOnly(!can_mutate);
        tc_run_exe.IsReadOnly(!can_mutate); tc_run_args.IsReadOnly(!can_mutate); tc_run_timeout.IsReadOnly(!can_mutate);
        tc_inherit_env.IsEnabled(can_mutate);
        tc_json_box.IsReadOnly(!can_mutate);
    };
    load_tc_fields();
    tc_picker.SelectionChanged([load_tc_fields](auto const&, auto const&) { load_tc_fields(); });
    rescan_btn.Click([weak, load_tc_fields](auto const&, auto const&) {
        if (auto self = weak.get()) {
            self->RescanToolchains();
            load_tc_fields();
        }
    });
    tc_mode_bar.SelectionChanged([tc_mode_bar, tc_visual_item, tc_visual_panel, tc_json_box](auto const&, auto const&) {
        bool const visual = tc_mode_bar.SelectedItem() == tc_visual_item;
        tc_visual_panel.Visibility(visual ? Visibility::Visible : Visibility::Collapsed);
        tc_json_box.Visibility(visual ? Visibility::Collapsed : Visibility::Visible);
    });
    tc_apply_btn.Click([weak, tc_picker, tc_mode_bar, tc_visual_item, tc_build_exe, tc_build_args, tc_build_timeout,
                        tc_run_exe, tc_run_args, tc_run_timeout, tc_inherit_env, tc_json_box, tc_feedback, load_tc_fields](auto const&, auto const&) {
        auto self = weak.get();
        if (!self || !self->WorkspaceMutable()) return;
        auto idx = tc_picker.SelectedIndex();
        if (idx < 0 || static_cast<std::size_t>(idx) >= self->runner_configuration_.toolchains.size()) return;
        auto& tc = self->runner_configuration_.toolchains[static_cast<std::size_t>(idx)];
        try {
            nlohmann::json validated;
            if (tc_mode_bar.SelectedItem() == tc_visual_item) {
                auto draft = RunnerConfiguration::visual_from_json(tc.profile);
                if (draft.has_build) {
                    draft.build_executable = to_string(tc_build_exe.Text());
                    draft.build_arguments = CheckerModel::parse_arguments_lines(to_string(tc_build_args.Text()));
                    draft.build_wall_timeout_ms = parse_positive_u64(tc_build_timeout.Text());
                }
                draft.run_executable = to_string(tc_run_exe.Text());
                draft.run_arguments = CheckerModel::parse_arguments_lines(to_string(tc_run_args.Text()));
                draft.run_wall_timeout_ms = parse_positive_u64(tc_run_timeout.Text());
                auto const inh = tc_inherit_env.IsChecked();
                draft.inherit_environment = inh && inh.Value();
                validated = RunnerConfiguration::apply_visual_to_json(tc.profile, draft);
            } else {
                validated = RunnerConfiguration::validate_toolchain_json(to_string(tc_json_box.Text()));
                if (validated.at("id").get<std::string>() != tc.id)
                    throw std::invalid_argument("Toolchain JSON id must match the selected toolchain.");
            }
            tc.profile = std::move(validated);
            for (auto const& doc : self->documents_) {
                if (doc->compiled_artifact && doc->compiled_artifact->toolchain_id == tc.id)
                    doc->compiled_artifact.reset();
            }
            self->toolchain_probes_ = ToolchainDiscovery::probe_all(self->runner_configuration_.toolchains, self->discovery_configuration_);
            load_tc_fields();
            self->RebuildToolchainComboBox();
            self->UpdateControls();
            self->ScheduleRecoverySave();
            tc_feedback.Text(self->strings_.get(L"SettingsToolchainApplied"));
        } catch (...) {
            tc_feedback.Text(CurrentError());
        }
    });

    // External Checker Fluent Card
    StackPanel chk_card_body;
    chk_card_body.Spacing(10);

    Grid chk_hdr;
    ColumnDefinition ch_c0, ch_c1;
    ch_c0.Width(GridLength{1, GridUnitType::Star});
    ch_c1.Width(GridLength{0, GridUnitType::Auto});
    chk_hdr.ColumnDefinitions().Append(ch_c0);
    chk_hdr.ColumnDefinitions().Append(ch_c1);

    TextBlock chk_title;
    chk_title.Text(strings_.get(L"SettingsCheckerSection"));
    chk_title.FontSize(14);
    chk_title.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
    chk_title.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(chk_title, 0);
    chk_hdr.Children().Append(chk_title);

    SelectorBar chk_mode_bar;
    SelectorBarItem chk_visual_item, chk_json_item;
    chk_visual_item.Text(strings_.get(L"SettingsModeVisual"));
    chk_json_item.Text(strings_.get(L"SettingsModeJson"));
    chk_mode_bar.Items().Append(chk_visual_item);
    chk_mode_bar.Items().Append(chk_json_item);
    chk_mode_bar.SelectedItem(chk_visual_item);
    Grid::SetColumn(chk_mode_bar, 1);
    chk_hdr.Children().Append(chk_mode_bar);
    chk_card_body.Children().Append(chk_hdr);

    ToggleSwitch chk_enable_switch;
    chk_enable_switch.Header(box_value(strings_.get(L"SettingsCheckerEnabled")));
    chk_enable_switch.IsOn(checker_settings_.enabled);
    chk_enable_switch.IsEnabled(can_mutate);
    chk_card_body.Children().Append(chk_enable_switch);

    StackPanel chk_visual_panel;
    chk_visual_panel.Spacing(8);
    TextBox chk_exe; chk_exe.Header(box_value(strings_.get(L"SettingsCheckerExecutable")));
    TextBox chk_args; chk_args.Header(box_value(strings_.get(L"SettingsCheckerArgs")));
    chk_args.AcceptsReturn(true); chk_args.FontFamily(mono); chk_args.MaxHeight(96);
    TextBox chk_timeout; chk_timeout.Header(box_value(strings_.get(L"SettingsCheckerTimeout")));
    TextBox chk_accept; chk_accept.Header(box_value(strings_.get(L"SettingsCheckerAcceptCodes")));
    TextBox chk_wa; chk_wa.Header(box_value(strings_.get(L"SettingsCheckerWaCodes")));
    CheckBox chk_inherit; chk_inherit.Content(box_value(strings_.get(L"SettingsInheritEnv")));
    chk_visual_panel.Children().Append(chk_exe);
    chk_visual_panel.Children().Append(chk_args);
    chk_visual_panel.Children().Append(chk_timeout);
    chk_visual_panel.Children().Append(chk_accept);
    chk_visual_panel.Children().Append(chk_wa);
    chk_visual_panel.Children().Append(chk_inherit);
    chk_card_body.Children().Append(chk_visual_panel);

    TextBox chk_json_box;
    chk_json_box.AcceptsReturn(true);
    chk_json_box.FontFamily(mono);
    chk_json_box.FontSize(12);
    chk_json_box.MinHeight(160);
    chk_json_box.MaxHeight(240);
    chk_json_box.Visibility(Visibility::Collapsed);
    chk_card_body.Children().Append(chk_json_box);

    TextBlock chk_feedback;
    chk_feedback.FontSize(12);
    chk_feedback.TextWrapping(TextWrapping::Wrap);
    chk_card_body.Children().Append(chk_feedback);

    Button chk_apply_btn;
    chk_apply_btn.Style(Application::Current().Resources().Lookup(box_value(L"AccentButtonStyle")).as<Style>());
    chk_apply_btn.Content(box_value(strings_.get(L"SettingsApplyChecker")));
    chk_apply_btn.IsEnabled(can_mutate);
    chk_card_body.Children().Append(chk_apply_btn);
    run_panel.Children().Append(make_settings_card(chk_card_body));

    auto load_chk_fields = [weak, chk_exe, chk_args, chk_timeout, chk_accept, chk_wa, chk_inherit, chk_json_box, can_mutate]() {
        auto self = weak.get();
        if (!self) return;
        auto const json_str = self->checker_settings_.json_text.empty()
            ? self->checker_configuration_.default_checker.dump(2)
            : self->checker_settings_.json_text;
        chk_json_box.Text(to_hstring(json_str));
        auto const draft = CheckerModel::visual_from_json(json_str, self->checker_configuration_.max_checker_json_bytes);
        chk_exe.Text(to_hstring(draft.executable));
        chk_args.Text(to_hstring(CheckerModel::format_arguments_lines(draft.arguments)));
        chk_timeout.Text(hstring{std::to_wstring(draft.wall_timeout_ms)});
        chk_accept.Text(to_hstring(CheckerModel::format_exit_codes(draft.accepted_exit_codes)));
        chk_wa.Text(to_hstring(CheckerModel::format_exit_codes(draft.wrong_answer_exit_codes)));
        set_checked(chk_inherit, draft.inherit_environment);
        chk_exe.IsReadOnly(!can_mutate); chk_args.IsReadOnly(!can_mutate); chk_timeout.IsReadOnly(!can_mutate);
        chk_accept.IsReadOnly(!can_mutate); chk_wa.IsReadOnly(!can_mutate); chk_inherit.IsEnabled(can_mutate);
        chk_json_box.IsReadOnly(!can_mutate);
    };
    load_chk_fields();

    chk_enable_switch.Toggled([weak, chk_enable_switch](auto const&, auto const&) {
        if (auto self = weak.get(); self && self->WorkspaceMutable()) {
            self->checker_settings_.enabled = chk_enable_switch.IsOn();
            self->ui_preferences_.checker = self->checker_settings_;
            self->ApplyWorkspaceUiPreferences();
            self->ScheduleRecoverySave();
        }
    });
    chk_mode_bar.SelectionChanged([chk_mode_bar, chk_visual_item, chk_visual_panel, chk_json_box](auto const&, auto const&) {
        bool const visual = chk_mode_bar.SelectedItem() == chk_visual_item;
        chk_visual_panel.Visibility(visual ? Visibility::Visible : Visibility::Collapsed);
        chk_json_box.Visibility(visual ? Visibility::Collapsed : Visibility::Visible);
    });
    chk_apply_btn.Click([weak, chk_enable_switch, chk_mode_bar, chk_visual_item, chk_exe, chk_args, chk_timeout,
                         chk_accept, chk_wa, chk_inherit, chk_json_box, chk_feedback, load_chk_fields](auto const&, auto const&) {
        auto self = weak.get();
        if (!self || !self->WorkspaceMutable()) return;
        try {
            std::string next_json;
            if (chk_mode_bar.SelectedItem() == chk_visual_item) {
                auto const base = self->checker_settings_.json_text.empty()
                    ? self->checker_configuration_.default_checker.dump(2)
                    : self->checker_settings_.json_text;
                auto draft = CheckerModel::visual_from_json(base, self->checker_configuration_.max_checker_json_bytes);
                draft.executable = to_string(chk_exe.Text());
                draft.arguments = CheckerModel::parse_arguments_lines(to_string(chk_args.Text()));
                draft.wall_timeout_ms = parse_positive_u64(chk_timeout.Text());
                draft.accepted_exit_codes = CheckerModel::parse_exit_codes(to_string(chk_accept.Text()));
                draft.wrong_answer_exit_codes = CheckerModel::parse_exit_codes(to_string(chk_wa.Text()));
                auto const inh = chk_inherit.IsChecked();
                draft.inherit_environment = inh && inh.Value();
                next_json = CheckerModel::apply_visual_to_json(base, draft, self->checker_configuration_.max_checker_json_bytes);
            } else {
                auto parsed = CheckerModel::validate_json(to_string(chk_json_box.Text()), self->checker_configuration_.max_checker_json_bytes);
                next_json = parsed.dump(2);
            }
            self->checker_settings_.enabled = chk_enable_switch.IsOn();
            self->checker_settings_.json_text = std::move(next_json);
            self->ui_preferences_.checker = self->checker_settings_;
            load_chk_fields();
            self->ApplyWorkspaceUiPreferences();
            self->ScheduleRecoverySave();
            chk_feedback.Text(self->strings_.get(L"SettingsCheckerApplied"));
        } catch (...) {
            chk_feedback.Text(CurrentError());
        }
    });

    run_scroll.Content(run_panel);

    Grid page_host;
    page_host.Children().Append(general_panel);
    page_host.Children().Append(editor_panel);
    page_host.Children().Append(run_scroll);
    shell.Children().Append(page_host);

    auto show_page = [nav_general, nav_editor, nav_run, general_panel, editor_panel, run_scroll](SelectorBarItem const& item) {
        general_panel.Visibility(item == nav_general ? Visibility::Visible : Visibility::Collapsed);
        editor_panel.Visibility(item == nav_editor ? Visibility::Visible : Visibility::Collapsed);
        run_scroll.Visibility(item == nav_run ? Visibility::Visible : Visibility::Collapsed);
    };
    nav_bar.SelectionChanged([nav_bar, show_page](auto const&, auto const&) {
        if (auto item = nav_bar.SelectedItem()) show_page(item);
    });
    auto initial_item = initial_tab == 1 ? nav_editor : (initial_tab == 2 ? nav_run : nav_general);
    nav_bar.SelectedItem(initial_item);
    show_page(initial_item);

    ContentDialog dialog;
    dialog.XamlRoot(Root().XamlRoot());
    dialog.Title(box_value(strings_.get(L"SettingsTitle")));
    dialog.Content(shell);
    dialog.CloseButtonText(strings_.get(L"Close"));
    dialog.DefaultButton(ContentDialogButton::Close);
    (void)(co_await ShowNativeDialogAsync(dialog));
}

fire_and_forget MainWindow::Settings_Click(IInspectable const&, RoutedEventArgs const&) {
    auto lifetime = get_strong();
    co_await ShowSettingsDialogAsync(0);
}

fire_and_forget MainWindow::ConfigureChecker_Click(IInspectable const&, RoutedEventArgs const&) {
    auto lifetime = get_strong();
    co_await ShowSettingsDialogAsync(2);
}

void MainWindow::UseExternalCheckerMenu_Click(IInspectable const&, RoutedEventArgs const&) {
    if (updating_ui_ || !WorkspaceMutable()) return;
    bool const next = MenuUseExternalCheckerItem().IsChecked();
    checker_settings_.enabled = next;
    if (checker_settings_.json_text.empty()) {
        checker_settings_.json_text = checker_configuration_.default_checker.dump(2);
    }
    ui_preferences_.checker = checker_settings_;
    ApplyWorkspaceUiPreferences();
    ScheduleRecoverySave();
    UpdateControls();
}

void MainWindow::ComparisonPolicy_Click(IInspectable const& sender, RoutedEventArgs const&) {
    if (updating_ui_) return;
    auto document = CurrentDocument();
    if (!document || !TestsMutable(document)) {
        UpdateCheckerMenuState(document);
        return;
    }
    std::string next = "trailing";
    if (sender == MenuCompareStrictItem()) next = "strict";
    else if (sender == MenuCompareByteItem()) next = "byte";
    if (document->comparison != next) {
        document->comparison = std::move(next);
        TestsChanged(document);
    }
    UpdateCheckerMenuState(document);
}

void MainWindow::RescanToolchains_Click(IInspectable const&, RoutedEventArgs const&) {
    RescanToolchains();
}
}
