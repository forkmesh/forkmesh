import XCTest
@testable import FMGit

final class RefspecTests: XCTestCase {
    func testParsesAndMapsStandardWildcardFetchRefspec() throws {
        let refspec = try GitFetchRefspec.parse("+refs/heads/*:refs/remotes/origin/*")
        XCTAssertTrue(refspec.isWildcard)
        XCTAssertTrue(refspec.force)
        XCTAssertEqual(refspec.destinationFor("refs/heads/main"), "refs/remotes/origin/main")
        XCTAssertEqual(refspec.destinationFor("refs/heads/feature/x"), "refs/remotes/origin/feature/x")
        XCTAssertNil(refspec.destinationFor("refs/tags/v1"))
        XCTAssertNil(refspec.destinationFor("refs/heads/"))
        XCTAssertEqual(try GitFetchRefspec.standard("origin").value, refspec.value)
    }

    func testParsesSingleBranchFetchRefspec() throws {
        let refspec = try GitFetchRefspec.parse("refs/heads/main:refs/remotes/origin/main")
        XCTAssertFalse(refspec.isWildcard)
        XCTAssertEqual(refspec.destinationFor("refs/heads/main"), "refs/remotes/origin/main")
        XCTAssertNil(refspec.destinationFor("refs/heads/dev"))
    }

    func testRejectsRefspecsThatCouldWriteOutsideRefsRemotes() {
        for value in [
            "refs/heads/*:refs/heads/*",
            "refs/heads/*:refs/tags/*",
            "+refs/*:refs/remotes/origin/*",
            "refs/heads/main:refs/remotes/origin/*",
            "refs/heads/*:refs/remotes/*",
            "refs/heads/a:refs/remotes/origin/a:extra",
            "refs/heads/*:refs/remotes/a/b/*",
            ":refs/remotes/origin/a",
        ] {
            assertGitError(.invalidRef) { _ = try GitFetchRefspec.parse(value) }
        }
    }

    func testPushRefspecMapsLocalToRemoteBranchAndDefaultsTheName() throws {
        let refspec = try GitPushRefspec.parse("main")
        XCTAssertEqual(refspec.local.value, "refs/heads/main")
        XCTAssertEqual(refspec.remote.value, "refs/heads/main")
        let renamed = try GitPushRefspec.parse("refs/heads/dev:refs/heads/staging")
        XCTAssertEqual(renamed.localBranch, "dev")
        XCTAssertEqual(renamed.remoteBranch, "staging")
    }

    func testPushRefspecRejectsWildcardsForceAndDeletion() {
        for value in ["refs/heads/*:refs/heads/*", "+main", ":main", "main:"] {
            assertGitError(.invalidRef) { _ = try GitPushRefspec.parse(value) }
        }
    }

    func testRemoteNamesAreValidated() {
        XCTAssertNoThrow(try GitRemoteName.validate("origin"))
        assertGitError(.invalidRef) { try GitRemoteName.validate("../evil") }
        assertGitError(.invalidRef) { try GitRemoteName.validate("") }
        assertGitError(.invalidRef) { try GitRemoteName.validate("a..b") }
        assertGitError(.invalidRef) { try GitRemoteName.validate("x.lock") }
        assertGitError(.invalidRef) { try GitRemoteName.validate(String(repeating: "a", count: 65)) }
    }
}
