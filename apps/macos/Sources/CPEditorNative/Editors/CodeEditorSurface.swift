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

    var body: some View {
        CodeEditor(text: $state.text, position: $state.position,
                   messages: $state.messages, language: state.language)
            .environment(\.codeEditorLayoutConfiguration,
                         CodeEditor.LayoutConfiguration(showMinimap: state.showMinimap,
                                                        wrapText: state.wrapText))
    }
}
