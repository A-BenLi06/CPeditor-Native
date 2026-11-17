import SwiftUI
import NativeCoreBridge
import NativeEditorModel

struct RunPanelView: View {
    @Bindable var session: NativeEditorSession
    @Bindable var runner: RunController

    private var enabledTestCount: Int {
        session.testCases.reduce(0) { $0 + ($1.enabled ? 1 : 0) }
    }

    private var canEditTests: Bool {
        !runner.isRunning && session.canMutatePersistence
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            RunExecutionToolbar(session: session, runner: runner)

            if let notice = session.persistenceNotice {
                NoticeBannerView(kind: .warning, message: notice)
            }
            if let notice = session.companionRunNotice {
                NoticeBannerView(kind: .warning, message: notice)
            }

            HSplitView {
                TestCasesPane(
                    session: session,
                    runner: runner,
                    enabledTestCount: enabledTestCount,
                    canEditTests: canEditTests
                )
                .frame(minWidth: 260, idealWidth: 490, maxHeight: .infinity)

                RunResultsView(runner: runner, session: session)
                    .frame(minWidth: 220, idealWidth: 350, maxHeight: .infinity)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .frame(minHeight: 215, idealHeight: 305)
        .background(.background)
        .alert(L10n.string("error_title"), isPresented: Binding(
            get: { runner.errorMessage != nil },
            set: { if !$0 { runner.errorMessage = nil } }
        )) {
            Button(L10n.string("ok"), role: .cancel) { runner.errorMessage = nil }
        } message: {
            Text(runner.errorMessage ?? "")
        }
    }
}

private struct RunExecutionToolbar: View {
    @Bindable var session: NativeEditorSession
    @Bindable var runner: RunController

    private var checkerControlsDisabled: Bool {
        runner.checkerPreferencesAreOpaque || !runner.checkerPreferenceStorageAvailable || runner.isRunning || !runner.persistentChangesAllowed
    }

    private var checkerExecutableLabel: String? {
        guard let executable = runner.externalChecker?.executable, !executable.isEmpty else { return nil }
        return URL(fileURLWithPath: executable).lastPathComponent
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 10) {
                HStack(spacing: 6) {
                    Image(systemName: "hammer")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Picker(L10n.string("toolchain"), selection: $runner.selectedProfileID) {
                        ForEach(runner.profiles) { profile in
                            Text(profile.id.rawValue).tag(profile.id)
                        }
                    }
                    .labelsHidden()
                    .controlSize(.small)
                    .frame(minWidth: 130, maxWidth: 210)
                    .disabled(runner.isRunning || !runner.persistentChangesAllowed)
                    .help(L10n.string("toolchain"))

                    Menu {
                        Button(L10n.string("load_toolchain")) { runner.loadProfile() }
                        Button(L10n.string("edit_toolchain")) { runner.beginProfileEdit() }
                    } label: {
                        Image(systemName: "gearshape")
                    }
                    .menuStyle(.borderlessButton)
                    .fixedSize()
                    .help(L10n.string("toolchain"))
                    .disabled(runner.isRunning || !runner.persistentChangesAllowed)
                }

                Divider().frame(height: 15)

                HStack(spacing: 8) {
                    Toggle(L10n.string("external_checker"), isOn: Binding(
                        get: { runner.externalCheckerEnabled },
                        set: { enabled in
                            if enabled, runner.externalChecker == nil { runner.beginCheckerEdit() }
                            else { runner.externalCheckerEnabled = enabled }
                        }
                    ))
                    .toggleStyle(.checkbox)
                    .controlSize(.small)
                    .fixedSize()
                    .disabled(checkerControlsDisabled)
                    .help(L10n.string("checker_expected_required"))

                    Button(L10n.string("edit_checker")) {
                        runner.beginCheckerEdit()
                    }
                    .controlSize(.small)
                    .fixedSize()
                    .disabled(checkerControlsDisabled)

                    if let executableName = checkerExecutableLabel, let fullPath = runner.externalChecker?.executable {
                        Text(executableName)
                            .font(.system(.caption2, design: .monospaced))
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                            .padding(.horizontal, 6)
                            .padding(.vertical, 2)
                            .background(.quaternary.opacity(0.6), in: RoundedRectangle(cornerRadius: 4))
                            .help(fullPath)
                    }
                }

                Spacer(minLength: 8)

                RunStateBadge(state: runner.state(for: session.document.id))

                if runner.isRunning {
                    Button {
                        runner.cancel()
                    } label: {
                        Label(L10n.string("cancel_run"), systemImage: "stop.fill")
                    }
                    .controlSize(.small)
                    .help(L10n.string("help_cancel_run"))
                } else {
                    Button {
                        runner.runAll(session)
                    } label: {
                        Label(L10n.string("run_all"), systemImage: "play.fill")
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.small)
                    .disabled(!runner.canRun(session))
                    .help(L10n.string("help_run_all"))
                }
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 7)
            .background(.bar)

