import SwiftUI
import NativeCoreBridge
import NativeEditorModel

enum TestCaseColumn: Hashable, CaseIterable {
    case input
    case output
    case expected
}

struct TestCaseGridMetrics: Equatable {
    static let minimumColumnWidth: CGFloat = 220
    static let dividerWidth: CGFloat = 1
    static let columnOrder: [TestCaseColumn] = [.input, .output, .expected]

    let viewportWidth: CGFloat
    let columnWidth: CGFloat
    let contentWidth: CGFloat
    let needsHorizontalScroll: Bool

    init(viewportWidth: CGFloat) {
        let clampedViewport = max(0, viewportWidth)
        let columnCount = Self.columnOrder.count
        let totalDividerWidth = Self.dividerWidth * CGFloat(max(0, columnCount - 1))
        let minimumContentWidth = Self.minimumColumnWidth * CGFloat(columnCount) + totalDividerWidth
        let resolvedContentWidth = max(clampedViewport, minimumContentWidth)
        self.viewportWidth = clampedViewport
        self.contentWidth = resolvedContentWidth
        self.columnWidth = (resolvedContentWidth - totalDividerWidth) / CGFloat(columnCount)
        self.needsHorizontalScroll = clampedViewport > 0 && resolvedContentWidth > clampedViewport + 0.5
    }
}

