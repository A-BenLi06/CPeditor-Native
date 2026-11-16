import Foundation

/// Core 的不透明 UTF-8 标识符按字节比较，不能采用 Swift String 的规范等价。
public struct RawIdentifier: RawRepresentable, Codable, Hashable, Sendable, CustomStringConvertible {
    public let rawValue: String
    public init(_ value: String) { rawValue = value }
    public init(rawValue: String) { self.rawValue = rawValue }
    public var description: String { rawValue }
    public var isEmpty: Bool { rawValue.isEmpty }
    public static func == (left: Self, right: Self) -> Bool { left.rawValue.utf8.elementsEqual(right.rawValue.utf8) }
    public func hash(into hasher: inout Hasher) { for byte in rawValue.utf8 { hasher.combine(byte) } }
    public init(from decoder: Decoder) throws { rawValue = try decoder.singleValueContainer().decode(String.self) }
    public func encode(to encoder: Encoder) throws { var box = encoder.singleValueContainer(); try box.encode(rawValue) }
}
