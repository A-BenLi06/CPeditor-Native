import SwiftUI
import NativeCoreBridge

struct RunPanelView: View {
    @Bindable var session: NativeEditorSession
    @Bindable var runner: RunController
    @State private var configurationDraft: ConfigurationJSONDraft?

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(L10n.string("tests_title")).font(.headline)
                Picker(L10n.string("toolchain"), selection: $runner.selectedProfileID) {
                    ForEach(runner.profiles) { profile in Text(profile.id.rawValue).tag(profile.id) }
                }.frame(maxWidth: 290).disabled(runner.isRunning)
                Menu {
                    Button(L10n.string("load_toolchain")) { runner.loadProfile() }
                    Button(L10n.string("edit_toolchain"), action: beginProfileEdit)
                } label: { Image(systemName: "gearshape") }
                    .help(L10n.string("toolchain")).disabled(runner.isRunning)
                Spacer()
                Text(L10n.jobState(runner.state)).foregroundStyle(.secondary)
                if runner.isRunning {
                    ProgressView().controlSize(.small)
                    Button(L10n.string("cancel_run")) { runner.cancel() }
                } else {
                    Button { runner.runAll(session) } label: {
                        Label(L10n.string("run_all"), systemImage: "play.fill")
                    }.disabled(runner.selectedProfile == nil || runner.isClosed)
                }
            }
            HStack {
                Toggle(L10n.string("external_checker"), isOn: Binding(
                    get: { runner.externalCheckerEnabled },
                    set: { enabled in
                        if enabled, runner.externalChecker == nil { beginCheckerEdit() }
                        else { runner.externalCheckerEnabled = enabled }
                    }))
                    .disabled(runner.checkerPreferencesAreOpaque || !runner.checkerPreferenceStorageAvailable || runner.isRunning)
                Button(L10n.string("edit_checker")) { beginCheckerEdit() }
                    .disabled(runner.checkerPreferencesAreOpaque || !runner.checkerPreferenceStorageAvailable || runner.isRunning)
                if let checker = runner.externalChecker { Text(checker.executable).font(.caption).lineLimit(1).textSelection(.enabled) }
                Spacer()
                if runner.externalCheckerEnabled { Text(L10n.string("checker_expected_required")).font(.caption).foregroundStyle(.secondary) }
            }
            HSplitView {
                VStack(alignment: .leading, spacing: 6) {
                    HStack {
                        Button(L10n.string("add_test")) { session.testCases.append(TestCaseInput()) }
                        Button(L10n.string("import_tests")) { runner.importTests(into: session) }
                        Spacer()
                        Picker(L10n.string("comparison"), selection: $runner.comparison) {
                            Text(L10n.string("comparison_trailing")).tag(ComparisonPolicy.trailing)
                            Text(L10n.string("comparison_strict")).tag(ComparisonPolicy.strict)
                            Text(L10n.string("comparison_byte")).tag(ComparisonPolicy.byte)
                        }.labelsHidden().frame(maxWidth: 170).disabled(runner.externalCheckerEnabled)
                    }.disabled(runner.isRunning)
                    ScrollView {
                        LazyVStack(alignment: .leading, spacing: 10) {
                            ForEach($session.testCases) { $test in
                                TestCaseEditorView(test: $test,
                                    number: (session.testCases.firstIndex(where: { $0.id == test.id }) ?? 0) + 1,
                                    remove: { session.testCases.removeAll { $0.id == test.id } })
                            }
                        }
                    }.disabled(runner.isRunning)
                }.frame(minWidth: 280, idealWidth: 480)
                RunResultsView(runner: runner, session: session).frame(minWidth: 220, idealWidth: 360)
            }
        }
        .padding(10).frame(minHeight: 230, idealHeight: 320)
        .sheet(item: $configurationDraft) { draft in
            ConfigurationJSONSheet(draft: draft) { jsonText in
                switch draft.kind {
                case .toolchain: try runner.applyProfile(jsonText)
                case .checker: try runner.applyChecker(jsonText)
                }
            }
        }
        .alert(L10n.string("error_title"), isPresented: Binding(
            get: { runner.errorMessage != nil }, set: { if !$0 { runner.errorMessage = nil } })) {
                Button(L10n.string("ok"), role: .cancel) { runner.errorMessage = nil }
            } message: { Text(runner.errorMessage ?? "") }
    }

    private func beginCheckerEdit() {
        do { configurationDraft = ConfigurationJSONDraft(kind: .checker, jsonText: try runner.checkerDraft()) }
        catch { runner.errorMessage = L10n.describe(error) }
    }

    private func beginProfileEdit() {
        do {
            guard let profile = runner.selectedProfile else { throw WireError.invalidProfile }
            configurationDraft = ConfigurationJSONDraft(kind: .toolchain, jsonText: try profile.jsonText())
        } catch { runner.errorMessage = L10n.describe(error) }
    }
}