struct RunPanelView: View {
    @Environment(\.openSettings) private var openSettings
    @Bindable var session: NativeEditorSession
    @Bindable var runner: RunController
    let canConfigureRunner: Bool
    var isInspector = false

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
            checkerControlBar
            Divider().opacity(0.45)

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
                   (result.buildOutcome != "Succeeded" && result.buildOutcome != "NotRequired")
                    || !build.stderr.data.isEmpty || !build.stdout.data.isEmpty {
                    CompilerOutputBanner(
                        build: build,
                        buildSucceeded: result.buildOutcome == "Succeeded" || result.buildOutcome == "NotRequired",
                        binaryPreviewBytes: runner.binaryPreviewBytes
                    )
                    .padding(.horizontal, 12)
                    .padding(.top, 10)
                }
            }

            if session.testCases.isEmpty && removedTestResults.isEmpty {
                emptyTestsPlaceholder
            } else {
                GeometryReader { proxy in
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
                                    TestCaseSectionView(
                                        test: $test,
                                        number: index + 1,
                                        availableWidth: proxy.size.width,
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
                                VStack(alignment: .leading, spacing: 8) {
                                    Text(L10n.string("removed_test_results_title"))
                                        .font(.caption.weight(.semibold))
                                        .foregroundStyle(.secondary)
                                        .padding(.horizontal, 2)
                                        .padding(.top, 2)
                                    ForEach(removedTestResults) { test in
                                        RemovedTestResultRow(
                                            test: test,
                                            binaryPreviewBytes: runner.binaryPreviewBytes
                                        )
                                    }
                                }
                            }
                        }
                        .padding(.horizontal, 12)
                        .padding(.vertical, 10)
                    }
                }
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(isInspector ? Color.clear : Color(nsColor: .windowBackgroundColor))
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
                if (result.buildOutcome == "Succeeded" || result.buildOutcome == "NotRequired") && !result.tests.isEmpty {
                    Text("\(passed)/\(result.tests.count)")
                        .font(.caption.monospacedDigit().weight(.medium))
                        .foregroundStyle(.secondary)
                } else if result.buildOutcome == "Succeeded", result.tests.isEmpty, let build = result.build {
                    Text(String(format: L10n.string("elapsed_format"), build.elapsedMS))
                        .font(.caption2.monospacedDigit())
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
                    .font(.caption2.monospacedDigit().weight(.medium))
                    .foregroundStyle(.secondary)
                    .padding(.horizontal, 6)
                    .padding(.vertical, 1.5)
                    .background(.quaternary.opacity(0.6), in: Capsule())
                    .help(String(format: L10n.string("tests_count_format"), enabledTestCount, session.testCases.count))
            }

            Spacer(minLength: 4)

            HStack(spacing: 2) {
                PanelIconButton(
                    systemImage: "plus",
                    helpKey: "add_test",
                    size: 24,
                    cornerRadius: 12,
                    font: .system(size: 12, weight: .medium),
                    isDisabled: !canEditTests
                ) {
                    session.testCases.append(TestCaseInput())
                }

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
                    Image(systemName: "ellipsis.circle")
                        .font(.system(size: 12, weight: .medium))
                        .foregroundStyle(canEditTests ? AnyShapeStyle(.secondary) : AnyShapeStyle(.quaternary))
                        .panelIconHoverChrome(size: 24, cornerRadius: 12, enabled: canEditTests)
                }
                .menuStyle(.borderlessButton)
                .menuIndicator(.hidden)
                .fixedSize()
                .disabled(!canEditTests)
                .help(L10n.string("more_actions"))
                .accessibilityLabel(L10n.string("more_actions"))
            }
        }
        .padding(.horizontal, 14)
        .padding(.top, 10)
        .padding(.bottom, 8)
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
        ViewThatFits(in: .horizontal) {
            HStack(spacing: 8) {
                checkerMenu(showPrefix: true)
                Spacer(minLength: 8)
                executionButtons(expanded: false)
            }

            VStack(alignment: .leading, spacing: 8) {
                executionButtons(expanded: true)

                HStack(spacing: 8) {
                    Text(L10n.string("checker_mode_prefix"))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Spacer(minLength: 8)
                    checkerMenu(showPrefix: false)
                }
            }
        }
        .padding(.horizontal, 14)
        .padding(.bottom, 10)
    }

    private func checkerMenu(showPrefix: Bool) -> some View {
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
                Text(showPrefix ? "\(L10n.string("checker_mode_prefix")) \(checkerModeSummary)" : checkerModeSummary)
                    .font(.caption)
                    .foregroundStyle(showPrefix ? .secondary : .primary)
                    .lineLimit(1)
            }
        }
        .menuStyle(.borderlessButton)
        .fixedSize()
        .disabled(runner.isRunning)
    }

    @ViewBuilder
    private func executionButtons(expanded: Bool) -> some View {
        let content = HStack(spacing: 6) {
            if runner.isRunning {
                Button {
                    runner.cancel()
                } label: {
                    Label {
                        Text(L10n.string("stop_short"))
                    } icon: {
                        Image(systemName: "stop.fill")
                            .foregroundStyle(.red)
                    }
                    .lineLimit(1)
                    .frame(maxWidth: expanded ? .infinity : nil)
                }
                .buttonStyle(.bordered)
                .help(L10n.string("help_cancel_run"))
            } else if runner.supportsCompile(for: session) {
                Button {
                    runner.compile(session)
                } label: {
                    Label(L10n.string("compile_short"), systemImage: "hammer")
                        .lineLimit(1)
                        .frame(maxWidth: expanded ? .infinity : nil)
                }
                .buttonStyle(.bordered)
                .disabled(!runner.canCompile(session))
                .help(L10n.string("help_compile"))

                Button {
                    runner.runOnly(session)
                } label: {
                    Label(L10n.string("run_short"), systemImage: "play.rectangle")
                        .lineLimit(1)
                        .frame(maxWidth: expanded ? .infinity : nil)
                }
                .buttonStyle(.bordered)
                .disabled(!runner.canRunOnly(session))
                .help(runner.runOnlyHelpText(for: session))

                Button {
                    runner.runAll(session)
                } label: {
                    Label(L10n.string("compile_and_run_short"), systemImage: "play.fill")
                        .lineLimit(1)
                        .frame(maxWidth: expanded ? .infinity : nil)
                }
                .buttonStyle(.borderedProminent)
                .disabled(!runner.canRun(session))
                .help(L10n.string("help_compile_and_run"))
            } else {
                Button {
                    runner.runAll(session)
                } label: {
                    Label(L10n.string("run_short"), systemImage: "play.fill")
                        .lineLimit(1)
                        .frame(maxWidth: expanded ? .infinity : nil)
                }
                .buttonStyle(.borderedProminent)
                .disabled(!runner.canRun(session))
                .help(L10n.string("help_run_interpreted"))
            }
        }
        .controlSize(expanded ? .regular : .small)
        .buttonBorderShape(.roundedRectangle(radius: 6))

        if expanded {
            content
        } else {
            content.fixedSize()
        }
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
                Button(L10n.string("import_tests")) {
                    runner.importTests(into: session)
                }
            }
            .controlSize(.regular)
            .buttonBorderShape(.roundedRectangle(radius: 6))
            .disabled(!canEditTests)
        }
        .padding(20)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

