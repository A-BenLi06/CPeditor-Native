import SwiftUI
import NativeCoreBridge

struct CompanionInboxView: View {
    @Bindable var controller: CompanionController
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text(L10n.string("companion_title")).font(.headline)
                Spacer()
                if controller.isOperating { ProgressView().controlSize(.small) }
                Button(L10n.string("companion_import_json")) { Task { await controller.importPanel() } }
                    .disabled(controller.isOperating || controller.paths == nil)
                Button(L10n.string("companion_refresh")) { Task { await controller.refresh() } }
                    .disabled(controller.isOperating || controller.paths == nil)
                Button(L10n.string("done")) { dismiss() }.keyboardShortcut(.cancelAction)
            }.padding()
            if let notice = controller.notice {
                Text(notice).font(.caption).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading).padding(.horizontal).padding(.bottom, 10)
            }
            HSplitView {
                VStack {
                    List(selection: $controller.selection) {
                        ForEach(controller.items) { item in
                            VStack(alignment: .leading, spacing: 4) {
                                Text(preview(item)?.name ?? item.importID.rawValue).lineLimit(2)
                                HStack {
                                    Text(item.state == .pending ? L10n.string("companion_pending") : L10n.string("companion_applied_state"))
                                    if item.previousImportID != nil { Text(L10n.string("companion_update")) }
                                }.font(.caption).foregroundStyle(.secondary)
                            }.tag(item.id)
                        }
                    }
                    if controller.hasMore {
                        Button(L10n.string("companion_more")) { Task { await controller.loadMore() } }.disabled(controller.isOperating).padding(8)
                    }
                }.frame(minWidth: 190, idealWidth: 240)
                ScrollView {
                    if let item = controller.selectedItem, let preview = preview(item) {
                        VStack(alignment: .leading, spacing: 12) {
                            Text(preview.name).font(.title2)
                            Text(preview.group).foregroundStyle(.secondary)
                            Text(preview.url).font(.caption).textSelection(.enabled)
                            Text(String(format: L10n.string("companion_source_limits"), preview.timeLimit, preview.memoryLimit)).font(.caption)
                            Text(L10n.string("companion_metadata_notice")).font(.caption).foregroundStyle(.secondary)
                            if let interactive = preview.interactive, interactive { Text(L10n.string("companion_interactive_source")) }
                            if let mode = preview.testType { Text(String(format: L10n.string("companion_test_mode"), mode)) }
                            if let mode = preview.input { Text(String(format: L10n.string("companion_input_mode"), mode.type)) }
                            if let mode = preview.output { Text(String(format: L10n.string("companion_output_mode"), mode.type)) }
                            Divider()
                            ForEach(preview.tests.indices, id: \.self) { index in
                                VStack(alignment: .leading, spacing: 6) {
                                    Text(String(format: L10n.string("companion_sample"), index + 1)).font(.headline)
                                    sampleText(preview.tests[index].input, label: "test_input")
                                    sampleText(preview.tests[index].output, label: "test_expected")
                                }
                            }
                            if preview.tests.isEmpty { Text(L10n.string("companion_no_samples")).foregroundStyle(.secondary) }
                            Button(L10n.string("companion_apply")) { Task { await controller.apply(importID: item.importID) } }
                                .disabled(controller.isOperating || controller.paths == nil)
                        }.padding().frame(maxWidth: .infinity, alignment: .leading)
                    } else {
                        ContentUnavailableView(L10n.string("companion_empty"), systemImage: "tray", description: Text(L10n.string("companion_empty_description"))).padding()
                    }
                }.frame(minWidth: 380)
            }
        }.frame(minWidth: 680, idealWidth: 850, minHeight: 420, idealHeight: 620)
        .interactiveDismissDisabled(controller.isOperating)
    }
    private func preview(_ item: CompanionItem) -> CompanionPreview? {
        try? JSONDecoder().decode(CompanionPreview.self, from: Data(item.originalJSON.utf8))
    }
    @ViewBuilder private func sampleText(_ text: String, label: String) -> some View {
        let bytes = WireBytes(text); let limit = controller.configuration?.previewBytes ?? 0
        VStack(alignment: .leading, spacing: 3) {
            Text(L10n.string(label)).font(.caption).foregroundStyle(.secondary)
            Text(bytes.canDisplayAsText ? String(decoding: bytes.data.prefix(limit), as: UTF8.self) : bytes.hexPreview(maximumBytes: limit))
                .font(.system(.caption, design: .monospaced)).textSelection(.enabled)
            if bytes.data.count > limit { Text(L10n.string("companion_preview_truncated")).font(.caption2).foregroundStyle(.secondary) }
        }
    }
}
