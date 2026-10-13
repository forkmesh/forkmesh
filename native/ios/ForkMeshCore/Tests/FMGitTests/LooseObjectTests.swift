import Foundation
import XCTest
@testable import FMGit

final class LooseObjectTests: GitTestCase {
    func testFramesHelloBlobWithCanonicalBytesAndObjectId() {
        let frame = GitObjectFrame.encode(.blob, utf8("hello\n"))
        XCTAssertEqual(text(frame), "blob 6\u{0}hello\n")
        XCTAssertEqual(GitObjectId.sha1Of(frame).hex, "ce013625030ba8dba906f756967f9e9ca394464a")
    }

    func testRejectsObjectBodyWhoseDeclaredLengthIsWrong() {
        assertGitError(.invalidObject) { _ = try GitObjectFrame.decode(utf8("blob 7\u{0}hello\n"), .standard) }
        assertGitError(.invalidObject) { _ = try GitObjectFrame.decode(utf8("blob 06\u{0}hello\n"), .standard) }
        assertGitError(.invalidObject) { _ = try GitObjectFrame.decode(utf8("blob  6\u{0}hello\n"), .standard) }
        assertGitError(.invalidObject) { _ = try GitObjectFrame.decode(utf8("blub 6\u{0}hello\n"), .standard) }
    }

    func testWritesAndReadsVerifiedLooseObject() throws {
        let root = try makeTemp("forkmesh-loose-object-")
        let store = LooseObjectStore(root, .standard)
        let id = try store.write(.blob, utf8("hello\n"))
        let object = try store.read(id)
        XCTAssertEqual(id.hex, "ce013625030ba8dba906f756967f9e9ca394464a")
        XCTAssertEqual(object.type, .blob)
        XCTAssertEqual(text(object.body), "hello\n")
    }

    func testRejectsLooseObjectStoredAtWrongObjectId() throws {
        let root = try makeTemp("forkmesh-object-mismatch-")
        let store = LooseObjectStore(root, .standard)
        let id = try store.write(.blob, utf8("hello\n"))
        let wrongId = try GitObjectId.parseSha1("0000000000000000000000000000000000000001")
        let wrong = store.fileFor(wrongId)
        try FileManager.default.createDirectory(
            atPath: (wrong as NSString).deletingLastPathComponent, withIntermediateDirectories: true
        )
        try FileManager.default.copyItem(atPath: store.fileFor(id), toPath: wrong)
        assertGitError(.objectHashMismatch) { _ = try store.read(wrongId) }
    }

    func testRejectsTrailingBytesAndCorruptStreams() throws {
        let root = try makeTemp("forkmesh-loose-corrupt-")
        let store = LooseObjectStore(root, .standard)
        let id = try store.write(.blob, utf8("hello\n"))
        let path = store.fileFor(id)
        let original = [UInt8](try Data(contentsOf: URL(fileURLWithPath: path)))
        try Data(original + [0]).write(to: URL(fileURLWithPath: path))
        assertGitError(.invalidObject) { _ = try store.read(id) }
        try Data(original.dropLast(2)).write(to: URL(fileURLWithPath: path))
        assertGitError(.invalidObject) { _ = try store.read(id) }
    }

    func testFailsClosedBeforeExposingUntrustedLooseObject() throws {
        let root = try makeTemp("forkmesh-untrusted-loose-")
        let store = LooseObjectStore(root, .standard)
        let id = try store.write(.blob, utf8("hello\n"))
        assertGitError(.hashScreeningUnavailable) { _ = try store.read(id, origin: .untrustedTransport) }
    }

    func testAdmitsUntrustedLooseObjectOnlyWithClearDetector() throws {
        let root = try makeTemp("forkmesh-screened-loose-")
        let store = LooseObjectStore(root, .standard, detector: ClearDetector())
        let id = try store.write(.blob, utf8("hello\n"))
        let object = try store.read(id, origin: .untrustedTransport)
        XCTAssertEqual(text(object.body), "hello\n")
    }

    func testInteroperatesWithGitCatFile() throws {
        guard let git = GitBinary.locate() else { throw XCTSkip("git is not installed") }
        let root = try makeTemp("forkmesh-loose-interop-")
        try git.run(["init", "--quiet", root])
        let store = LooseObjectStore(root + "/.git", .standard)
        let id = try store.write(.blob, utf8("hello\n"))
        XCTAssertEqual(try git.run(["-C", root, "cat-file", "-p", id.hex]), "hello\n")
        let foreign = try git.run(["-C", root, "hash-object", "-w", "--stdin"], input: utf8("from git\n"))
            .trimmingCharacters(in: .whitespacesAndNewlines)
        XCTAssertEqual(text(try store.read(try GitObjectId.parseSha1(foreign)).body), "from git\n")
    }
}
