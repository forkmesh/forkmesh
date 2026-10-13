// Shared helpers for the FMGit tests (the role of the Kotlin port's GitTestSupport.kt).

import Foundation
import XCTest
@testable import FMGit

func utf8(_ text: String) -> [UInt8] { Array(text.utf8) }

func text(_ bytes: [UInt8]) -> String { String(decoding: bytes, as: UTF8.self) }

func fixtureId(_ byte: Int) -> GitObjectId {
    let pair = String(byte, radix: 16)
    return try! GitObjectId.parseSha1(String(repeating: pair.count == 1 ? "0" + pair : pair, count: 20))
}

/// Asserts that `body` throws a `GitException`, optionally with `code`.
func assertGitError(
    _ code: GitErrorCode? = nil,
    file: StaticString = #filePath,
    line: UInt = #line,
    _ body: () throws -> Void
) {
    do {
        try body()
        XCTFail("expected GitException", file: file, line: line)
    } catch let error as GitException {
        if let code { XCTAssertEqual(error.code, code, error.message, file: file, line: line) }
    } catch {
        XCTFail("expected GitException, got \(error)", file: file, line: line)
    }
}

class GitTestCase: XCTestCase {
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

final class ClearDetector: GitSha1CollisionDetector {
    func start() throws -> GitSha1CollisionDetectorSession { ClearDetectorSession() }
}

final class ClearDetectorSession: GitSha1CollisionDetectorSession {
    func add(_ bytes: [UInt8]) throws {}
    func close() throws -> GitSha1CollisionDetectorStatus { .clear }
    func dispose() throws {}
}