private struct InspectorCardChrome: ViewModifier {
    @Environment(\.colorScheme) private var colorScheme
    let accentColor: Color?
    var isDimmed = false

    func body(content: Content) -> some View {
        let shape = RoundedRectangle(cornerRadius: 10, style: .continuous)
        let borderColor: Color = if let accentColor {
            accentColor.opacity(isDimmed ? 0.18 : 0.35)
        } else {
            PanelSurfaceStyle.hairlineBorder
        }

        content
            .background(
                Color(nsColor: .controlBackgroundColor).opacity(colorScheme == .dark ? 0.55 : 0.78),
                in: shape
            )
            .overlay(alignment: .leading) {
                if let accentColor {
                    Capsule(style: .continuous)
                        .fill(accentColor.opacity(isDimmed ? 0.28 : 0.78))
                        .frame(width: 3)
                        .padding(.vertical, 10)
                        .padding(.leading, 4)
                }
            }
            .clipShape(shape)
            .overlay(shape.strokeBorder(borderColor, lineWidth: 0.75))
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
        DisclosureGroup(isExpanded: $isExpanded) {
            ProcessOutputView(report: build, binaryPreviewBytes: binaryPreviewBytes)
                .padding(.top, 8)
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
        .padding(.leading, buildSucceeded ? 12 : 14)
        .padding(.trailing, 12)
        .padding(.vertical, 9)
        .modifier(InspectorCardChrome(accentColor: buildSucceeded ? nil : .orange))
    }
}

private struct TestCaseSectionView: View {
    @Binding var test: TestCaseInput
    let number: Int
    let availableWidth: CGFloat
    let canEdit: Bool
    let matchingResult: TestResult?
    let resultIsStale: Bool
    let binaryPreviewBytes: Int
    let useOutputAsExpected: (WireBytes) -> Void
    let duplicate: () -> Void
    let remove: () -> Void

    @State private var isExpandedHeight = false
    @State private var wellWidth: CGFloat = 0

    private var replacementExpectedOutput: WireBytes? {
        guard canEdit, !resultIsStale, let output = matchingResult?.process.completeTextOutput,
              test.expected != output else { return nil }
        return output
    }

    private var verdictColor: Color? {
        matchingResult.map { VerdictDescriptor.forVerdict($0.verdict).tone.color }
    }

    private var outputHeaderBackground: Color {
        verdictColor.map { $0.opacity(resultIsStale ? 0.03 : 0.09) } ?? PanelSurfaceStyle.cellHeaderBackground
    }

    private var outputBodyBackground: Color {
        verdictColor.map { $0.opacity(resultIsStale ? 0.0 : 0.035) } ?? Color.clear
    }

    private var hasDiagnostics: Bool {
        guard let matchingResult else { return false }
        return !matchingResult.process.stderr.data.isEmpty
            || matchingResult.process.stdoutTruncated
            || matchingResult.process.stderrTruncated
            || !matchingResult.detail.isEmpty
            || matchingResult.checker != nil
    }

