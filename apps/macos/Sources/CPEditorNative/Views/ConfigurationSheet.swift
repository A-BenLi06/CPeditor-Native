import AppKit
import SwiftUI
import NativeCoreBridge

struct ConfigurationSheet: View {
    private enum EditorMode: String, CaseIterable, Identifiable {
        case visual
        case json

        var id: String { rawValue }
    }

    @Environment(\.dismiss) private var dismiss
    private let kind: ConfigurationJSONDraft.Kind
    private let apply: (String) throws -> Void

    @State private var mode: EditorMode
    @State private var jsonText: String
    @State private var errorMessage: String?

    @State private var baseToolchain: ToolchainProfile?
    @State private var baseChecker: CheckerConfiguration?

    @State private var executable = ""
    @State private var argumentsText = ""
    @State private var buildTimeoutText = "30000"
    @State private var runTimeoutText = "2000"
    @State private var acceptedCodesText = "0"
    @State private var wrongCodesText = "1, 2"

    private var title: String {
        L10n.string(kind == .checker ? "checker_json" : "toolchain_json")
    }

    private var subtitle: String {
        L10n.string(kind == .checker ? "checker_configuration_help" : "toolchain_configuration_help")
    }

    private var iconName: String {
        kind == .checker ? "checkmark.shield" : "hammer"
    }