            Divider()
        }
    }
}

private struct TestCasesPane: View {
    @Bindable var session: NativeEditorSession
    @Bindable var runner: RunController
    let enabledTestCount: Int
    let canEditTests: Bool

    private var latestResult: BatchResult? {
        runner.result(for: session.document.id)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 8) {
                Text(L10n.string("tests_title"))
                    .font(.subheadline.weight(.semibold))
                    .fixedSize()
                Text("\(enabledTestCount)/\(session.testCases.count)")
                    .font(.caption2.monospacedDigit())
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .fixedSize()
                    .padding(.horizontal, 6)
                    .padding(.vertical, 2)
                    .background(.quaternary.opacity(0.5), in: Capsule())
                    .help(String(format: L10n.string("tests_count_format"), enabledTestCount, session.testCases.count))

                Spacer(minLength: 6)

                Picker(L10n.string("comparison"), selection: $runner.comparison) {
                    Text(L10n.string("comparison_trailing")).tag(ComparisonPolicy.trailing)
                    Text(L10n.string("comparison_strict")).tag(ComparisonPolicy.strict)
                    Text(L10n.string("comparison_byte")).tag(ComparisonPolicy.byte)
                }
                .labelsHidden()
                .controlSize(.small)
                .fixedSize()
                .disabled(runner.externalCheckerEnabled || runner.isRunning || !runner.persistentChangesAllowed)
                .help(L10n.string("comparison"))

                Button {
                    session.testCases.append(TestCaseInput())
                } label: {
                    Label(L10n.string("add_test"), systemImage: "plus")
                }
                .controlSize(.small)
                .fixedSize()
                .disabled(!canEditTests)

                Button(L10n.string("import_tests")) {
                    runner.importTests(into: session)
                }
                .controlSize(.small)
                .fixedSize()
                .disabled(!canEditTests)
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 6)

            Divider()

            if session.testCases.isEmpty {
                VStack(spacing: 10) {
                    Image(systemName: "list.bullet.rectangle")
                        .font(.title2)
                        .foregroundStyle(.tertiary)
                    Text(L10n.string("no_tests_title"))
                        .font(.subheadline.weight(.medium))
                    Text(L10n.string("no_tests_description"))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                    HStack(spacing: 8) {
                        Button {
                            session.testCases.append(TestCaseInput())
                        } label: {
                            Label(L10n.string("add_test"), systemImage: "plus")
                        }
                        .controlSize(.small)
                        Button(L10n.string("import_tests")) {
                            runner.importTests(into: session)
                        }
                        .controlSize(.small)
                    }
                    .disabled(!canEditTests)
                }
                .padding(16)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                let currentSnapshot = session.document.snapshot()
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 10) {
                        ForEach($session.testCases) { $test in
                            let index = session.testCases.firstIndex(where: { $0.id == test.id }) ?? 0
                            let matchingTestResult = latestResult?.tests.first(where: { $0.testcaseID == test.id.rawValue })
                            let isStaleResult = latestResult.map { !$0.matches(currentSnapshot) } ?? false
                            TestCaseEditorView(
                                test: $test,
                                number: index + 1,
                                canEdit: canEditTests,
                                matchingResult: matchingTestResult,
                                resultIsStale: isStaleResult,
                                duplicate: {
                                    var copy = TestCaseInput(enabled: test.enabled)
                                    copy.input = test.input
                                    copy.expected = test.expected
                                    let insertion = min(index + 1, session.testCases.count)
                                    session.testCases.insert(copy, at: insertion)
                                },
                                remove: {
                                    session.testCases.removeAll { $0.id == test.id }
                                }
                            )
                        }
                    }
                    .padding(10)
                }
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }
}

struct TestCaseEditorView: View {
    @Binding var test: TestCaseInput
    let number: Int
    var canEdit = true
    var matchingResult: TestResult? = nil
    var resultIsStale = false
    var duplicate: (() -> Void)? = nil
    let remove: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                Toggle(isOn: $test.enabled) {
                    Text(String(format: L10n.string("test_number"), number))
                        .font(.subheadline.weight(.semibold))
                }
                .toggleStyle(.checkbox)
                .disabled(!canEdit)

