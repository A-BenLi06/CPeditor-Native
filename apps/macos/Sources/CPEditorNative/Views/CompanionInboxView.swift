import SwiftUI
import NativeCoreBridge
import NativeEditorModel

private enum CompanionInboxFilter: String, CaseIterable, Identifiable {
    case all
    case pending
    case applied

    var id: String { rawValue }
}

struct CompanionInboxView: View {
    @Bindable var controller: CompanionController
    @Environment(\.dismiss) private var dismiss
    @State private var filter: CompanionInboxFilter = .all

    private var filteredItems: [CompanionItem] {
        switch filter {
        case .all: return controller.items
        case .pending: return controller.items.filter { $0.state == .pending }
        case .applied: return controller.items.filter { $0.state == .applied }
        }
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 10) {
                Label(L10n.string("companion_title"), systemImage: "tray.and.arrow.down.fill")
                    .font(.headline)

                if !controller.items.isEmpty {
                    Picker("", selection: $filter) {
                        Text(String(format: L10n.string("companion_filter_all"), controller.items.count))
                            .tag(CompanionInboxFilter.all)
                        Text(String(format: L10n.string("companion_filter_pending"), controller.pendingCount))
                            .tag(CompanionInboxFilter.pending)
                        Text(String(format: L10n.string("companion_filter_applied"), controller.appliedCount))
                            .tag(CompanionInboxFilter.applied)
                    }
                    .pickerStyle(.segmented)
                    .labelsHidden()
                    .controlSize(.small)
                    .fixedSize()
                }

                Spacer(minLength: 8)

                if controller.isOperating {
                    ProgressView()
                        .controlSize(.small)
                }

                Button {
                    Task { await controller.importPanel() }
                } label: {
                    Label(L10n.string("companion_import_json"), systemImage: "square.and.arrow.down")
                }
                .controlSize(.small)
                .disabled(controller.isOperating || controller.paths == nil)

                Button {
                    Task { await controller.refresh() }
                } label: {
                    Label(L10n.string("companion_refresh"), systemImage: "arrow.clockwise")
                }
                .controlSize(.small)
                .disabled(controller.isOperating || controller.paths == nil)

                Button(L10n.string("done")) {
                    dismiss()
                }
                .controlSize(.small)
                .keyboardShortcut(.cancelAction)
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 11)
            .background(.bar)

            Divider()

            if let notice = controller.notice {
                NoticeBannerView(kind: .info, message: notice)
            }

            HSplitView {
                VStack(spacing: 0) {
                    if filteredItems.isEmpty {
                        VStack(spacing: 10) {
                            Image(systemName: "tray")
                                .font(.title2)
                                .foregroundStyle(.tertiary)
                            Text(L10n.string(controller.items.isEmpty ? "companion_list_empty" : "companion_list_filter_empty"))
                                .font(.subheadline.weight(.medium))
                                .foregroundStyle(.secondary)
                            if controller.items.isEmpty {
                                Button(L10n.string("companion_import_json")) {
                                    Task { await controller.importPanel() }
                                }
                                .buttonStyle(.borderedProminent)
                                .controlSize(.small)
                                .disabled(controller.isOperating || controller.paths == nil)
                            }
                        }
                        .padding(16)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                    } else {
                        List(selection: $controller.selection) {
                            ForEach(filteredItems) { item in
                                CompanionInboxRow(
                                    item: item,
                                    preview: controller.preview(for: item),
                                    isSelected: controller.selection == item.id
                                )
                                .tag(item.id)
                            }
                        }
                        .listStyle(.sidebar)
                    }

                    if controller.hasMore {
                        Divider()
                        Button(L10n.string("companion_more")) {
                            Task { await controller.loadMore() }
                        }
                        .controlSize(.small)
                        .disabled(controller.isOperating)
                        .padding(8)
                    }
                }
                .frame(minWidth: 230, idealWidth: 270, maxHeight: .infinity)

                if let item = controller.selectedItem, let preview = controller.preview(for: item) {
                    CompanionDetailPane(controller: controller, item: item, preview: preview)
                        .frame(minWidth: 420, maxWidth: .infinity, maxHeight: .infinity)
                } else {
                    ContentUnavailableView(
                        L10n.string("companion_empty"),
                        systemImage: "tray",
                        description: Text(L10n.string("companion_empty_description"))
                    )
                    .padding()
                    .frame(minWidth: 420, maxWidth: .infinity, maxHeight: .infinity)
                }
            }
        }
        .frame(minWidth: 720, idealWidth: 900, minHeight: 460, idealHeight: 640)
        .interactiveDismissDisabled(controller.isOperating)
        .onChange(of: filter) { _, _ in
            if let selection = controller.selection, filteredItems.contains(where: { $0.id == selection }) {
                return
            }
            controller.selection = filteredItems.first?.id
        }
    }
}

