import Foundation
import NativeEditorModel

/// 对未知工具链字段保留原始 JSON，由共享 Core 的配置校验决定是否接受。
public enum JSONValue: Codable, Sendable, Equatable {
    case object([String: JSONValue]), array([JSONValue]), string(String), number(Double), largeInteger(Int64), bool(Bool), null
    public init(from decoder: Decoder) throws {
        let box = try decoder.singleValueContainer()
        if box.decodeNil() { self = .null }
        else if let value = try? box.decode(Bool.self) { self = .bool(value) }
        else if let value = try? box.decode(String.self) { self = .string(value) }
        else if let value = try? box.decode([String: Self].self) { self = .object(value) }
        else if let value = try? box.decode([Self].self) { self = .array(value) }
        else if let value = try? box.decode(Int64.self), Double(exactly: value) == nil { self = .largeInteger(value) }
        else { self = .number(try box.decode(Double.self)) }
    }
    public func encode(to encoder: Encoder) throws {
        var box = encoder.singleValueContainer()
        switch self {
        case .object(let value): try box.encode(value)
        case .array(let value): try box.encode(value)
        case .string(let value): try box.encode(value)
        case .number(let value): try box.encode(value)
        case .largeInteger(let value): try box.encode(value)
        case .bool(let value): try box.encode(value)
        case .null: try box.encodeNil()
        }
    }
    public subscript(_ key: String) -> Self? { if case .object(let values) = self { return values[key] }; return nil }
    public var object: [String: JSONValue]? { if case .object(let values) = self { return values }; return nil }
    public var array: [JSONValue]? { if case .array(let values) = self { return values }; return nil }
    public var string: String? { if case .string(let value) = self { return value }; return nil }
    public var integerValue: Int? {
        switch self {
        case .number(let value): Int(exactly: value)
        case .largeInteger(let value): Int(exactly: value)
        default: nil
        }
    }

    public static func == (left: Self, right: Self) -> Bool {
        switch (left, right) {
        case (.object(let left), .object(let right)): left == right
        case (.array(let left), .array(let right)): left == right
        case (.string(let left), .string(let right)): left.utf8.elementsEqual(right.utf8)
        case (.number(let left), .number(let right)): left == right
        case (.largeInteger(let left), .largeInteger(let right)): left == right
        case (.bool(let left), .bool(let right)): left == right
        case (.null, .null): true
        default: false
        }
    }
}

public struct ToolchainProfile: Codable, Sendable, Identifiable, Equatable {
    public let payload: JSONValue
    public enum Field: String {
        case id
        case languageID = "language_id"
        case schemaVersion = "schema_version"
        case artifactName = "artifact_name"
        case build
        case run
        case executable
        case arguments
        case buildLimits = "build_limits"
        case runLimits = "run_limits"
        case wallTimeoutMS = "wall_timeout_ms"
    }
    public var id: RawIdentifier { RawIdentifier(payload[Field.id.rawValue]?.string ?? "") }
    public var languageID: String { payload[Field.languageID.rawValue]?.string ?? "" }
    public var artifactName: String? { payload[Field.artifactName.rawValue]?.string }
    public var buildExecutable: String? { payload[Field.build.rawValue]?[Field.executable.rawValue]?.string }
    public var buildArguments: [String] {
        payload[Field.build.rawValue]?[Field.arguments.rawValue]?.array?.compactMap(\.string) ?? []
    }
    public var buildWallTimeoutMS: Int? {
        payload[Field.buildLimits.rawValue]?[Field.wallTimeoutMS.rawValue]?.integerValue
    }
    public var runWallTimeoutMS: Int? {
        payload[Field.runLimits.rawValue]?[Field.wallTimeoutMS.rawValue]?.integerValue
    }

    public var displayName: String {
        let baseLanguage: String = switch languageID.lowercased() {
        case "cpp": "C++"
        case "c": "C"
        case "python": "Python"
        case "java": "Java"
        default: languageID.uppercased()
        }
        let compilerName: String? = buildExecutable.map { path in
            let name = URL(fileURLWithPath: path).lastPathComponent.lowercased()
            if name.contains("clang") { return "Clang" }
            if name.contains("g++") || name.contains("gcc") { return "GCC" }
            if name == "cl" || name == "cl.exe" { return "MSVC" }
            return URL(fileURLWithPath: path).lastPathComponent
        }
        let standardSuffix: String = {
            for arg in buildArguments where arg.hasPrefix("-std=") {
                let raw = String(arg.dropFirst(5)).lowercased()
                if raw.hasPrefix("c++") { return String(raw.dropFirst(3)).uppercased() }
                if raw.hasPrefix("gnu++") {
                    let version = String(raw.dropFirst(5)).uppercased()
                    return compilerName == "GCC" ? version : version + " (GNU)"
                }
                return " " + raw.uppercased()
            }
            return ""
        }()
        let coreTitle: String
        if let compilerName, !compilerName.isEmpty {
            coreTitle = "\(baseLanguage)\(standardSuffix) (\(compilerName))"
        } else {
            coreTitle = "\(baseLanguage)\(standardSuffix)"
        }
        let builtinIDs = ["macos-clang-cpp17", "macos-clang-cpp20", "macos-gcc-cpp17", "macos-gcc-cpp20"]
        if builtinIDs.contains(id.rawValue) { return coreTitle }
        return "\(coreTitle) · \(id.rawValue)"
    }

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