                if let matchingResult {
                    VerdictBadge(verdict: matchingResult.verdict, compact: true, dimmed: resultIsStale)
                    Text(String(format: L10n.string("elapsed_format"), matchingResult.process.elapsedMS))
                        .font(.caption2.monospacedDigit())
                        .foregroundStyle(.secondary)
                }

                Spacer(minLength: 6)

                Toggle(L10n.string("check_expected"), isOn: Binding(
                    get: { test.expected != nil },
                    set: { test.expected = $0 ? WireBytes("") : nil }
                ))
                .toggleStyle(.checkbox)
                .controlSize(.small)
                .foregroundStyle(.secondary)
                .disabled(!canEdit)

                if let duplicate {
                    Button(action: duplicate) {
                        Image(systemName: "plus.square.on.square")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    .buttonStyle(.borderless)
                    .disabled(!canEdit)
                    .help(L10n.string("duplicate_test"))
                }

                Button(action: remove) {
                    Image(systemName: "trash")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .buttonStyle(.borderless)
                .disabled(!canEdit)
                .help(L10n.string("remove_test"))
            }

            ViewThatFits(in: .horizontal) {
                HStack(alignment: .top, spacing: 10) {
                    inputColumn.frame(minWidth: 165, maxWidth: .infinity)
                    expectedColumn.frame(minWidth: 165, maxWidth: .infinity)
                }
                VStack(alignment: .leading, spacing: 8) {
                    inputColumn
                    expectedColumn
                }
            }
            .opacity(test.enabled ? 1.0 : 0.68)
        }
        .padding(10)
        .background(Color(nsColor: .controlBackgroundColor).opacity(0.55), in: RoundedRectangle(cornerRadius: 8))
        .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(.separator, lineWidth: 1))
    }

    private var inputColumn: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 6) {
                Text(L10n.string("test_input"))
                    .font(.caption.weight(.medium))
                    .foregroundStyle(.secondary)
                Text(String(format: L10n.string("bytes_count_format"), UInt64(test.input.data.count)))
                    .font(.caption2.monospacedDigit())
                    .foregroundStyle(.tertiary)
                Spacer(minLength: 4)
                CopyTextButton(text: test.input.text)
            }
            TextEditor(text: Binding(
                get: { test.input.text },
                set: { test.input = WireBytes($0) }
            ))
            .font(.system(.callout, design: .monospaced))
            .scrollContentBackground(.hidden)
            .padding(5)
            .frame(minHeight: 68, idealHeight: 82, maxHeight: 140)
            .background(Color(nsColor: .textBackgroundColor), in: RoundedRectangle(cornerRadius: 6))
            .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(.quaternary, lineWidth: 1))
            .disabled(!canEdit)
            .accessibilityLabel(L10n.string("test_input"))

            if !test.input.isUTF8 {
                Text(L10n.string("binary_data"))
                    .font(.caption2)
                    .foregroundStyle(.orange)
            }
        }
    }

    @ViewBuilder
    private var expectedColumn: some View {
        if let expected = test.expected {
            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: 6) {
                    Text(L10n.string("test_expected"))
                        .font(.caption.weight(.medium))
                        .foregroundStyle(.secondary)
                    Text(String(format: L10n.string("bytes_count_format"), UInt64(expected.data.count)))
                        .font(.caption2.monospacedDigit())
                        .foregroundStyle(.tertiary)
                    Spacer(minLength: 4)
                    CopyTextButton(text: expected.text)
                }
                TextEditor(text: Binding(
                    get: { test.expected?.text ?? "" },
                    set: { test.expected = WireBytes($0) }
                ))
                .font(.system(.callout, design: .monospaced))
                .scrollContentBackground(.hidden)
                .padding(5)
                .frame(minHeight: 68, idealHeight: 82, maxHeight: 140)
                .background(Color(nsColor: .textBackgroundColor), in: RoundedRectangle(cornerRadius: 6))
                .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(.quaternary, lineWidth: 1))
                .disabled(!canEdit)
                .accessibilityLabel(L10n.string("test_expected"))

                if !expected.isUTF8 {
                    Text(L10n.string("binary_data"))
                        .font(.caption2)
                        .foregroundStyle(.orange)
                }
            }
        } else {
            VStack(alignment: .leading, spacing: 6) {
                Text(L10n.string("test_expected"))
                    .font(.caption.weight(.medium))
                    .foregroundStyle(.secondary)
                VStack(spacing: 6) {
                    Text(L10n.string("expected_unchecked_hint"))
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                    Button(L10n.string("enable_expected")) {
                        test.expected = WireBytes("")
                    }
                    .controlSize(.mini)
                    .disabled(!canEdit)
                }
                .padding(8)
                .frame(maxWidth: .infinity, minHeight: 78)
                .background(.quaternary.opacity(0.25), in: RoundedRectangle(cornerRadius: 6))
                .overlay(
                    RoundedRectangle(cornerRadius: 6)
                        .strokeBorder(style: StrokeStyle(lineWidth: 1, dash: [4, 3]))
                        .foregroundStyle(.quaternary)
                )
            }
        }
    }
}

