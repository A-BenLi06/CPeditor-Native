import AppKit
import Foundation
import Testing
import NativeCoreBridge
import NativeEditorModel
@testable import CPEditorNative

@MainActor @Suite(.serialized) struct SettingsTests {
    private let checkerJSON = #"{"schema_version":1,"command":{"executable":"/usr/bin/true","arguments":[]}}"#

    @Test func updatingCheckerHonorsExplicitEnableChoice() throws {
        _ = NSApplication.shared
        let workspace = Workspace()
        defer { workspace.sessions.forEach { $0.dispose() } }

        let draft = ConfigurationJSONDraft(kind: .checker, jsonText: checkerJSON)
        try workspace.applyRunnerConfiguration(draft, jsonText: checkerJSON)
        #expect(workspace.runner.externalChecker?.originalJSON == checkerJSON)
        #expect(!workspace.runner.externalCheckerEnabled)

        let replacement = #"{"schema_version":1,"command":{"executable":"/usr/bin/false","arguments":[]}}"#
        let edit = ConfigurationJSONDraft(kind: .checker, jsonText: checkerJSON)
        try workspace.applyRunnerConfiguration(edit, jsonText: replacement)
        #expect(workspace.runner.externalChecker?.originalJSON == replacement)
        #expect(!workspace.runner.externalCheckerEnabled)

        let enable = ConfigurationJSONDraft(kind: .checker, jsonText: replacement, enableCheckerOnApply: true)
        try workspace.applyRunnerConfiguration(enable, jsonText: replacement)
        #expect(workspace.runner.externalCheckerEnabled)
    }