    init(draft: ConfigurationJSONDraft, apply: @escaping (String) throws -> Void) {
        kind = draft.kind
        self.apply = apply
        _jsonText = State(initialValue: draft.jsonText)

        switch draft.kind {
        case .toolchain:
            if let profile = try? JSONDecoder().decode(ToolchainProfile.self, from: Data(draft.jsonText.utf8)),
               let buildMS = profile.buildWallTimeoutMS,
               let runMS = profile.runWallTimeoutMS {
                _mode = State(initialValue: .visual)
                _baseToolchain = State(initialValue: profile)
                _executable = State(initialValue: profile.buildExecutable ?? "")
                _argumentsText = State(initialValue: Self.formatArguments(profile.buildArguments))
                _buildTimeoutText = State(initialValue: String(buildMS))
                _runTimeoutText = State(initialValue: String(runMS))
            } else {
                _mode = State(initialValue: .json)
            }
        case .checker:
            if let checker = try? CheckerConfiguration(jsonText: draft.jsonText) {
                let values = checker.formValues()
                _mode = State(initialValue: .visual)
                _baseChecker = State(initialValue: checker)
                _executable = State(initialValue: values.executable)
                _argumentsText = State(initialValue: Self.formatArguments(values.arguments))
                _runTimeoutText = State(initialValue: String(values.wallTimeoutMS))
                _acceptedCodesText = State(initialValue: values.acceptedExitCodes.map(String.init).joined(separator: ", "))
                _wrongCodesText = State(initialValue: values.wrongAnswerExitCodes.map(String.init).joined(separator: ", "))
            } else {
                _mode = State(initialValue: .json)
            }
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(alignment: .center, spacing: 12) {
                VStack(alignment: .leading, spacing: 3) {
                    Label(title, systemImage: iconName)
                        .font(.headline)
                    Text(subtitle)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(2)
                }

                Spacer(minLength: 12)

                Picker(L10n.string("config_mode"), selection: Binding(
                    get: { mode },
                    set: { switchMode(to: $0) }
                )) {
                    Text(L10n.string("config_mode_visual")).tag(EditorMode.visual)
                    Text(L10n.string("config_mode_json")).tag(EditorMode.json)
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .controlSize(.small)
                .fixedSize()
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 12)
            .background(.bar)

            Divider()

            if let errorMessage {
                NoticeBannerView(kind: .error, message: errorMessage)
            }

            Group {
                if mode == .visual {
                    visualForm
                } else {
                    TextEditor(text: $jsonText)
                        .font(.system(.body, design: .monospaced))
                        .scrollContentBackground(.hidden)
                        .padding(8)
                        .background(Color(nsColor: .textBackgroundColor), in: RoundedRectangle(cornerRadius: 8))
                        .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(.separator, lineWidth: 1))
                        .padding(14)
                        .accessibilityLabel(title)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)

            Divider()

            HStack {
                if mode == .json {
                    Button(L10n.string("format_json")) {
                        formatCurrentJSON()
                    }
                    .controlSize(.small)
                }
                Spacer()
                Button(L10n.string("cancel"), role: .cancel) {
                    dismiss()
                }
                .keyboardShortcut(.cancelAction)

                Button(L10n.string("apply"), action: applyDraft)
                    .buttonStyle(.borderedProminent)
                    .keyboardShortcut(.defaultAction)
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 11)
            .background(.bar)
        }
        .frame(minWidth: 560, idealWidth: 640, minHeight: 420, idealHeight: kind == .checker ? 500 : 480)
    }

    @ViewBuilder
    private var visualForm: some View {
        switch kind {
        case .toolchain:
            Form {
                if let profile = baseToolchain {
                    Section(L10n.string("toolchain_section_overview")) {
                        LabeledContent(L10n.string("toolchain_field_id")) {
                            Text(profile.id.rawValue)
                                .font(.system(.body, design: .monospaced))
                                .foregroundStyle(.secondary)
                                .textSelection(.enabled)
                        }
                        LabeledContent(L10n.string("toolchain_field_language")) {
                            Text(profile.languageID.uppercased())
                                .foregroundStyle(.secondary)
                        }
                    }
                }

                Section {
                    HStack(spacing: 8) {
                        TextField(L10n.string("toolchain_field_executable"), text: $executable)
                            .font(.system(.body, design: .monospaced))
                        Button(L10n.string("choose_executable")) {
                            chooseExecutable()
                        }
                    }

                    VStack(alignment: .leading, spacing: 4) {
                        TextField(L10n.string("toolchain_field_arguments"), text: $argumentsText)
                            .font(.system(.body, design: .monospaced))
                        Text(L10n.string("toolchain_arguments_hint"))
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                    }
                } header: {
                    Text(L10n.string("toolchain_section_compiler"))
                }

                Section(L10n.string("toolchain_section_limits")) {
                    HStack(spacing: 8) {
                        TextField(L10n.string("toolchain_field_build_timeout"), text: $buildTimeoutText)
                            .monospacedDigit()
                        Text(L10n.string("ms_unit"))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    HStack(spacing: 8) {
                        TextField(L10n.string("toolchain_field_run_timeout"), text: $runTimeoutText)
                            .monospacedDigit()
                        Text(L10n.string("ms_unit"))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
            }
            .formStyle(.grouped)

        case .checker:
            Form {
                Section {
                    HStack(spacing: 8) {
                        TextField(L10n.string("checker_field_executable"), text: $executable)
                            .font(.system(.body, design: .monospaced))
                        Button(L10n.string("choose_executable")) {
                            chooseExecutable()
                        }
                    }

                    VStack(alignment: .leading, spacing: 6) {
                        HStack(spacing: 8) {
                            TextField(L10n.string("checker_field_arguments"), text: $argumentsText)
                                .font(.system(.body, design: .monospaced))
                            Button(L10n.string("checker_restore_default_args")) {
                                argumentsText = "${input} ${actual} ${expected}"
                            }
                            .controlSize(.small)
                        }
                        Text(L10n.string("checker_arguments_hint"))
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                    }

                    HStack(spacing: 8) {
                        TextField(L10n.string("checker_field_timeout"), text: $runTimeoutText)
                            .monospacedDigit()
                        Text(L10n.string("ms_unit"))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                } header: {
                    Text(L10n.string("checker_section_command"))
                }

                Section {
                    TextField(L10n.string("checker_field_accepted_codes"), text: $acceptedCodesText)
                        .font(.system(.body, design: .monospaced))
                    TextField(L10n.string("checker_field_wrong_codes"), text: $wrongCodesText)
                        .font(.system(.body, design: .monospaced))
                    Text(L10n.string("checker_exit_codes_hint"))
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                } header: {
                    Text(L10n.string("checker_section_verdicts"))
                }
            }
            .formStyle(.grouped)
        }
    }

    private func switchMode(to target: EditorMode) {
        guard target != mode else { return }
        errorMessage = nil
        do {
            if target == .json {
                jsonText = try buildJSONFromVisual()
                mode = .json
            } else {
                try syncVisualFromJSON(jsonText)
                mode = .visual
            }
        } catch {
            errorMessage = L10n.describe(error)
        }
    }

    private func syncVisualFromJSON(_ text: String) throws {
        switch kind {
        case .toolchain:
            let profile = try ToolchainProfile(validating: Data(text.utf8))
            guard let buildMS = profile.buildWallTimeoutMS, let runMS = profile.runWallTimeoutMS else {
                throw WireError.invalidProfile
            }
            baseToolchain = profile
            executable = profile.buildExecutable ?? ""
            argumentsText = Self.formatArguments(profile.buildArguments)
            buildTimeoutText = String(buildMS)
            runTimeoutText = String(runMS)
        case .checker:
            let checker = try CheckerConfiguration(jsonText: text)
            let values = checker.formValues()
            baseChecker = checker
            executable = values.executable
            argumentsText = Self.formatArguments(values.arguments)
            runTimeoutText = String(values.wallTimeoutMS)
            acceptedCodesText = values.acceptedExitCodes.map(String.init).joined(separator: ", ")
            wrongCodesText = values.wrongAnswerExitCodes.map(String.init).joined(separator: ", ")
        }
    }

    private func buildJSONFromVisual() throws -> String {
        let parsedArguments = try Self.parseArguments(argumentsText)
        switch kind {
        case .toolchain:
            let base = try baseToolchain ?? JSONDecoder().decode(ToolchainProfile.self, from: Data(jsonText.utf8))
            guard let buildMS = Int(buildTimeoutText.trimmingCharacters(in: .whitespacesAndNewlines)),
                  let runMS = Int(runTimeoutText.trimmingCharacters(in: .whitespacesAndNewlines)),
                  buildMS > 0, runMS > 0 else {
                throw FormValidationError(message: L10n.string("invalid_visual_form_number"))
            }
            let updated = try base.updating(
                buildExecutable: executable,
                buildArguments: parsedArguments,
                buildWallTimeoutMS: buildMS,
                runWallTimeoutMS: runMS
            )
            baseToolchain = updated
            return try updated.jsonText()

        case .checker:
            let base = try baseChecker ?? CheckerConfiguration(jsonText: jsonText)
            guard let wallMS = UInt64(runTimeoutText.trimmingCharacters(in: .whitespacesAndNewlines)), wallMS > 0 else {
                throw FormValidationError(message: L10n.string("invalid_visual_form_number"))
            }
            let accepted = try Self.parseExitCodes(acceptedCodesText)
            let wrong = try Self.parseExitCodes(wrongCodesText)
            let updatedJSON = try base.updatingJSON(
                executable: executable.trimmingCharacters(in: .whitespacesAndNewlines),
                arguments: parsedArguments,
                wallTimeoutMS: wallMS,
                acceptedExitCodes: accepted,
                wrongAnswerExitCodes: wrong
            )
            baseChecker = try CheckerConfiguration(jsonText: updatedJSON)
            return updatedJSON
        }
    }

    private func formatCurrentJSON() {
        errorMessage = nil
        do {
            switch kind {
            case .toolchain:
                let profile = try ToolchainProfile(validating: Data(jsonText.utf8))
                jsonText = try profile.jsonText()
            case .checker:
                let checker = try CheckerConfiguration(jsonText: jsonText)
                if let pretty = try? JSONSerialization.data(
                    withJSONObject: JSONSerialization.jsonObject(with: Data(checker.originalJSON.utf8)),
                    options: [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
                ) {
                    jsonText = String(decoding: pretty, as: UTF8.self)
                }
            }
        } catch {
            errorMessage = L10n.describe(error)
        }
    }

    private func applyDraft() {
        errorMessage = nil
        do {
            let finalText = mode == .visual ? try buildJSONFromVisual() : jsonText
            try apply(finalText)
            dismiss()
        } catch {
            errorMessage = L10n.describe(error)
        }
    }

    private func chooseExecutable() {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        panel.canChooseFiles = true
        if !executable.isEmpty {
            let currentURL = URL(fileURLWithPath: executable)
            if FileManager.default.fileExists(atPath: currentURL.deletingLastPathComponent().path) {
                panel.directoryURL = currentURL.deletingLastPathComponent()
            }
        }
        if panel.runModal() == .OK, let url = panel.url {
            executable = url.path
        }
    }

    private static func formatArguments(_ arguments: [String]) -> String {
        arguments.map { arg in
            if arg.isEmpty || arg.contains(where: \.isWhitespace) || arg.contains("\"") {
                let escaped = arg.replacingOccurrences(of: "\\", with: "\\\\")
                    .replacingOccurrences(of: "\"", with: "\\\"")
                return "\"\(escaped)\""
            }
            return arg
        }.joined(separator: " ")
    }

    private static func parseArguments(_ text: String) throws -> [String] {
        var result: [String] = []
        var current = ""
        var inToken = false
        var inQuotes = false
        var escaping = false

        for ch in text {
            if escaping {
                current.append(ch)
                escaping = false
                inToken = true
                continue
            }
            if ch == "\\" && inQuotes {
                escaping = true
                inToken = true
                continue
            }
            if ch == "\"" {
                inQuotes.toggle()
                inToken = true
                continue
            }
            if ch.isWhitespace && !inQuotes {
                if inToken {
                    result.append(current)
                    current.removeAll(keepingCapacity: true)
                    inToken = false
                }
                continue
            }
            current.append(ch)
            inToken = true
        }
        if escaping || inQuotes {
            throw FormValidationError(message: L10n.string("invalid_visual_form_number"))
        }
        if inToken {
            result.append(current)
        }
        return result
    }

    private static func parseExitCodes(_ text: String) throws -> [UInt64] {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty { return [] }
        let parts = trimmed.split(separator: ",", omittingEmptySubsequences: false)
        var codes: [UInt64] = []
        for part in parts {
            let token = part.trimmingCharacters(in: .whitespacesAndNewlines)
            guard let code = UInt64(token), code <= 255 else {
                throw FormValidationError(message: L10n.string("invalid_visual_form_number"))
            }
            codes.append(code)
        }
        return codes
    }
}

private struct FormValidationError: LocalizedError {
    let message: String
    var errorDescription: String? { message }
}
