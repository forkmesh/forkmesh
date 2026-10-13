// Helpers for the Local*Tests (on-device repositories): temp directories, the
// real `git` binary for interoperability checks (as FMGitTests/GitBinary.swift
// does; tests skip when it is missing), and a tiny zip writer.

import FMGit
import Foundation
import XCTest
@testable import FMCode

let localTestAuthor = GitAuthor(name: "Alice Mobile", email: "alice@example.com")

/// A fresh directory under the system temp dir, removed by `addTeardownBlock`.
func localTempDirectory(_ test: XCTestCase, _ prefix: String) throws -> URL {
    let url = FileManager.default.temporaryDirectory
        .appendingPathComponent("\(prefix)-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
    test.addTeardownBlock { try? FileManager.default.removeItem(at: url) }
    return url
}

func localWrite(_ url: URL, _ text: String) throws {
    try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
    try Data(text.utf8).write(to: url)
}

func localRead(_ url: URL) throws -> String {
    String(decoding: try Data(contentsOf: url), as: UTF8.self)
}

/// The real `git`, with a hermetic environment.
struct LocalTestGit {
    let path: String

    static func locate() -> LocalTestGit? {
        #if os(iOS) || os(tvOS) || os(watchOS)
        return nil
        #else
        for candidate in ["/usr/bin/git", "/usr/local/bin/git", "/opt/homebrew/bin/git", "/bin/git"]
        where FileManager.default.isExecutableFile(atPath: candidate) {
            return LocalTestGit(path: candidate)
        }
        return nil
        #endif
    }

    static func require() throws -> LocalTestGit {
        guard let git = locate() else { throw XCTSkip("git binary not on PATH") }
        return git
    }

    /// Runs git; returns (exit status, stdout, stderr).
    @discardableResult
    func run(_ arguments: [String], directory: String? = nil) throws -> (status: Int32, out: String, err: String) {
        #if os(iOS) || os(tvOS) || os(watchOS)
        throw XCTSkip("no subprocesses on this platform")
        #else
        let process = Process()
        process.executableURL = URL(fileURLWithPath: path)
        process.arguments = arguments
        if let directory { process.currentDirectoryURL = URL(fileURLWithPath: directory) }
        let home = NSTemporaryDirectory() + "forkmesh-git-home-\(ProcessInfo.processInfo.processIdentifier)"
        try? FileManager.default.createDirectory(atPath: home, withIntermediateDirectories: true)
        var environment = [
            "PATH": "/usr/bin:/bin:/usr/local/bin:/opt/homebrew/bin",
            "HOME": home,
            "XDG_CONFIG_HOME": home,
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_CONFIG_GLOBAL": "/dev/null",
            "GIT_ATTR_NOSYSTEM": "1",
            "GIT_TERMINAL_PROMPT": "0",
            "GIT_AUTHOR_NAME": "Interop", "GIT_AUTHOR_EMAIL": "interop@example.test",
            "GIT_COMMITTER_NAME": "Interop", "GIT_COMMITTER_EMAIL": "interop@example.test",
            "LC_ALL": "C",
        ]
        if let developer = ProcessInfo.processInfo.environment["DEVELOPER_DIR"] {
            environment["DEVELOPER_DIR"] = developer
        }
        process.environment = environment
        let output = Pipe()
        let errors = Pipe()
        process.standardOutput = output
        process.standardError = errors
        process.standardInput = FileHandle.nullDevice
        let exited = DispatchSemaphore(value: 0)
        process.terminationHandler = { _ in exited.signal() }
        try process.run()
        let drained = DispatchGroup()
        let lock = NSLock()
        var errorData = Data()
        DispatchQueue.global().async(group: drained) {
            let data = errors.fileHandleForReading.readDataToEndOfFile()
            lock.lock()
            errorData = data
            lock.unlock()
        }
        let data = output.fileHandleForReading.readDataToEndOfFile()
        drained.wait()
        exited.wait()
        lock.lock()
        defer { lock.unlock() }
        return (process.terminationStatus, String(decoding: data, as: UTF8.self), String(decoding: errorData, as: UTF8.self))
        #endif
    }
}

/// Writes a zip archive: `deflate` entries are raw-deflated with the system
/// zlib, the rest stored.
enum LocalTestZip {
    struct File {
        let name: String
        let bytes: [UInt8]
        var deflate = false
        var unixMode: UInt32? = nil
    }

    static func build(_ files: [File]) throws -> [UInt8] {
        var out: [UInt8] = []
        var central: [UInt8] = []
        for file in files {
            let crc = gitCrc32(file.bytes)
            let payload: [UInt8]
            if file.deflate {
                let wrapped = try GitZlib.deflate(file.bytes)
                payload = Array(wrapped[2..<(wrapped.count - 4)]) // strip zlib header and adler32
            } else {
                payload = file.bytes
            }
            let method: UInt16 = file.deflate ? 8 : 0
            let name = Array(file.name.utf8)
            let offset = UInt32(out.count)
            out += le32(0x0403_4b50) + le16(20) + le16(0x0800) + le16(method) + le16(0) + le16(0)
            out += le32(crc) + le32(UInt32(payload.count)) + le32(UInt32(file.bytes.count))
            out += le16(UInt16(name.count)) + le16(0) + name + payload
            let madeBy: UInt16 = file.unixMode == nil ? 20 : (3 << 8) | 20
            central += le32(0x0201_4b50) + le16(madeBy) + le16(20) + le16(0x0800) + le16(method) + le16(0) + le16(0)
            central += le32(crc) + le32(UInt32(payload.count)) + le32(UInt32(file.bytes.count))
            central += le16(UInt16(name.count)) + le16(0) + le16(0) + le16(0) + le16(0)
            central += le32((file.unixMode ?? 0) << 16) + le32(offset) + name
        }
        let centralOffset = UInt32(out.count)
        out += central
        out += le32(0x0605_4b50) + le16(0) + le16(0) + le16(UInt16(files.count)) + le16(UInt16(files.count))
        out += le32(UInt32(central.count)) + le32(centralOffset) + le16(0)
        return out
    }

    private static func le16(_ value: UInt16) -> [UInt8] { [UInt8(value & 0xff), UInt8(value >> 8)] }
    private static func le32(_ value: UInt32) -> [UInt8] {
        [UInt8(value & 0xff), UInt8((value >> 8) & 0xff), UInt8((value >> 16) & 0xff), UInt8(value >> 24)]
    }
}

/// `XCTAssertThrowsError` for async calls.
func localAssertThrows<T>(
    _ body: @autoclosure () async throws -> T,
    file: StaticString = #filePath,
    line: UInt = #line,
    _ check: (Error) -> Void = { _ in }
) async {
    do {
        _ = try await body()
        XCTFail("expected an error", file: file, line: line)
    } catch {
        check(error)
    }
}
