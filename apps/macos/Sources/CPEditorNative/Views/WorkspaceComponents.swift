import AppKit
import SwiftUI
import NativeCoreBridge

enum VerdictTone: Sendable {
    case passed
    case wrong
    case warning
    case error
    case checker
    case neutral

    var color: Color {
        switch self {
        case .passed: return .green
        case .wrong, .error: return .red
        case .warning: return .orange
        case .checker: return .purple
        case .neutral: return .secondary
        }
    }
}

struct VerdictDescriptor: Sendable {
    let label: String
    let symbol: String
    let tone: VerdictTone

    static func forVerdict(_ verdict: String) -> Self {
        switch verdict {
        case "AC", "Accepted":
            return Self(label: verdict, symbol: "checkmark.circle.fill", tone: .passed)
        case "WA", "WrongAnswer":
            return Self(label: verdict, symbol: "xmark.circle.fill", tone: .wrong)
        case "TLE":
            return Self(label: verdict, symbol: "clock.badge.exclamationmark", tone: .warning)
        case "OLE":
            return Self(label: verdict, symbol: "doc.badge.ellipsis", tone: .warning)
        case "RE":
            return Self(label: verdict, symbol: "exclamationmark.octagon.fill", tone: .error)
        case "CE":
            return Self(label: L10n.string("verdict_ce"), symbol: "hammer.fill", tone: .warning)
        case "CheckerError":
            return Self(label: verdict, symbol: "wrench.and.screwdriver.fill", tone: .checker)
        case "Cancelled":
            return Self(label: verdict, symbol: "slash.circle", tone: .neutral)
        case "Unchecked":
            return Self(label: verdict, symbol: "minus.circle", tone: .neutral)
        default:
            return Self(label: verdict, symbol: "questionmark.circle", tone: .neutral)
        }
    }

    static func summary(for result: BatchResult) -> Self {
        if result.buildOutcome == "Cancelled" {
            return .forVerdict("Cancelled")
        }
        if result.buildOutcome != "Succeeded" {
            return .forVerdict("CE")
        }
        guard !result.tests.isEmpty else {
            return .forVerdict("Unchecked")
        }
        if result.tests.allSatisfy({ $0.verdict == "AC" }) {
            return .forVerdict("AC")
        }
        if let priority = ["CheckerError", "RE", "TLE", "OLE", "WA", "Cancelled", "Unchecked"]
            .first(where: { candidate in result.tests.contains { $0.verdict == candidate } }) {
            return .forVerdict(priority)
        }
        return .forVerdict(result.tests[0].verdict)
    }
}

struct VerdictBadge: View {
    let descriptor: VerdictDescriptor
    var compact = false
    var dimmed = false
    var prominentSelection = false

    init(verdict: String, compact: Bool = false, dimmed: Bool = false, prominentSelection: Bool = false) {
        self.descriptor = .forVerdict(verdict)
        self.compact = compact
        self.dimmed = dimmed
        self.prominentSelection = prominentSelection
    }

    init(descriptor: VerdictDescriptor, compact: Bool = false, dimmed: Bool = false, prominentSelection: Bool = false) {
        self.descriptor = descriptor
        self.compact = compact
        self.dimmed = dimmed
        self.prominentSelection = prominentSelection
    }

    var body: some View {
        let foreground: Color = prominentSelection ? .white : descriptor.tone.color
        let background: Color = prominentSelection
            ? .white.opacity(dimmed ? 0.14 : 0.24)
            : descriptor.tone.color.opacity(dimmed ? 0.08 : 0.14)
        let border: Color = prominentSelection
            ? .white.opacity(dimmed ? 0.25 : 0.45)
            : descriptor.tone.color.opacity(dimmed ? 0.18 : 0.32)

        HStack(spacing: compact ? 3 : 4) {
            if !compact {
                Image(systemName: descriptor.symbol)
                    .font(.caption2.weight(.semibold))
            }
            Text(descriptor.label)
                .font((compact ? Font.caption2 : Font.caption).weight(.semibold))
                .lineLimit(1)
        }
        .foregroundStyle(foreground)
        .padding(.horizontal, compact ? 5 : 7)
        .padding(.vertical, compact ? 1.5 : 3)
        .background(background, in: Capsule())
        .overlay(Capsule().strokeBorder(border, lineWidth: 1))
        .opacity(dimmed ? 0.72 : 1)
    }
}

struct RunStateBadge: View {
    let state: JobState?

    private var isActive: Bool {
        guard let state else { return false }
        return !state.isTerminal
    }