    private var editorMinHeight: CGFloat { isExpandedHeight ? 224 : 112 }
    private var ioPaneHeight: CGFloat { PanelSurfaceStyle.cellHeaderHeight + 1 + editorMinHeight }

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
        let outputBytes = matchingResult?.process.stdout ?? WireBytes("")
        let outputCanDisplay = outputBytes.canDisplayAsText
        let outputDisplayText = outputCanDisplay
            ? outputBytes.text
            : outputBytes.hexPreview(maximumBytes: binaryPreviewBytes)
        let metrics = TestCaseGridMetrics(viewportWidth: wellWidth > 0 ? wellWidth : max(0, availableWidth - 40))
        let hasFootnotes = !test.input.isUTF8 || test.expected?.isUTF8 == false || test.expected == nil || !outputCanDisplay

        VStack(alignment: .leading, spacing: 0) {
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

                HStack(spacing: 2) {
                    if let output = replacementExpectedOutput {
                        PanelIconButton(
                            systemImage: "arrow.uturn.up.square",
                            helpKey: "use_output_as_expected",
                            tint: Color.accentColor
                        ) {
                            useOutputAsExpected(output)
                        }
                    }

                    PanelIconButton(
                        systemImage: isExpandedHeight ? "rectangle.compress.vertical" : "rectangle.expand.vertical",
                        helpKey: isExpandedHeight ? "collapse_editor" : "expand_editor"
                    ) {
                        isExpandedHeight.toggle()
                    }

                    PanelIconButton(
                        systemImage: "plus.square.on.square",
                        helpKey: "duplicate_test",
                        isDisabled: !canEdit,
                        action: duplicate
                    )

                    PanelIconButton(
                        systemImage: "trash",
                        helpKey: "remove_test",
                        isDisabled: !canEdit,
                        action: remove
                    )
                }
            }
            .padding(.leading, 12)
            .padding(.trailing, 10)
            .padding(.top, 9)
            .padding(.bottom, 7)

            ScrollView(.horizontal) {
                HStack(alignment: .top, spacing: 0) {
                    ForEach(TestCaseGridMetrics.columnOrder, id: \.self) { column in
                        if column != TestCaseGridMetrics.columnOrder.first {
                            Divider()
                        }
                        columnPane(column, outputBytes: outputBytes, outputDisplayText: outputDisplayText)
                            .frame(width: metrics.columnWidth)
                    }
                }
                .frame(width: metrics.contentWidth, height: ioPaneHeight, alignment: .topLeading)
            }
            .frame(height: ioPaneHeight)
            .scrollIndicators(metrics.needsHorizontalScroll ? .visible : .hidden, axes: .horizontal)
            .scrollBounceBehavior(.basedOnSize, axes: .horizontal)
            .onGeometryChange(for: CGFloat.self, of: \.size.width) { wellWidth = $0 }
            .panelSurfaceWell()
            .opacity(test.enabled ? 1.0 : 0.55)
            .padding(.leading, 10)
            .padding(.trailing, 8)
            .padding(.bottom, (hasFootnotes || hasDiagnostics) ? 6 : 9)

            if hasFootnotes || (hasDiagnostics && matchingResult != nil) {
                VStack(alignment: .leading, spacing: 6) {
                    footnotes(outputBytes: outputBytes, outputCanDisplay: outputCanDisplay)

                    if hasDiagnostics, let matchingResult {
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
                .padding(.leading, 12)
                .padding(.trailing, 10)
                .padding(.bottom, 9)
            }
        }
        .modifier(InspectorCardChrome(accentColor: verdictColor, isDimmed: resultIsStale))
    }

    @ViewBuilder
    private func columnPane(_ column: TestCaseColumn, outputBytes: WireBytes, outputDisplayText: String) -> some View {
        switch column {
        case .input:
            inputPane
        case .output:
            outputPane(bytes: outputBytes, displayText: outputDisplayText)
        case .expected:
            expectedPane
        }
    }

