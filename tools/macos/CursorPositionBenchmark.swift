import Foundation
import CryptoKit

@inline(never) func originalPosition(_ text: String, _ location: Int) -> (line: Int, column: Int) {
    let prefix = text.utf16.prefix(max(0, location))
    let line = prefix.filter { $0 == 10 }.count + 1
    let column = prefix.reversed().prefix { $0 != 10 }.count + 1
    return (line, column)
}
@inline(never) func originalStatusBody(_ text: String, _ location: Int) -> Int {
    originalPosition(text, location).line &+ originalPosition(text, location).column
}
@MainActor @inline(never) func cachedStatusBody(_ document: SourceDocument, _ location: Int) -> Int {
    let position = document.position(atUTF16Offset: location)
    return position.line &+ position.column
}
func durationNS(_ body: () -> Int) -> (Double, Int) {
    let begin = DispatchTime.now().uptimeNanoseconds
    let checksum = body()
    return (Double(DispatchTime.now().uptimeNanoseconds - begin), checksum)
}
func stats(_ input: [Double]) -> [String: Double] {
    let sorted = input.sorted()
    return ["p50_ns": sorted[sorted.count/2], "p95_ns": sorted[Int(Double(sorted.count - 1)*0.95)]]
}
@main struct Comparison {
    @MainActor static func main() throws {
        let unit = "// 中文🙂e\u{301} iostream\r\nint main() { return 0; }\n"
        var reports: [[String: Any]] = []
        var sink = 0
        for shape in ["multiline", "longline", "lf_only"] {
            for size in [0, 1_048_576, 10_485_760, 16_777_216] {
                let row = shape == "multiline" ? unit : (shape == "longline" ? "// 中文🙂e\u{301} iostream " : "\n")
                let text = String(repeating: row, count: size / row.utf8.count)
                let length = text.utf16.count
                let locations = (0..<21).map { length - min(length, $0 * 97) }
                var before: [Double] = []
                _ = originalStatusBody(text, length)
                for location in locations {
                    let (time, checksum) = durationNS { originalStatusBody(text, location) }; before.append(time); sink &+= checksum
                }
                var build: [Double] = []
                var memory: [String: Int] = [:]
                for _ in 0..<7 {
                    let begin = DispatchTime.now().uptimeNanoseconds
                    let index = TextLineIndex(text)
                    let elapsed = Double(DispatchTime.now().uptimeNanoseconds - begin)
                    sink &+= index.position(atUTF16Offset: length).line
                    let stored = Mirror(reflecting: index).children.first { $0.label == "segments" }!.value
                    let entries = Mirror(reflecting: stored).children.count
                    memory = ["segment_count":entries,"segment_payload_bytes":entries * 3 * MemoryLayout<Int>.stride]
                    build.append(elapsed)
                }
                let document = SourceDocument(text: text, languageID:"cpp")
                let (coldTime, coldSum) = durationNS { cachedStatusBody(document, length) }; sink &+= coldSum
                var after: [Double] = []
                let batchCount = 1024
                for location in locations {
                    let (time, checksum) = durationNS {
                        var sum = 0
                        for offset in 0..<batchCount { sum &+= cachedStatusBody(document, max(0, location - offset % 97)) }
                        return sum
                    }; after.append(time / Double(batchCount)); sink &+= checksum
                    let expected = originalPosition(text, location)
                    let actual = document.position(atUTF16Offset:location)
                    precondition(expected.line == actual.line && expected.column == actual.column)
                }
                reports.append(["shape":shape,"bytes":text.utf8.count,"utf16_count":length,"source_sha256":SHA256.hash(data:Data(text.utf8)).map{String(format:"%02x",$0)}.joined(),"original_two_evaluations":stats(before),"cached_one_evaluation":stats(after),"index_build":stats(build),"document_cold_lookup_ns":coldTime,"index_memory":memory])
            }
        }
        let result: [String: Any] = ["samples_per_fixture":21,"cached_batch_count":1024,"queries":"near EOF, two original position evaluations vs one cached document evaluation; cached samples averaged over 1024 offsets","configuration":"swiftc -O, Xcode 27.2, macOS SDK 27.2, arm64-apple-macos26.0","memory_scope":"24-byte sparse segment logical payload; allocator capacity and model/text/hash memory excluded","sink":sink,"fixtures":reports]
        try JSONSerialization.data(withJSONObject:result,options:[.prettyPrinted,.sortedKeys]).write(to:URL(fileURLWithPath:CommandLine.arguments[1]))
    }
}
