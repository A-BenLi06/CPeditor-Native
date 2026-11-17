import Foundation
import Dispatch
import NativeEditorModel

public struct CheckerConfigurationError: Error, LocalizedError, Sendable {
    public let localizationKey: String
    public let context: String?
    public var errorDescription: String? {
        let format = NSLocalizedString(localizationKey, bundle: .module, comment: "")
        return context.map { String(format: format, $0) } ?? format
    }
    public init(_ localizationKey: String, context: String? = nil) { self.localizationKey = localizationKey; self.context = context }
}

/// ABI 1 的 external_checker 版本 1；字符串原件供原生偏好无损保存。
public struct CheckerConfiguration: Encodable, Sendable, Equatable {
    public let originalJSON: String
    public let executable: String
    public let arguments: [String]
    private let value: CheckerJSON
    public static let maximumJSONBytes = 1024 * 1024
    private enum Field: String {
        case schemaVersion = "schema_version", command, limits, accepted = "accepted_exit_codes"
        case wrong = "wrong_answer_exit_codes", inherit = "inherit_environment", environment
        case executable, arguments, name, value
        case spawn = "spawn_timeout_ms", wall = "wall_timeout_ms", grace = "terminate_grace_ms", drain = "drain_timeout_ms"
        case stdout = "stdout_bytes", stderr = "stderr_bytes", combined = "combined_bytes"
    }
    private enum Variable: String, CaseIterable { case input, actual, expected, jobDir = "job_dir", sourceDir = "source_dir" }
    public init(jsonText: String) throws {
        guard jsonText.utf8.count <= Self.maximumJSONBytes else { throw CheckerConfigurationError("checker_error_json_limit") }
        var parser = CheckerJSONParser(bytes: Array(jsonText.utf8))
        let root = try parser.parse()
        let object = try root.object(allowed: [Field.schemaVersion, .command, .limits, .accepted, .wrong, .inherit, .environment].map(\.rawValue))
        guard try object.required(Field.schemaVersion.rawValue).integer() == 1 else { throw CheckerConfigurationError("checker_error_schema") }
        let command = try object.required(Field.command.rawValue).object(allowed: [Field.executable.rawValue, Field.arguments.rawValue])
        let executable = try command.required(Field.executable.rawValue).string()
        let arguments = try command.required(Field.arguments.rawValue).array().map { try $0.string() }
        guard !executable.isEmpty, arguments.count <= 128 else { throw CheckerConfigurationError("checker_error_command") }
        var commandBytes = executable.utf8.count
        try Self.template(executable)
        for argument in arguments { try Self.template(argument); commandBytes += 1 + argument.utf8.count }
        let accepted = try object[Field.accepted.rawValue].map { try $0.array().map { try $0.integer(maximum: 255) } } ?? [0]
        let wrong = try object[Field.wrong.rawValue].map { try $0.array().map { try $0.integer(maximum: 255) } } ?? [1, 2]
        guard !accepted.isEmpty, accepted.count <= 256, wrong.count <= 256,
              Set(accepted + wrong).count == accepted.count + wrong.count else { throw CheckerConfigurationError("checker_error_exit_codes") }
        if let inherit = object[Field.inherit.rawValue] { try inherit.boolean() }
        if let environment = object[Field.environment.rawValue] {
            let pairs = try environment.array()
            guard pairs.count <= 256 else { throw CheckerConfigurationError("checker_error_environment_limit") }
            var names: Set<RawIdentifier> = []
            for pair in pairs {
                let fields = try pair.object(allowed: [Field.name.rawValue, Field.value.rawValue])
                let name = try fields.required(Field.name.rawValue).string()
                let value = try fields.required(Field.value.rawValue).string()
                try Self.text(name); try Self.text(value)
                let folded = String(decoding: name.utf8.map { (97...122).contains($0) ? $0 - 32 : $0 }, as: UTF8.self)
                guard !name.isEmpty, !name.utf8.contains(61), names.insert(RawIdentifier(folded)).inserted else { throw CheckerConfigurationError("checker_error_environment_name") }
                commandBytes += name.utf8.count + value.utf8.count
            }
        }
        guard commandBytes <= 64 * 1024 else { throw CheckerConfigurationError("checker_error_command_limit") }
        var durations: [UInt64] = [5000, 2000, 100, 500]
        if let limits = object[Field.limits.rawValue] {
            let limits = try limits.object(allowed: [Field.spawn, .wall, .grace, .drain, .stdout, .stderr, .combined].map(\.rawValue))
            for (index, field) in [Field.spawn, .wall, .grace, .drain].enumerated() {
                if let limit = limits[field.rawValue] { durations[index] = try limit.integer(maximum: UInt64(Int64.max)) }
            }
            for field in [Field.stdout, .stderr, .combined] {
                if let limit = limits[field.rawValue] { _ = try limit.integer(maximum: UInt64(Int64.max)) }
            }
        }
        guard durations[0] > 0, durations[1] > 0, durations[3] > 0 else { throw CheckerConfigurationError("checker_error_deadlines") }
        let now = min(DispatchTime.now().uptimeNanoseconds, UInt64(Int64.max))
        var available = (UInt64(Int64.max) - now) / 1_000_000
        for duration in durations {
            guard duration <= available else { throw CheckerConfigurationError("checker_error_clock") }
            available -= duration
        }
        originalJSON = jsonText; self.executable = executable; self.arguments = arguments; value = root
    }
    public func encode(to encoder: Encoder) throws { try value.encode(to: encoder) }
    public func jsonText() -> String { originalJSON }
    public static func == (a: Self, b: Self) -> Bool { a.originalJSON.utf8.elementsEqual(b.originalJSON.utf8) }
    private static func text(_ text: String) throws { if text.utf8.contains(0) { throw CheckerConfigurationError("checker_error_nul") } }
    private static func template(_ text: String) throws {
        try self.text(text)
        let bytes = Array(text.utf8)
        var position = 0
        while position < bytes.count {
            if bytes[position] == 36, position + 1 < bytes.count, bytes[position + 1] == 123 {
                guard let end = bytes[(position + 2)...].firstIndex(of: 125),
                      Variable(rawValue: String(decoding: bytes[(position + 2)..<end], as: UTF8.self)) != nil else { throw CheckerConfigurationError("checker_error_placeholder") }
                position = end + 1
            } else { position += 1 }
        }
    }
}

