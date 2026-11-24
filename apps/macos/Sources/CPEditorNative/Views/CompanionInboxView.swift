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

            HStack(spacing: 0) {
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
                        ScrollView {
                            LazyVStack(spacing: 4) {
                                ForEach(filteredItems) { item in
                                    CompanionInboxRow(
                                        item: item,
                                        preview: controller.preview(for: item),
                                        isSelected: controller.selection == item.id
                                    )
                                    .contentShape(RoundedRectangle(cornerRadius: 7))
                                    .onTapGesture {
                                        controller.selection = item.id
                                    }
                                }
                            }
                            .padding(8)
                        }
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
                .frame(width: 265)
                .frame(maxHeight: .infinity)
                .background(Color(nsColor: .controlBackgroundColor).opacity(0.45))

                Divider()

                if let item = controller.selectedItem, let preview = controller.preview(for: item) {
                    CompanionDetailPane(controller: controller, item: item, preview: preview)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                } else {
                    ContentUnavailableView(
                        L10n.string("companion_empty"),
                        systemImage: "tray",
                        description: Text(L10n.string("companion_empty_description"))
                    )
                    .padding()
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
            }
        }
        .frame(minWidth: 740, idealWidth: 900, minHeight: 460, idealHeight: 620)
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
    let isSelected: Bool

    private var sampleCountText: String? {
        guard let count = preview?.tests.count else { return nil }
        if count == 1 { return L10n.string("companion_samples_count_single") }
        return String(format: L10n.string("companion_samples_count_plural"), count)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            Text(preview?.name ?? item.importID.rawValue)
                .font(.body.weight(item.state == .pending || isSelected ? .semibold : .regular))
                .foregroundStyle(.primary)
                .lineLimit(2)

            if let group = preview?.group, !group.isEmpty {
                Text(group)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }

            HStack(spacing: 5) {
                CompanionStatePill(state: item.state)
                if item.previousImportID != nil {
                    Text(L10n.string("companion_update"))
                        .font(.caption2.weight(.medium))
                        .foregroundStyle(.purple)
                        .padding(.horizontal, 5)
                        .padding(.vertical, 1)
                        .background(Color.purple.opacity(0.12), in: Capsule())
                }
                if preview?.requiresSpecialIOMode == true {
                    Text(L10n.string("companion_non_stdio_badge"))
                        .font(.caption2.weight(.medium))
                        .foregroundStyle(.orange)
                        .padding(.horizontal, 5)
                        .padding(.vertical, 1)
                        .background(Color.orange.opacity(0.12), in: Capsule())
                }
                Spacer(minLength: 2)
                if let sampleCountText {
                    Text(sampleCountText)
                        .font(.caption2.monospacedDigit())
                        .foregroundStyle(.secondary)
                }
            }
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 8)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background {
            if isSelected {
                RoundedRectangle(cornerRadius: 7)
                    .fill(Color.accentColor.opacity(0.14))
            }
        }
        .overlay {
            if isSelected {
                RoundedRectangle(cornerRadius: 7)
                    .strokeBorder(Color.accentColor.opacity(0.35), lineWidth: 1)
            }
        }
    }
}

private struct CompanionStatePill: View {
    let state: CompanionState

    var body: some View {
        let isPending = state == .pending
        let tint: Color = isPending ? .accentColor : .green
        HStack(spacing: 3) {
            Image(systemName: isPending ? "circle.dotted" : "checkmark.circle.fill")
                .font(.system(size: 9))
            Text(L10n.string(isPending ? "companion_pending" : "companion_applied_state"))
                .font(.caption2.weight(.semibold))
        }
        .foregroundStyle(tint)
        .padding(.horizontal, 6)
        .padding(.vertical, 1.5)
        .background(tint.opacity(0.14), in: Capsule())
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
                if preview.tests.isEmpty {
                    Text(L10n.string("companion_no_samples"))
                        .font(.callout)
                        .foregroundStyle(.secondary)
                        .padding(14)
                        .frame(maxWidth: .infinity, alignment: .leading)
                } else {
                    LazyVStack(alignment: .leading, spacing: 0) {
                        ForEach(preview.tests.indices, id: \.self) { index in
                            let inputData = samplePreviewData(preview.tests[index].input)
                            let expectedData = samplePreviewData(preview.tests[index].output)

                            VStack(alignment: .leading, spacing: 0) {
                                VStack(alignment: .leading, spacing: 8) {
                                    Text(String(format: L10n.string("companion_sample"), index + 1))
                                        .font(.subheadline.weight(.semibold))

                                    ViewThatFits(in: .horizontal) {
                                        HStack(alignment: .top, spacing: 0) {
                                            samplePane(inputData, labelKey: "test_input")
                                                .frame(minWidth: 160, idealWidth: 160, maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
                                            Divider()
                                            samplePane(expectedData, labelKey: "test_expected")
                                                .frame(minWidth: 160, idealWidth: 160, maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
                                        }
                                        .fixedSize(horizontal: false, vertical: true)

                                        VStack(alignment: .leading, spacing: 0) {
                                            samplePane(inputData, labelKey: "test_input")
                                            Divider()
                                            samplePane(expectedData, labelKey: "test_expected")
                                        }
                                    }
                                    .panelSurfaceWell()

                                    sampleFootnotes(input: inputData, expected: expectedData)
                                }
                                .padding(.horizontal, 14)
                                .padding(.vertical, 10)

                                Divider()
                            }
                        }
                    }
                }
            }
        }
    }

    private struct SamplePreviewData {
        let displayText: String
        let byteCount: UInt64
        let isBinaryPreview: Bool
        let shownBytes: Int
        let isTruncated: Bool
    }

    private func samplePreviewData(_ text: String) -> SamplePreviewData {
        let bytes = WireBytes(text)
        let limit = controller.configuration?.previewBytes ?? 0
        let canDisplay = bytes.canDisplayAsText
        let display = canDisplay
            ? String(decoding: bytes.data.prefix(limit), as: UTF8.self)
            : bytes.hexPreview(maximumBytes: limit)
        return SamplePreviewData(
            displayText: display,
            byteCount: UInt64(bytes.data.count),
            isBinaryPreview: !canDisplay,
            shownBytes: min(bytes.data.count, limit),
            isTruncated: bytes.data.count > limit
        )
    }

    private func samplePane(_ data: SamplePreviewData, labelKey: String) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            PanelCellHeaderBar(
                title: L10n.string(labelKey),
                byteCount: data.byteCount
            ) {
                CopyTextButton(text: data.displayText)
            }
            PanelBytesContent(
                displayText: data.displayText,
                byteCount: data.byteCount,
                verticalPadding: 6
            )
        }
    }

    @ViewBuilder
    private func sampleFootnotes(input: SamplePreviewData, expected: SamplePreviewData) -> some View {
        if input.isBinaryPreview || expected.isBinaryPreview || input.isTruncated || expected.isTruncated {
            VStack(alignment: .leading, spacing: 2) {
                if input.isBinaryPreview {
                    Text("\(L10n.string("test_input")): \(String(format: L10n.string("output_binary_preview"), input.shownBytes, input.byteCount))")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
                if expected.isBinaryPreview {
                    Text("\(L10n.string("test_expected")): \(String(format: L10n.string("output_binary_preview"), expected.shownBytes, expected.byteCount))")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
                if input.isTruncated || expected.isTruncated {
                    Text(L10n.string("companion_preview_truncated"))
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
            }
        }
    }
}
