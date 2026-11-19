import SwiftUI
import NativeCoreBridge
import NativeEditorModel

struct RunPanelView: View {
    @Environment(\.openSettings) private var openSettings
    @Bindable var session: NativeEditorSession
    @Bindable var runner: RunController
    let canConfigureRunner: Bool

    private var enabledTestCount: Int {
        session.testCases.reduce(0) { $0 + ($1.enabled ? 1 : 0) }
    }

    private var canEditTests: Bool {
        !runner.isRunning && session.canMutatePersistence
    }

    private var latestResult: BatchResult? {
        runner.result(for: session.document.id)
    }

    private var currentState: JobState? {
        runner.state(for: session.document.id)
    }

    private var removedTestResults: [TestResult] {
        guard let result = latestResult else { return [] }
        let activeIDs = Set(session.testCases.map(\.id))
        return result.tests.filter { !activeIDs.contains(RawIdentifier($0.testcaseID)) }
    }

    var body: some View {
        let resultIsStale = latestResult != nil && !runner.resultIsCurrent(for: session)
        VStack(alignment: .leading, spacing: 0) {
            headerBar(resultIsStale: resultIsStale)
            Divider()
            checkerControlBar
            Divider()

            if let notice = runner.toolchainNotice(for: session) {
                NoticeBannerView(kind: .warning, message: notice)
            }
            if let notice = session.persistenceNotice {
                NoticeBannerView(kind: .warning, message: notice)
            }
            if let notice = session.companionRunNotice {
                NoticeBannerView(kind: .warning, message: notice)
            }
            if let result = latestResult {
                if !result.error.isEmpty {
                    NoticeBannerView(kind: .error, message: result.error)
                }
                if let build = result.build,
                   result.buildOutcome != "Succeeded" || !build.stderr.data.isEmpty || !build.stdout.data.isEmpty {
                    CompilerOutputBanner(
                        build: build,
                        buildSucceeded: result.buildOutcome == "Succeeded",
                        binaryPreviewBytes: runner.binaryPreviewBytes
                    )
                }
            }

            if session.testCases.isEmpty && removedTestResults.isEmpty {
                emptyTestsPlaceholder
            } else {
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 10) {
                        if session.testCases.isEmpty {
                            emptyTestsPlaceholder
                                .frame(minHeight: 140)
                        } else {
                            ForEach($session.testCases) { $test in
                                let id = test.id
                                let index = session.testCases.firstIndex(where: { $0.id == id }) ?? 0
                                let matchingResult = latestResult?.tests.first(where: { RawIdentifier($0.testcaseID) == id })
                                TestCaseCardView(
                                    test: $test,
                                    number: index + 1,
                                    canEdit: canEditTests,
                                    matchingResult: matchingResult,
                                    resultIsStale: resultIsStale,
                                    binaryPreviewBytes: runner.binaryPreviewBytes,
                                    useOutputAsExpected: { runner.useOutputAsExpected($0, for: session, testcaseID: id) },
                                    duplicate: {
                                        var copy = TestCaseInput(enabled: test.enabled)
                                        copy.input = test.input
                                        copy.expected = test.expected
                                        session.testCases.insert(copy, at: min(index + 1, session.testCases.count))
                                    },
                                    remove: {
                                        session.testCases.removeAll { $0.id == test.id }
                                    }
                                )
                            }
                        }

                        if !removedTestResults.isEmpty {
                            VStack(alignment: .leading, spacing: 6) {
                                Text(L10n.string("removed_test_results_title"))
                                    .font(.caption.weight(.semibold))
                                    .foregroundStyle(.secondary)
                                ForEach(removedTestResults) { test in
                                    RemovedTestResultCard(
                                        test: test,
                                        binaryPreviewBytes: runner.binaryPreviewBytes
                                    )
                                }
                            }
                            .padding(.top, 4)
                        }
                    }
                    .padding(12)
                }
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(Color(nsColor: .windowBackgroundColor))
    }

