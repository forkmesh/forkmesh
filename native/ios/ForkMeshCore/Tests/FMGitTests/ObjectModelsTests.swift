import XCTest
@testable import FMGit

final class ObjectModelsTests: XCTestCase {
    func testTreeWriterUsesGitRawByteDirectoryOrdering() throws {
        let tree = try GitTree([
            try GitTreeEntry.regular(utf8("z"), fixtureId(1)),
            try GitTreeEntry.directory(utf8("a"), fixtureId(2)),
        ])
        XCTAssertEqual(tree.entries.map(\.nameText), ["a", "z"])
        XCTAssertEqual(try GitTree.parse(tree.encode()).entries.count, 2)
    }

    func testDirectorySortsAsIfItsNameEndedInSlash() throws {
        // "a.b" < "a/" < "a0" bytewise, so the directory "a" lands between them.
        let tree = try GitTree([
            try GitTreeEntry.regular(utf8("a0"), fixtureId(1)),
            try GitTreeEntry.directory(utf8("a"), fixtureId(2)),
            try GitTreeEntry.regular(utf8("a.b"), fixtureId(3)),
        ])
        XCTAssertEqual(tree.entries.map(\.nameText), ["a.b", "a", "a0"])
        assertGitError(.invalidTree) {
            _ = try GitTree([
                try GitTreeEntry.regular(utf8("a"), fixtureId(1)),
                try GitTreeEntry.regular(utf8("a"), fixtureId(2)),
            ])
        }
    }

    func testTreeParserRejectsUnsortedOrGitlinkEntries() {
        let unsorted = utf8("100644 z\u{0}") + fixtureId(1).bytes + utf8("100644 a\u{0}") + fixtureId(2).bytes
        let gitlink = utf8("160000 module\u{0}") + fixtureId(1).bytes
        assertGitError(.invalidTree) { _ = try GitTree.parse(unsorted) }
        assertGitError(.unsupportedRepositoryFormat) { _ = try GitTree.parse(gitlink) }
    }

    func testCommitParserPreservesParentOrderAndMessageBytes() throws {
        let commit = try GitCommit.parse(utf8(
            "tree \(fixtureId(1).hex)\n"
                + "parent \(fixtureId(2).hex)\n"
                + "parent \(fixtureId(3).hex)\n"
                + "author Ada <ada@example.test> 0 +0000\n"
                + "committer Ada <ada@example.test> 0 +0000\n"
                + "\n"
                + "message\n"
        ))
        XCTAssertEqual(commit.tree, fixtureId(1))
        XCTAssertEqual(commit.parents, [fixtureId(2), fixtureId(3)])
        XCTAssertEqual(text(commit.message), "message\n")
        XCTAssertEqual(commit.author?.name, "Ada")
        XCTAssertEqual(commit.author?.email, "ada@example.test")
    }

    func testIdentityParsing() throws {
        let identity = try GitCommitIdentity.parse("Ada Lovelace  <ada@example.test> 1700000000 -0530")
        XCTAssertEqual(identity.name, "Ada Lovelace")
        XCTAssertEqual(identity.timestampSeconds, 1_700_000_000)
        XCTAssertEqual(identity.timezoneOffsetMinutes, -330)
        XCTAssertEqual(GitCommitIdentity.tryParse("A <b> <c@d> 1 +0000")?.name, "A <b>")
        XCTAssertEqual(GitCommitIdentity.tryParse("<> 1 +0000")?.name, "")
        XCTAssertNil(GitCommitIdentity.tryParse("Ada <ada@example.test> 1 +0060"))
        XCTAssertNil(GitCommitIdentity.tryParse("Ada <ada@example.test> +0000"))
        XCTAssertNil(GitCommitIdentity.tryParse("Ada ada@example.test 1 +0000"))
        XCTAssertNil(GitCommitIdentity.tryParse("Ada <ada@example.test> 99999999999999999999 +0000"))
        assertGitError(.invalidObject) { _ = try GitCommitIdentity.parse("nope") }
    }

    func testCommitHeaderContinuationLines() throws {
        let commit = try GitCommit.parse(utf8(
            "tree \(fixtureId(1).hex)\n"
                + "author Ada <ada@example.test> 0 +0000\n"
                + "committer Ada <ada@example.test> 0 +0000\n"
                + "gpgsig line one\n line two"
        ))
        XCTAssertEqual(commit.headers["gpgsig"], ["line one\nline two"])
        XCTAssertEqual(commit.message, [])
        assertGitError(.invalidObject) { _ = try GitCommit.parse(utf8(" orphan\n")) }
    }

    func testAnnotatedTagParser() throws {
        assertGitError { _ = try GitAnnotatedTag.parse(utf8("object not-an-oid\ntype commit\ntag v1\n\nmessage\n")) }
        let tag = try GitAnnotatedTag.parse(utf8("object \(fixtureId(4).hex)\ntype commit\ntag v1\n\nmessage\n"))
        XCTAssertEqual(tag.target, fixtureId(4))
        XCTAssertEqual(tag.targetType, .commit)
        XCTAssertEqual(tag.name, "v1")
    }
}
