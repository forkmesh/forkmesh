// Worktree paths, with the extra worktree cases of the Kotlin port's GitWorktreeTest.kt. These
// guard a security boundary, so the Swift-specific traps (grapheme clusters, canonical equivalence)
// get their own cases.

import Foundation
import XCTest
@testable import FMGit

final class WorktreePathTests: GitTestCase {
    func testAcceptsPortableUtf8RepositoryRelativePath() throws {
        let path = try GitWorktreePath("nested/caf\u{e9}.txt")
        XCTAssertEqual(path.components, ["nested", "caf\u{e9}.txt"])
        XCTAssertEqual(text(path.bytes), "nested/caf\u{e9}.txt")
    }

    func testRejectsTraversalDeviceAndPlatformAmbiguousNames() {
        for value in [
            "../outside", "/absolute", "nested/../outside", "CON", "name.", "name ", "name:stream",
            "nested\\backslash", "", "a//b", "a/", "./a", "a/.", "\\a", "con.txt", "nested/Aux", "LPT1", "com9.log",
            "nul", "prn.tar.gz", "a\u{0}b", "a\nb", "a\u{7f}b", "tab\there",
        ] {
            assertGitError(.pathEscapesWorktree) { _ = try GitWorktreePath(value) }
        }
        XCTAssertNoThrow(try GitWorktreePath("COM0"))
        XCTAssertNoThrow(try GitWorktreePath("console"))
        XCTAssertNoThrow(try GitWorktreePath("COM10"))
        XCTAssertNoThrow(try GitWorktreePath(".gitignore"))
    }

    func testLengthLimitsCountUtf8Bytes() {
        XCTAssertNoThrow(try GitWorktreePath(String(repeating: "a", count: 255)))
        assertGitError(.pathEscapesWorktree) { _ = try GitWorktreePath(String(repeating: "a", count: 256)) }
        // 128 two-byte scalars: 128 characters but 256 bytes.
        assertGitError(.pathEscapesWorktree) { _ = try GitWorktreePath(String(repeating: "\u{e9}", count: 128)) }
        let long = Array(repeating: String(repeating: "a", count: 200), count: 21).joined(separator: "/")
        assertGitError(.pathEscapesWorktree) { _ = try GitWorktreePath(long) }
    }

    func testSeparatorFollowedByCombiningMarkStillSplits() throws {
        // "/" + U+0301 is one Swift Character; the path must still split there.
        let path = try GitWorktreePath("a/\u{301}b")
        XCTAssertEqual(path.components.count, 2)
        // ".." + combining mark is not "..", but "../" hidden behind a mark is.
        assertGitError(.pathEscapesWorktree) { _ = try GitWorktreePath("..\u{301}/../x") }
        assertGitError(.pathEscapesWorktree) { _ = try GitWorktreePath("a/..") }
    }

    func testEqualityIsByBytesNotCanonicalEquivalence() throws {
        let composed = try GitWorktreePath("caf\u{e9}")
        let decomposed = try GitWorktreePath("cafe\u{301}")
        XCTAssertNotEqual(composed, decomposed)
        XCTAssertEqual(Set([composed, decomposed]).count, 2)
    }

    func testRefusesToTraverseExistingSymlinkWhileWriting() throws {
        let root = try makeTemp("forkmesh-worktree-")
        let outside = try makeTemp("forkmesh-outside-")
        try FileManager.default.createSymbolicLink(atPath: root + "/escape", withDestinationPath: outside)
        let worktree = GitWorktree(root)
        assertGitError(.symlinkEscape) { try worktree.writeFile(try GitWorktreePath("escape/owned.txt"), [1]) }
        XCTAssertFalse(FileManager.default.fileExists(atPath: outside + "/owned.txt"))
        assertGitError(.symlinkEscape) { _ = try worktree.readFile(try GitWorktreePath("escape/owned.txt")) }
        assertGitError(.symlinkEscape) { try worktree.deleteFile(try GitWorktreePath("escape/owned.txt")) }
    }

    func testRefusesSymlinkedDestinationAndRoot() throws {
        let root = try makeTemp("forkmesh-worktree-dest-")
        let outside = try makeTemp("forkmesh-outside-dest-")
        try Data([9]).write(to: URL(fileURLWithPath: outside + "/victim"))
        try FileManager.default.createSymbolicLink(atPath: root + "/link", withDestinationPath: outside + "/victim")
        let worktree = GitWorktree(root)
        assertGitError(.symlinkEscape) { try worktree.writeFile(try GitWorktreePath("link"), [1]) }
        XCTAssertEqual([UInt8](try Data(contentsOf: URL(fileURLWithPath: outside + "/victim"))), [9])

        let linkedRoot = outside + "/rootlink"
        try FileManager.default.createSymbolicLink(atPath: linkedRoot, withDestinationPath: root)
        assertGitError(.symlinkEscape) { try GitWorktree(linkedRoot).writeFile(try GitWorktreePath("a"), [1]) }
    }

    func testParentThatIsAFileIsRejected() throws {
        let root = try makeTemp("forkmesh-worktree-file-parent-")
        let worktree = GitWorktree(root)
        try worktree.writeFile(try GitWorktreePath("a"), [1])
        assertGitError(.pathEscapesWorktree) { try worktree.writeFile(try GitWorktreePath("a/b"), [1]) }
    }

    func testWritesReadsAndDeletesNestedFiles() throws {
        let root = try makeTemp("forkmesh-worktree-rw-")
        let worktree = GitWorktree(root)
        let path = try GitWorktreePath("a/b/c.txt")
        try worktree.writeFile(path, utf8("hi"))
        XCTAssertEqual(try worktree.readFile(path), utf8("hi"))
        XCTAssertEqual(try worktree.typeOf(path), .file)
        try worktree.deleteFile(path)
        XCTAssertEqual(try worktree.typeOf(path), .notFound)
        assertGitError(.invalidRepository) { _ = try worktree.readFile(path) }
    }

    func testSymlinkTargetsMustStayRelativeAndInside() throws {
        let root = try makeTemp("forkmesh-worktree-symlink-")
        let worktree = GitWorktree(root)
        let path = try GitWorktreePath("dir/link")
        for target in ["/etc/passwd", "../outside", "a/../b", "", "a//b", "a\\b", "./a"] {
            assertGitError(.symlinkEscape) { try worktree.writeSymlink(path, target) }
        }
        try worktree.writeSymlink(path, "target.txt")
        XCTAssertEqual(try worktree.readSymlink(path), "target.txt")
        XCTAssertEqual(try worktree.typeOf(path), .link)
        // Replacing a link is allowed; replacing a directory is not.
        try worktree.writeSymlink(path, "other.txt")
        XCTAssertEqual(try worktree.readSymlink(path), "other.txt")
        assertGitError(.pathEscapesWorktree) { try worktree.writeSymlink(try GitWorktreePath("dir"), "x") }
        // A link some other tool pointed outside is reported, never followed.
        try FileManager.default.removeItem(atPath: root + "/dir/link")
        try FileManager.default.createSymbolicLink(atPath: root + "/dir/link", withDestinationPath: "/etc/passwd")
        assertGitError(.symlinkEscape) { _ = try worktree.readSymlink(path) }
        assertGitError(.invalidRepository) {
            try worktree.writeFile(try GitWorktreePath("plain"), [1])
            _ = try worktree.readSymlink(try GitWorktreePath("plain"))
        }
    }
}
