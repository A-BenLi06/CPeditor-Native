import Foundation
import Testing
import NativeEditorModel
@testable import NativeCoreBridge

@Suite(.serialized) struct CheckerTests {
    private let base = "{\"schema_version\":1,\"command\":{\"executable\":\"checker\",\"arguments\":[\"${input}\",\"${actual}\",\"${expected}\"]}}"
    private func changing(_ transform: (inout [String: Any]) -> Void) throws -> String {
        var value = try #require(JSONSerialization.jsonObject(with: Data(base.utf8)) as? [String: Any])
        transform(&value)
        return String(decoding: try JSONSerialization.data(withJSONObject: value, options: [.sortedKeys]), as: UTF8.self)
    }
    private func checker(mode: String, extra: [String] = [], wallMS: Int = 2000) throws -> CheckerConfiguration {
        let macos = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        let library = ProcessInfo.processInfo.environment["CPN_CORE_LIBRARY_DIR"] ?? macos.appendingPathComponent("../../out/core/dev").standardized.path
        let helper = URL(fileURLWithPath: library).appendingPathComponent("cpn_checker_helper")
        #expect(FileManager.default.isExecutableFile(atPath: helper.path))
        let object: [String: Any] = ["schema_version": 1, "command": ["executable": helper.path,
            "arguments": [mode, "${input}", "${actual}", "${expected}"] + extra], "limits": ["wall_timeout_ms": wallMS]]
        return try CheckerConfiguration(jsonText: String(decoding: JSONSerialization.data(withJSONObject: object), as: UTF8.self))
    }
    private func profile() throws -> ToolchainProfile {
        let json = """
        {"schema_version":1,"id":"checker-clang","language_id":"cpp","source_filename":"main.cpp","artifact_name":"program",
         "build":{"executable":"/usr/bin/clang++","arguments":["-std=c++17","${source}","-o","${artifact}"]},
         "run":{"executable":"${artifact}","arguments":[]},"build_limits":{"wall_timeout_ms":30000},"run_limits":{"wall_timeout_ms":2000}}
        """
        return try JSONDecoder().decode(ToolchainProfile.self, from: Data(json.utf8))
    }
    @MainActor private func snapshot(_ text: String) -> SourceSnapshot { SourceDocument(text: text, languageID: "cpp").snapshot() }