    private func headerBar(resultIsStale: Bool) -> some View {
        HStack(spacing: 8) {
            Text(L10n.string("test_cases_title"))
                .font(.subheadline.weight(.semibold))
                .fixedSize()

            if let state = currentState, !state.isTerminal {
                RunStateBadge(state: state)
            } else if let result = latestResult {
                let descriptor = VerdictDescriptor.summary(for: result)
                let passed = result.tests.reduce(0) { $0 + ($1.verdict == "AC" ? 1 : 0) }
                VerdictBadge(descriptor: descriptor, compact: true, dimmed: resultIsStale)
                if result.buildOutcome == "Succeeded" && !result.tests.isEmpty {
                    Text("\(passed)/\(result.tests.count)")
                        .font(.caption.monospacedDigit().weight(.medium))
                        .foregroundStyle(.secondary)
                }
                if resultIsStale {
                    Text(L10n.string("outdated_short"))
                        .font(.caption2.weight(.medium))
                        .foregroundStyle(.orange)
                        .padding(.horizontal, 5)
                        .padding(.vertical, 1.5)
                        .background(Color.orange.opacity(0.12), in: Capsule())
                        .help(L10n.string("run_result_outdated_help"))
                }
            } else {
                Text("\(enabledTestCount)/\(session.testCases.count)")
                    .font(.caption2.monospacedDigit())
                    .foregroundStyle(.secondary)
                    .padding(.horizontal, 6)
                    .padding(.vertical, 2)
                    .background(.quaternary.opacity(0.5), in: Capsule())
                    .help(String(format: L10n.string("tests_count_format"), enabledTestCount, session.testCases.count))
            }

            Spacer(minLength: 4)

            Button {
                session.testCases.append(TestCaseInput())
            } label: {
                Label(L10n.string("add_short"), systemImage: "plus")
            }
            .controlSize(.small)
            .fixedSize()
            .disabled(!canEditTests)
            .help(L10n.string("add_test"))

            Menu {
                Button(L10n.string("import_tests")) {
                    runner.importTests(into: session)
                }
                Divider()
                Button(L10n.string("enable_all_tests")) {
                    for index in session.testCases.indices { session.testCases[index].enabled = true }
                }
                .disabled(session.testCases.isEmpty)
                Button(L10n.string("disable_all_tests")) {
                    for index in session.testCases.indices { session.testCases[index].enabled = false }
                }
                .disabled(session.testCases.isEmpty)
                Button(L10n.string("remove_disabled_tests")) {
                    session.testCases.removeAll { !$0.enabled }
                }
                .disabled(!session.testCases.contains { !$0.enabled })
                Button(L10n.string("clear_all_tests"), role: .destructive) {
                    session.testCases.removeAll()
                }
                .disabled(session.testCases.isEmpty)
            } label: {
                Label(L10n.string("more_actions"), systemImage: "ellipsis.circle")
                    .labelStyle(.iconOnly)
            }
            .menuStyle(.borderlessButton)
            .fixedSize()
            .disabled(!canEditTests)
            .help(L10n.string("more_actions"))
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(.bar)
    }

    private var checkerModeSummary: String {
        if runner.externalCheckerEnabled {
            let name = runner.externalChecker.map { URL(fileURLWithPath: $0.executable).lastPathComponent } ?? L10n.string("external_checker")
            return String(format: L10n.string("checker_mode_external_format"), name)
        }
        return L10n.comparisonName(runner.comparison)
    }

    private var checkerControlsDisabled: Bool {
        !runner.canConfigureChecker || !canConfigureRunner
    }

    private var checkerControlBar: some View {
        HStack(spacing: 8) {
            Menu {
                Toggle(L10n.string("use_external_checker"), isOn: Binding(
                    get: { runner.externalCheckerEnabled },
                    set: { enabled in
                        if enabled, runner.externalChecker == nil {
                            runner.beginCheckerEdit(enableOnApply: true)
                            if runner.configurationDraft != nil { openSettings() }
                        } else { runner.externalCheckerEnabled = enabled }
                    }
                ))
                .disabled(checkerControlsDisabled)

                Button(L10n.string("checker_settings")) {
                    runner.beginCheckerEdit()
                    if runner.configurationDraft != nil { openSettings() }
                }
                .disabled(checkerControlsDisabled)

                Divider()

                Picker(L10n.string("comparison"), selection: $runner.comparison) {
                    Text(L10n.string("comparison_trailing")).tag(ComparisonPolicy.trailing)
                    Text(L10n.string("comparison_strict")).tag(ComparisonPolicy.strict)
                    Text(L10n.string("comparison_byte")).tag(ComparisonPolicy.byte)
                }
                .pickerStyle(.inline)
                .disabled(runner.externalCheckerEnabled || !canConfigureRunner)
            } label: {
                HStack(spacing: 5) {
                    Image(systemName: runner.externalCheckerEnabled ? "checkmark.shield" : "slider.horizontal.3")
                        .font(.caption)
                        .foregroundStyle(runner.externalCheckerEnabled ? Color.accentColor : Color.secondary)
                    Text("\(L10n.string("checker_mode_prefix")) \(checkerModeSummary)")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                    Image(systemName: "chevron.down")
                        .font(.system(size: 8, weight: .semibold))
                        .foregroundStyle(.tertiary)
                }
                .padding(.horizontal, 6)
                .padding(.vertical, 3)
                .background(.quaternary.opacity(0.45), in: RoundedRectangle(cornerRadius: 5))
            }
            .menuStyle(.borderlessButton)
            .fixedSize()
            .disabled(runner.isRunning)

            Spacer(minLength: 4)

            if runner.isRunning {
                Button {
                    runner.cancel()
                } label: {
                    Label(L10n.string("stop_short"), systemImage: "stop.fill")
                }
                .controlSize(.small)
                .help(L10n.string("help_cancel_run"))
            } else {
                Button {
                    runner.runAll(session)
                } label: {
                    Label(L10n.string("run_short"), systemImage: "play.fill")
                }
                .controlSize(.small)
                .disabled(!runner.canRun(session))
                .help(L10n.string("help_run_all"))
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
    }

    private var emptyTestsPlaceholder: some View {
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
        .padding(20)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

private struct CompilerOutputBanner: View {
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
        VStack(spacing: 0) {
            DisclosureGroup(isExpanded: $isExpanded) {
                ProcessOutputView(report: build, binaryPreviewBytes: binaryPreviewBytes)
                    .padding(.top, 6)
            } label: {
                HStack(spacing: 6) {
                    Image(systemName: buildSucceeded ? "terminal" : "exclamationmark.triangle.fill")
                        .font(.caption)
                        .foregroundStyle(buildSucceeded ? Color.secondary : Color.orange)
                    Text(L10n.string("compiler_output"))
                        .font(.caption.weight(.semibold))
                    if !buildSucceeded {
                        VerdictBadge(verdict: "CE", compact: true)
                    }
                    Spacer(minLength: 4)
                    Text(String(format: L10n.string("elapsed_format"), build.elapsedMS))
                        .font(.caption2.monospacedDigit())
                        .foregroundStyle(.secondary)
                }
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
            .background(buildSucceeded ? Color.clear : Color.orange.opacity(0.08))

            Divider()
        }
    }
}

private struct TestCaseCardView: View {
    @Binding var test: TestCaseInput
    let number: Int
    let canEdit: Bool
    let matchingResult: TestResult?
    let resultIsStale: Bool
    let binaryPreviewBytes: Int
    let useOutputAsExpected: (WireBytes) -> Void
    let duplicate: () -> Void
    let remove: () -> Void

    @State private var isExpandedHeight = false

    private var replacementExpectedOutput: WireBytes? {
        guard canEdit, !resultIsStale, let output = matchingResult?.process.completeTextOutput,
              test.expected != output else { return nil }
        return output
    }

    private var cardBorderColor: Color {
        guard let matchingResult, !resultIsStale else {
            return Color(nsColor: .separatorColor).opacity(0.75)
        }
        return VerdictDescriptor.forVerdict(matchingResult.verdict).tone.color.opacity(0.40)
    }

    private var outputBorderColor: Color? {
        guard let matchingResult else { return nil }
        return VerdictDescriptor.forVerdict(matchingResult.verdict).tone.color.opacity(resultIsStale ? 0.25 : 0.45)
    }

    private func outputStatusSummary(for result: TestResult) -> String {
        var parts = [result.process.termination]
        if let exit = result.process.exitCode, exit != 0 {
            parts.append(String(format: L10n.string("solution_exit_code_format"), exit))
        }
        if let signal = result.process.signal {
            parts.append(String(format: L10n.string("solution_signal_format"), signal))
        }
        return parts.joined(separator: " · ")
    }

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

                Spacer(minLength: 4)

                if let output = replacementExpectedOutput {
                    Button {
                        useOutputAsExpected(output)
                    } label: {
                        Image(systemName: "arrow.uturn.up.square")
                            .font(.caption)
                            .foregroundStyle(Color.accentColor)
                    }
                    .buttonStyle(.borderless)
                    .help(L10n.string("use_output_as_expected"))
                    .accessibilityLabel(L10n.string("use_output_as_expected"))
                }

                Button {
                    isExpandedHeight.toggle()
                } label: {
                    Image(systemName: isExpandedHeight ? "rectangle.compress.vertical" : "rectangle.expand.vertical")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .buttonStyle(.borderless)
                .help(L10n.string(isExpandedHeight ? "collapse_editor" : "expand_editor"))

                Button(action: duplicate) {
                    Image(systemName: "plus.square.on.square")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .buttonStyle(.borderless)
                .disabled(!canEdit)
                .help(L10n.string("duplicate_test"))

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
                HStack(alignment: .top, spacing: 8) {
                    inputColumn.frame(minWidth: 110, maxWidth: .infinity)
                    expectedColumn.frame(minWidth: 110, maxWidth: .infinity)
                    outputColumn.frame(minWidth: 110, maxWidth: .infinity)
                }
                VStack(alignment: .leading, spacing: 8) {
                    inputColumn
                    expectedColumn
                    outputColumn
                }
            }
            .opacity(test.enabled ? 1.0 : 0.65)

            if let matchingResult {
                Divider()
                    .padding(.top, 2)

                VStack(alignment: .leading, spacing: 6) {
                    if !matchingResult.process.stderr.data.isEmpty {
                        OutputBytesView(
                            title: L10n.string("stderr_title"),
                            bytes: matchingResult.process.stderr,
                            binaryPreviewBytes: binaryPreviewBytes
                        )
                    }

                    if matchingResult.process.stdoutTruncated || matchingResult.process.stderrTruncated {
                        Label(L10n.string("output_truncated"), systemImage: "scissors")
                            .font(.caption2)
                            .foregroundStyle(.orange)
                    }

                    if !matchingResult.detail.isEmpty {
                        Text(matchingResult.detail)
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                            .textSelection(.enabled)
                    }

                    if let checker = matchingResult.checker {
                        CheckerDiagnosticsView(report: checker, binaryPreviewBytes: binaryPreviewBytes)
                    }
                }
            }
        }
        .padding(10)
        .background(Color(nsColor: .controlBackgroundColor).opacity(0.65), in: RoundedRectangle(cornerRadius: 8))
        .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(cardBorderColor, lineWidth: 1))
    }

    private var editorMinHeight: CGFloat { isExpandedHeight ? 132 : 68 }
    private var outputColumn: some View {
        OutputBytesView(title: L10n.string("output_title"), bytes: matchingResult?.process.stdout ?? WireBytes(""),
                        binaryPreviewBytes: binaryPreviewBytes,
                        trailingSubtitle: matchingResult.map(outputStatusSummary), borderColor: outputBorderColor,
                        contentHeight: editorMinHeight)
    }

    private var inputColumn: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 5) {
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
            .padding(4)
            .frame(height: editorMinHeight)
            .background(Color(nsColor: .textBackgroundColor), in: RoundedRectangle(cornerRadius: 6))
            .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(Color(nsColor: .separatorColor).opacity(0.6), lineWidth: 1))
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
        VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 5) {
                Text(L10n.string("test_expected"))
                    .font(.caption.weight(.medium))
                    .foregroundStyle(.secondary)
                if let expected = test.expected {
                    Text(String(format: L10n.string("bytes_count_format"), UInt64(expected.data.count)))
                        .font(.caption2.monospacedDigit())
                        .foregroundStyle(.tertiary)
                }
                Spacer(minLength: 4)
                Toggle("", isOn: Binding(
                    get: { test.expected != nil },
                    set: { test.expected = $0 ? WireBytes("") : nil }
                ))
                .toggleStyle(.checkbox)
                .labelsHidden()
                .controlSize(.mini)
                .disabled(!canEdit)
                .help(L10n.string("check_expected"))
                .accessibilityLabel(L10n.string("check_expected"))

                if let expected = test.expected {
                    CopyTextButton(text: expected.text)
                }
            }

            TextEditor(text: Binding(
                get: { test.expected?.text ?? "" },
                set: { test.expected = WireBytes($0) }
            ))
            .font(.system(.callout, design: .monospaced))
            .scrollContentBackground(.hidden)
            .padding(4)
            .frame(height: editorMinHeight)
            .background(Color(nsColor: .textBackgroundColor), in: RoundedRectangle(cornerRadius: 6))
            .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(Color(nsColor: .separatorColor).opacity(0.6), lineWidth: 1))
            .disabled(!canEdit)
            .accessibilityLabel(L10n.string("test_expected"))