private struct CompanionInboxRow: View {
    let item: CompanionItem
    let preview: CompanionPreview?
    var isSelected = false

    private var sampleCountText: String? {
        guard let count = preview?.tests.count else { return nil }
        if count == 1 { return L10n.string("companion_samples_count_single") }
        return String(format: L10n.string("companion_samples_count_plural"), count)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            Text(preview?.name ?? item.importID.rawValue)
                .font(.body.weight(item.state == .pending ? .semibold : .regular))
                .lineLimit(2)

            if let group = preview?.group, !group.isEmpty {
                Text(group)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }

            HStack(spacing: 5) {
                CompanionStatePill(state: item.state, prominentSelection: isSelected)
                if item.previousImportID != nil {
                    Text(L10n.string("companion_update"))
                        .font(.caption2.weight(.medium))
                        .foregroundStyle(isSelected ? .white : .purple)
                        .padding(.horizontal, 5)
                        .padding(.vertical, 1)
                        .background((isSelected ? Color.white : Color.purple).opacity(isSelected ? 0.22 : 0.12), in: Capsule())
                }
                if preview?.requiresSpecialIOMode == true {
                    Text(L10n.string("companion_non_stdio_badge"))
                        .font(.caption2.weight(.medium))
                        .foregroundStyle(isSelected ? .white : .orange)
                        .padding(.horizontal, 5)
                        .padding(.vertical, 1)
                        .background((isSelected ? Color.white : Color.orange).opacity(isSelected ? 0.22 : 0.12), in: Capsule())
                }
                Spacer(minLength: 2)
                if let sampleCountText {
                    Text(sampleCountText)
                        .font(.caption2.monospacedDigit())
                        .foregroundStyle(isSelected ? .white.opacity(0.85) : .secondary)
                }
            }
        }
        .padding(.vertical, 3)
    }
}

private struct CompanionStatePill: View {
    let state: CompanionState
    var prominentSelection = false

    var body: some View {
        let isPending = state == .pending
        let tint: Color = isPending ? .accentColor : .green
        HStack(spacing: 3) {
            Image(systemName: isPending ? "circle.dotted" : "checkmark.circle.fill")
                .font(.system(size: 9))
            Text(L10n.string(isPending ? "companion_pending" : "companion_applied_state"))
                .font(.caption2.weight(.semibold))
        }
        .foregroundStyle(prominentSelection ? .white : tint)
        .padding(.horizontal, 6)
        .padding(.vertical, 1.5)
        .background(prominentSelection ? Color.white.opacity(0.22) : tint.opacity(0.14), in: Capsule())
    }
}

