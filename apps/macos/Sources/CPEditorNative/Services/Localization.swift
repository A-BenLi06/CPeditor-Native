import Foundation
import NativeEditorModel
import NativeCoreBridge

enum L10n {
    static func string(_ key: String) -> String {
        NSLocalizedString(key, bundle: .module, comment: "")
    }

    static func editorName(_ editor: EditorKind) -> String {
        string(editor == .codeEditorView ? "editor_codeeditorview" : "editor_sttextview")
    }

    static func describe(_ error: Error) -> String {
        guard let wire = error as? WireError else { return error.localizedDescription }
        switch wire {
        case .invalidProfile: return string("invalid_toolchain")
        case .invalidBase64: return string("invalid_test_bytes")
        case .unsupportedSchema: return string("unsupported_schema")
        case .malformedTerminal: return string("invalid_core_event")
        case .busy: return string("core_busy")
        case .closed: return string("core_closed")
        }
    }

    static func jobState(_ state: JobState?) -> String {
        guard let state else { return string("ready_to_run") }
        switch state {
        case .queued: return string("job_queued")
        case .building: return string("job_building")
        case .running: return string("job_running")
        case .checking: return string("job_checking")
        case .completed: return string("job_completed")
        case .cancelled: return string("job_cancelled")
        case .failed: return string("job_failed")
        }
    }
}
