import Foundation
import CryptoKit

public struct SourceFileContents: Sendable {
    public let text: String
    public let diskDigest: String
}

public enum CoordinatedSourceFile {
    /// 调用在后台 actor/task 完成。NSFileCoordinator 只协调遵守该协议的 writer；
    /// 摘要是乐观冲突校验，不能阻止绕过协调的外部程序。
    public static func read(_ url: URL, maximumBytes: Int) throws -> SourceFileContents {
        guard maximumBytes > 0, maximumBytes < Int.max else { throw CocoaError(.fileReadTooLarge) }
        let coordinator = NSFileCoordinator()
        var coordinationError: NSError?
        var outcome: Result<SourceFileContents, Error>?
        coordinator.coordinate(readingItemAt: url, options: [], error: &coordinationError) { source in
            outcome = Result {
                let handle = try FileHandle(forReadingFrom: source)
                defer { try? handle.close() }
                var data = Data()
                while data.count <= maximumBytes {
                    let chunk = try handle.read(upToCount: maximumBytes + 1 - data.count) ?? Data()
                    if chunk.isEmpty { break }
                    data.append(chunk)
                }
                guard data.count <= maximumBytes else { throw CocoaError(.fileReadTooLarge) }
                guard String(data: data, encoding: .utf8) != nil else { throw DocumentError.invalidUTF8 }
                // Foundation 的 String(data:encoding:) 会吞 BOM；验证后按原始字节构造。
                return SourceFileContents(text: String(decoding: data, as: UTF8.self), diskDigest: SourceDigest.sha256(data))
            }
        }
        if let coordinationError { throw coordinationError }
        return try (outcome ?? .failure(CocoaError(.fileReadUnknown))).get()
    }

    public static func aliases(_ left: URL, _ right: URL) throws -> Bool {
        let normalizedLeft = left.standardizedFileURL.resolvingSymlinksInPath()
        let normalizedRight = right.standardizedFileURL.resolvingSymlinksInPath()
        if normalizedLeft.path.utf8.elementsEqual(normalizedRight.path.utf8) { return true }
        let manager = FileManager.default
        guard manager.fileExists(atPath: normalizedLeft.path), manager.fileExists(atPath: normalizedRight.path) else { return false }
        let a = try manager.attributesOfItem(atPath: normalizedLeft.path)
        let b = try manager.attributesOfItem(atPath: normalizedRight.path)
        return (a[.systemNumber] as? NSNumber) == (b[.systemNumber] as? NSNumber)
            && (a[.systemFileNumber] as? NSNumber) == (b[.systemFileNumber] as? NSNumber)
    }

    private static func diskDigest(_ url: URL) throws -> String {
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        var digest = SHA256()
        while let data = try handle.read(upToCount: 64 * 1024), !data.isEmpty { digest.update(data: data) }
        return digest.finalize().map { String(format: "%02x", $0) }.joined()
    }

    public static func write(_ snapshot: SourceSnapshot, to url: URL,
                             expectedDiskDigest: String?, allowSelectedReplacement: Bool = false,
                             reservedPaths: [String] = []) throws -> String {
        let coordinator = NSFileCoordinator()
        var coordinationError: NSError?
        var outcome: Result<String, Error>?
        coordinator.coordinate(writingItemAt: url, options: .forReplacing, error: &coordinationError) { destination in
            outcome = Result {
                for path in reservedPaths where try aliases(destination, URL(fileURLWithPath: path)) {
                    throw DocumentError.reservedRecoveryPath
                }
                let exists = FileManager.default.fileExists(atPath: destination.path)
                let observed = exists ? try diskDigest(destination) : nil
                if !allowSelectedReplacement, observed != expectedDiskDigest { throw DocumentError.diskConflict }
                try snapshot.utf8.write(to: destination, options: .atomic)
                return snapshot.sourceDigest
            }
        }
        if let coordinationError { throw coordinationError }
        return try (outcome ?? .failure(CocoaError(.fileWriteUnknown))).get()
    }
}