struct RunResultsView: View {
    let runner: RunController
    let session: NativeEditorSession

    private var currentResult: BatchResult? {
        runner.result(for: session.document.id)
    }

    private var currentState: JobState? {
        runner.state(for: session.document.id)
    }

    private func displayName(for testcaseID: String) -> (primary: String, secondary: String?) {
        if let index = session.testCases.firstIndex(where: { $0.id.rawValue == testcaseID }) {
            let primary = String(format: L10n.string("test_number"), index + 1)
            let secondary = testcaseID.count > 16 ? String(testcaseID.prefix(8)) + "…" : testcaseID
            return (primary, secondary)
        }
        if testcaseID.count > 24 {
            return (String(testcaseID.prefix(12)) + "…", testcaseID)
        }
        return (testcaseID, nil)
    }

    var body: some View {
        let snapshot = session.document.snapshot()
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 8) {
                Text(L10n.string("last_run"))
                    .font(.subheadline.weight(.semibold))
                Spacer(minLength: 6)
                if let result = currentResult {
                    let isCurrent = result.matches(snapshot)
                    HStack(spacing: 4) {
                        Image(systemName: isCurrent ? "checkmark.circle.fill" : "exclamationmark.arrow.triangle.2.circlepath")
                            .font(.caption2)
                        Text(isCurrent ? L10n.string("source_current_short") : L10n.string("source_older_short"))
                            .font(.caption2.weight(.medium))
                    }
                    .foregroundStyle(isCurrent ? Color.green : Color.orange)
                    .padding(.horizontal, 6)
                    .padding(.vertical, 2)
                    .background((isCurrent ? Color.green : Color.orange).opacity(0.12), in: Capsule())
                    .help(isCurrent ? L10n.string("source_current") : L10n.string("source_older"))
                }
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 6)

            Divider()

            if let result = currentResult {
                ScrollView {
                    VStack(alignment: .leading, spacing: 10) {
                        RunSummaryBanner(result: result, isCurrentSource: result.matches(snapshot))

                        if let build = result.build,
                           !build.stderr.data.isEmpty || !build.stdout.data.isEmpty || result.buildOutcome != "Succeeded" {
                            CompilerOutputCard(
                                build: build,
                                buildSucceeded: result.buildOutcome == "Succeeded",
                                binaryPreviewBytes: runner.binaryPreviewBytes
                            )
                        }

                        ForEach(result.tests) { test in
                            let label = displayName(for: test.testcaseID)
                            TestResultCard(
                                test: test,
                                primaryLabel: label.primary,
                                secondaryID: label.secondary,
                                binaryPreviewBytes: runner.binaryPreviewBytes
                            )
                        }
                    }
                    .padding(10)
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
            } else if let state = currentState, !state.isTerminal {
                VStack(spacing: 10) {
                    ProgressView()
                    Text(L10n.jobState(state))
                        .font(.subheadline.weight(.semibold))
                    Text(L10n.string("results_running_description"))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                }
                .padding(20)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                VStack(spacing: 10) {
                    Image(systemName: "play.rectangle")
                        .font(.title2)
                        .foregroundStyle(.tertiary)
                    Text(L10n.string("results_empty_title"))
                        .font(.subheadline.weight(.medium))
                    Text(L10n.string("results_empty_description"))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                    if runner.canRun(session) {
                        Button {
                            runner.runAll(session)
                        } label: {
                            Label(L10n.string("run_all"), systemImage: "play.fill")
                        }
                        .controlSize(.small)
                    }
                }
                .padding(20)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }
}

private struct RunSummaryBanner: View {
    let result: BatchResult
    let isCurrentSource: Bool