struct TestCaseEditorView: View {
    @Binding var test: TestCaseInput
    let number: Int
    let remove: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            HStack {
                Text(String(format: L10n.string("test_number"), number)).font(.subheadline.bold())
                Toggle(L10n.string("test_enabled"), isOn: $test.enabled)
                Spacer()
                Button(action: remove) { Image(systemName: "trash") }.help(L10n.string("remove_test"))
            }
            Text(L10n.string("test_input")).font(.caption)
            TextEditor(text: Binding(get: { test.input.text }, set: { test.input = WireBytes($0) }))
                .font(.system(.body, design: .monospaced)).frame(minHeight: 60)
                .border(.quaternary)
                .accessibilityLabel(L10n.string("test_input"))
            if !test.input.isUTF8 { Text(L10n.string("binary_data")).font(.caption).foregroundStyle(.secondary) }
            Toggle(L10n.string("check_expected"), isOn: Binding(
                get: { test.expected != nil }, set: { test.expected = $0 ? WireBytes("") : nil }))
            if test.expected != nil {
                Text(L10n.string("test_expected")).font(.caption)
                TextEditor(text: Binding(get: { test.expected?.text ?? "" }, set: { test.expected = WireBytes($0) }))
                    .font(.system(.body, design: .monospaced)).frame(minHeight: 60)
                    .border(.quaternary)
                    .accessibilityLabel(L10n.string("test_expected"))
                if test.expected?.isUTF8 == false { Text(L10n.string("binary_data")).font(.caption).foregroundStyle(.secondary) }
            }
        }.padding(8).background(.background).overlay(RoundedRectangle(cornerRadius: 5).stroke(.separator))
    }
}

struct RunResultsView: View {
    let runner: RunController
    let session: NativeEditorSession

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 8) {
                Text(L10n.string("last_run")).font(.headline)
                if let result = runner.result {
                    Text(result.matches(session.document.snapshot()) ? L10n.string("source_current") : L10n.string("source_older"))
                        .font(.caption).foregroundStyle(result.matches(session.document.snapshot()) ? Color.secondary : Color.orange)
                    Text(String(format: L10n.string("run_revision"), result.revision)).font(.caption)
                    Text(result.profileID).font(.caption).foregroundStyle(.secondary)
                    if !result.error.isEmpty { Text(result.error).textSelection(.enabled) }
                    if let build = result.build, !build.stderr.data.isEmpty || !build.stdout.data.isEmpty {
                        DisclosureGroup(L10n.string("compiler_output")) { ProcessOutputView(report: build, binaryPreviewBytes: runner.binaryPreviewBytes) }
                    }
                    ForEach(result.tests) { test in
                        VStack(alignment: .leading, spacing: 4) {
                            HStack {
                                Text(test.verdict).font(.headline)
                                Spacer()
                                Text(String(format: L10n.string("elapsed_format"), test.process.elapsedMS))
                                    .font(.caption).foregroundStyle(.secondary)
                            }
                            Text(test.testcaseID).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                            if !test.detail.isEmpty { Text(test.detail).font(.caption).textSelection(.enabled) }
                            Text(String(format: L10n.string("solution_status_format"), test.process.termination)).font(.caption).foregroundStyle(.secondary)
                            ProcessOutputView(report: test.process, binaryPreviewBytes: runner.binaryPreviewBytes)
                            if let checker = test.checker { CheckerDiagnosticsView(report: checker, binaryPreviewBytes: runner.binaryPreviewBytes) }
                        }.padding(6).overlay(RoundedRectangle(cornerRadius: 4).stroke(.separator))
                    }
                } else { Text(L10n.jobState(runner.state)).foregroundStyle(.secondary) }
            }.frame(maxWidth: .infinity, alignment: .leading)
        }.padding(.leading, 8)
    }
}