            if test.expected?.isUTF8 == false {
                Text(L10n.string("binary_data"))
                    .font(.caption2)
                    .foregroundStyle(.orange)
            }
            if test.expected == nil {
                Text(L10n.string("expected_unchecked_hint"))
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
    }
}

private struct RemovedTestResultCard: View {
    let test: TestResult
    let binaryPreviewBytes: Int

    var body: some View {
        let descriptor = VerdictDescriptor.forVerdict(test.verdict)
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                VerdictBadge(descriptor: descriptor, compact: true)
                Text(test.testcaseID)
                    .font(.system(.caption, design: .monospaced))
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                Spacer(minLength: 4)
                Text(String(format: L10n.string("elapsed_format"), test.process.elapsedMS))
                    .font(.caption2.monospacedDigit())
                    .foregroundStyle(.secondary)
            }
            ProcessOutputView(report: test.process, binaryPreviewBytes: binaryPreviewBytes)
            if let checker = test.checker {
                CheckerDiagnosticsView(report: checker, binaryPreviewBytes: binaryPreviewBytes)
            }
        }
        .padding(9)
        .background(Color(nsColor: .controlBackgroundColor).opacity(0.5), in: RoundedRectangle(cornerRadius: 8))
        .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(descriptor.tone.color.opacity(0.3), lineWidth: 1))
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
    var trailingSubtitle: String? = nil
    var borderColor: Color? = nil
    var contentHeight: CGFloat? = nil

    var body: some View {
        let canDisplay = bytes.canDisplayAsText
        let content = canDisplay ? bytes.text : bytes.hexPreview(maximumBytes: binaryPreviewBytes)
        CodeBlockBox(
            title: title,
            displayText: content,
            byteCount: UInt64(bytes.data.count),
            trailingSubtitle: trailingSubtitle,
            borderColor: borderColor,
            isBinaryPreview: !canDisplay,
            binaryPreviewShownBytes: min(bytes.data.count, binaryPreviewBytes),
            contentHeight: contentHeight
        )
    }
}
