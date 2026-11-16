import SwiftUI
import AppKit

@main struct CPEditorNativeApp: App {
    @NSApplicationDelegateAdaptor(ApplicationDelegate.self) private var delegate

    var body: some Scene {
        WindowGroup(L10n.string("app_name")) { WorkspaceView() }
            .defaultSize(width: 1100, height: 720)
            .commands { EditorCommands() }
    }
}

@MainActor final class ApplicationDelegate: NSObject, NSApplicationDelegate {
    private final class Registration {
        weak var workspace: Workspace?
        init(_ workspace: Workspace) { self.workspace = workspace }
    }
    private static var registrations: [Registration] = []

    static func register(_ workspace: Workspace) {
        registrations.removeAll { $0.workspace == nil }
        if !registrations.contains(where: { $0.workspace === workspace }) {
            registrations.append(Registration(workspace))
        }
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        Self.registrations.compactMap(\.workspace).allSatisfy { $0.canCloseAll() } ? .terminateNow : .terminateCancel
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        AppTelemetry.lifecycle.notice("Application launched")
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
    }
}