    private var inputPane: some View {
        VStack(alignment: .leading, spacing: 0) {
            PanelCellHeaderBar(
                title: L10n.string("test_input"),
                byteCount: UInt64(test.input.data.count)
            ) {
                CopyTextButton(text: test.input.text)
            }

            ioTextEditor(
                text: Binding(
                    get: { test.input.text },
                    set: { test.input = WireBytes($0) }
                ),
                accessibilityLabel: L10n.string("test_input")
            )
        }
    }

    private var expectedPane: some View {
        VStack(alignment: .leading, spacing: 0) {
            PanelCellHeaderBar(
                title: L10n.string("test_expected"),
                byteCount: test.expected.map { UInt64($0.data.count) }
            ) {
                HStack(spacing: 4) {
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
            }

            ioTextEditor(
                text: Binding(
                    get: { test.expected?.text ?? "" },
                    set: { test.expected = WireBytes($0) }
                ),
                accessibilityLabel: L10n.string("test_expected"),
                background: test.expected == nil ? Color.primary.opacity(0.015) : Color.clear
            )
        }
    }

    private func outputPane(bytes: WireBytes, displayText: String) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            PanelCellHeaderBar(
                title: L10n.string("output_title"),
                byteCount: UInt64(bytes.data.count),
                trailingSubtitle: matchingResult.map(outputStatusSummary),
                tint: outputHeaderBackground
            ) {
                CopyTextButton(text: displayText)
            }

            ScrollView(.vertical) {
                PanelBytesContent(
                    displayText: displayText,
                    byteCount: UInt64(bytes.data.count),
                    verticalPadding: 6
                )
            }
            .frame(height: editorMinHeight)
            .background(outputBodyBackground)
        }
    }

    private func ioTextEditor(
        text: Binding<String>,
        accessibilityLabel: String,
        background: Color = .clear
    ) -> some View {
        TextEditor(text: text)
            .font(.system(.callout, design: .monospaced))
            .scrollContentBackground(.hidden)
            .padding(.horizontal, 5)
            .padding(.vertical, 6)
            .frame(height: editorMinHeight)
            .background(background)
            .disabled(!canEdit)
            .accessibilityLabel(accessibilityLabel)
    }

    @ViewBuilder
    private func footnotes(outputBytes: WireBytes, outputCanDisplay: Bool) -> some View {
        let hasBinaryInput = !test.input.isUTF8
        let hasBinaryExpected = test.expected?.isUTF8 == false
        let isUnchecked = test.expected == nil
        let hasBinaryOutput = !outputCanDisplay

        if hasBinaryInput || hasBinaryExpected || isUnchecked || hasBinaryOutput {
            VStack(alignment: .leading, spacing: 2) {
                if hasBinaryInput {
                    Text("\(L10n.string("test_input")): \(L10n.string("binary_data"))")
                        .font(.caption2)
                        .foregroundStyle(.orange)
                }
                if hasBinaryExpected {
                    Text("\(L10n.string("test_expected")): \(L10n.string("binary_data"))")
                        .font(.caption2)
                        .foregroundStyle(.orange)
                }
                if isUnchecked {
                    Text(L10n.string("expected_unchecked_hint"))
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
                if hasBinaryOutput {
                    Text(String(
                        format: L10n.string("output_binary_preview"),
                        min(outputBytes.data.count, binaryPreviewBytes),
                        UInt64(outputBytes.data.count)
                    ))
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                }
            }
        }
    }
}

private struct RemovedTestResultRow: View {
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
        .padding(.leading, 12)
        .padding(.trailing, 10)
        .padding(.vertical, 10)
        .modifier(InspectorCardChrome(accentColor: descriptor.tone.color))
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
        .padding(.leading, 10)
        .padding(.vertical, 2)
        .overlay(alignment: .leading) {
            RoundedRectangle(cornerRadius: 1)
                .fill(Color(nsColor: .separatorColor).opacity(0.8))
                .frame(width: 2)
        }
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