struct CheckerDiagnosticsView: View {
    let report: CheckerReport
    let binaryPreviewBytes: Int
    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(L10n.string("checker_diagnostics")).font(.caption.bold())
            Text(String(format: L10n.string("checker_verdict_format"), L10n.checkerVerdict(report.verdict))).font(.caption)
            Text(report.processAttempted ? L10n.string("checker_process_attempted") : L10n.string("checker_process_not_attempted"))
                .font(.caption).foregroundStyle(.secondary)
            if !report.detail.isEmpty { Text(report.detail).font(.caption).textSelection(.enabled) }
            if report.processAttempted {
                Text(String(format: L10n.string("checker_status_format"), report.process.termination)).font(.caption)
                if let exit = report.process.exitCode { Text(String(format: L10n.string("checker_exit_format"), exit)).font(.caption) }
                if let signal = report.process.signal { Text(String(format: L10n.string("checker_signal_format"), signal)).font(.caption) }
                if report.process.nativeError != 0 { Text(String(format: L10n.string("checker_native_error_format"), report.process.nativeError)).font(.caption) }
                Text(String(format: L10n.string("elapsed_format"), report.process.elapsedMS)).font(.caption).foregroundStyle(.secondary)
                Text(String(format: L10n.string("checker_bytes_format"), report.process.stdoutTotalBytes, report.process.stderrTotalBytes)).font(.caption)
                if !report.process.detail.isEmpty { Text(report.process.detail).font(.caption).textSelection(.enabled) }
                ProcessOutputView(report: report.process, binaryPreviewBytes: binaryPreviewBytes)
            }
        }.padding(6).background(.quaternary)
    }
}

struct ProcessOutputView: View {
    let report: ProcessReport
    let binaryPreviewBytes: Int
    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            if !report.stdout.data.isEmpty {
                Text(L10n.string("output_title")).font(.caption.bold())
                OutputBytesView(bytes: report.stdout, binaryPreviewBytes: binaryPreviewBytes)
            }
            if !report.stderr.data.isEmpty {
                Text(L10n.string("stderr_title")).font(.caption.bold())
                OutputBytesView(bytes: report.stderr, binaryPreviewBytes: binaryPreviewBytes)
            }
            if report.stdoutTruncated || report.stderrTruncated {
                Text(L10n.string("output_truncated")).font(.caption).foregroundStyle(.orange)
            }
        }.frame(maxWidth: .infinity, alignment: .leading)
    }
}

struct OutputBytesView: View {
    let bytes: WireBytes
    let binaryPreviewBytes: Int
    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(bytes.canDisplayAsText ? bytes.text : bytes.hexPreview(maximumBytes: binaryPreviewBytes))
                .font(.system(.caption, design: .monospaced)).textSelection(.enabled)
            if !bytes.canDisplayAsText {
                Text(String(format: L10n.string("output_binary_preview"), min(bytes.data.count, binaryPreviewBytes), UInt64(bytes.data.count)))
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
    }
}
