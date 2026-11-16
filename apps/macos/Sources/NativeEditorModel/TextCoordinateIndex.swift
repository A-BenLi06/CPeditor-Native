import Foundation

/// 在同一不可变文本内映射 Unicode 标量边界；编辑额外要求字素簇边界。
public struct TextCoordinateIndex: Sendable {
    public let text: String
    private let byteByUTF16: [Int: Int]

    public init(_ text: String) throws {
        self.text = text
        var map = [0: 0]
        var byte = 0
        var unit = 0
        for scalar in text.unicodeScalars {
            byte += scalar.utf8.count
            unit += scalar.utf16.count
            map[unit] = byte
        }
        byteByUTF16 = map
    }

    public func utf8Range(for range: NSRange) throws -> Range<Int> {
        guard range.location >= 0, range.length >= 0,
              range.location <= text.utf16.count,
              range.length <= text.utf16.count - range.location,
              let start = byteByUTF16[range.location],
              let end = byteByUTF16[range.location + range.length] else {
            throw DocumentError.invalidRange
        }
        return start..<end
    }

    public func characterRange(_ range: NSRange) throws -> Range<String.Index> {
        _ = try utf8Range(for: range)
        guard let converted = Range(range, in: text),
              (converted.lowerBound == text.endIndex || text.indices.contains(converted.lowerBound)),
              (converted.upperBound == text.endIndex || text.indices.contains(converted.upperBound)) else {
            throw DocumentError.invalidRange
        }
        return converted
    }
}
