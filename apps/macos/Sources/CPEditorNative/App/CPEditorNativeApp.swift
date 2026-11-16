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
    private var terminationTask: Task<Void, Never>?

    static func register(_ workspace: Workspace) {
        registrations.removeAll { $0.workspace == nil }
        if !registrations.contains(where: { $0.workspace === workspace }) {
            registrations.append(Registration(workspace))
        }
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard terminationTask == nil else { return .terminateLater }
        let workspaces = Self.registrations.compactMap(\.workspace)
        guard workspaces.allSatisfy({ $0.canCloseAll() }) else { return .terminateCancel }
        terminationTask = Task { @MainActor in
            for workspace in workspaces { await workspace.runner.shutdown() }
            sender.reply(toApplicationShouldTerminate: true)
        }
        return .terminateLater
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        AppTelemetry.lifecycle.notice("Application launched")
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
    }
}
