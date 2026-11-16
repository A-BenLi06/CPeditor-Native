import Foundation
import OSLog

enum AppTelemetry {
    private static let subsystem = Bundle.main.bundleIdentifier ?? "CPEditorNative"
    static let lifecycle = Logger(subsystem: subsystem, category: "Lifecycle")
    static let documents = Logger(subsystem: subsystem, category: "Documents")
}
