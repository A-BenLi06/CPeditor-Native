import SwiftUI

struct ConfigurationJSONSheet: View {
    @Environment(\.dismiss) private var dismiss
    private let kind: ConfigurationJSONDraft.Kind
    private let apply: (String) throws -> Void
    @State private var jsonText: String
    @State private var errorMessage: String?

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
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            VStack(alignment: .leading, spacing: 4) {
                Label(title, systemImage: iconName)
                    .font(.headline)
                Text(subtitle)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 12)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(.bar)

            Divider()

            if let errorMessage {
                NoticeBannerView(kind: .error, message: errorMessage)
            }

            VStack(spacing: 0) {
                TextEditor(text: $jsonText)
                    .font(.system(.body, design: .monospaced))
                    .scrollContentBackground(.hidden)
                    .padding(8)
                    .background(Color(nsColor: .textBackgroundColor), in: RoundedRectangle(cornerRadius: 8))
                    .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(.separator, lineWidth: 1))
                    .accessibilityLabel(title)
            }
            .padding(14)

            Divider()

            HStack {
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
        .frame(minWidth: 620, idealWidth: 700, minHeight: 440, idealHeight: kind == .checker ? 540 : 500)
    }

    private func applyDraft() {
        do {
            try apply(jsonText)
            dismiss()
        } catch {
            errorMessage = L10n.describe(error)
        }
    }
}
