import Foundation
import NativeEditorModel

/// Companion C ABI v1 的字段集中定义；来源原文始终作为字符串传递。
enum CompanionField: String, CodingKey {
    case schemaVersion = "schema_version", workspaceID = "workspace_id", paths
    case inboxFile = "inbox_file", backupFile = "backup_file", lockFile = "lock_file"
    case afterSequence = "after_sequence", maxItems = "max_items", maxPageBytes = "max_page_bytes"
    case originalJSON = "original_json", lockTimeoutMS = "lock_timeout_ms", importID = "import_id"
    case session, defaults, sessionPaths = "session_paths", sourceText = "source_text"
    case languageID = "language_id", toolchainProfileID = "toolchain_profile_id", documentPreferences = "document_preferences"
    case status, detail, inboxID = "inbox_id", revision, diskDigest = "disk_digest"
    case nextAfterSequence = "next_after_sequence", hasMore = "has_more", items, item
    case inboxReplaced = "inbox_replaced", backupReplaced = "backup_replaced", sequence, state
    case problemID = "problem_id", documentID = "document_id", testIDs = "test_ids", testCount = "test_count"
    case previousImportID = "previous_import_id", payloadDigest = "payload_digest"
    case ordinaryStdioCompatible = "ordinary_stdio_compatible", appliedSessionDigest = "applied_session_digest"
    case appliedSessionRevision = "applied_session_revision", inboxStatus = "inbox_status", receipt, receipts
}