    private var tint: Color {
        guard let state else { return .secondary }
        switch state {
        case .queued, .building, .running, .checking: return .accentColor
        case .completed: return .secondary
        case .cancelled: return .orange
        case .failed: return .red
        }
    }

    var body: some View {
        HStack(spacing: 5) {
            if isActive {
                ProgressView()
                    .controlSize(.mini)
            } else {
                Circle()
                    .fill(tint.opacity(state == nil ? 0.45 : 0.8))
                    .frame(width: 6, height: 6)
            }
            Text(L10n.jobState(state))
                .font(.caption.weight(.medium))
                .foregroundStyle(isActive ? .primary : .secondary)
                .lineLimit(1)
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 3)
        .background(.quaternary.opacity(0.5), in: Capsule())
    }
}

enum NoticeKind {
    case info
    case warning
    case error

    var symbol: String {
        switch self {
        case .info: return "info.circle.fill"
        case .warning: return "exclamationmark.triangle.fill"
        case .error: return "xmark.octagon.fill"
        }
    }

    var color: Color {
        switch self {
        case .info: return .accentColor
        case .warning: return .orange
        case .error: return .red
        }
    }
}

struct NoticeBannerView<Actions: View>: View {
    let kind: NoticeKind
    let message: String
    private let actions: Actions

    init(kind: NoticeKind, message: String, @ViewBuilder actions: () -> Actions) {
        self.kind = kind
        self.message = message
        self.actions = actions()
    }

    var body: some View {
        HStack(alignment: .center, spacing: 8) {
            Image(systemName: kind.symbol)
                .foregroundStyle(kind.color)
                .font(.callout)
            Text(message)
                .font(.caption)
                .foregroundStyle(.primary)
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
            actions
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 7)
        .background(kind.color.opacity(0.10))
        .overlay(alignment: .bottom) {
            Divider()
        }
    }
}

extension NoticeBannerView where Actions == EmptyView {
    init(kind: NoticeKind, message: String) {
        self.init(kind: kind, message: message) { EmptyView() }
    }
}

struct CopyTextButton: View {
    let text: String

    var body: some View {
        Button {
            NSPasteboard.general.clearContents()
            NSPasteboard.general.setString(text, forType: .string)
        } label: {
            Image(systemName: "doc.on.doc")
                .font(.caption2)
                .foregroundStyle(.secondary)
        }
        .buttonStyle(.borderless)
        .disabled(text.isEmpty)
        .help(L10n.string("copy_clipboard"))
    }
}

struct CodeBlockBox: View {
    let title: String?
    let displayText: String
    let byteCount: UInt64
    var trailingSubtitle: String? = nil
    var borderColor: Color? = nil
    var isBinaryPreview = false
    var binaryPreviewShownBytes = 0
    var contentHeight: CGFloat? = nil

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            if title != nil || trailingSubtitle != nil {
                HStack(spacing: 6) {
                    if let title {
                        Text(title)
                            .font(.caption.weight(.medium))
                            .foregroundStyle(.secondary)
                    }
                    Text(String(format: L10n.string("bytes_count_format"), byteCount))
                        .font(.caption2.monospacedDigit())
                        .foregroundStyle(.tertiary)
                    if let trailingSubtitle, !trailingSubtitle.isEmpty {
                        Text("·")
                            .font(.caption2)
                            .foregroundStyle(.tertiary)
                        Text(trailingSubtitle)
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                    }
                    Spacer(minLength: 4)
                    CopyTextButton(text: displayText)
                }
            }
            Group {
                if let contentHeight {
                    ScrollView { content }
                        .frame(height: contentHeight)
                } else {
                    content
                }
            }
            .background(Color(nsColor: .textBackgroundColor), in: RoundedRectangle(cornerRadius: 6))
            .overlay(
                RoundedRectangle(cornerRadius: 6)
                    .strokeBorder(borderColor ?? Color(nsColor: .separatorColor).opacity(0.6), lineWidth: 1)
            )

            if isBinaryPreview {
                Text(String(format: L10n.string("output_binary_preview"), binaryPreviewShownBytes, byteCount))
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
    }

    private var content: some View {
        Group {
            if byteCount == 0 {
                Text(L10n.string("empty_bytes"))
                    .font(.caption.italic())
                    .foregroundStyle(.tertiary)
            } else {
                Text(displayText)
                    .font(.system(.callout, design: .monospaced))
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, 8)
        .padding(.vertical, 6)
    }

}
