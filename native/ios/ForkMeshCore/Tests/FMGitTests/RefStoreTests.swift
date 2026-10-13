// The ref cases (the repository case lives in LocalEngineWorkflowTests.swift).

import Foundation
import XCTest
@testable import FMGit

final class RefStoreTests: GitTestCase {
    func testRejectsTraversalAndLockSuffixRefs() {
        for value in ["refs/heads/../main", "refs/heads/main.lock", "refs/heads/a..b", "refs/tags/v1"] {
            assertGitError(.invalidRef) { _ = try GitRefName.branch(value) }
        }
        for value in [
            "refs/heads/", "refs/heads/a b", "refs/heads/a~1", "refs/heads/a^", "refs/heads/a:b", "refs/heads/a?",
            "refs/heads/a*", "refs/heads/a\\b", "refs/heads/a[b", "refs/heads/a//b", "refs/heads/a.", "HEAD",
            "refs/notes/x", "refs/heads/a\u{1}",
        ] {
            assertGitError(.invalidRef) { _ = try GitRefName.parse(value) }
        }
        XCTAssertNoThrow(try GitRefName.parse("refs/heads/feature/x"))
        XCTAssertNoThrow(try GitRefName.parse("refs/remotes/origin/main"))
        // The character class leaves `]` out.
        XCTAssertNoThrow(try GitRefName.parse("refs/heads/a]b"))
    }

    func testPackedRefsReadsPeeledTag() throws {
        let refs = try PackedRefs.parse(
            utf8("# pack-refs with: peeled\n\(fixtureId(1).hex) refs/tags/v1\n^\(fixtureId(2).hex)\n"),
            .standard
        )
        XCTAssertEqual(refs.lookup("refs/tags/v1")?.peeled, fixtureId(2))
        XCTAssertEqual(refs.lookup("refs/tags/v1")?.objectId, fixtureId(1))
    }

    func testPackedRefsRejectsUnsortedAndMalformedRecords() {
        assertGitError(.invalidRef) {
            _ = try PackedRefs.parse(
                utf8("\(fixtureId(1).hex) refs/tags/v2\n\(fixtureId(2).hex) refs/tags/v1\n"), .standard
            )
        }
        assertGitError(.invalidRef) { _ = try PackedRefs.parse(utf8("^\(fixtureId(2).hex)\n"), .standard) }
        assertGitError(.invalidRef) { _ = try PackedRefs.parse(utf8("abc refs/tags/v1\n"), .standard) }
    }

    func testStaleRefUpdateLeavesOriginalObjectIdUnchanged() throws {
        let root = try makeTemp("forkmesh-ref-")
        let refs = RefStore(root)
        let ref = try GitRefName.branch("refs/heads/main")
        XCTAssertEqual(try refs.update(ref, expectedOld: nil, next: fixtureId(3)), .updated)
        let result = try refs.update(ref, expectedOld: fixtureId(1), next: fixtureId(2))
        XCTAssertEqual(result, .nonFastForward)
        XCTAssertEqual(try refs.readDirect(ref), fixtureId(3))
        XCTAssertFalse(FileManager.default.fileExists(atPath: root + "/refs/heads/main.lock"))
    }

    func testLockedRefIsReported() throws {
        let root = try makeTemp("forkmesh-ref-lock-")
        let refs = RefStore(root)
        try FileManager.default.createDirectory(atPath: root + "/refs/heads", withIntermediateDirectories: true)
        try Data().write(to: URL(fileURLWithPath: root + "/refs/heads/main.lock"))
        assertGitError(.refLocked) {
            _ = try refs.update(try GitRefName.branch("refs/heads/main"), expectedOld: nil, next: fixtureId(1))
        }
    }

    func testListDirectMergesLooseOverPacked() throws {
        let root = try makeTemp("forkmesh-ref-list-")
        let refs = RefStore(root)
        try Data(utf8(
            "\(fixtureId(1).hex) refs/heads/main\n\(fixtureId(2).hex) refs/heads/old\n\(fixtureId(9).hex) refs/tags/v1\n"
        )).write(to: URL(fileURLWithPath: root + "/packed-refs"))
        _ = try refs.update(try GitRefName.branch("refs/heads/main"), expectedOld: nil, next: fixtureId(3))
        _ = try refs.update(try GitRefName.branch("refs/heads/feature/x"), expectedOld: nil, next: fixtureId(4))
        try Data(utf8("ref: refs/heads/main\n")).write(to: URL(fileURLWithPath: root + "/refs/heads/alias"))
        let listed = try refs.listDirect("refs/heads/", .standard)
        XCTAssertEqual(listed, [
            "refs/heads/main": fixtureId(3), "refs/heads/old": fixtureId(2), "refs/heads/feature/x": fixtureId(4),
        ])
        assertGitError(.invalidRef) { _ = try refs.listDirect("heads/", .standard) }
    }
}