public struct CompanionPaths: Codable, Sendable, Equatable {
    public let inboxFile: String; public let backupFile: String; public let lockFile: String
    public init(inboxFile: String, backupFile: String, lockFile: String) {
        self.inboxFile = inboxFile; self.backupFile = backupFile; self.lockFile = lockFile
    }
    public var all: [String] { [inboxFile, backupFile, lockFile] }
    public init(from decoder: Decoder) throws {
        let box = try decoder.container(keyedBy: CompanionField.self)
        inboxFile = try box.decode(String.self, forKey: .inboxFile)
        backupFile = try box.decode(String.self, forKey: .backupFile)
        lockFile = try box.decode(String.self, forKey: .lockFile)
        guard all.allSatisfy({ $0.hasPrefix("/") && !$0.utf8.contains(0) }), Set(all.map { RawIdentifier($0) }).count == 3 else { throw WireError.unsupportedSchema }
    }
    public func encode(to encoder: Encoder) throws {
        var box = encoder.container(keyedBy: CompanionField.self)
        try box.encode(inboxFile, forKey: .inboxFile); try box.encode(backupFile, forKey: .backupFile); try box.encode(lockFile, forKey: .lockFile)
    }
}
public struct CompanionContext: Sendable {
    public let workspaceID: RawIdentifier; public let paths: CompanionPaths
    public init(workspaceID: RawIdentifier, paths: CompanionPaths) { self.workspaceID = workspaceID; self.paths = paths }
    func encode(to encoder: Encoder) throws {
        var box = encoder.container(keyedBy: CompanionField.self)
        try box.encode(1, forKey: .schemaVersion); try box.encode(workspaceID, forKey: .workspaceID); try box.encode(paths, forKey: .paths)
    }
}
public enum CompanionState: String, Decodable, Sendable { case pending = "Pending", applied = "Applied" }
public struct CompanionItem: Decodable, Sendable, Identifiable {
    public let sequence: UInt64; public let state: CompanionState
    public let importID: RawIdentifier; public var id: RawIdentifier { importID }
    public let problemID: RawIdentifier; public let documentID: RawIdentifier; public let testIDs: [RawIdentifier]
    public let previousImportID: RawIdentifier?; public let payloadDigest: String; public let originalJSON: String
    public init(from decoder: Decoder) throws {
        let box = try decoder.container(keyedBy: CompanionField.self)
        sequence = try box.decode(UInt64.self, forKey: .sequence); state = try box.decode(CompanionState.self, forKey: .state)
        importID = try box.decode(RawIdentifier.self, forKey: .importID); problemID = try box.decode(RawIdentifier.self, forKey: .problemID)
        documentID = try box.decode(RawIdentifier.self, forKey: .documentID); testIDs = try box.decode([RawIdentifier].self, forKey: .testIDs)
        previousImportID = try box.decodeIfPresent(RawIdentifier.self, forKey: .previousImportID)
        payloadDigest = try box.decode(String.self, forKey: .payloadDigest); originalJSON = try box.decode(String.self, forKey: .originalJSON)
        guard sequence > 0, payloadDigest == SourceDigest.sha256(Data(originalJSON.utf8)) else { throw WireError.malformedTerminal }
    }
}
public struct CompanionPage: Decodable, Sendable {
    public let schemaVersion: UInt64; public let status: SessionLoadStatus; public let detail: String
    public let inboxID: RawIdentifier?; public let workspaceID: RawIdentifier; public let revision: UInt64?
    public let diskDigest: String?; public let nextAfterSequence: UInt64; public let hasMore: Bool; public let items: [CompanionItem]
    public init(from decoder: Decoder) throws {
        let box = try decoder.container(keyedBy: CompanionField.self)
        schemaVersion = try box.decode(UInt64.self, forKey: .schemaVersion); status = try box.decode(SessionLoadStatus.self, forKey: .status)
        detail = try box.decode(String.self, forKey: .detail); inboxID = try box.decodeIfPresent(RawIdentifier.self, forKey: .inboxID)
        workspaceID = try box.decode(RawIdentifier.self, forKey: .workspaceID); revision = try box.decodeIfPresent(UInt64.self, forKey: .revision)
        diskDigest = try box.decodeIfPresent(String.self, forKey: .diskDigest); nextAfterSequence = try box.decode(UInt64.self, forKey: .nextAfterSequence)
        hasMore = try box.decode(Bool.self, forKey: .hasMore); items = try box.decode([CompanionItem].self, forKey: .items)
    }
}
public struct CompanionWriteResponse: Decodable, Sendable {
    public let schemaVersion: UInt64; public let status: String; public let detail: String
    public let diskDigest: String?; public let inboxReplaced: Bool; public let backupReplaced: Bool
    public init(from decoder: Decoder) throws {
        let box = try decoder.container(keyedBy: CompanionField.self)
        schemaVersion = try box.decode(UInt64.self, forKey: .schemaVersion); status = try box.decode(String.self, forKey: .status)
        detail = try box.decode(String.self, forKey: .detail); diskDigest = try box.decodeIfPresent(String.self, forKey: .diskDigest)
        inboxReplaced = try box.decode(Bool.self, forKey: .inboxReplaced); backupReplaced = try box.decode(Bool.self, forKey: .backupReplaced)
    }
}
public struct CompanionReceipt: Codable, Sendable, Equatable {
    public let inboxID: RawIdentifier; public let importID: RawIdentifier; public let problemID: RawIdentifier
    public let documentID: RawIdentifier; public let payloadDigest: String; public let testIDs: [RawIdentifier]
    public init(from decoder: Decoder) throws {
        let box = try decoder.container(keyedBy: CompanionField.self)
        inboxID = try box.decode(RawIdentifier.self, forKey: .inboxID); importID = try box.decode(RawIdentifier.self, forKey: .importID)
        problemID = try box.decode(RawIdentifier.self, forKey: .problemID); documentID = try box.decode(RawIdentifier.self, forKey: .documentID)
        payloadDigest = try box.decode(String.self, forKey: .payloadDigest); testIDs = try box.decode([RawIdentifier].self, forKey: .testIDs)
        try validate()
    }
    public func encode(to encoder: Encoder) throws {
        var box = encoder.container(keyedBy: CompanionField.self)
        try box.encode(inboxID, forKey: .inboxID); try box.encode(importID, forKey: .importID); try box.encode(problemID, forKey: .problemID)
        try box.encode(documentID, forKey: .documentID); try box.encode(payloadDigest, forKey: .payloadDigest); try box.encode(testIDs, forKey: .testIDs)
    }
    func validate() throws {
        guard !inboxID.isEmpty, inboxID.rawValue.utf8.count <= 1024, !inboxID.rawValue.utf8.contains(0), testIDs.count <= 4096,
              Set(testIDs).count == testIDs.count, ([importID.rawValue, problemID.rawValue, documentID.rawValue, payloadDigest] + testIDs.map(\.rawValue)).allSatisfy(Self.digest) else { throw WireError.unsupportedSchema }
    }
    private static func digest(_ text: String) -> Bool { text.utf8.count == 64 && text.utf8.allSatisfy { (48...57).contains($0) || (97...102).contains($0) } }
}
public struct CompanionDefaults: Encodable, Sendable {
    public let sourceText: String; public let languageID: String?; public let toolchainProfileID: RawIdentifier?
    public let documentPreferences: [NativePreferences]
    public init(sourceText: String, languageID: String?, toolchainProfileID: RawIdentifier?, documentPreferences: [NativePreferences]) {
        self.sourceText = sourceText; self.languageID = languageID; self.toolchainProfileID = toolchainProfileID; self.documentPreferences = documentPreferences
    }
    public func encode(to encoder: Encoder) throws {
        var box = encoder.container(keyedBy: CompanionField.self)
        try box.encode(sourceText, forKey: .sourceText); try box.encodeIfPresent(languageID, forKey: .languageID)
        try box.encodeIfPresent(toolchainProfileID, forKey: .toolchainProfileID); try box.encode(documentPreferences, forKey: .documentPreferences)
    }
}
public struct CompanionPreparation: Decodable, Sendable {
    public let schemaVersion: UInt64; public let inboxStatus: SessionLoadStatus; public let status: String; public let detail: String
    public let session: WorkspaceSessionRecord?; public let receipt: CompanionReceipt?
    public init(from decoder: Decoder) throws {
        let box = try decoder.container(keyedBy: CompanionField.self)
        schemaVersion = try box.decode(UInt64.self, forKey: .schemaVersion); inboxStatus = try box.decode(SessionLoadStatus.self, forKey: .inboxStatus)
        status = try box.decode(String.self, forKey: .status); detail = try box.decode(String.self, forKey: .detail)
        session = try box.decodeIfPresent(WorkspaceSessionRecord.self, forKey: .session); receipt = try box.decodeIfPresent(CompanionReceipt.self, forKey: .receipt)
    }
}