    /// 仅更新可视化表单中的编译命令与超时字段，完整保留其余未知字段。
    public func updating(buildExecutable: String?, buildArguments: [String], buildWallTimeoutMS: Int, runWallTimeoutMS: Int) throws -> ToolchainProfile {
        guard var root = payload.object, buildWallTimeoutMS > 0, runWallTimeoutMS > 0 else {
            throw WireError.invalidProfile
        }
        if let executable = buildExecutable?.trimmingCharacters(in: .whitespacesAndNewlines), !executable.isEmpty {
            var buildObject = root[Field.build.rawValue]?.object ?? [:]
            buildObject[Field.executable.rawValue] = .string(executable)
            buildObject[Field.arguments.rawValue] = .array(buildArguments.map { .string($0) })
            root[Field.build.rawValue] = .object(buildObject)
        } else if root[Field.build.rawValue]?.object != nil {
            throw WireError.invalidProfile
        }
        var buildLimits = root[Field.buildLimits.rawValue]?.object ?? [:]
        buildLimits[Field.wallTimeoutMS.rawValue] = .number(Double(buildWallTimeoutMS))
        root[Field.buildLimits.rawValue] = .object(buildLimits)

        var runLimits = root[Field.runLimits.rawValue]?.object ?? [:]
        runLimits[Field.wallTimeoutMS.rawValue] = .number(Double(runWallTimeoutMS))
        root[Field.runLimits.rawValue] = .object(runLimits)
        return try ToolchainProfile(payload: .object(root))
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
    public var canDisplayAsText: Bool { isUTF8 && !data.contains { ($0 < 32 && ![9, 10, 13].contains($0)) || $0 == 127 } }
    public func hexPreview(maximumBytes: Int) -> String {
        data.prefix(max(0, maximumBytes)).map { String(format: "%02X", $0) }.joined(separator: " ")
    }
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
public enum BatchMode: String, Codable, Sendable {
    case compileAndRun = "compile_and_run"
    case compileOnly = "compile_only"
    case runOnly = "run_only"
}
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
    let externalChecker: CheckerConfiguration?
    let mode: BatchMode
    let artifactPath: String?
    enum CodingKeys: String, CodingKey {
        case schemaVersion = "schema_version", source, toolchain, tests, comparison
        case externalChecker = "external_checker", mode, artifactPath = "artifact_path"
    }
    public init(snapshot: SourceSnapshot, originalPath: String?, toolchain: ToolchainProfile,
                tests: [TestCaseInput], comparison: ComparisonPolicy = .trailing,
                externalChecker: CheckerConfiguration? = nil,
                mode: BatchMode = .compileAndRun, artifactPath: String? = nil) {
        source = Source(documentID: snapshot.documentID, revision: snapshot.revision,
                        text: snapshot.text, originalPath: originalPath)
        self.toolchain = toolchain; self.tests = tests; self.comparison = comparison
        self.externalChecker = externalChecker
        self.mode = mode; self.artifactPath = artifactPath
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
    public let signal: Int?
    public let nativeError: Int
    public let stdoutTotalBytes: UInt64
    public let stderrTotalBytes: UInt64
    public var completeTextOutput: WireBytes? {
        guard termination == "Exited", exitCode == 0, signal == nil,
              !stdoutTruncated, UInt64(stdout.data.count) == stdoutTotalBytes, stdout.canDisplayAsText else { return nil }
        return stdout
    }
    enum CodingKeys: String, CodingKey {
        case termination, stdout, stderr, detail, elapsedMS = "elapsed_ms", exitCode = "exit_code"
        case stdoutTruncated = "stdout_truncated", stderrTruncated = "stderr_truncated"
        case signal, nativeError = "native_error", stdoutTotalBytes = "stdout_total_bytes", stderrTotalBytes = "stderr_total_bytes"
    }
}
public struct CheckerReport: Decodable, Sendable, Equatable {
    public let verdict: String
    public let processAttempted: Bool
    public let process: ProcessReport
    public let detail: String
    enum CodingKeys: String, CodingKey { case verdict, processAttempted = "process_attempted", process, detail }
}
public struct TestResult: Decodable, Sendable, Equatable, Identifiable {
    public let testcaseID: String; public var id: RawIdentifier { RawIdentifier(testcaseID) }
    public let verdict: String; public let process: ProcessReport; public let detail: String
    public let checker: CheckerReport?
    enum CodingKeys: String, CodingKey { case testcaseID = "testcase_id", verdict, process, detail, checker }
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