indirect enum CheckerJSON: Encodable, Sendable {
    case object([RawIdentifier: CheckerJSON]), array([CheckerJSON]), string(String), integer(UInt64), invalidNumber, bool(Bool), null
    func object(allowed: [String]) throws -> [String: CheckerJSON] {
        guard case .object(let fields) = self else { throw CheckerConfigurationError("checker_error_object") }
        var result: [String: CheckerJSON] = [:]
        for (key, value) in fields {
            guard allowed.contains(where: { key.rawValue.utf8.elementsEqual($0.utf8) }) else { throw CheckerConfigurationError("checker_error_unknown_field", context: String(key.rawValue.prefix(80))) }
            result[key.rawValue] = value
        }
        return result
    }
    func array() throws -> [CheckerJSON] { guard case .array(let value) = self else { throw CheckerConfigurationError("checker_error_array") }; return value }
    func string() throws -> String { guard case .string(let value) = self else { throw CheckerConfigurationError("checker_error_string") }; return value }
    func integer(maximum: UInt64 = .max) throws -> UInt64 {
        guard case .integer(let value) = self, value <= maximum else { throw CheckerConfigurationError("checker_error_integer") }; return value
    }
    func boolean() throws { guard case .bool = self else { throw CheckerConfigurationError("checker_error_boolean") } }
    func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        switch self {
        case .object(let fields): try container.encode(Dictionary(uniqueKeysWithValues: fields.map { ($0.key.rawValue, $0.value) }))
        case .array(let values): try container.encode(values)
        case .string(let value): try container.encode(value)
        case .integer(let value): try container.encode(value)
        case .bool(let value): try container.encode(value)
        case .null: try container.encodeNil()
        case .invalidNumber: throw CheckerConfigurationError("checker_error_number")
        }
    }
}
private extension Dictionary where Key == String, Value == CheckerJSON {
    func required(_ key: String) throws -> CheckerJSON { guard let value = self[key] else { throw CheckerConfigurationError("checker_error_missing_field", context: key) }; return value }
}

