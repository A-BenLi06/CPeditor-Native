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

    static func comparisonName(_ policy: ComparisonPolicy) -> String {
        switch policy {
        case .trailing: string("comparison_trailing")
        case .strict: string("comparison_strict")
        case .byte: string("comparison_byte")
        }
    }

    static func describe(_ error: Error) -> String {
        if error is SessionCapacityError { return string("recovery_capacity_exceeded") }
        if let document = error as? DocumentError {
            switch document {
            case .staleRevision: return string("stale_document_revision")
            case .invalidRange: return string("invalid_document_range")
            case .invalidUTF8: return string("invalid_utf8_source")
            case .diskConflict: return string("source_disk_conflict")
            case .revisionExhausted: return string("revision_exhausted")
            case .reservedRecoveryPath: return string("reserved_recovery_path")
            }
        }
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

    static func sessionSaveStatus(_ status: SessionSaveStatus) -> String {
        switch status {
        case .saved: return string("recovery_saved")
        case .conflict: return string("recovery_conflict")
        case .invalidSession: return string("recovery_invalid_session")
        case .invalidExisting: return string("recovery_preserved")
        case .ioFailed: return string("recovery_io_failed")
        case .durabilityUnknown: return string("recovery_durability_unknown")
        case .lockUnavailable: return string("recovery_lock_unavailable")
        case .cancelled: return string("recovery_cancelled")
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

    static func checkerVerdict(_ verdict: String) -> String {
        switch verdict {
        case "Accepted": return string("checker_verdict_accepted")
        case "WrongAnswer": return string("checker_verdict_wrong_answer")
        case "Cancelled": return string("checker_verdict_cancelled")
        case "CheckerError": return string("checker_verdict_error")
        default: return String(format: string("checker_verdict_unknown"), verdict)
        }
    }
}