private struct CompanionDetailPane: View {
    @Bindable var controller: CompanionController
    let item: CompanionItem
    let preview: CompanionPreview

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            VStack(alignment: .leading, spacing: 8) {
                HStack(alignment: .top, spacing: 12) {
                    VStack(alignment: .leading, spacing: 4) {
                        Text(preview.name)
                            .font(.title2.weight(.semibold))
                            .lineLimit(2)
                        HStack(spacing: 8) {
                            CompanionStatePill(state: item.state)
                            if !preview.group.isEmpty {
                                Text(preview.group)
                                    .font(.subheadline)
                                    .foregroundStyle(.secondary)
                                    .lineLimit(1)
                            }
                        }
                    }

                    Spacer(minLength: 8)

                    if item.state == .pending {
                        Button {
                            Task { await controller.apply(importID: item.importID) }
                        } label: {
                            Label(L10n.string("companion_apply"), systemImage: "square.and.arrow.down.on.square")
                        }
                        .buttonStyle(.borderedProminent)
                        .disabled(controller.isOperating || controller.paths == nil)
                    } else {
                        Button {
                            Task { await controller.apply(importID: item.importID) }
                        } label: {
                            Label(L10n.string("companion_apply"), systemImage: "checkmark.circle")
                        }
                        .buttonStyle(.bordered)
                        .disabled(controller.isOperating || controller.paths == nil)
                        .help(L10n.string("companion_verify_receipt"))
                    }
                }

                if !preview.url.isEmpty {
                    Text(preview.url)
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(.secondary)
                        .textSelection(.enabled)
                }

                HStack(spacing: 8) {
                    Label(String(format: L10n.string("companion_source_limits"), preview.timeLimit, preview.memoryLimit), systemImage: "speedometer")
                        .font(.caption.monospacedDigit())
                        .padding(.horizontal, 7)
                        .padding(.vertical, 3)
                        .background(.quaternary.opacity(0.5), in: RoundedRectangle(cornerRadius: 5))

                    if let mode = preview.testType {
                        Text(String(format: L10n.string("companion_test_mode"), mode))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    if let mode = preview.input {
                        Text(String(format: L10n.string("companion_input_mode"), mode.type))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    if let mode = preview.output {
                        Text(String(format: L10n.string("companion_output_mode"), mode.type))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }

                if preview.interactive == true {
                    Label(L10n.string("companion_interactive_source"), systemImage: "exclamationmark.triangle.fill")
                        .font(.caption)
                        .foregroundStyle(.orange)
                }

                Text(L10n.string("companion_metadata_notice"))
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
            .padding(14)
            .background(.background)

            Divider()

            ScrollView {
                VStack(alignment: .leading, spacing: 12) {
                    if preview.tests.isEmpty {
                        Text(L10n.string("companion_no_samples"))
                            .font(.callout)
                            .foregroundStyle(.secondary)
                            .padding(.vertical, 12)
                    } else {
                        ForEach(preview.tests.indices, id: \.self) { index in
                            VStack(alignment: .leading, spacing: 8) {
                                Text(String(format: L10n.string("companion_sample"), index + 1))
                                    .font(.subheadline.weight(.semibold))

                                ViewThatFits(in: .horizontal) {
                                    HStack(alignment: .top, spacing: 10) {
                                        sampleBlock(preview.tests[index].input, labelKey: "test_input")
                                            .frame(minWidth: 170, maxWidth: .infinity)
                                        sampleBlock(preview.tests[index].output, labelKey: "test_expected")
                                            .frame(minWidth: 170, maxWidth: .infinity)
                                    }
                                    VStack(alignment: .leading, spacing: 8) {
                                        sampleBlock(preview.tests[index].input, labelKey: "test_input")
                                        sampleBlock(preview.tests[index].output, labelKey: "test_expected")
                                    }
                                }
                            }
                            .padding(10)
                            .background(Color(nsColor: .controlBackgroundColor).opacity(0.55), in: RoundedRectangle(cornerRadius: 8))
                            .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(.separator, lineWidth: 1))
                        }
                    }
                }
                .padding(14)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
    }

    @ViewBuilder
    private func sampleBlock(_ text: String, labelKey: String) -> some View {
        let bytes = WireBytes(text)
        let limit = controller.configuration?.previewBytes ?? 0
        let display = bytes.canDisplayAsText
            ? String(decoding: bytes.data.prefix(limit), as: UTF8.self)
            : bytes.hexPreview(maximumBytes: limit)
        VStack(alignment: .leading, spacing: 3) {
            CodeBlockBox(
                title: L10n.string(labelKey),
                displayText: display,
                byteCount: UInt64(bytes.data.count),
                isBinaryPreview: !bytes.canDisplayAsText,
                binaryPreviewShownBytes: min(bytes.data.count, limit)
            )
            if bytes.data.count > limit {
                Text(L10n.string("companion_preview_truncated"))
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
    }
}