/// 保留整数词法类型并拒绝重复键；Foundation JSONDecoder 对 1.0 和重复键的宽松行为不能用于契约校验。
struct CheckerJSONParser {
    let bytes: [UInt8]
    var cursor = 0
    mutating func parse() throws -> CheckerJSON {
        let value = try value(depth: 0); whitespace()
        guard cursor == bytes.count else { throw invalid() }; return value
    }
    private func invalid() -> CheckerConfigurationError { CheckerConfigurationError("checker_error_json", context: String(cursor)) }
    private mutating func whitespace() { while cursor < bytes.count, [9, 10, 13, 32].contains(bytes[cursor]) { cursor += 1 } }
    private mutating func take(_ byte: UInt8) throws { whitespace(); guard cursor < bytes.count, bytes[cursor] == byte else { throw invalid() }; cursor += 1 }
    private mutating func string() throws -> String {
        whitespace(); let start = cursor; try take(34)
        while cursor < bytes.count {
            let byte = bytes[cursor]; cursor += 1
            if byte == 34 {
                do { return try JSONDecoder().decode(String.self, from: Data(bytes[start..<cursor])) }
                catch { throw invalid() }
            }
            if byte == 92 { guard cursor < bytes.count else { throw invalid() }; cursor += 1 }
            else if byte < 32 { throw invalid() }
        }
        throw invalid()
    }
    private mutating func value(depth: Int) throws -> CheckerJSON {
        whitespace(); guard depth <= 64, cursor < bytes.count else { throw invalid() }
        switch bytes[cursor] {
        case 123:
            cursor += 1; whitespace(); var result: [RawIdentifier: CheckerJSON] = [:]
            if cursor < bytes.count, bytes[cursor] == 125 { cursor += 1; return .object(result) }
            while true {
                let key = RawIdentifier(try string()); try take(58)
                guard result[key] == nil else { throw CheckerConfigurationError("checker_error_duplicate_field", context: String(key.rawValue.prefix(80))) }
                result[key] = try value(depth: depth + 1); whitespace()
                if cursor < bytes.count, bytes[cursor] == 125 { cursor += 1; return .object(result) }
                try take(44)
            }
        case 91:
            cursor += 1; whitespace(); var result: [CheckerJSON] = []
            if cursor < bytes.count, bytes[cursor] == 93 { cursor += 1; return .array(result) }
            while true {
                result.append(try value(depth: depth + 1)); whitespace()
                if cursor < bytes.count, bytes[cursor] == 93 { cursor += 1; return .array(result) }
                try take(44)
            }
        case 34: return .string(try string())
        case 116: try literal("true"); return .bool(true)
        case 102: try literal("false"); return .bool(false)
        case 110: try literal("null"); return .null
        default:
            let start = cursor
            if bytes[cursor] == 45 { cursor += 1 }
            guard cursor < bytes.count else { throw invalid() }
            if bytes[cursor] == 48 { cursor += 1 }
            else {
                guard (49...57).contains(bytes[cursor]) else { throw invalid() }
                while cursor < bytes.count, (48...57).contains(bytes[cursor]) { cursor += 1 }
            }
            var integral = true
            if cursor < bytes.count, bytes[cursor] == 46 {
                integral = false; cursor += 1; let digits = cursor
                while cursor < bytes.count, (48...57).contains(bytes[cursor]) { cursor += 1 }
                guard cursor > digits else { throw invalid() }
            }
            if cursor < bytes.count, [69, 101].contains(bytes[cursor]) {
                integral = false; cursor += 1
                if cursor < bytes.count, [43, 45].contains(bytes[cursor]) { cursor += 1 }
                let digits = cursor
                while cursor < bytes.count, (48...57).contains(bytes[cursor]) { cursor += 1 }
                guard cursor > digits else { throw invalid() }
            }
            let token = String(decoding: bytes[start..<cursor], as: UTF8.self)
            if integral, let number = UInt64(token) { return .integer(number) }
            if integral, let number = Int64(token), number == 0 { return .integer(0) }
            return .invalidNumber
        }
    }
    private mutating func literal(_ literal: String) throws {
        let expected = Array(literal.utf8)
        guard bytes.count - cursor >= expected.count, bytes[cursor..<cursor + expected.count].elementsEqual(expected) else { throw invalid() }
        cursor += expected.count
    }
}
