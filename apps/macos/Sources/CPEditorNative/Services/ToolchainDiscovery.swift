import Foundation
import NativeCoreBridge

/// Discovery is configuration-driven and never treats Apple's /usr/bin/g++ alias as GNU GCC.
struct ToolchainDiscovery: Decodable {
    let schemaVersion: Int
    let searchDirectories: [String]
    let executablePattern: String
    let profileResources: [String]

    static func load() throws -> Self {
        guard let url = Bundle.module.url(forResource: "toolchain-discovery", withExtension: "json") else {
            throw CocoaError(.fileNoSuchFile)
        }
        let config = try JSONDecoder().decode(Self.self, from: Data(contentsOf: url))
        guard config.schemaVersion == 1 else { throw WireError.unsupportedSchema }
        return config
    }

    func findCompiler(path: String = ProcessInfo.processInfo.environment["PATH"] ?? "") throws -> URL? {
        let expression = try NSRegularExpression(pattern: executablePattern)
        let directories = directories(path: path)
        var candidates: [(version: Int, order: Int, url: URL)] = []
        for (order, directory) in directories.enumerated() where directory.hasPrefix("/") {
            let root = URL(fileURLWithPath: directory, isDirectory: true)
            for name in (try? FileManager.default.contentsOfDirectory(atPath: directory)) ?? [] {
                guard let match = expression.firstMatch(in: name, range: NSRange(name.startIndex..., in: name)),
                      match.numberOfRanges > 1, let range = Range(match.range(at: 1), in: name),
                      let version = Int(name[range]) else { continue }
                let url = root.appendingPathComponent(name)
                var isDirectory: ObjCBool = false
                guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirectory), !isDirectory.boolValue,
                      FileManager.default.isExecutableFile(atPath: url.path) else { continue }
                candidates.append((version, order, url))
            }
        }
        return candidates.sorted { $0.version == $1.version ? $0.order < $1.order : $0.version > $1.version }.first?.url
    }

    struct Catalog {
        let templates: [ToolchainProfile]
        let profiles: [ToolchainProfile]
        let unavailable: [ToolchainProfile]
        let managedDirectories: Set<String>
        let executablePattern: NSRegularExpression

        func isMissingManagedCompiler(_ executable: String) -> Bool {
            guard executable.hasPrefix("/"), !FileManager.default.fileExists(atPath: executable) else { return false }
            let url = URL(fileURLWithPath: executable).standardizedFileURL
            let name = url.lastPathComponent
            let range = NSRange(name.startIndex..., in: name)
            return managedDirectories.contains(url.deletingLastPathComponent().path)
                && executablePattern.firstMatch(in: name, range: range)?.range == range
        }
    }

    private func directories(path: String) -> [String] {
        (searchDirectories + path.split(separator: ":").map(String.init))
            .filter { $0.hasPrefix("/") }
            .map { URL(fileURLWithPath: $0, isDirectory: true).standardizedFileURL.path }
    }

    func discover(path: String = ProcessInfo.processInfo.environment["PATH"] ?? "") throws -> Catalog {
        let compiler = try findCompiler(path: path)
        let templates = try profileResources.map { resource in
            guard let url = Bundle.module.url(forResource: resource, withExtension: "json") else { throw CocoaError(.fileNoSuchFile) }
            let template = try ToolchainProfile(validating: Data(contentsOf: url))
            return template
        }
        let profiles = try templates.map { template in
            guard let compiler else { return template }
            guard let buildTimeout = template.buildWallTimeoutMS, let runTimeout = template.runWallTimeoutMS else { throw WireError.invalidProfile }
            let profile = try template.updating(buildExecutable: compiler.path, buildArguments: template.buildArguments,
                                               buildWallTimeoutMS: buildTimeout, runWallTimeoutMS: runTimeout)
            try profile.validateForExecution()
            return profile
        }
        return Catalog(templates: templates, profiles: profiles, unavailable: compiler == nil ? profiles : [],
                       managedDirectories: Set(directories(path: path)),
                       executablePattern: try NSRegularExpression(pattern: executablePattern))
    }
}
