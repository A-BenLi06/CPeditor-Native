import Foundation
import Observation

public enum EditorKind: String, CaseIterable, Codable, Sendable {
    case codeEditorView = "codeeditorview"
    case stTextView = "sttextview"

    public var alternative: Self { self == .codeEditorView ? .stTextView : .codeEditorView }
}

public struct SourceSnapshot: Equatable, Sendable {
    public let documentID: UUID
    public let revision: UInt64
    public let text: String
    public var utf8: Data { Data(text.utf8) }
}

public enum DocumentError: Error, Equatable {
    case staleRevision
    case invalidRange
    case invalidUTF8
}

/// 原生编辑器拥有 live buffer；这里仅记录已提交的文本及保存点。
@MainActor @Observable
public final class SourceDocument: Identifiable {
    public let id: UUID
    public let editorKind: EditorKind
    public let languageID: String
    public private(set) var url: URL?
    public private(set) var text: String
    public private(set) var revision: UInt64 = 0
    private var savedText: String
    public var isDirty: Bool { text != savedText }

    public init(id: UUID = UUID(), text: String = "", url: URL? = nil,
                editorKind: EditorKind = .codeEditorView, languageID: String,
                saved: Bool = true) {
        self.id = id
        self.text = text
        self.url = url
        self.editorKind = editorKind
        self.languageID = languageID
        savedText = saved ? text : ""
    }

    @discardableResult public func commit(_ committedText: String) -> Bool {
        guard committedText != text else { return false }
        text = committedText
        revision += 1
        return true
    }

    public func snapshot() -> SourceSnapshot {
        SourceSnapshot(documentID: id, revision: revision, text: text)
    }

    public func markSaved(at url: URL, snapshot: SourceSnapshot) throws {
        guard snapshot.documentID == id else { throw DocumentError.staleRevision }
        self.url = url
        // 写盘期间的新编辑不得被误标为已保存。
        savedText = snapshot.text
    }

    public func replacing(_ range: NSRange, with replacement: String,
                          expectedRevision: UInt64) throws -> String {
        guard revision == expectedRevision else { throw DocumentError.staleRevision }
        let index = try TextCoordinateIndex(text)
        let swiftRange = try index.characterRange(range)
        return text.replacingCharacters(in: swiftRange, with: replacement)
    }
}

public enum SourceFile {
    public static func readUTF8(_ url: URL) throws -> String {
        let data = try Data(contentsOf: url)
        guard let text = String(data: data, encoding: .utf8) else { throw DocumentError.invalidUTF8 }
        return text
    }

    public static func write(_ snapshot: SourceSnapshot, to url: URL) throws {
        try snapshot.utf8.write(to: url, options: .atomic)
    }
}
