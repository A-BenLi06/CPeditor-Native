import Foundation

public struct LanguageProfile: Decodable, Sendable {
    public let schemaVersion: Int
    public let id: String
    public let displayName: String
    public let fileExtensions: [String]
    public let suggestedFileName: String
    public let supportsSquareBrackets: Bool
    public let supportsCurlyBrackets: Bool
    public let stringPattern: String
    public let characterPattern: String
    public let numberPattern: String
    public let identifierPattern: String
    public let operatorPattern: String
    public let singleLineComment: String
    public let blockCommentOpen: String
    public let blockCommentClose: String
    public let keywords: [String]
    public let operators: [String]
    public let editing: EditingConfiguration

    public static func load(id: String) throws -> Self {
        guard let url = Bundle.module.url(forResource: id, withExtension: "json") else {
            throw CocoaError(.fileNoSuchFile)
        }
        let profile = try JSONDecoder().decode(Self.self, from: Data(contentsOf: url))
        guard profile.schemaVersion == 1, profile.id == id else {
            throw CocoaError(.fileReadCorruptFile)
        }
        return profile
    }
}

public enum TokenKind: Sendable { case keyword, string, number, comment, directive }
public struct HighlightToken: Sendable {
    public let range: NSRange
    public let kind: TokenKind
}

/// 不替换字符存储的词法显示层；不等同于编译器或完整 C++ 解析器。
public struct LexicalHighlighter: Sendable {
    private let expression: NSRegularExpression
    private let keywords: Set<String>
    public init(profile: LanguageProfile) throws {
        let line = NSRegularExpression.escapedPattern(for: profile.singleLineComment)
        let open = NSRegularExpression.escapedPattern(for: profile.blockCommentOpen)
        let close = NSRegularExpression.escapedPattern(for: profile.blockCommentClose)
        let alternatives = [
            "(?<comment>\(line)[^\\r\\n]*|\(open)[\\s\\S]*?(?:\(close)|$))",
            "(?<string>\(profile.stringPattern)|\(profile.characterPattern))",
            "(?<number>\(profile.numberPattern))",
            "(?<identifier>\(profile.identifierPattern))"
        ]
        expression = try NSRegularExpression(pattern: alternatives.joined(separator: "|"))
        keywords = Set(profile.keywords)
    }

    public func tokens(in text: String) -> [HighlightToken] {
        let source = text as NSString
        return expression.matches(in: text, range: NSRange(location: 0, length: source.length)).compactMap { match in
            let comment = match.range(withName: "comment")
            if comment.location != NSNotFound { return HighlightToken(range: comment, kind: .comment) }
            let string = match.range(withName: "string")
            if string.location != NSNotFound { return HighlightToken(range: string, kind: .string) }
            let number = match.range(withName: "number")
            if number.location != NSNotFound { return HighlightToken(range: number, kind: .number) }
            let identifier = match.range(withName: "identifier")
            guard identifier.location != NSNotFound else { return nil }
            let lexeme = source.substring(with: identifier)
            if lexeme.hasPrefix("#") { return HighlightToken(range: identifier, kind: .directive) }
            return keywords.contains(lexeme) ? HighlightToken(range: identifier, kind: .keyword) : nil
        }
    }
}
