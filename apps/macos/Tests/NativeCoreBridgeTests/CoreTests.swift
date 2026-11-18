import Foundation
import Testing
import NativeEditorModel
@testable import NativeCoreBridge

@Suite(.serialized) struct CoreTests {
    private func profile(timeout: Int = 2000) throws -> ToolchainProfile {
        let json = """
        {"schema_version":1,"id":"clang-test","language_id":"cpp","source_filename":"main.cpp",
         "artifact_name":"program","build":{"executable":"/usr/bin/clang++","arguments":["-std=c++17","${source}","-o","${artifact}"]},
         "run":{"executable":"${artifact}","arguments":[]},"build_limits":{"wall_timeout_ms":30000},
         "run_limits":{"wall_timeout_ms":\(timeout)}}
        """
        return try JSONDecoder().decode(ToolchainProfile.self, from: Data(json.utf8))
    }

    @MainActor private func snapshot(_ text: String) -> SourceSnapshot {
        SourceDocument(text: text, languageID: "cpp").snapshot()
    }

    @Test func compileUnsavedCppAndReleaseBuffersBetweenRuns() async throws {
        let coordinator = CoreCoordinator(pollingMilliseconds: 10, maximumBuffers: 1, maximumJobs: 1)
        let source = await snapshot("#include <iostream>\nint main(){int a,b;if(std::cin>>a>>b)std::cout<<a+b<<'\\n';}\n")
        let request = BatchRequest(snapshot: source, originalPath: nil, toolchain: try profile(),
                                   tests: [TestCaseInput(input: "1 2\n", expected: "3\n")])
        for _ in 0..<2 {
            let result = try await coordinator.run(request)
            #expect(result.matches(source))
            #expect(result.buildOutcome == "Succeeded")
            #expect(result.tests.first?.verdict == "AC")
            #expect(result.tests.first?.process.stdout.data == Data("3\n".utf8))
        }
        await coordinator.shutdown()
    }

    @Test func cancellationReturnsTerminalAndAllowsNextJob() async throws {
        let coordinator = CoreCoordinator(pollingMilliseconds: 10, maximumBuffers: 1, maximumJobs: 1)
        let source = await snapshot("int main(){for(;;){}}\n")
        let request = BatchRequest(snapshot: source, originalPath: nil, toolchain: try profile(timeout: 10000),
                                   tests: [TestCaseInput(input: "", expected: nil)])
        let result = try await coordinator.run(request) { event in
            if event.state == .running { try? await coordinator.cancel() }
        }
        #expect(result.tests.first?.process.termination == "Cancelled" || result.buildOutcome == "Cancelled")
        let next = await snapshot("int main(){return 0;}\n")
        let nextResult = try await coordinator.run(BatchRequest(snapshot: next, originalPath: nil,
                                                                toolchain: try profile(), tests: [TestCaseInput()]))
        #expect(nextResult.tests.first?.verdict == "AC")
        await coordinator.shutdown()
    }

    @Test func emptyAbsentBinaryAndUnknownProfileFieldsSurviveEncoding() throws {
        let data = Data("[{\"id\":\"a\",\"input\":{\"encoding\":\"base64\",\"data\":\"YQD/\"},\"expected\":\"\"},{\"id\":\"b\",\"input\":\"\"}]".utf8)
        let tests = try JSONDecoder().decode([TestCaseInput].self, from: data)
        #expect(tests[0].input.data == Data([97, 0, 255]))
        #expect(tests[0].expected?.data == Data())
        #expect(tests[1].expected == nil)
        #expect(try JSONDecoder().decode([TestCaseInput].self, from: JSONEncoder().encode(tests)) == tests)
        var fields: [String: JSONValue]
        guard case .object(let original) = try profile().payload else { return }
        fields = original
        fields["unknown_for_core_validation"] = .bool(true)
        let preserving = try ToolchainProfile(payload: .object(fields))
        let restored = try JSONDecoder().decode(ToolchainProfile.self, from: JSONEncoder().encode(preserving))
        #expect(restored.payload["unknown_for_core_validation"] == .bool(true))
        #expect(throws: WireError.invalidBase64) {
            try JSONDecoder().decode(WireBytes.self, from: Data("{\"encoding\":\"base64\",\"data\":\"Zh==\"}".utf8))
        }
    }

    @Test func toolchainValidationChecksOriginalJSONAndRetainsLargeIntegers() throws {
        let prefix = #"{"id":"custom","language_id":"cpp","source_filename":"main.cpp","run":{"executable":"/usr/bin/true","arguments":[]},"#
        let valid = prefix + #""schema_version":1,"run_limits":{"stdout_bytes":9007199254740993}}"#
        let profile = try ToolchainProfile(validating: Data(valid.utf8))
        try profile.validateForExecution()
        #expect(try profile.jsonText().contains("9007199254740993"))

        for version in [#""schema_version":1.0}"#, #""schema_version":1,"schema_version":1}"#] {
            #expect(throws: CoreFailure.self) { try ToolchainProfile(validating: Data((prefix + version).utf8)) }
        }
    }
}
