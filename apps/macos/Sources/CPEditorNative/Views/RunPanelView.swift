import SwiftUI
import NativeCoreBridge

struct RunPanelView: View {
    @Bindable var session: NativeEditorSession
    @Bindable var runner: RunController
    @State private var editingProfile = false
    @State private var profileJSON = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(L10n.string("tests_title")).font(.headline)
                Picker(L10n.string("toolchain"), selection: $runner.selectedProfileID) {
                    ForEach(runner.profiles) { profile in Text(profile.id).tag(profile.id) }
                }.frame(maxWidth: 290).disabled(runner.isRunning)
                Menu {
                    Button(L10n.string("load_toolchain")) { runner.loadProfile() }
                    Button(L10n.string("edit_toolchain")) {
                        do { profileJSON = try runner.selectedProfile?.jsonText() ?? ""; editingProfile = true }
                        catch { runner.errorMessage = L10n.describe(error) }
                    }
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
                        }.labelsHidden().frame(maxWidth: 170)
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
        .sheet(isPresented: $editingProfile) {
            VStack(alignment: .leading) {
                Text(L10n.string("toolchain_json")).font(.headline)
                TextEditor(text: $profileJSON).font(.system(.body, design: .monospaced))
                HStack {
                    Spacer()
                    Button(L10n.string("cancel"), role: .cancel) { editingProfile = false }
                    Button(L10n.string("apply")) {
                        do { try runner.applyProfile(profileJSON); editingProfile = false }
                        catch { runner.errorMessage = L10n.describe(error) }
                    }.keyboardShortcut(.defaultAction)
                }
            }.padding().frame(width: 680, height: 480)
        }
        .alert(L10n.string("error_title"), isPresented: Binding(
            get: { runner.errorMessage != nil }, set: { if !$0 { runner.errorMessage = nil } })) {
                Button(L10n.string("ok"), role: .cancel) { runner.errorMessage = nil }
            } message: { Text(runner.errorMessage ?? "") }
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
                        DisclosureGroup(L10n.string("compiler_output")) { ProcessOutputView(report: build) }
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
                            ProcessOutputView(report: test.process)
                        }.padding(6).overlay(RoundedRectangle(cornerRadius: 4).stroke(.separator))
                    }
                } else { Text(L10n.jobState(runner.state)).foregroundStyle(.secondary) }
            }.frame(maxWidth: .infinity, alignment: .leading)
        }.padding(.leading, 8)
    }
}

struct ProcessOutputView: View {
    let report: ProcessReport
    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            if !report.stdout.data.isEmpty {
                Text(L10n.string("output_title")).font(.caption.bold())
                Text(report.stdout.text).font(.system(.caption, design: .monospaced)).textSelection(.enabled)
            }
            if !report.stderr.data.isEmpty {
                Text(L10n.string("stderr_title")).font(.caption.bold())
                Text(report.stderr.text).font(.system(.caption, design: .monospaced)).textSelection(.enabled)
            }
            if report.stdoutTruncated || report.stderrTruncated {
                Text(L10n.string("output_truncated")).font(.caption).foregroundStyle(.orange)
            }
        }.frame(maxWidth: .infinity, alignment: .leading)
    }
}
