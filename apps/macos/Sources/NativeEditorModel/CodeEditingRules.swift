import Foundation

public struct EditingConfiguration: Decodable, Sendable {
    public struct Pair: Decodable, Sendable {
        public let open: String
        public let close: String
        public let quote: Bool
        public let indents: Bool
    }
    public let pairs: [Pair]
    public let indent: String
    public let rawStringPrefix: String?
}

public struct CodeEdit: Equatable, Sendable {
    public let range: NSRange
    public let replacement: String
    public let selection: NSRange
}

/// Plans an atomic native edit. Coordinates stay in UTF-16, matching AppKit.
public struct CodeEditingRules: Sendable {
    public enum Action { case insert(String), newline, backspace, indent, outdent }
    private let config: EditingConfiguration
    private let lineComment: String
    private let blockOpen: String
    private let blockClose: String

    public init(profile: LanguageProfile) {
        config = profile.editing
        lineComment = profile.singleLineComment
        blockOpen = profile.blockCommentOpen
        blockClose = profile.blockCommentClose
    }

    public func edit(_ action: Action, text: String, selection: NSRange) -> CodeEdit? {
        if case .insert(let value) = action,
           !config.pairs.contains(where: { $0.open == value || $0.close == value }) { return nil }
        guard Range(selection, in: text) != nil else { return nil }
        let source = text as NSString
        let start = selection.location, end = NSMaxRange(selection)
        func matches(_ value: String, at index: Int) -> Bool {
            let count = value.utf16.count
            return index >= 0 && index + count <= source.length && source.substring(with: NSRange(location: index, length: count)) == value
        }
        func replacement(_ value: String, offset: Int? = nil, length: Int = 0) -> CodeEdit {
            CodeEdit(range: selection, replacement: value,
                     selection: NSRange(location: start + (offset ?? value.utf16.count), length: length))
        }
        let context: Context
        switch action {
        case .indent, .outdent: context = Context()
        default: context = self.context(in: source, before: start)
        }
        var backslashes = 0
        while start > backslashes && source.character(at: start - backslashes - 1) == 92 { backslashes += 1 }
        switch action {
        case .insert(let value):
            guard value.utf16.count == 1 else { return nil }
            if selection.length == 0, config.pairs.contains(where: { $0.close == value }), matches(value, at: start),
               !context.comment, backslashes % 2 == 0, (!context.isString || context.quote == value) {
                return CodeEdit(range: selection, replacement: "", selection: NSRange(location: start + value.utf16.count, length: 0))
            }
            guard !context.comment, !context.isString,
                  let pair = config.pairs.first(where: { $0.open == value }) else { return nil }
            if pair.quote && selection.length == 0 {
                // Do not turn C++ digit separators or apostrophes within identifiers into strings.
                if start > 0, let scalar = UnicodeScalar(source.character(at: start - 1)),
                   CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "_\\")).contains(scalar) { return nil }
                if start < source.length, let scalar = UnicodeScalar(source.character(at: start)),
                   CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "_")).contains(scalar) { return nil }
            }
            let selected = source.substring(with: selection)
            return replacement(pair.open + selected + pair.close, offset: pair.open.utf16.count, length: selection.length)
        case .backspace:
            guard selection.length == 0, !context.comment, backslashes % 2 == 0 else { return nil }
            for pair in config.pairs where matches(pair.open, at: start - pair.open.utf16.count) && matches(pair.close, at: start) {
                guard !context.isString || context.quote == pair.open else { continue }
                let range = NSRange(location: start - pair.open.utf16.count, length: pair.open.utf16.count + pair.close.utf16.count)
                return CodeEdit(range: range, replacement: "", selection: NSRange(location: range.location, length: 0))
            }
            return nil
        case .newline:
            let line = source.lineRange(for: NSRange(location: start, length: 0))
            let before = source.substring(with: NSRange(location: line.location, length: start - line.location))
            let whitespace = String(before.prefix { $0 == " " || $0 == "\t" })
            let trimmed = before.trimmingCharacters(in: .whitespaces)
            let eol = text.contains("\r\n") ? "\r\n" : "\n"
            let pair = context.comment || context.isString ? nil : config.pairs.first { $0.indents && trimmed.hasSuffix($0.open) }
            let indent = whitespace + (pair == nil ? "" : config.indent)
            let first = eol + indent
            if let pair, matches(pair.close, at: end) {
                return replacement(first + eol + whitespace, offset: first.utf16.count)
            }
            return replacement(first)
        case .indent, .outdent:
            let isOutdent: Bool = if case .outdent = action { true } else { false }
            let selected = source.substring(with: selection)
            if !isOutdent && !selected.unicodeScalars.contains(where: { $0 == "\n" || $0 == "\r" }) {
                return replacement(config.indent)
            }
            // Exclude the next line when the selection ends at its beginning.
            let lineRange = source.lineRange(for: NSRange(location: start, length: max(0, selection.length - 1)))
            let lines = source.substring(with: lineRange) as NSString
            var cursor = 0, result = "", startDelta = 0, totalDelta = 0
            while cursor < lines.length {
                let range = lines.lineRange(for: NSRange(location: cursor, length: 0))
                let line = lines.substring(with: range)
                let removed = isOutdent ? (line.hasPrefix("\t") ? 1 : min(config.indent.utf16.count, line.prefix { $0 == " " }.count)) : 0
                let delta = isOutdent ? -removed : config.indent.utf16.count
                if cursor == 0 { startDelta = isOutdent ? -min(removed, start - lineRange.location) : delta }
                totalDelta += delta
                result += isOutdent ? String(line.dropFirst(removed)) : config.indent + line
                cursor = NSMaxRange(range)
            }
            let location = max(lineRange.location, start + startDelta)
            let length = selection.length == 0 ? 0 : max(0, selection.length + totalDelta - startDelta)
            return CodeEdit(range: lineRange, replacement: result, selection: NSRange(location: location, length: length))
        }
    }

    private struct Context {
        var comment = false
        var isString = false
        var quote: String?
    }

    private func rawDelimiter(in source: NSString, start: Int, end: Int) -> String? {
        let tail = source.substring(with: NSRange(location: start, length: end - start))
        guard let opening = tail.firstIndex(of: "(") else { return nil }
        let delimiter = String(tail[..<opening])
        guard !delimiter.contains(where: { $0.isWhitespace || $0 == "\\" || $0 == ")" }) else { return nil }
        return delimiter
    }

    private func context(in source: NSString, before end: Int) -> Context {
        var index = 0
        var block = false, line = false
        var quote: String?, rawEnd: String?
        func matches(_ value: String, at position: Int) -> Bool {
            !value.isEmpty && position + value.utf16.count <= end && source.substring(with: NSRange(location: position, length: value.utf16.count)) == value
        }
        while index < end {
            let char = source.character(at: index)
            if let closing = rawEnd {
                if matches(closing, at: index) { index += closing.utf16.count; rawEnd = nil } else { index += 1 }
            } else if line {
                if char == 10 || char == 13 { line = false }
                index += 1
            } else if block {
                if matches(blockClose, at: index) { index += blockClose.utf16.count; block = false } else { index += 1 }
            } else if let delimiter = quote {
                if char == 92 { index += 2 }
                else if matches(delimiter, at: index) { index += delimiter.utf16.count; quote = nil }
                else { index += 1 }
            } else if matches(lineComment, at: index) {
                line = true; index += lineComment.utf16.count
            } else if matches(blockOpen, at: index) {
                block = true; index += blockOpen.utf16.count
            } else if let prefix = config.rawStringPrefix, matches(prefix, at: index),
                      let delimiter = rawDelimiter(in: source, start: index + prefix.utf16.count, end: end) {
                rawEnd = ")" + delimiter + "\""
                index += prefix.utf16.count + delimiter.utf16.count + 1
            } else if let pair = config.pairs.first(where: { $0.quote && matches($0.open, at: index) }) {
                let isSeparator = pair.open == "'" && index > 0 && (48...57).contains(source.character(at: index - 1))
                if !isSeparator { quote = pair.close }
                index += pair.open.utf16.count
            } else { index += 1 }
        }
        return Context(comment: block || line, isString: quote != nil || rawEnd != nil, quote: rawEnd == nil ? quote : nil)
    }
}