    private var passedCount: Int {
        result.tests.reduce(0) { $0 + ($1.verdict == "AC" ? 1 : 0) }
    }

    private var maxElapsedMS: UInt64? {
        result.tests.map(\.process.elapsedMS).max()
    }

    private var headlineText: String {
        if result.buildOutcome == "Cancelled" {
            return L10n.string("summary_build_cancelled")
        }
        if result.buildOutcome != "Succeeded" {
            return L10n.string("summary_build_failed")
        }
        guard !result.tests.isEmpty else {
            return L10n.jobState(.completed)
        }
        if passedCount == result.tests.count {
            return String(format: L10n.string("summary_all_passed_format"), passedCount, result.tests.count)
        }
        return String(format: L10n.string("summary_passed_count_format"), passedCount, result.tests.count)
    }

    var body: some View {
        let summaryDescriptor = VerdictDescriptor.summary(for: result)
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                VerdictBadge(descriptor: summaryDescriptor)
                Text(headlineText)
                    .font(.subheadline.weight(.semibold))
                Spacer(minLength: 4)
                if let maxElapsedMS {
                    Text(String(format: L10n.string("summary_max_time_format"), maxElapsedMS))
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.secondary)
                }
            }

            HStack(spacing: 8) {
                Text(String(format: L10n.string("run_revision"), result.revision))
                    .monospacedDigit()
                Text("·")
                Text(result.profileID)
                if !isCurrentSource {
                    Text("·")
                    Text(L10n.string("source_older"))
                        .foregroundStyle(.orange)
                }
            }
            .font(.caption2)
            .foregroundStyle(.secondary)

            if !result.error.isEmpty {
                Text(result.error)
                    .font(.caption)
                    .foregroundStyle(.red)
                    .textSelection(.enabled)
            }
        }
        .padding(10)
        .background(summaryDescriptor.tone.color.opacity(0.08), in: RoundedRectangle(cornerRadius: 8))
        .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(summaryDescriptor.tone.color.opacity(0.25), lineWidth: 1))
    }
}

private struct CompilerOutputCard: View {
    let build: ProcessReport
    let buildSucceeded: Bool
    let binaryPreviewBytes: Int
    @State private var isExpanded: Bool

    init(build: ProcessReport, buildSucceeded: Bool, binaryPreviewBytes: Int) {
        self.build = build
        self.buildSucceeded = buildSucceeded
        self.binaryPreviewBytes = binaryPreviewBytes
        _isExpanded = State(initialValue: !buildSucceeded)
    }

    var body: some View {
        DisclosureGroup(isExpanded: $isExpanded) {
            ProcessOutputView(report: build, binaryPreviewBytes: binaryPreviewBytes)
                .padding(.top, 6)
        } label: {
            HStack(spacing: 6) {
                Image(systemName: buildSucceeded ? "terminal" : "exclamationmark.triangle.fill")
                    .foregroundStyle(buildSucceeded ? Color.secondary : Color.orange)
                Text(L10n.string("compiler_output"))
                    .font(.caption.weight(.semibold))
                Spacer(minLength: 4)
                Text(String(format: L10n.string("elapsed_format"), build.elapsedMS))
                    .font(.caption2.monospacedDigit())
                    .foregroundStyle(.secondary)
            }
        }
        .padding(9)
        .background(Color(nsColor: .controlBackgroundColor).opacity(0.6), in: RoundedRectangle(cornerRadius: 8))
        .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(.separator, lineWidth: 1))
    }
}

private struct TestResultCard: View {
    let test: TestResult
    let primaryLabel: String
    let secondaryID: String?
    let binaryPreviewBytes: Int

    private var terminationSummary: String {
        var parts = [String(format: L10n.string("solution_status_format"), test.process.termination)]
        if let exit = test.process.exitCode, exit != 0 {
            parts.append(String(format: L10n.string("solution_exit_code_format"), exit))
        }
        if let signal = test.process.signal {
            parts.append(String(format: L10n.string("solution_signal_format"), signal))
        }
        return parts.joined(separator: " · ")
    }

