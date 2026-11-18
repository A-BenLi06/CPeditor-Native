import SwiftUI
import NativeCoreBridge

enum WorkspaceAppearance: String, CaseIterable {
    case system
    case light
    case dark

    static let defaultsKey = "workspaceAppearance"

    var colorScheme: ColorScheme? {
        switch self {
        case .system: nil
        case .light: .light
        case .dark: .dark
        }
    }

    var displayName: String {
        switch self {
        case .system: L10n.string("settings_appearance_system")
        case .light: L10n.string("settings_appearance_light")
        case .dark: L10n.string("settings_appearance_dark")
        }
    }
}

struct SettingsView: View {
    private enum Page: Hashable { case general, editor, run }

    @Bindable var workspace: Workspace
    @Binding var appearance: WorkspaceAppearance
    @State private var page = Page.general
    @State private var importError: String?

    var body: some View {
        @Bindable var runner = workspace.runner

        TabView(selection: $page) {
            Tab(L10n.string("settings_general"), systemImage: "gearshape", value: .general) {
                generalSettings
            }
            Tab(L10n.string("settings_editor"), systemImage: "doc.text", value: .editor) {
                editorSettings
            }
            Tab(L10n.string("run_menu"), systemImage: "play.rectangle", value: .run) {
                runSettings
            }
        }
        .frame(width: 580, height: 340)
        .onChange(of: runner.configurationDraft?.id, initial: true) {
            if runner.configurationDraft != nil { page = .run }
        }
        .sheet(item: $runner.configurationDraft) { draft in
            ConfigurationSheet(draft: draft) { jsonText in
                try workspace.applyRunnerConfiguration(draft, jsonText: jsonText)
            }
        }
        .alert(L10n.string("error_title"), isPresented: Binding(
            get: { importError != nil },
            set: { if !$0 { importError = nil } }
        )) {
            Button(L10n.string("ok"), role: .cancel) { importError = nil }
        } message: {
            Text(importError ?? "")
        }
    }

    private var generalSettings: some View {
        Form {
            Section {
                LabeledContent(L10n.string("settings_appearance")) {
                    Picker(L10n.string("settings_appearance"), selection: $appearance) {
                        ForEach(WorkspaceAppearance.allCases, id: \.self) { option in
                            Text(option.displayName).tag(option)
                        }
                    }
                    .labelsHidden()
                    .pickerStyle(.segmented)
                    .fixedSize()
                }

                Picker(L10n.string("layout_mode"), selection: $workspace.layoutMode) {
                    ForEach(WorkspaceLayoutMode.allCases, id: \.self) { mode in
                        Text(mode.localizedTitle).tag(mode)
                    }
                }
                .disabled(!workspace.canChangeLayout)
            }
        }
        .formStyle(.grouped)
    }

    private var editorSettings: some View {
        Form {
            Section {
                Toggle(L10n.string("wrap_lines"), isOn: $workspace.wrapText)
                Toggle(L10n.string("show_minimap"), isOn: $workspace.showMinimap)
            }
            .disabled(!workspace.canChangeEditorSettings)
        }
        .formStyle(.grouped)
    }

    private var runSettings: some View {
        @Bindable var runner = workspace.runner

        return Form {
            Section(L10n.string("toolchain")) {
                if runner.profiles.isEmpty {
                    LabeledContent(L10n.string("settings_toolchain_profile"), value: L10n.string("settings_no_toolchain"))
                } else {
                    Picker(L10n.string("settings_toolchain_profile"), selection: Binding(
                        get: { runner.selectedProfileID },
                        set: { workspace.selectToolchain($0) }
                    )) {
                        if runner.selectedProfile == nil {
                            Text(L10n.string("settings_no_toolchain")).tag(runner.selectedProfileID)
                        }
                        ForEach(runner.profiles) { profile in
                            Text(profile.displayName).tag(profile.id)
                        }
                    }
                }

                HStack {
                    Spacer()
                    Button(L10n.string("edit_toolchain")) { runner.beginProfileEdit() }
                        .disabled(runner.selectedProfile == nil)
                    Button(L10n.string("import_toolchain_profile")) {
                        workspace.importToolchain(onError: { importError = $0 })
                    }
                }
            }

            Section(L10n.string("settings_judging")) {
                Picker(L10n.string("comparison"), selection: $runner.comparison) {
                    ForEach(ComparisonPolicy.allCases, id: \.self) { policy in
                        Text(L10n.comparisonName(policy)).tag(policy)
                    }
                }
                .disabled(runner.externalCheckerEnabled)

                Toggle(L10n.string("use_external_checker"), isOn: Binding(
                    get: { runner.externalCheckerEnabled },
                    set: { enabled in
                        if enabled, runner.externalChecker == nil { runner.beginCheckerEdit(enableOnApply: true) }
                        else { runner.externalCheckerEnabled = enabled }
                    }
                ))
                .disabled(!runner.canConfigureChecker)

                HStack {
                    Spacer()
                    Button(L10n.string("edit_checker")) { runner.beginCheckerEdit() }
                        .disabled(!runner.canConfigureChecker)
                }
            }
        }
        .formStyle(.grouped)
        .disabled(!workspace.canConfigureRunner)
    }
}