/// 自身来源偏好严格读取；其他 owner 原文不解释、不修改。恢复读入与 Run All 共用此门禁。
public enum CompanionProvenance {
    public static let documentNamespace = "cpeditor.native.companion.document"
    public static let receiptsNamespace = "cpeditor.native.companion.receipts"
    public static func ordinaryStdioAllowed(documentID: RawIdentifier, preferences: [NativePreferences]) throws -> Bool {
        guard let own = try own(preferences, namespace: documentNamespace) else { return true }
        let fields = try object(own.objectJSON, allowed: [.inboxID, .importID, .problemID, .documentID, .payloadDigest, .testIDs, .ordinaryStdioCompatible])
        let receipt = try JSONDecoder().decode(CompanionReceipt.self, from: Data(own.objectJSON.utf8))
        guard receipt.documentID == documentID, case .bool(let compatible) = fields[CompanionField.ordinaryStdioCompatible.rawValue] else { throw WireError.unsupportedSchema }
        return compatible
    }
    public static func receipts(_ preferences: [NativePreferences]) throws -> [CompanionReceipt] {
        guard let own = try own(preferences, namespace: receiptsNamespace) else { return [] }
        let fields = try object(own.objectJSON, allowed: [.receipts])
        guard case .array(let values) = fields[CompanionField.receipts.rawValue], values.count <= 256 else { throw WireError.unsupportedSchema }
        for value in values { _ = try value.object(allowed: [CompanionField.inboxID, .importID, .problemID, .documentID, .payloadDigest, .testIDs].map(\.rawValue)) }
        struct ReceiptOwner: Decodable {
            let receipts: [CompanionReceipt]
            init(from decoder: Decoder) throws {
                let box = try decoder.container(keyedBy: CompanionField.self)
                receipts = try box.decode([CompanionReceipt].self, forKey: .receipts)
            }
        }
        let receipts = try JSONDecoder().decode(ReceiptOwner.self, from: Data(own.objectJSON.utf8)).receipts
        let identities = receipts.map { RawIdentifier($0.inboxID.rawValue + "\0" + $0.importID.rawValue) }
        guard Set(identities).count == receipts.count else { throw WireError.unsupportedSchema }
        return receipts
    }
    static func own(_ preferences: [NativePreferences], namespace: String) throws -> NativePreferences? {
        let own = preferences.filter { $0.namespaceID.utf8.elementsEqual(namespace.utf8) }
        guard own.count <= 1, own.allSatisfy({ $0.schemaVersion == 1 }) else { throw WireError.unsupportedSchema }
        return own.first
    }
    static func object(_ text: String, allowed: [CompanionField]) throws -> [String: CheckerJSON] {
        guard text.utf8.count <= 65536 else { throw WireError.unsupportedSchema }
        var parser = CheckerJSONParser(bytes: Array(text.utf8))
        let fields = try parser.parse().object(allowed: allowed.map(\.rawValue))
        guard fields.count == allowed.count else { throw WireError.unsupportedSchema }
        return fields
    }
}

