/// 状态栏沿用 LF 分行和 UTF-16 列计数，不参与编辑范围／字素簇校验。
public struct TextPosition: Equatable, Sendable {
    public let line: Int
    public let column: Int
}

/// 仅为含 LF 的 UTF-16 块保存位集和累计行数，不保留第二份源码。
public struct TextLineIndex: Sendable {
    private struct Segment: Sendable {
        let utf16Offset: Int
        let lineCountBefore: Int
        let lineBreaks: UInt64
    }
    private static let unitsPerSegment = UInt64.bitWidth
    private let segments: [Segment]
    private let utf16Count: Int

    public init(_ text: String) {
        var entries: [Segment] = []
        var offset = 0
        var lineCount = 0
        var bits: UInt64 = 0
        for unit in text.utf16 {
            if unit == 10 { bits |= UInt64(1) << (offset % Self.unitsPerSegment) }
            offset += 1
            if offset % Self.unitsPerSegment == 0, bits != 0 {
                entries.append(Segment(utf16Offset: offset - Self.unitsPerSegment,
                                       lineCountBefore: lineCount, lineBreaks: bits))
                lineCount += bits.nonzeroBitCount
                bits = 0
            }
        }
        if bits != 0 {
            entries.append(Segment(utf16Offset: offset - offset % Self.unitsPerSegment,
                                   lineCountBefore: lineCount, lineBreaks: bits))
        }
        segments = entries
        utf16Count = offset
    }

    public func position(atUTF16Offset requestedOffset: Int) -> TextPosition {
        let offset = min(max(0, requestedOffset), utf16Count)
        var lower = 0
        var upper = segments.count
        // prefix 不包括 offset 处的单元；块起点与 offset 相同时选前一块。
        while lower < upper {
            let middle = lower + (upper - lower) / 2
            if segments[middle].utf16Offset < offset { lower = middle + 1 }
            else { upper = middle }
        }
        guard lower > 0 else { return TextPosition(line: 1, column: offset + 1) }
        let index = lower - 1
        let segment = segments[index]
        let units = offset - segment.utf16Offset
        let mask = units >= Self.unitsPerSegment ? UInt64.max : (UInt64(1) << units) - 1
        let preceding = segment.lineBreaks & mask
        if preceding != 0 {
            let lastLF = segment.utf16Offset + Self.unitsPerSegment - 1 - preceding.leadingZeroBitCount
            return TextPosition(line: segment.lineCountBefore + preceding.nonzeroBitCount + 1,
                                column: offset - lastLF)
        }
        guard index > 0 else { return TextPosition(line: 1, column: offset + 1) }
        let previous = segments[index - 1]
        let lastLF = previous.utf16Offset + Self.unitsPerSegment - 1 - previous.lineBreaks.leadingZeroBitCount
        return TextPosition(line: segment.lineCountBefore + 1, column: offset - lastLF)
    }
}
