import AppKit
import Observation
import NativeEditorModel
import UniformTypeIdentifiers

struct WorkspaceConfiguration: Decodable {
    let schemaVersion: Int
    let defaultLanguageID: String
    let languageIDs: [String]
    let defaultEditor: EditorKind

    static func load() throws -> Self {
        guard let url = Bundle.module.url(forResource: "workspace", withExtension: "json") else {
            throw CocoaError(.fileNoSuchFile)
        }
        let config = try JSONDecoder().decode(Self.self, from: Data(contentsOf: url))
        guard config.schemaVersion == 1, config.languageIDs.contains(config.defaultLanguageID) else {
            throw CocoaError(.fileReadCorruptFile)
        }
        return config
    }
}

@MainActor @Observable final class Workspace {
    var sessions: [NativeEditorSession] = []
    var selection: UUID?
    var newEditor: EditorKind = .codeEditorView
    var wrapText = false
    var showMinimap = false
    var errorMessage: String?
    @ObservationIgnored private var configuration: WorkspaceConfiguration?
    @ObservationIgnored private var profiles: [String: LanguageProfile] = [:]

    var activeSession: NativeEditorSession? { sessions.first { $0.document.id == selection } }

    init(arguments: [String] = []) {
        do {
            let config = try WorkspaceConfiguration.load()
            configuration = config
            for id in config.languageIDs { profiles[id] = try LanguageProfile.load(id: id) }
            newEditor = config.defaultEditor
            var files: [String] = []
            var cursor = 0
            while cursor < arguments.count {
                if arguments[cursor] == "--editor", cursor + 1 < arguments.count,
                   let kind = EditorKind(rawValue: arguments[cursor + 1]) {
                    newEditor = kind
                    cursor += 2
                } else if !arguments[cursor].hasPrefix("-") {
                    files.append(arguments[cursor])
                    cursor += 1
                } else { cursor += 1 }
            }
            if files.isEmpty { createDocument() }
            else { files.forEach { open(URL(fileURLWithPath: $0)) } }
        } catch { errorMessage = error.localizedDescription }
    }

    func title(for session: NativeEditorSession) -> String {
        session.document.url?.lastPathComponent ?? L10n.string("untitled")
    }

    func createDocument(text: String = "", editor: EditorKind? = nil, languageID: String? = nil) {
        guard let config = configuration, let profile = profiles[languageID ?? config.defaultLanguageID] else { return }
        do {
            let document = SourceDocument(text: text, editorKind: editor ?? newEditor,
                                          languageID: profile.id, saved: text.isEmpty)
            let session = try NativeEditorSession(document: document, profile: profile)
            sessions.append(session)
            selection = document.id
            AppTelemetry.documents.info("Created document with editor \(document.editorKind.rawValue, privacy: .public)")
        } catch { errorMessage = error.localizedDescription }
    }

    func openPanel() {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = true
        panel.canChooseDirectories = false
        if panel.runModal() == .OK { panel.urls.forEach(open) }
    }

    func open(_ url: URL) {
        let normalized = url.standardizedFileURL
        if let existing = sessions.first(where: { $0.document.url?.standardizedFileURL == normalized }) {
            selection = existing.document.id
            return
        }
        guard let config = configuration,
              let profile = profiles.values.first(where: { $0.fileExtensions.contains(url.pathExtension.lowercased()) })
                ?? profiles[config.defaultLanguageID] else { return }
        do {
            let text = try SourceFile.readUTF8(url)
            let document = SourceDocument(text: text, url: url, editorKind: newEditor, languageID: profile.id)
            let session = try NativeEditorSession(document: document, profile: profile)
            sessions.append(session)
            selection = document.id
            AppTelemetry.documents.info("Opened document with editor \(document.editorKind.rawValue, privacy: .public)")
        } catch { errorMessage = error.localizedDescription }
    }

    func openCopyInAlternativeEditor() {
        guard let source = activeSession else { return }
        source.capture()
        guard !source.isComposing else { errorMessage = L10n.string("finish_composition"); return }
        createDocument(text: source.document.text, editor: source.document.editorKind.alternative,
                       languageID: source.document.languageID)
    }

    @discardableResult func save(_ session: NativeEditorSession? = nil, chooseURL: Bool = false) -> Bool {
        guard let session = session ?? activeSession else { return false }
        session.capture()
        guard !session.isComposing else { errorMessage = L10n.string("finish_composition"); return false }
        var destination = chooseURL ? nil : session.document.url
        if destination == nil {
            let panel = NSSavePanel()
            panel.nameFieldStringValue = session.document.url?.lastPathComponent
                ?? profiles[session.document.languageID]?.suggestedFileName ?? L10n.string("untitled")
            if panel.runModal() == .OK { destination = panel.url }
        }
        guard let destination else { return false }
        do {
            let snapshot = session.document.snapshot()
            try SourceFile.write(snapshot, to: destination)
            try session.document.markSaved(at: destination, snapshot: snapshot)
            AppTelemetry.documents.info("Saved document revision \(snapshot.revision, privacy: .public)")
            return true
        } catch { errorMessage = error.localizedDescription; return false }
    }

    @discardableResult func canClose(_ session: NativeEditorSession) -> Bool {
        session.capture()
        if session.isComposing { errorMessage = L10n.string("finish_composition"); return false }
        guard session.document.isDirty else { return true }
        let alert = NSAlert()
        alert.messageText = String(format: L10n.string("save_changes_title"), title(for: session))
        alert.informativeText = L10n.string("save_changes_message")
        alert.addButton(withTitle: L10n.string("save"))
        alert.addButton(withTitle: L10n.string("cancel"))
        alert.addButton(withTitle: L10n.string("discard"))
        switch alert.runModal() {
        case .alertFirstButtonReturn: return save(session)
        case .alertThirdButtonReturn: return true
        default: return false
        }
    }

    func closeActiveDocument() {
        guard let session = activeSession, canClose(session) else { return }
        session.dispose()
        sessions.removeAll { $0.document.id == session.document.id }
        selection = sessions.last?.document.id
        AppTelemetry.documents.info("Closed document")
    }

    func canCloseAll() -> Bool { sessions.allSatisfy(canClose) }
}
