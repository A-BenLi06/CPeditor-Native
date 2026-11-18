import CPNativeCore
import Foundation

extension ToolchainProfile {
    public init(validating json: Data) throws {
        // Validate the original bytes before Swift decoding can normalize JSON numbers or keys.
        try Self.validate(json)
        self = try JSONDecoder().decode(Self.self, from: json)
    }

    public func validateForExecution() throws {
        try Self.validate(JSONEncoder().encode(self))
    }

    private static func validate(_ json: Data) throws {
        var error = CoreCoordinator.errorStorage()
        let status = json.withUnsafeBytes { bytes in
            let input = cpn_bytes(data: bytes.bindMemory(to: UInt8.self).baseAddress, size: bytes.count)
            return cpn_validate_toolchain_json(input, &error)
        }
        try CoreCoordinator.check(status, &error)
    }
}
