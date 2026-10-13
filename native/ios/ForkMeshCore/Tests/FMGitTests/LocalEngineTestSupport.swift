// Shared helpers for the LocalEngine* tests, including the fake server, after the Kotlin port's
// GitTestSupport.kt and RemoteTestSupport.kt.
// Every name carries a `le`/`LocalEngine` prefix so it cannot clash with
// another FMGitTests file.

import Foundation
import XCTest
@testable import FMGit

func leUtf8(_ text: String) -> [UInt8] { Array(text.utf8) }

func leText(_ bytes: [UInt8]) -> String { String(decoding: bytes, as: UTF8.self) }

/// `latin1.decode(bytes)`.
func leLatin1(_ bytes: [UInt8]) -> String {
    var scalars = String.UnicodeScalarView()
    for byte in bytes { scalars.append(Unicode.Scalar(byte)) }
    return String(scalars)
}

func leFixtureId(_ byte: Int) -> GitObjectId {
    let pair = String(byte, radix: 16)
    return try! GitObjectId.parseSha1(String(repeating: pair.count == 1 ? "0" + pair : pair, count: 20))
}

/// Asserts that `body` throws a `GitException`, optionally with `code`.
@discardableResult
func leAssertGitError(
    _ code: GitErrorCode? = nil,
    file: StaticString = #filePath,
    line: UInt = #line,
    _ body: () throws -> Void
) -> GitException? {
    do {
        try body()
        XCTFail("expected GitException", file: file, line: line)
    } catch let error as GitException {
        if let code { XCTAssertEqual(error.code, code, error.message, file: file, line: line) }
        return error
    } catch {
        XCTFail("expected GitException, got \(error)", file: file, line: line)
    }
    return nil
}

/// The async form of `leAssertGitError`.
@discardableResult
func leAssertGitErrorAsync(
    _ code: GitErrorCode? = nil,
    file: StaticString = #filePath,
    line: UInt = #line,
    _ body: () async throws -> Void
) async -> GitException? {
    do {
        try await body()
        XCTFail("expected GitException", file: file, line: line)
    } catch let error as GitException {
        if let code { XCTAssertEqual(error.code, code, error.message, file: file, line: line) }
        return error
    } catch {
        XCTFail("expected GitException, got \(error)", file: file, line: line)
    }
    return nil
}

func leWriteFile(_ path: String, _ text: String) throws {
    try Data(text.utf8).write(to: URL(fileURLWithPath: path))
}

func leReadFile(_ path: String) throws -> String {
    String(decoding: try Data(contentsOf: URL(fileURLWithPath: path)), as: UTF8.self)
}

func leExists(_ path: String) -> Bool { FileManager.default.fileExists(atPath: path) }

func leIdentity(_ seconds: Int = 0, name: String = "Ada") -> GitIdentity {
    try! GitIdentity(name: name, email: "ada@example.test", timestampSeconds: seconds, timezoneOffsetMinutes: 0)
}

/// Writes `file`, stages it and commits.
@discardableResult
func leCommit(
    _ repository: GitRepository,
    _ file: String,
    _ content: String,
    _ message: String,
    seconds: Int = 0,
    name: String = "Ada"
) throws -> GitObjectId {
    let path = "\(repository.worktree)/\(file)"
    try FileManager.default.createDirectory(
        atPath: (path as NSString).deletingLastPathComponent, withIntermediateDirectories: true
    )
    try leWriteFile(path, content)
    try repository.add([try GitWorktreePath(file)])
    let identity = leIdentity(seconds, name: name)
    return try repository.commit(try GitCommitRequest(message: message, author: identity, committer: identity))
}

class LocalEngineTestCase: XCTestCase {
    private var temporaryDirectories = [String]()

    /// `Directory.systemTemp.createTemp(prefix)`, removed at tear-down.
    func makeTemp(_ prefix: String) throws -> String {
        let path = NSTemporaryDirectory() + prefix + UUID().uuidString
        try FileManager.default.createDirectory(atPath: path, withIntermediateDirectories: true)
        // The engine refuses a symlinked worktree root; /tmp may be one (macOS).
        let resolved = URL(fileURLWithPath: path).resolvingSymlinksInPath().path
        temporaryDirectories.append(resolved)
        return resolved
    }

    override func tearDown() {
        for path in temporaryDirectories { try? FileManager.default.removeItem(atPath: path) }
        temporaryDirectories = []
        super.tearDown()
    }
}

/// Test-only collision detector that reports every frame as clear so
/// untrusted-transport screening can pass in fixtures.
final class LocalEngineClearDetector: GitSha1CollisionDetector {
    func start() throws -> GitSha1CollisionDetectorSession { LocalEngineClearSession() }
}

final class LocalEngineClearSession: GitSha1CollisionDetectorSession {
    func add(_ bytes: [UInt8]) throws {}
    func close() throws -> GitSha1CollisionDetectorStatus { .clear }
    func dispose() {}
}

struct LocalEngineTestFailure: Error, CustomStringConvertible {
    let description: String
}