public struct CompanionStorageBinding: Codable, Sendable {
    public let workspaceID: RawIdentifier; public let paths: CompanionPaths
    public init(workspaceID: RawIdentifier, paths: CompanionPaths) { self.workspaceID = workspaceID; self.paths = paths }
    public init(from decoder: Decoder) throws {
        let box = try decoder.container(keyedBy: CompanionField.self)
        workspaceID = try box.decode(RawIdentifier.self, forKey: .workspaceID); paths = try box.decode(CompanionPaths.self, forKey: .paths)
        guard !workspaceID.isEmpty, workspaceID.rawValue.utf8.count <= 1024, !workspaceID.rawValue.utf8.contains(0) else { throw WireError.unsupportedSchema }
    }
    public func encode(to encoder: Encoder) throws {
        var box = encoder.container(keyedBy: CompanionField.self)
        try box.encode(workspaceID, forKey: .workspaceID); try box.encode(paths, forKey: .paths)
    }
    public static func read(_ preferences: [NativePreferences], namespace: String) throws -> Self? {
        guard let own = try CompanionProvenance.own(preferences, namespace: namespace) else { return nil }
        let fields = try CompanionProvenance.object(own.objectJSON, allowed: [.workspaceID, .paths])
        _ = try fields[CompanionField.paths.rawValue]?.object(allowed: [CompanionField.inboxFile, .backupFile, .lockFile].map(\.rawValue))
        return try JSONDecoder().decode(Self.self, from: Data(own.objectJSON.utf8))
    }
}

struct CompanionRequest: Encodable {
    enum Operation { case page(UInt64, UInt64, UInt64), accept(String, UInt64), prepare(RawIdentifier, WorkspaceSessionRecord, CompanionDefaults), mark(RawIdentifier, SessionPaths, UInt64) }
    let context: CompanionContext; let operation: Operation
    func encode(to encoder: Encoder) throws {
        try context.encode(to: encoder)
        var box = encoder.container(keyedBy: CompanionField.self)
        switch operation {
        case .page(let after, let count, let bytes):
            try box.encode(after, forKey: .afterSequence); try box.encode(count, forKey: .maxItems); try box.encode(bytes, forKey: .maxPageBytes)
        case .accept(let original, let timeout):
            try box.encode(original, forKey: .originalJSON); try box.encode(timeout, forKey: .lockTimeoutMS)
        case .prepare(let id, let session, let defaults):
            try box.encode(id, forKey: .importID); try box.encode(session, forKey: .session); try box.encode(defaults, forKey: .defaults)
        case .mark(let id, let paths, let timeout):
            try box.encode(id, forKey: .importID); try box.encode(paths, forKey: .sessionPaths); try box.encode(timeout, forKey: .lockTimeoutMS)
        }
    }
}
