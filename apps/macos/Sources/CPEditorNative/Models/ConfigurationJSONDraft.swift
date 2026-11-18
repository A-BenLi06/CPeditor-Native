import Foundation

struct ConfigurationJSONDraft: Identifiable {
    enum Kind { case toolchain, checker }

    let id = UUID()
    let kind: Kind
    let jsonText: String
    let enableCheckerOnApply: Bool

    init(kind: Kind, jsonText: String, enableCheckerOnApply: Bool = false) {
        self.kind = kind
        self.jsonText = jsonText
        self.enableCheckerOnApply = enableCheckerOnApply
    }
}
