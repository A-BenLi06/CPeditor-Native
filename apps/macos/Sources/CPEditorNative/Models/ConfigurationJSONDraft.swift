import Foundation

struct ConfigurationJSONDraft: Identifiable {
    enum Kind { case toolchain, checker }

    let id = UUID()
    let kind: Kind
    let jsonText: String
}
