import Foundation
import CPNativeCore
import NativeEditorModel

public enum SessionCapacityError: Error, Equatable { case exceeded }

public enum SessionLimits {
    private static let limits = cpn_session_default_limits()
    public static let maximumRequestBytes = limits.max_request_bytes
    public static let maximumDocuments = Int(limits.max_documents)
    public static let maximumTestsPerDocument = Int(limits.max_tests_per_document)
    public static let maximumTotalTests = Int(limits.max_total_tests)
    public static let maximumSourceBytes = Int(limits.max_source_bytes)
    public static let maximumTestBytes = Int(limits.max_test_bytes)
    public static let maximumTotalContentBytes = Int(limits.max_total_content_bytes)
    public static let maximumPreferenceBytes = Int(limits.max_preferences_bytes)
    public static let maximumIdentifierBytes = Int(limits.max_id_bytes)
}

public struct NativePreferences: Codable, Equatable, Sendable {
    public let namespaceID: String
    public let schemaVersion: UInt64
    public let objectJSON: String
    public init(namespaceID: String, schemaVersion: UInt64, objectJSON: String) {
        self.namespaceID = namespaceID; self.schemaVersion = schemaVersion; self.objectJSON = objectJSON
    }
    enum CodingKeys: String, CodingKey { case namespaceID = "namespace_id", schemaVersion = "schema_version", objectJSON = "object_json" }
    public static func == (left: Self, right: Self) -> Bool {
        left.schemaVersion == right.schemaVersion && left.namespaceID.utf8.elementsEqual(right.namespaceID.utf8)
            && left.objectJSON.utf8.elementsEqual(right.objectJSON.utf8)
    }
}

public struct SessionDocumentRecord: Codable, Sendable {
    public let documentID: RawIdentifier
    public let revision: UInt64
    public let sourceText: String
    public let sourceDigest: String
    public let originalPath: String?
    public let languageID: String?
    public let toolchainProfileID: RawIdentifier?
    public let savePoint: SourceSavePoint?
    public let tests: [TestCaseInput]
    public let editorPreferences: [NativePreferences]
    enum CodingKeys: String, CodingKey {
        case documentID = "document_id", revision, sourceText = "source_text", sourceDigest = "source_digest"
        case originalPath = "original_path", languageID = "language_id", toolchainProfileID = "toolchain_profile_id"
        case savePoint = "save_point", tests, editorPreferences = "editor_preferences"
    }
    public init(snapshot: SourceSnapshot, originalPath: String?, languageID: String?, toolchainProfileID: RawIdentifier?,
                savePoint: SourceSavePoint?, tests: [TestCaseInput], editorPreferences: [NativePreferences]) {
        documentID = snapshot.documentID; revision = snapshot.revision; sourceText = snapshot.text
        sourceDigest = snapshot.sourceDigest; self.originalPath = originalPath; self.languageID = languageID
        self.toolchainProfileID = toolchainProfileID; self.savePoint = savePoint; self.tests = tests; self.editorPreferences = editorPreferences
    }
}

public struct WorkspaceSessionRecord: Codable, Sendable {
    public let schemaVersion: UInt64
    public let workspaceID: RawIdentifier
    public let revision: UInt64
    public let selectedDocumentID: RawIdentifier?
    public let documents: [SessionDocumentRecord]
    public let editorPreferences: [NativePreferences]
    enum CodingKeys: String, CodingKey {
        case schemaVersion = "schema_version", workspaceID = "workspace_id", revision
        case selectedDocumentID = "selected_document_id", documents, editorPreferences = "editor_preferences"
    }
    public init(workspaceID: RawIdentifier, revision: UInt64, selectedDocumentID: RawIdentifier?,
                documents: [SessionDocumentRecord], editorPreferences: [NativePreferences]) {
        schemaVersion = 1; self.workspaceID = workspaceID; self.revision = revision
        self.selectedDocumentID = selectedDocumentID; self.documents = documents; self.editorPreferences = editorPreferences
    }

    // Core validates the digest against the UTF-8 bytes when loading, and local snapshots own their digest.
    public func hasSameContent(as other: Self?) -> Bool {
        guard let other, schemaVersion == other.schemaVersion, workspaceID == other.workspaceID,
              selectedDocumentID == other.selectedDocumentID, editorPreferences == other.editorPreferences,
              documents.count == other.documents.count else { return false }
        return zip(documents, other.documents).allSatisfy { lhs, rhs in
            lhs.documentID == rhs.documentID && lhs.revision == rhs.revision && lhs.sourceDigest == rhs.sourceDigest
                && lhs.originalPath.map { RawIdentifier($0) } == rhs.originalPath.map { RawIdentifier($0) }
                && lhs.languageID.map { RawIdentifier($0) } == rhs.languageID.map { RawIdentifier($0) }
                && lhs.toolchainProfileID == rhs.toolchainProfileID && lhs.savePoint == rhs.savePoint
                && lhs.tests == rhs.tests && lhs.editorPreferences == rhs.editorPreferences
        }
    }
}

public struct SessionPaths: Codable, Sendable {
    public let recoveryFile: String
    public let backupFile: String
    public let lockFile: String
    public init(recoveryFile: String, backupFile: String, lockFile: String) {
        self.recoveryFile = recoveryFile; self.backupFile = backupFile; self.lockFile = lockFile
    }
    enum CodingKeys: String, CodingKey { case recoveryFile = "recovery_file", backupFile = "backup_file", lockFile = "lock_file" }
}
public enum SessionLoadStatus: String, Codable, Sendable { case loaded = "Loaded", missing = "Missing", invalid = "Invalid", unsupportedSchema = "UnsupportedSchema", ioFailed = "IOFailed" }
public struct SessionLoadResponse: Decodable, Sendable {
    public let schemaVersion: UInt64; public let status: SessionLoadStatus
    public let session: WorkspaceSessionRecord?; public let diskDigest: String?; public let detail: String
    enum CodingKeys: String, CodingKey { case schemaVersion = "schema_version", status, session, diskDigest = "disk_digest", detail }
}
public enum SessionSaveStatus: String, Codable, Sendable {
    case saved = "Saved", conflict = "Conflict", invalidSession = "InvalidSession", invalidExisting = "InvalidExisting"
    case ioFailed = "IOFailed", durabilityUnknown = "DurabilityUnknown", lockUnavailable = "LockUnavailable", cancelled = "Cancelled"
}
public struct SessionSaveResponse: Decodable, Sendable {
    public let schemaVersion: UInt64; public let status: SessionSaveStatus; public let diskDigest: String?
    public let recoveryReplaced: Bool; public let backupReplaced: Bool; public let detail: String
    enum CodingKeys: String, CodingKey {
        case schemaVersion = "schema_version", status, diskDigest = "disk_digest", recoveryReplaced = "recovery_replaced", backupReplaced = "backup_replaced", detail
    }
}
struct SessionSaveRequest: Encodable {
    let schemaVersion = 1; let paths: SessionPaths; let session: WorkspaceSessionRecord
    let expectedDiskDigest: String?; let lockTimeoutMS: UInt64
    enum CodingKeys: String, CodingKey { case schemaVersion = "schema_version", paths, session, expectedDiskDigest = "expected_disk_digest", lockTimeoutMS = "lock_timeout_ms" }
}
