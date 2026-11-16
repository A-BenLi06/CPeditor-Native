import Foundation
import NativeEditorModel

enum L10n {
    static func string(_ key: String) -> String {
        NSLocalizedString(key, bundle: .module, comment: "")
    }

    static func editorName(_ editor: EditorKind) -> String {
        string(editor == .codeEditorView ? "editor_codeeditorview" : "editor_sttextview")
    }
}