    @Test func unrunToolchainChoicesAndEditorSettingsSurviveWorkspaceRecovery() async throws {
        _ = NSApplication.shared
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: folder) }
        let file = folder.appendingPathComponent("workspace.json")
        let workspace = Workspace(recoveryURL: file)
        await workspace.bootstrap()
        let first = try #require(workspace.activeSession)
        let firstChoice = try #require(workspace.runner.selectedProfile)
        let secondChoice = try #require(workspace.runner.profiles.first { $0.id != firstChoice.id })

        workspace.selectToolchain(secondChoice.id)
        workspace.newEditor = .stTextView
        workspace.wrapText = true
        workspace.showMinimap = true
        workspace.runner.comparison = .strict
        let second = try #require(workspace.createDocument())
        workspace.selectToolchain(firstChoice.id)

        workspace.selection = first.document.id
        #expect(workspace.runner.selectedProfileID == secondChoice.id)
        workspace.selection = second.document.id
        #expect(workspace.runner.selectedProfileID == firstChoice.id)
        #expect(!workspace.runner.isRunning)

        #expect(await workspace.recovery?.flush() == true)
        await workspace.recovery?.shutdown()
        await workspace.runner.shutdown()
        workspace.sessions.forEach { $0.dispose() }

        let reopened = Workspace(recoveryURL: file)
        await reopened.bootstrap()
        let rows = reopened.sessions
        #expect(reopened.selection == second.document.id)
        #expect(rows.first { $0.document.id == first.document.id }?.toolchainProfileID == secondChoice.id)
        #expect(rows.first { $0.document.id == second.document.id }?.toolchainProfileID == firstChoice.id)
        reopened.selection = first.document.id
        #expect(reopened.runner.selectedProfileID == secondChoice.id)
        #expect(reopened.newEditor == .stTextView)
        #expect(reopened.wrapText)
        #expect(reopened.showMinimap)
        #expect(reopened.runner.comparison == .strict)
        await reopened.recovery?.shutdown()
        await reopened.runner.shutdown()
        reopened.sessions.forEach { $0.dispose() }
    }

    @Test func invalidToolchainEditsKeepTheSelectedProfileIntact() throws {
        _ = NSApplication.shared
        let workspace = Workspace()
        defer { workspace.sessions.forEach { $0.dispose() } }
        let session = try #require(workspace.activeSession)
        let original = try #require(workspace.runner.selectedProfile)
        let originalCatalog = workspace.runner.profiles
        workspace.selectToolchain(original.id)
        let draft = ConfigurationJSONDraft(kind: .toolchain, jsonText: try original.jsonText())

        let invalid = try JSONEncoder().encode(ToolchainProfile(payload: .object([
            ToolchainProfile.Field.id.rawValue: .string(original.id.rawValue),
            ToolchainProfile.Field.languageID.rawValue: .string(original.languageID),
            ToolchainProfile.Field.schemaVersion.rawValue: .number(1)
        ])))
        #expect(throws: CoreFailure.self) {
            try workspace.applyRunnerConfiguration(draft, jsonText: String(decoding: invalid, as: UTF8.self))
        }

        let oversized = try original.updating(buildExecutable: original.buildExecutable,
            buildArguments: [String(repeating: "a", count: SessionLimits.maximumPreferenceBytes)],
            buildWallTimeoutMS: 30_000, runWallTimeoutMS: 2_000)
        #expect(throws: ToolchainPreferenceCapacityError.self) {
            try workspace.applyRunnerConfiguration(draft, jsonText: oversized.jsonText())
        }
        #expect(workspace.runner.profiles == originalCatalog)
        #expect(workspace.runner.selectedProfile == original)
        #expect(session.toolchainProfileID == original.id)

        var replacementFields = try #require(original.payload.object)
        replacementFields[ToolchainProfile.Field.id.rawValue] = .string("custom-toolchain")
        let replacement = try ToolchainProfile(payload: .object(replacementFields))
        try workspace.applyRunnerConfiguration(draft, jsonText: replacement.jsonText())
        #expect(workspace.runner.selectedProfile == replacement)
        #expect(session.toolchainProfileID == replacement.id)
    }

    @Test func recoveryReplacesLegacyInvalidToolchainAndKeepsDocuments() async throws {
        _ = NSApplication.shared
        let workspace = Workspace()
        defer { workspace.sessions.forEach { $0.dispose() } }
        let configuration = try RecoveryConfiguration.load()
        let original = try #require(workspace.runner.selectedProfile)
        let source = "// recover this source\nint main() {}\n"
        let document = try #require(workspace.createDocument(text: source))
        document.testCases = [TestCaseInput(input: "", expected: "")]
        let invalidCustomID = RawIdentifier("legacy-incomplete")
        document.toolchainProfileID = invalidCustomID
        let previous = try workspace.sessionRecord(revision: 1, configuration: configuration)
        var invalidFields: [String: JSONValue] = [
            ToolchainProfile.Field.id.rawValue: .string(original.id.rawValue),
            ToolchainProfile.Field.languageID.rawValue: .string(original.languageID),
            ToolchainProfile.Field.schemaVersion.rawValue: .number(1)
        ]
        let invalidBundled = try ToolchainProfile(payload: .object(invalidFields))
        invalidFields[ToolchainProfile.Field.id.rawValue] = .string(invalidCustomID.rawValue)
        let invalidCustom = try ToolchainProfile(payload: .object(invalidFields))
        let native = WorkspaceNativePreferences(profiles: [invalidBundled, invalidCustom], selectedProfileID: invalidCustomID,
            comparison: .byte, newEditor: .stTextView, wrapText: true, showMinimap: false)
        let preference = NativePreferences(namespaceID: configuration.workspaceNamespace, schemaVersion: 1,
            objectJSON: String(decoding: try JSONEncoder().encode(native), as: UTF8.self))
        let legacy = WorkspaceSessionRecord(workspaceID: previous.workspaceID, revision: previous.revision,
            selectedDocumentID: document.document.id, documents: previous.documents, editorPreferences: [preference])

        try workspace.applyRecoveryCandidate(legacy, configuration: configuration)
        let restored = try #require(workspace.activeSession)
        #expect(workspace.runner.selectedProfile == original)
        #expect(restored.toolchainProfileID == invalidCustomID)
        #expect(workspace.runner.comparison == .byte)
        #expect(restored.document.text == source)
        #expect(workspace.preferenceNotice?.contains(L10n.string("recovered_invalid_toolchains")) == true)
        workspace.runner.runAll(restored)
        await workspace.runner.waitForCurrentRun()
        #expect(workspace.runner.result(for: restored.document.id)?.profileID == original.id.rawValue)
        #expect(workspace.runner.result(for: restored.document.id)?.tests.first?.verdict == "AC")
        await workspace.runner.shutdown()
    }

    @Test func frozenWorkspaceRejectsPersistentSettingsButAllowsLayoutChanges() throws {
        _ = NSApplication.shared
        let workspace = Workspace()
        defer { workspace.sessions.forEach { $0.dispose() } }
        let configuration = try RecoveryConfiguration.load()
        let record = try workspace.sessionRecord(revision: .max, configuration: configuration)
        try workspace.applyRecoveryCandidate(record, configuration: configuration)

        let initialProfile = workspace.runner.selectedProfileID
        let alternate = try #require(workspace.runner.profiles.first { $0.id != initialProfile })
        let draft = ConfigurationJSONDraft(kind: .checker, jsonText: checkerJSON)
        #expect(!workspace.canConfigureRunner)
        #expect(!workspace.canChangeEditorSettings)
        #expect(workspace.canChangeLayout)
        workspace.selectToolchain(alternate.id)
        #expect(workspace.runner.selectedProfileID == initialProfile)
        #expect(throws: DocumentError.revisionExhausted) {
            try workspace.applyRunnerConfiguration(draft, jsonText: checkerJSON)
        }
        #expect(workspace.runner.externalChecker == nil)
    }
}