    @Test func strictCheckerJSONRejectsDuplicatesFloatTypesUnknownsAndOverflows() throws {
        _ = try CheckerConfiguration(jsonText: base)
        let invalid = [
            base.replacingOccurrences(of: "\"schema_version\":1", with: "\"schema_version\":1.0"),
            base.replacingOccurrences(of: "\"schema_version\":1", with: "\"schema_version\":1,\"schema_version\":1"),
            base.replacingOccurrences(of: "\"schema_version\":1", with: "\"schema_version\":1,\"schema_vers\\u0069on\":1"),
            try changing { $0["limits"] = ["memory_bytes": 1024] },
            try changing { $0["limits"] = ["wall_timeout_ms": 0] },
            try changing { $0["limits"] = ["wall_timeout_ms": Int64.max] },
            try changing { $0["accepted_exit_codes"] = [0, 1] },
            try changing { $0["accepted_exit_codes"] = [] },
            try changing { $0["wrong_answer_exit_codes"] = [2, 2] },
            try changing { $0["accepted_exit_codes"] = [256] },
            try changing { $0["environment"] = [["name": "PATH", "value": "a"], ["name": "path", "value": "b"]] },
            try changing { $0["environment"] = [["name": "A=B", "value": "a"]] },
            try changing { $0["environment"] = [["name": "A=\u{301}", "value": "a"]] },
            try changing { $0["environment"] = [["name": "A", "value": "a\0b"]] },
            try changing { $0["environment"] = [["name": "A", "value": "a\0\u{301}"]] },
            try changing { $0["command"] = ["executable": "checker", "arguments": ["${unsupported}"]] },
            try changing { $0["command"] = ["executable": "checker", "arguments": ["${input"]] },
            try changing { $0["command"] = ["executable": "checker\0", "arguments": []] },
            try changing { $0["command"] = ["executable": "checker\0\u{301}", "arguments": []] },
            try changing { $0["command"] = ["executable": "checker", "arguments": ["${\u{301}input}"]] },
            try changing { $0["command"] = ["executable": "checker", "arguments": Array(repeating: "arg", count: 129)] },
            try changing { $0["command"] = ["executable": String(repeating: "a", count: 65537), "arguments": []] },
            base.replacingOccurrences(of: "}}", with: "},\"limits\":{\"wall_timeout_ms\":2.0}}"),
            base.replacingOccurrences(of: "}}", with: "},\"accepted_exit_codes\":[0.0]}"),
            base.replacingOccurrences(of: "}}", with: "},\"limits\":{\"stdout_bytes\":18446744073709551616}}"),
        ]
        for text in invalid { #expect(throws: CheckerConfigurationError.self) { try CheckerConfiguration(jsonText: text) } }
        let permitted = try changing { $0["limits"] = ["terminate_grace_ms": 0, "stdout_bytes": Int64.max, "combined_bytes": 0] }
        let value = try CheckerConfiguration(jsonText: permitted)
        let encoded = String(decoding: try JSONEncoder().encode(value), as: UTF8.self)
        #expect(encoded.contains("9223372036854775807"))
        #expect(value.originalJSON.utf8.elementsEqual(permitted.utf8))
        _ = try CheckerConfiguration(jsonText: changing { $0["command"] = ["executable": "checker", "arguments": ["${input}\u{301}"]] })
    }

    @Test func checkerErrorsAreIndependentFromSuccessfulSolutionAndMissingExpectedSkipsChecker() async throws {
        let coordinator = CoreCoordinator(maximumBuffers: 1, maximumJobs: 1)
        let source = await snapshot("#include <iostream>\nint main(){std::cout<<\"same\";}\n")
        let result = try await coordinator.run(BatchRequest(snapshot: source, originalPath: nil, toolchain: try profile(),
            tests: [TestCaseInput(id: "ac", input: "", expected: "same"), TestCaseInput(id: "wa", input: "", expected: "different"),
                    TestCaseInput(id: "empty", input: "", expected: ""), TestCaseInput(id: "unchecked", input: "", expected: nil)], externalChecker: try checker(mode: "compare")))
        #expect(result.tests.map(\.verdict) == ["AC", "WA", "WA", "Unchecked"])
        #expect(result.tests[0].checker?.process.exitCode == 0)
        #expect(result.tests[1].checker?.process.exitCode == 1)
        #expect(result.tests[2].checker?.processAttempted == true)
        #expect(result.tests[3].checker == nil)
        #expect(result.tests.allSatisfy { $0.process.termination == "Exited" && $0.process.exitCode == 0 })
        let timed = try await coordinator.run(BatchRequest(snapshot: source, originalPath: nil, toolchain: try profile(),
            tests: [TestCaseInput(input: "", expected: "same")], externalChecker: try checker(mode: "sleep", wallMS: 100)))
        #expect(timed.tests.first?.verdict == "CheckerError")
        #expect(timed.tests.first?.process.termination == "Exited")
        #expect(timed.tests.first?.checker?.process.termination == "TimedOut")
        #expect(timed.tests.first?.checker?.processAttempted == true)
        let score = try await coordinator.run(BatchRequest(snapshot: source, originalPath: nil, toolchain: try profile(),
            tests: [TestCaseInput(input: "", expected: "same")], externalChecker: try checker(mode: "exit", extra: ["7"])))
        #expect(score.tests.first?.verdict == "CheckerError")
        #expect(score.tests.first?.checker?.process.exitCode == 7)
        #expect(score.tests.first?.checker?.process.stdout.data == Data("0.5\n".utf8))
        await coordinator.shutdown()
    }

    @Test func rawCheckerDiagnosticBytesTotalsAndSignalsSurviveBridge() async throws {
        let coordinator = CoreCoordinator()
        let source = await snapshot("#include <iostream>\nint main(){const char v[]={97,99,116,117,97,108,0,(char)255};std::cout.write(v,sizeof(v));}\n")
        var test = TestCaseInput(id: "binary", expected: nil)
        test.input = WireBytes(Data("input".utf8) + Data([0,255]))
        test.expected = WireBytes(Data("expected".utf8) + Data([0,255]))
        let result = try await coordinator.run(BatchRequest(snapshot: source, originalPath: nil, toolchain: try profile(), tests: [test],
            externalChecker: try checker(mode: "verify")))
        let report = try #require(result.tests.first?.checker)
        #expect(report.verdict == "Accepted")
        #expect(report.process.stderr.data == Data("diagnostic".utf8) + Data([0,255]))
        #expect(report.process.stderrTotalBytes == UInt64(report.process.stderr.data.count))
        #expect(!report.process.stderrTruncated)
        let signalled = try await coordinator.run(BatchRequest(snapshot: source, originalPath: nil, toolchain: try profile(), tests: [test],
            externalChecker: try checker(mode: "signal")))
        #expect(signalled.tests.first?.verdict == "CheckerError")
        #expect(signalled.tests.first?.process.termination == "Exited")
        #expect(signalled.tests.first?.checker?.process.signal != nil)
        await coordinator.shutdown()
    }

    @Test func solutionRuntimeErrorSkipsCheckerAndCheckerOutputLimitStaysSeparate() async throws {
        let coordinator = CoreCoordinator()
        let failed = await snapshot("int main(){return 2;}\n")
        let result = try await coordinator.run(BatchRequest(snapshot: failed, originalPath: nil, toolchain: try profile(),
            tests: [TestCaseInput(input: "", expected: "")], externalChecker: try checker(mode: "exit", extra: ["0"])))
        #expect(result.tests.first?.verdict == "RE")
        #expect(result.tests.first?.checker == nil)
        let infinite = await snapshot("int main(){for(;;){}}\n")
        guard case .object(var shortFields) = try profile().payload else { throw CocoaError(.fileReadCorruptFile) }
        shortFields["run_limits"] = .object(["wall_timeout_ms": .number(100)])
        let timedSolution = try await coordinator.run(BatchRequest(snapshot: infinite, originalPath: nil,
            toolchain: try ToolchainProfile(payload: .object(shortFields)), tests: [TestCaseInput(input: "", expected: "")],
            externalChecker: try checker(mode: "exit", extra: ["0"])))
        #expect(timedSolution.tests.first?.verdict == "TLE")
        #expect(timedSolution.tests.first?.checker == nil)
        let source = await snapshot("int main(){return 0;}\n")
        let configured = try checker(mode: "flood")
        var object = try #require(JSONSerialization.jsonObject(with: Data(configured.originalJSON.utf8)) as? [String: Any])
        object["limits"] = ["wall_timeout_ms": 2000, "stdout_bytes": 16, "stderr_bytes": 16, "combined_bytes": 24]
        let limited = try CheckerConfiguration(jsonText: String(decoding: JSONSerialization.data(withJSONObject: object), as: UTF8.self))
        let output = try await coordinator.run(BatchRequest(snapshot: source, originalPath: nil, toolchain: try profile(),
            tests: [TestCaseInput(input: "", expected: "")], externalChecker: limited))
        let report = try #require(output.tests.first?.checker)
        #expect(output.tests.first?.verdict == "CheckerError")
        #expect(output.tests.first?.process.termination == "Exited")
        #expect(report.process.termination == "OutputLimit")
        #expect(report.process.stdoutTruncated || report.process.stderrTruncated)
        #expect(report.process.stdout.data.count + report.process.stderr.data.count <= 24)
        let cancelled = try await coordinator.run(BatchRequest(snapshot: source, originalPath: nil, toolchain: try profile(),
            tests: [TestCaseInput(input: "", expected: "")], externalChecker: try checker(mode: "sleep"))) { event in
            if event.state == .checking { try? await coordinator.cancel() }
        }
        #expect(cancelled.tests.first?.verdict == "Cancelled")
        #expect(cancelled.tests.first?.process.termination == "Exited")
        #expect(cancelled.tests.first?.checker?.verdict == "Cancelled")
        await coordinator.shutdown()
    }

    @Test func binaryPreviewDoesNotChangeEditorTextOrRawOutput() {
        let nul = WireBytes(Data([65, 0, 66]))
        #expect(nul.isUTF8)
        #expect(!nul.canDisplayAsText)
        #expect(nul.hexPreview(maximumBytes: 2) == "41 00")
        #expect(nul.data == Data([65, 0, 66]))
        #expect(nul.text.utf8.elementsEqual([65, 0, 66]))
        let invalid = WireBytes(Data([255, 0]))
        #expect(!invalid.canDisplayAsText)
        #expect(invalid.hexPreview(maximumBytes: 1) == "FF")
        #expect(invalid.hexPreview(maximumBytes: 0).isEmpty)
        #expect(WireBytes("中文\t\r\n").canDisplayAsText)
    }
}
