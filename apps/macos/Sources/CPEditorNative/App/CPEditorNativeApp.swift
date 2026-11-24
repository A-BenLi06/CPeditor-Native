import SwiftUI
import AppKit

@main struct CPEditorNativeApp: App {
    @NSApplicationDelegateAdaptor(ApplicationDelegate.self) private var delegate
    @AppStorage(WorkspaceAppearance.defaultsKey) private var appearance: WorkspaceAppearance = .system
    @State private var workspace = Workspace(arguments: Array(CommandLine.arguments.dropFirst()), automaticRecovery: true)

    var body: some Scene {
        Window(L10n.string("app_name"), id: "workspace") {
            WorkspaceView(workspace: workspace)
                .preferredColorScheme(appearance.colorScheme)
        }
        .defaultSize(width: 1240, height: 780)
        .windowToolbarStyle(.unified)
        .commands { EditorCommands() }

        Settings {
            SettingsView(workspace: workspace, appearance: $appearance)
                .preferredColorScheme(appearance.colorScheme)
        }
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
        terminationTask = Task { @MainActor in
            for workspace in workspaces {
                guard await workspace.prepareToClose() else {
                    terminationTask = nil
                    sender.reply(toApplicationShouldTerminate: false)
                    return
                }
            }
            sender.reply(toApplicationShouldTerminate: true)
        }
        return .terminateLater
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    func applicationDidFinishLaunching(_ notification: Notification) {
        AppTelemetry.lifecycle.notice("Application launched")
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
    }
}
