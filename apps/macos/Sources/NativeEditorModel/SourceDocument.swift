import Foundation
import Observation
import CryptoKit

public enum EditorKind: String, CaseIterable, Codable, Sendable {
    case codeEditorView = "codeeditorview"
    case stTextView = "sttextview"

    public var alternative: Self { self == .codeEditorView ? .stTextView : .codeEditorView }
}

public struct SourceSnapshot: Equatable, Sendable {
    public let documentID: RawIdentifier
    public let revision: UInt64
    public let text: String
    public var utf8: Data { Data(text.utf8) }
    public var sourceDigest: String { SourceDigest.sha256(utf8) }
    public static func == (left: Self, right: Self) -> Bool {
        left.documentID == right.documentID && left.revision == right.revision && left.text.utf8.elementsEqual(right.text.utf8)
    }
}

public enum SourceDigest {
    public static func sha256(_ data: Data) -> String {
        SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }
}

public struct SourceSavePoint: Codable, Sendable, Equatable {
    public let revision: UInt64
    public let sourceDigest: String
    public let diskDigest: String?
    public init(revision: UInt64, sourceDigest: String, diskDigest: String?) {
        self.revision = revision; self.sourceDigest = sourceDigest; self.diskDigest = diskDigest
    }
    enum CodingKeys: String, CodingKey { case revision, sourceDigest = "source_digest", diskDigest = "disk_digest" }
}

/// 仅由文档为当前快照发放；已开始的写入可在最后一次编辑后登记真实保存点。
@MainActor public final class SourceSaveLease {
    fileprivate let snapshot: SourceSnapshot
    fileprivate init(snapshot: SourceSnapshot) { self.snapshot = snapshot }
}

public enum DocumentError: Error, Equatable {
    case staleRevision
    case invalidRange
    case invalidUTF8
    case diskConflict
    case revisionExhausted
    case reservedRecoveryPath
}

/// 原生编辑器拥有 live buffer；这里仅记录已提交的文本及保存点。
@MainActor @Observable
public final class SourceDocument: Identifiable {
    public let id: RawIdentifier
    public let editorKind: EditorKind
    public let languageID: String
    public private(set) var url: URL?
    public private(set) var text: String
    public private(set) var revision: UInt64
    public private(set) var savePoint: SourceSavePoint?
    public private(set) var sourceDigest: String
    public private(set) var persistenceIsReadOnly = false
    @ObservationIgnored private var activeSourceSave: SourceSaveLease?
    @ObservationIgnored private var cachedLineIndex: (revision: UInt64, index: TextLineIndex)?
    private var savedDigest: String
    public var isDirty: Bool { sourceDigest != savedDigest }

    public init(id: RawIdentifier = RawIdentifier(UUID().uuidString), text: String = "", url: URL? = nil,
                editorKind: EditorKind = .codeEditorView, languageID: String,
                saved: Bool = true, revision: UInt64 = 0, savePoint: SourceSavePoint? = nil) {
        self.id = id
        self.text = text
        self.url = url
        self.editorKind = editorKind
        self.languageID = languageID
        self.revision = revision
        self.savePoint = savePoint
        sourceDigest = SourceDigest.sha256(Data(text.utf8))
        savedDigest = savePoint?.sourceDigest ?? SourceDigest.sha256(Data((saved ? text : "").utf8))
    }

    @discardableResult public func commit(_ committedText: String) throws -> Bool {
        guard !committedText.utf8.elementsEqual(text.utf8) else { return false }
        guard !persistenceIsReadOnly, revision < UInt64.max else { throw DocumentError.revisionExhausted }
        text = committedText
        sourceDigest = SourceDigest.sha256(Data(committedText.utf8))
        cachedLineIndex = nil
        revision += 1
        return true
    }

    public func setPersistenceReadOnly(_ readOnly: Bool) { persistenceIsReadOnly = readOnly }

    public func snapshot() -> SourceSnapshot {
        SourceSnapshot(documentID: id, revision: revision, text: text)
    }

    public func position(atUTF16Offset offset: Int) -> TextPosition {
        // revision 的读取保留 Observation 依赖；选择变化只查询同版本索引。
        let currentRevision = revision
        if cachedLineIndex?.revision != currentRevision {
            cachedLineIndex = (currentRevision, TextLineIndex(text))
        }
        return cachedLineIndex!.index.position(atUTF16Offset: offset)
    }

    public func markSaved(at url: URL, snapshot: SourceSnapshot, diskDigest: String? = nil) throws {
        guard !persistenceIsReadOnly, revision < UInt64.max else { throw DocumentError.revisionExhausted }
        guard activeSourceSave == nil else { throw DocumentError.staleRevision }
        guard snapshot.documentID == id, snapshot.revision <= revision else { throw DocumentError.staleRevision }
        applySavePoint(at: url, snapshot: snapshot, diskDigest: diskDigest)
    }

    public func beginSourceSave(snapshot: SourceSnapshot) throws -> SourceSaveLease {
        guard !persistenceIsReadOnly, revision < UInt64.max else { throw DocumentError.revisionExhausted }
        guard activeSourceSave == nil, snapshot == self.snapshot() else { throw DocumentError.staleRevision }
        let lease = SourceSaveLease(snapshot: snapshot)
        activeSourceSave = lease
        return lease
    }

    public func completeSourceSave(_ lease: SourceSaveLease, at url: URL, diskDigest: String) throws {
        guard activeSourceSave === lease, lease.snapshot.documentID == id,
              lease.snapshot.revision <= revision else { throw DocumentError.staleRevision }
        applySavePoint(at: url, snapshot: lease.snapshot, diskDigest: diskDigest)
        activeSourceSave = nil
    }

    public func cancelSourceSave(_ lease: SourceSaveLease) {
        if activeSourceSave === lease { activeSourceSave = nil }
    }

    private func applySavePoint(at url: URL, snapshot: SourceSnapshot, diskDigest: String?) {
        self.url = url
        // 写盘期间的新编辑不得被误标为已保存。
        savedDigest = snapshot.sourceDigest
        savePoint = SourceSavePoint(revision: snapshot.revision, sourceDigest: snapshot.sourceDigest,
                                    diskDigest: diskDigest ?? snapshot.sourceDigest)
    }

    public func replacing(_ range: NSRange, with replacement: String,
                          expectedRevision: UInt64) throws -> String {
        guard !persistenceIsReadOnly, revision < UInt64.max else { throw DocumentError.revisionExhausted }
        guard revision == expectedRevision else { throw DocumentError.staleRevision }
        let index = try TextCoordinateIndex(text)
        let swiftRange = try index.characterRange(range)
        return text.replacingCharacters(in: swiftRange, with: replacement)
    }
}

public enum SourceFile {
    public static func readUTF8(_ url: URL) throws -> String {
        let data = try Data(contentsOf: url)
        guard String(data: data, encoding: .utf8) != nil else { throw DocumentError.invalidUTF8 }
        return String(decoding: data, as: UTF8.self)
    }

    public static func write(_ snapshot: SourceSnapshot, to url: URL) throws {
        try snapshot.utf8.write(to: url, options: .atomic)
    }
}
