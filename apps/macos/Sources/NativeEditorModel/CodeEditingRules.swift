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
    private let numberExpression: NSRegularExpression

    public init(profile: LanguageProfile) throws {
        config = profile.editing
        lineComment = profile.singleLineComment
        blockOpen = profile.blockCommentOpen
        blockClose = profile.blockCommentClose
        numberExpression = try NSRegularExpression(pattern: profile.numberPattern)
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
            let closesContext = context.isString ? context.quote == value : context.brackets.last?.pair.close == value
            if selection.length == 0, closesContext, matches(value, at: start),
               !context.comment, backslashes % 2 == 0 {
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
                let openingLocation = start - pair.open.utf16.count
                if pair.quote {
                    guard context.quote == pair.close, context.quoteLocation == openingLocation else { continue }
                } else {
                    guard !context.isString, context.brackets.last?.location == openingLocation else { continue }
                }
                let range = NSRange(location: start - pair.open.utf16.count, length: pair.open.utf16.count + pair.close.utf16.count)
                return CodeEdit(range: range, replacement: "", selection: NSRange(location: range.location, length: 0))
            }
            return nil
        case .newline:
            let line = source.lineRange(for: NSRange(location: start, length: 0))
            let before = source.substring(with: NSRange(location: line.location, length: start - line.location))
            let whitespace = String(before.prefix { $0 == " " || $0 == "\t" })
            let trimmed = before.trimmingCharacters(in: .whitespaces)
            let eol = lineEnding(in: source, at: start)
            let pair = context.comment || context.isString ? nil : config.pairs.first { $0.indents && trimmed.hasSuffix($0.open) }
            let indent = whitespace + (pair == nil ? "" : config.indent)
            let first = eol + indent
            if let pair, matches(pair.close, at: end) {
                return replacement(first + eol + whitespace, offset: first.utf16.count)
            }
            return replacement(first)
        case .indent, .outdent:
            let isOutdent: Bool = if case .outdent = action { true } else { false }
            if !isOutdent && selection.length == 0 {
                return replacement(config.indent)
            }
            // NSString uses the half-open range, excluding a line starting at the selection end.
            let lineRange = source.lineRange(for: selection)
            let lines = source.substring(with: lineRange) as NSString
            var cursor = 0, result = "", startDelta = 0, endDelta = 0
            while cursor < lines.length {
                let range = lines.lineRange(for: NSRange(location: cursor, length: 0))
                let line = lines.substring(with: range)
                let removed = isOutdent ? (line.hasPrefix("\t") ? 1 : min(config.indent.utf16.count, line.prefix { $0 == " " }.count)) : 0
                let lineStart = lineRange.location + cursor
                func delta(at anchor: Int) -> Int {
                    guard anchor >= lineStart else { return 0 }
                    return isOutdent ? -min(removed, anchor - lineStart) : config.indent.utf16.count
                }
                startDelta += delta(at: start)
                endDelta += delta(at: end)
                result += isOutdent ? String(line.dropFirst(removed)) : config.indent + line
                cursor = NSMaxRange(range)
            }
            if result.utf16.elementsEqual((lines as String).utf16) {
                return CodeEdit(range: NSRange(location: start, length: 0), replacement: "", selection: selection)
            }
            let location = max(lineRange.location, start + startDelta)
            let length = max(0, end + endDelta - location)
            return CodeEdit(range: lineRange, replacement: result, selection: NSRange(location: location, length: length))
        }
    }

    private struct Context {
        var comment = false
        var isString = false
        var quote: String?
        var quoteLocation: Int?
        var brackets: [(pair: EditingConfiguration.Pair, location: Int)] = []
    }

    private func lineEnding(in source: NSString, at position: Int) -> String {
        var start = 0, end = 0, contentsEnd = 0
        source.getLineStart(&start, end: &end, contentsEnd: &contentsEnd, for: NSRange(location: position, length: 0))
        if end > contentsEnd { return source.substring(with: NSRange(location: contentsEnd, length: end - contentsEnd)) }
        // An unterminated last line inherits the preceding line's terminator.
        if start > 0 {
            source.getLineStart(nil, end: &end, contentsEnd: &contentsEnd, for: NSRange(location: start - 1, length: 0))
            if end > contentsEnd { return source.substring(with: NSRange(location: contentsEnd, length: end - contentsEnd)) }
        }
        return "\n"
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
        var quoteLocation: Int?
        var brackets: [(pair: EditingConfiguration.Pair, location: Int)] = []
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
                if char == 92 {
                    index += matches("\r\n", at: index + 1) ? 3 : 2
                } else if matches(delimiter, at: index) {
                    index += delimiter.utf16.count; quote = nil; quoteLocation = nil
                } else if char == 10 || char == 13 {
                    // A half-written ordinary literal ends at an unescaped newline.
                    quote = nil; quoteLocation = nil; index += 1
                }
                else { index += 1 }
            } else if matches(lineComment, at: index) {
                line = true; index += lineComment.utf16.count
            } else if matches(blockOpen, at: index) {
                block = true; index += blockOpen.utf16.count
            } else if let prefix = config.rawStringPrefix, matches(prefix, at: index),
                      let delimiter = rawDelimiter(in: source, start: index + prefix.utf16.count, end: end) {
                rawEnd = ")" + delimiter + "\""
                index += prefix.utf16.count + delimiter.utf16.count + 1
            } else if (48...57).contains(char),
                      let number = numberExpression.firstMatch(in: source as String, options: [.anchored, .withTransparentBounds],
                                                              range: NSRange(location: index, length: end - index)), number.range.length > 0 {
                index = NSMaxRange(number.range)
            } else if let pair = config.pairs.first(where: { $0.quote && matches($0.open, at: index) }) {
                quote = pair.close
                quoteLocation = index
                index += pair.open.utf16.count
            } else if let pair = config.pairs.first(where: { !$0.quote && matches($0.open, at: index) }) {
                brackets.append((pair, index))
                index += pair.open.utf16.count
            } else if let pair = config.pairs.first(where: { !$0.quote && matches($0.close, at: index) }) {
                if brackets.last?.pair.close == pair.close { brackets.removeLast() }
                index += pair.close.utf16.count
            } else { index += 1 }
        }
        return Context(comment: block || line, isString: quote != nil || rawEnd != nil, quote: rawEnd == nil ? quote : nil,
                       quoteLocation: quoteLocation, brackets: brackets)
    }
}
