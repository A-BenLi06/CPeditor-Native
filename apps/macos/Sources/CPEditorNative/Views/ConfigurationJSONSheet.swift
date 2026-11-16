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

    init(draft: ConfigurationJSONDraft, apply: @escaping (String) throws -> Void) {
        kind = draft.kind
        self.apply = apply
        _jsonText = State(initialValue: draft.jsonText)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title).font(.headline)
            if kind == .checker {
                Text(L10n.string("checker_configuration_help")).font(.caption).foregroundStyle(.secondary)
            }
            TextEditor(text: $jsonText).font(.system(.body, design: .monospaced))
                .accessibilityLabel(title)
            if let errorMessage {
                Text(errorMessage).font(.caption).foregroundStyle(.red).textSelection(.enabled)
            }
            HStack {
                Spacer()
                Button(L10n.string("cancel"), role: .cancel) { dismiss() }.keyboardShortcut(.cancelAction)
                Button(L10n.string("apply"), action: applyDraft).keyboardShortcut(.defaultAction)
            }
        }.padding().frame(width: 680, height: kind == .checker ? 520 : 480)
    }

    private func applyDraft() {
        do { try apply(jsonText); dismiss() }
        catch { errorMessage = L10n.describe(error) }
    }
}
