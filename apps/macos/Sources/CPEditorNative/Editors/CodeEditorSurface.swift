import AppKit
import SwiftUI
import CodeEditorView
import LanguageSupport
import NativeEditorModel
import Observation

@MainActor @Observable final class CodeEditorState {
    var text: String
    var position = CodeEditor.Position()
    var messages = Set<TextLocated<Message>>()
    var wrapText = false
    var showMinimap = false
    let language: LanguageConfiguration

    init(text: String, profile: LanguageProfile) throws {
        self.text = text
        language = LanguageConfiguration(
            name: profile.displayName,
            supportsSquareBrackets: profile.supportsSquareBrackets,
            supportsCurlyBrackets: profile.supportsCurlyBrackets,
            stringRegex: try Regex<Substring>(profile.stringPattern, as: Substring.self),
            characterRegex: try Regex<Substring>(profile.characterPattern, as: Substring.self),
            numberRegex: try Regex<Substring>(profile.numberPattern, as: Substring.self),
            singleLineComment: profile.singleLineComment,
            nestedComment: (profile.blockCommentOpen, profile.blockCommentClose),
            identifierRegex: try Regex<Substring>(profile.identifierPattern, as: Substring.self),
            operatorRegex: try Regex<Substring>(profile.operatorPattern, as: Substring.self),
            reservedIdentifiers: profile.keywords,
            reservedOperators: profile.operators)
    }
}

struct CodeEditorSurface: View {
    @Bindable var state: CodeEditorState
    @Environment(\.colorScheme) private var colorScheme

    @MainActor private static let darkTheme = Theme(
        colourScheme: .dark,
        fontName: "SFMono-Regular",
        fontSize: 13.0,
        textColour: NSColor(red: 0.87, green: 0.87, blue: 0.88, alpha: 1.0),
        commentColour: NSColor(red: 0.51, green: 0.55, blue: 0.59, alpha: 1.0),
        stringColour: NSColor(red: 0.94, green: 0.53, blue: 0.46, alpha: 1.0),
        characterColour: NSColor(red: 0.84, green: 0.79, blue: 0.53, alpha: 1.0),
        numberColour: NSColor(red: 0.81, green: 0.74, blue: 0.40, alpha: 1.0),
        identifierColour: NSColor(red: 0.41, green: 0.72, blue: 0.64, alpha: 1.0),
        operatorColour: NSColor(red: 0.62, green: 0.94, blue: 0.87, alpha: 1.0),
        keywordColour: NSColor(red: 0.94, green: 0.51, blue: 0.69, alpha: 1.0),
        symbolColour: NSColor(red: 0.72, green: 0.72, blue: 0.73, alpha: 1.0),
        typeColour: NSColor(red: 0.36, green: 0.85, blue: 1.0, alpha: 1.0),
        fieldColour: NSColor(red: 0.63, green: 0.40, blue: 0.90, alpha: 1.0),
        caseColour: NSColor(red: 0.82, green: 0.66, blue: 1.0, alpha: 1.0),
        backgroundColour: NSColor(red: 0.16, green: 0.16, blue: 0.18, alpha: 1.0),
        currentLineColour: NSColor(red: 0.19, green: 0.20, blue: 0.22, alpha: 1.0),
        selectionColour: NSColor(red: 0.40, green: 0.44, blue: 0.51, alpha: 1.0),
        cursorColour: NSColor(red: 1.0, green: 1.0, blue: 1.0, alpha: 1.0),
        invisiblesColour: NSColor(red: 0.33, green: 0.37, blue: 0.42, alpha: 1.0)
    )

    @MainActor private static let lightTheme = Theme(
        colourScheme: .light,
        fontName: "SFMono-Regular",
        fontSize: 13.0,
        textColour: NSColor(red: 0.15, green: 0.15, blue: 0.15, alpha: 1.0),
        commentColour: NSColor(red: 0.45, green: 0.50, blue: 0.55, alpha: 1.0),
        stringColour: NSColor(red: 0.76, green: 0.24, blue: 0.16, alpha: 1.0),
        characterColour: NSColor(red: 0.14, green: 0.19, blue: 0.81, alpha: 1.0),
        numberColour: NSColor(red: 0.0, green: 0.05, blue: 1.0, alpha: 1.0),
        identifierColour: NSColor(red: 0.23, green: 0.50, blue: 0.54, alpha: 1.0),
        operatorColour: NSColor(red: 0.18, green: 0.05, blue: 0.43, alpha: 1.0),
        keywordColour: NSColor(red: 0.63, green: 0.28, blue: 0.62, alpha: 1.0),
        symbolColour: NSColor(red: 0.24, green: 0.13, blue: 0.48, alpha: 1.0),
        typeColour: NSColor(red: 0.04, green: 0.29, blue: 0.46, alpha: 1.0),
        fieldColour: NSColor(red: 0.36, green: 0.15, blue: 0.60, alpha: 1.0),
        caseColour: NSColor(red: 0.18, green: 0.05, blue: 0.43, alpha: 1.0),
        backgroundColour: NSColor(red: 1.0, green: 1.0, blue: 1.0, alpha: 1.0),
        currentLineColour: NSColor(red: 0.93, green: 0.96, blue: 1.0, alpha: 1.0),
        selectionColour: NSColor(red: 0.73, green: 0.84, blue: 0.99, alpha: 1.0),
        cursorColour: NSColor(red: 0.0, green: 0.0, blue: 0.0, alpha: 1.0),
        invisiblesColour: NSColor(red: 0.84, green: 0.84, blue: 0.84, alpha: 1.0)
    )

    @MainActor static func canvasBackgroundColor(for colorScheme: ColorScheme) -> Color {
        Color(nsColor: colorScheme == .dark ? darkTheme.backgroundColour : lightTheme.backgroundColour)
    }

    var body: some View {
        CodeEditor(text: $state.text, position: $state.position,
                   messages: $state.messages, language: state.language)
            .environment(\.codeEditorTheme, colorScheme == .dark ? Self.darkTheme : Self.lightTheme)
            .environment(\.codeEditorLayoutConfiguration,
                         CodeEditor.LayoutConfiguration(showMinimap: state.showMinimap,
                                                        wrapText: state.wrapText))
    }
}
