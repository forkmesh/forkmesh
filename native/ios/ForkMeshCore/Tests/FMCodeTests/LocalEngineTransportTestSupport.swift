// The helpers LocalEngineTransportTests needs. The rest of the engine's tests
// moved to FMGitTests with the engine (LocalEngineTestSupport.swift there);
// GitNetHttpTransport stays in FMCode because it is built on FMNet.

import FMGit
import Foundation
import XCTest

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

func leReadFile(_ path: String) throws -> String {
    String(decoding: try Data(contentsOf: URL(fileURLWithPath: path)), as: UTF8.self)
}

class LocalEngineTestCase: XCTestCase {
    private var temporaryDirectories = [String]()

    /// `Directory.systemTemp.createTemp(prefix)`, removed at tear-down.
    func makeTemp(_ prefix: String) throws -> String {
        let path = NSTemporaryDirectory() + prefix + UUID().uuidString
        try FileManager.default.createDirectory(atPath: path, withIntermediateDirectories: true)
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

struct LocalEngineTestFailure: Error, CustomStringConvertible {
    let description: String
}
