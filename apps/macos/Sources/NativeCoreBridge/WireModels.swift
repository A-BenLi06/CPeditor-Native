import Foundation
import NativeEditorModel

/// 对未知工具链字段保留原始 JSON，由共享 Core 的配置校验决定是否接受。
public enum JSONValue: Codable, Sendable, Equatable {
    case object([String: JSONValue]), array([JSONValue]), string(String), number(Double), bool(Bool), null
    public init(from decoder: Decoder) throws {
        let box = try decoder.singleValueContainer()
        if box.decodeNil() { self = .null }
        else if let value = try? box.decode(Bool.self) { self = .bool(value) }
        else if let value = try? box.decode(String.self) { self = .string(value) }
        else if let value = try? box.decode([String: Self].self) { self = .object(value) }
        else if let value = try? box.decode([Self].self) { self = .array(value) }
        else { self = .number(try box.decode(Double.self)) }
    }
    public func encode(to encoder: Encoder) throws {
        var box = encoder.singleValueContainer()
        switch self {
        case .object(let value): try box.encode(value)
        case .array(let value): try box.encode(value)
        case .string(let value): try box.encode(value)
        case .number(let value): try box.encode(value)
        case .bool(let value): try box.encode(value)
        case .null: try box.encodeNil()
        }
    }
    public subscript(_ key: String) -> Self? { if case .object(let values) = self { return values[key] }; return nil }
    public var string: String? { if case .string(let value) = self { return value }; return nil }
}

public struct ToolchainProfile: Codable, Sendable, Identifiable, Equatable {
    public let payload: JSONValue
    public enum Field: String { case id, languageID = "language_id", schemaVersion = "schema_version" }
    public var id: RawIdentifier { RawIdentifier(payload[Field.id.rawValue]?.string ?? "") }
    public var languageID: String { payload[Field.languageID.rawValue]?.string ?? "" }
    public init(payload: JSONValue) throws {
        guard payload[Field.schemaVersion.rawValue] == .number(1),
              !(payload[Field.id.rawValue]?.string ?? "").isEmpty,
              !(payload[Field.languageID.rawValue]?.string ?? "").isEmpty else { throw WireError.invalidProfile }
        self.payload = payload
    }
    public init(from decoder: Decoder) throws { try self.init(payload: JSONValue(from: decoder)) }
    public func encode(to encoder: Encoder) throws { try payload.encode(to: encoder) }
    public func jsonText() throws -> String {
        let encoder = JSONEncoder(); encoder.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
        return String(decoding: try encoder.encode(self), as: UTF8.self)
    }
}

public enum WireError: Error, Equatable { case invalidProfile, invalidBase64, unsupportedSchema, malformedTerminal, busy, closed }

public struct WireBytes: Codable, Sendable, Equatable {
    public var data: Data
    public init(_ data: Data) { self.data = data }
    public init(_ text: String) { data = Data(text.utf8) }
    enum CodingKeys: String, CodingKey { case encoding, data }
    public init(from decoder: Decoder) throws {
        if let text = try? decoder.singleValueContainer().decode(String.self) { data = Data(text.utf8); return }
        let box = try decoder.container(keyedBy: CodingKeys.self)
        let encoded = try box.decode(String.self, forKey: .data)
        guard try box.decode(String.self, forKey: .encoding) == "base64",
              let value = Data(base64Encoded: encoded), value.base64EncodedString() == encoded else { throw WireError.invalidBase64 }
        data = value
    }
    public func encode(to encoder: Encoder) throws {
        var box = encoder.container(keyedBy: CodingKeys.self)
        try box.encode("base64", forKey: .encoding)
        try box.encode(data.base64EncodedString(), forKey: .data)
    }
    public var text: String { String(decoding: data, as: UTF8.self) }
    public var isUTF8: Bool { String(data: data, encoding: .utf8) != nil }
}