/// In-memory `GitHttpTransport` that routes requests to a handler and
/// records every request for assertions.
final class LocalEngineFakeTransport: GitHttpTransport, @unchecked Sendable {
    typealias Handler = (GitHttpRequest) throws -> GitHttpResponse

    private let handler: Handler
    private let lock = NSLock()
    private var recorded = [GitHttpRequest]()
    private var wasClosed = false

    init(_ handler: @escaping Handler) { self.handler = handler }

    var requests: [GitHttpRequest] {
        lock.lock()
        defer { lock.unlock() }
        return recorded
    }

    var closed: Bool {
        lock.lock()
        defer { lock.unlock() }
        return wasClosed
    }

    func send(
        _ request: GitHttpRequest,
        cancellation: GitCancellationToken?,
        limits: GitResourceLimits?
    ) async throws -> GitHttpResponse {
        try cancellation?.throwIfCancelled()
        record(request)
        let response = try handler(request)
        if response.body.count > request.maxResponseBytes {
            throw GitException(.resourceLimitExceeded, "response body exceeds the configured limit")
        }
        return response
    }

    private func record(_ request: GitHttpRequest) {
        lock.lock()
        recorded.append(request)
        lock.unlock()
    }

    func close() async { markClosed() }

    private func markClosed() {
        lock.lock()
        wasClosed = true
        lock.unlock()
    }
}

func lePackets(_ lines: [GitPktLine]) -> [UInt8] {
    var output = [UInt8]()
    for line in lines { output.append(contentsOf: try! line.encode()) }
    return output
}

func leDataLine(_ text: String) -> GitPktLine { .data(Array(text.utf8)) }

/// `GET info/refs?service=git-upload-pack` body advertising protocol v2.
func leUploadPackAdvertisementBody(
    capabilities: [String] = ["agent=git/test", "ls-refs=unborn", "fetch=shallow wait-for-done", "object-format=sha1"]
) -> [UInt8] {
    var lines = [leDataLine("# service=git-upload-pack\n"), .flush(), leDataLine("version 2\n")]
    for capability in capabilities { lines.append(leDataLine("\(capability)\n")) }
    lines.append(.flush())
    return lePackets(lines)
}

/// An `ls-refs` result body.
func leLsRefsBody(_ records: [String]) -> [UInt8] {
    lePackets(records.map { leDataLine("\($0)\n") } + [.flush()])
}

/// A v2 `fetch` result carrying only an acknowledgments section.
func leAcknowledgmentsOnlyBody(_ lines: [String]) -> [UInt8] {
    lePackets([leDataLine("acknowledgments\n")] + lines.map { leDataLine("\($0)\n") } + [.flush()])
}

/// A v2 `fetch` result whose packfile section carries `packBytes` on
/// sideband channel 1, optionally preceded by an acknowledgments section.
func lePackfileBody(_ packBytes: [UInt8], acknowledgments: [String] = []) -> [UInt8] {
    var lines = [GitPktLine]()
    if !acknowledgments.isEmpty {
        lines.append(leDataLine("acknowledgments\n"))
        for line in acknowledgments { lines.append(leDataLine("\(line)\n")) }
        lines.append(.delimiter())
    }
    lines.append(leDataLine("packfile\n"))
    var offset = 0
    while offset < packBytes.count {
        let end = min(offset + 8192, packBytes.count)
        lines.append(.data([1] + Array(packBytes[offset..<end])))
        offset += 8192
    }
    lines.append(.flush())
    return lePackets(lines)
}

/// `GET info/refs?service=git-receive-pack` advertisement body.
func leReceivePackAdvertisementBody(
    _ refs: [(String, GitObjectId)],
    capabilities: String = "report-status report-status-v2 delete-refs side-band-64k quiet "
        + "atomic ofs-delta object-format=sha1 agent=git/test"
) -> [UInt8] {
    var lines = [leDataLine("# service=git-receive-pack\n"), .flush()]
    if refs.isEmpty {
        lines.append(.data(Array("\(String(repeating: "0", count: 40)) capabilities^{}".utf8) + [0]
            + Array("\(capabilities)\n".utf8)))
    } else {
        for (index, entry) in refs.enumerated() {
            if index == 0 {
                lines.append(.data(Array("\(entry.1.hex) \(entry.0)".utf8) + [0] + Array("\(capabilities)\n".utf8)))
            } else {
                lines.append(leDataLine("\(entry.1.hex) \(entry.0)\n"))
            }
        }
    }
    lines.append(.flush())
    return lePackets(lines)
}

/// A receive-pack report wrapped in sideband channel 1.
func leSidebandReportBody(_ reportLines: [String]) -> [UInt8] {
    let report = lePackets(reportLines.map { leDataLine("\($0)\n") } + [.flush()])
    return lePackets([.data([1] + report), .flush()])
}

func leOkResponse(_ request: GitHttpRequest, _ body: [UInt8]) -> GitHttpResponse {
    GitHttpResponse(statusCode: 200, headers: ["content-type": request.expectedContentType ?? ""], body: body)
}