    var body: some View {
        let descriptor = VerdictDescriptor.forVerdict(test.verdict)
        VStack(alignment: .leading, spacing: 7) {
            HStack(spacing: 8) {
                VerdictBadge(descriptor: descriptor)
                Text(primaryLabel)
                    .font(.subheadline.weight(.semibold))
                if let secondaryID {
                    Text(secondaryID)
                        .font(.system(.caption2, design: .monospaced))
                        .foregroundStyle(.tertiary)
                        .lineLimit(1)
                        .help(test.testcaseID)
                }
                Spacer(minLength: 4)
                Text(String(format: L10n.string("elapsed_format"), test.process.elapsedMS))
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.secondary)
            }

            Text(terminationSummary)
                .font(.caption2)
                .foregroundStyle(.secondary)

            if !test.detail.isEmpty {
                Text(test.detail)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .textSelection(.enabled)
            }

            ProcessOutputView(report: test.process, binaryPreviewBytes: binaryPreviewBytes)

            if let checker = test.checker {
                CheckerDiagnosticsView(report: checker, binaryPreviewBytes: binaryPreviewBytes)
            }
        }
        .padding(10)
        .background(Color(nsColor: .controlBackgroundColor).opacity(0.55), in: RoundedRectangle(cornerRadius: 8))
        .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(descriptor.tone.color.opacity(0.28), lineWidth: 1))
    }
}

struct CheckerDiagnosticsView: View {
    let report: CheckerReport
    let binaryPreviewBytes: Int

    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            HStack(spacing: 6) {
                Image(systemName: "checkmark.shield")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Text(L10n.string("checker_diagnostics"))
                    .font(.caption.weight(.semibold))
                Spacer(minLength: 4)
                Text(L10n.checkerVerdict(report.verdict))
                    .font(.caption2.weight(.medium))
                    .foregroundStyle(.secondary)
            }

            Text(report.processAttempted ? L10n.string("checker_process_attempted") : L10n.string("checker_process_not_attempted"))
                .font(.caption2)
                .foregroundStyle(.secondary)

            if !report.detail.isEmpty {
                Text(report.detail)
                    .font(.caption)
                    .textSelection(.enabled)
            }

            if report.processAttempted {
                HStack(spacing: 8) {
                    Text(String(format: L10n.string("checker_status_format"), report.process.termination))
                    if let exit = report.process.exitCode {
                        Text(String(format: L10n.string("checker_exit_format"), exit))
                    }
                    if let signal = report.process.signal {
                        Text(String(format: L10n.string("checker_signal_format"), signal))
                    }
                    if report.process.nativeError != 0 {
                        Text(String(format: L10n.string("checker_native_error_format"), report.process.nativeError))
                    }
                }
                .font(.caption2)
                .foregroundStyle(.secondary)

                HStack(spacing: 8) {
                    Text(String(format: L10n.string("elapsed_format"), report.process.elapsedMS))
                        .monospacedDigit()
                    Text("·")
                    Text(String(format: L10n.string("checker_bytes_format"), report.process.stdoutTotalBytes, report.process.stderrTotalBytes))
                        .monospacedDigit()
                }
                .font(.caption2)
                .foregroundStyle(.secondary)

                if !report.process.detail.isEmpty {
                    Text(report.process.detail)
                        .font(.caption)
                        .textSelection(.enabled)
                }

                ProcessOutputView(report: report.process, binaryPreviewBytes: binaryPreviewBytes)
            }
        }
        .padding(8)
        .background(.quaternary.opacity(0.35), in: RoundedRectangle(cornerRadius: 6))
    }
}

struct ProcessOutputView: View {
    let report: ProcessReport
    let binaryPreviewBytes: Int

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            if !report.stdout.data.isEmpty {
                OutputBytesView(
                    title: L10n.string("output_title"),
                    bytes: report.stdout,
                    binaryPreviewBytes: binaryPreviewBytes
                )
            }
            if !report.stderr.data.isEmpty {
                OutputBytesView(
                    title: L10n.string("stderr_title"),
                    bytes: report.stderr,
                    binaryPreviewBytes: binaryPreviewBytes
                )
            }
            if report.stdoutTruncated || report.stderrTruncated {
                Label(L10n.string("output_truncated"), systemImage: "scissors")
                    .font(.caption2)
                    .foregroundStyle(.orange)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

struct OutputBytesView: View {
    var title: String? = nil
    let bytes: WireBytes
    let binaryPreviewBytes: Int

    var body: some View {
        let canDisplay = bytes.canDisplayAsText
        let content = canDisplay ? bytes.text : bytes.hexPreview(maximumBytes: binaryPreviewBytes)
        CodeBlockBox(
            title: title,
            displayText: content,
            byteCount: UInt64(bytes.data.count),
            isBinaryPreview: !canDisplay,
            binaryPreviewShownBytes: min(bytes.data.count, binaryPreviewBytes)
        )
    }
}