public struct TestCaseInput: Codable, Sendable, Equatable, Identifiable {
    public var id: RawIdentifier
    public var input: WireBytes
    public var expected: WireBytes?
    public var enabled: Bool
    public init(id: String = UUID().uuidString, input: String = "", expected: String? = "", enabled: Bool = true) {
        self.id = RawIdentifier(id); self.input = WireBytes(input); self.expected = expected.map(WireBytes.init); self.enabled = enabled
    }
    enum CodingKeys: String, CodingKey { case id, input, expected, enabled }
    public init(from decoder: Decoder) throws {
        let box = try decoder.container(keyedBy: CodingKeys.self)
        id = try box.decode(RawIdentifier.self, forKey: .id)
        input = try box.decode(WireBytes.self, forKey: .input)
        expected = try box.decodeIfPresent(WireBytes.self, forKey: .expected)
        enabled = try box.decodeIfPresent(Bool.self, forKey: .enabled) ?? true
    }
}

public enum ComparisonPolicy: String, Codable, CaseIterable, Sendable { case trailing, strict, byte }
public struct BatchRequest: Encodable, Sendable {
    public struct Source: Encodable, Sendable {
        let documentID: RawIdentifier; let revision: UInt64; let text: String; let originalPath: String?
        enum CodingKeys: String, CodingKey { case documentID = "document_id", revision, text, originalPath = "original_path" }
    }
    let schemaVersion = 1
    let source: Source
    let toolchain: ToolchainProfile
    let tests: [TestCaseInput]
    let comparison: ComparisonPolicy
    enum CodingKeys: String, CodingKey { case schemaVersion = "schema_version", source, toolchain, tests, comparison }
    public init(snapshot: SourceSnapshot, originalPath: String?, toolchain: ToolchainProfile,
                tests: [TestCaseInput], comparison: ComparisonPolicy = .trailing) {
        source = Source(documentID: snapshot.documentID, revision: snapshot.revision,
                        text: snapshot.text, originalPath: originalPath)
        self.toolchain = toolchain; self.tests = tests; self.comparison = comparison
    }
}

public enum JobState: String, Codable, Sendable {
    case queued = "Queued", building = "Building", running = "Running", checking = "Checking"
    case completed = "Completed", cancelled = "Cancelled", failed = "Failed"
    public var isTerminal: Bool { self == .completed || self == .cancelled || self == .failed }
}
public struct ProcessReport: Decodable, Sendable, Equatable {
    public let termination: String
    public let elapsedMS: UInt64
    public let stdout: WireBytes
    public let stderr: WireBytes
    public let detail: String
    public let exitCode: Int?
    public let stdoutTruncated: Bool
    public let stderrTruncated: Bool
    enum CodingKeys: String, CodingKey {
        case termination, stdout, stderr, detail, elapsedMS = "elapsed_ms", exitCode = "exit_code"
        case stdoutTruncated = "stdout_truncated", stderrTruncated = "stderr_truncated"
    }
}
public struct TestResult: Decodable, Sendable, Equatable, Identifiable {
    public let testcaseID: String; public var id: RawIdentifier { RawIdentifier(testcaseID) }
    public let verdict: String; public let process: ProcessReport; public let detail: String
    enum CodingKeys: String, CodingKey { case testcaseID = "testcase_id", verdict, process, detail }
}
public struct BatchResult: Decodable, Sendable, Equatable {
    public let schemaVersion: Int; public let documentID: RawIdentifier; public let revision: UInt64
    public let profileID: String; public let sourceDigest: String; public let buildOutcome: String
    public let error: String; public let build: ProcessReport?; public let tests: [TestResult]
    enum CodingKeys: String, CodingKey {
        case schemaVersion = "schema_version", documentID = "document_id", revision, profileID = "profile_id"
        case sourceDigest = "source_digest", buildOutcome = "build_outcome", error, build, tests
    }
    public func matches(_ snapshot: SourceSnapshot) -> Bool {
        schemaVersion == 1 && documentID == snapshot.documentID && revision == snapshot.revision
    }
}
public struct CoreEvent: Decodable, Sendable {
    public let jobID: UInt64; public let sequence: UInt64; public let state: JobState
    public let testcaseID: String; public let result: BatchResult?
    enum CodingKeys: String, CodingKey { case jobID = "job_id", sequence, state, testcaseID = "testcase_id", result }
}
struct EventBatch: Decodable { let schemaVersion: Int; let events: [CoreEvent]
    enum CodingKeys: String, CodingKey { case schemaVersion = "schema_version", events }
}
