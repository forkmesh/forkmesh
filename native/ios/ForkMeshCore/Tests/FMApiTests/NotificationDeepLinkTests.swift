// Notification deep links, as in native/android NotificationDeepLinkTest.kt.

import XCTest
@testable import FMApi

final class NotificationDeepLinkTests: XCTestCase {
    func testParsesSafeRepoContextHrefs() {
        let issue = NotificationDeepLink.parse("/mona/forkmesh/issues/12")
        XCTAssertEqual(issue?.repo.fullName, "mona/forkmesh")
        XCTAssertEqual(issue?.initialTab, .issues)
        XCTAssertEqual(issue?.number, 12)

        let pull = NotificationDeepLink.parse("/mona/forkmesh/pulls/7")
        XCTAssertEqual(pull?.initialTab, .pulls)
        XCTAssertEqual(pull?.number, 7)

        let commit = NotificationDeepLink.parse("/mona/forkmesh/commit/abc123")
        XCTAssertEqual(commit?.initialTab, .commits)
        XCTAssertEqual(commit?.reference, "abc123")

        XCTAssertEqual(NotificationDeepLink.parse("/mona/forkmesh/agents")?.initialTab, .agents)
        XCTAssertEqual(NotificationDeepLink.parse("/mona/forkmesh/actions")?.initialTab, .actions)
        XCTAssertNil(NotificationDeepLink.parse("https://evil.test/mona/forkmesh"))
        XCTAssertNil(NotificationDeepLink.parse("/api/notifications"))
    }

    func testIssueLinkOpensTheIssueTabWithItsNumber() throws {
        let link = try XCTUnwrap(NotificationDeepLink.parse("/forkmesh/forkmesh/issues/692"))
        XCTAssertEqual(link.repo.owner, "forkmesh")
        XCTAssertEqual(link.repo.name, "forkmesh")
        XCTAssertEqual(link.initialTab, .issues)
        XCTAssertEqual(link.number, 692)
    }

    func testCommitLinkKeepsTheReference() {
        let link = NotificationDeepLink.parse("/a/b/commit/abc123?x=1#frag")
        XCTAssertEqual(link?.initialTab, .commits)
        XCTAssertEqual(link?.reference, "abc123")
        // `commits/<x>` names a branch unless it is a bare hash.
        XCTAssertEqual(NotificationDeepLink.parse("/a/b/commits/a%252Fb")?.branch, "a/b", "decoded twice, as in Dart")
        XCTAssertEqual(NotificationDeepLink.parse("/a/b/commits/a%252Fb")?.reference, "")
        XCTAssertEqual(NotificationDeepLink.parse("/a/b/commits/abc1234")?.reference, "abc1234")
    }

    func testCodePushLinkOpensTheCommitsTabAtTheBranch() throws {
        // What a code-push ping carries: /owner/repo/commits/<branch>.
        let link = try XCTUnwrap(NotificationDeepLink.parse("/alice/atlas/commits/main"))
        XCTAssertEqual(link.repo.fullName, "alice/atlas")
        XCTAssertEqual(link.initialTab, .commits)
        XCTAssertEqual(link.branch, "main")
        XCTAssertEqual(link.reference, "")
        XCTAssertTrue(link.isBranchCommits)
        // Branch names may hold slashes.
        XCTAssertEqual(NotificationDeepLink.parse("/alice/atlas/commits/feature/push-pings")?.branch, "feature/push-pings")
        // The tree and query spellings name a branch too.
        XCTAssertEqual(NotificationDeepLink.parse("/a/b/tree/dev/x")?.branch, "dev/x")
        XCTAssertEqual(NotificationDeepLink.parse("/a/b/tree/dev")?.initialTab, .code)
        XCTAssertEqual(NotificationDeepLink.parse("/a/b/commits?branch=release%2F1")?.branch, "release/1")
        XCTAssertEqual(NotificationDeepLink.parse("/a/b?ref=dev")?.branch, "dev")
        XCTAssertFalse(NotificationDeepLink.parse("/a/b/commit/abc1234")?.isBranchCommits ?? true)
    }

    func testCodeIsTheDefaultAndUnknownSectionsFallBackToIt() {
        XCTAssertEqual(NotificationDeepLink.parse("/a/b")?.initialTab, .code)
        XCTAssertEqual(NotificationDeepLink.parse("/a/b/blob/main/README.md")?.initialTab, .code)
        XCTAssertEqual(NotificationDeepLink.parse("/a/b/settings")?.initialTab, .code)
        XCTAssertEqual(NotificationDeepLink.parse("/a/b/pulls/x")?.number, 0)
        XCTAssertEqual(NotificationDeepLink.parse("a/b/discussions/3")?.initialTab, .discussions)
        XCTAssertEqual(NotificationDeepLink.parse("/a/b/worktrees")?.initialTab, .worktrees)
    }

    func testRejectsExternalApiAndUnsafeLinks() {
        XCTAssertNil(NotificationDeepLink.parse("https://forkmesh.com/a/b"), "absolute URL")
        XCTAssertNil(NotificationDeepLink.parse("//forkmesh.com/a/b"), "authority")
        XCTAssertNil(NotificationDeepLink.parse("/api/accounts"), "api path")
        XCTAssertNil(NotificationDeepLink.parse("/dashboard/settings"), "dashboard path")
        XCTAssertNil(NotificationDeepLink.parse("/a"), "too short")
        XCTAssertNil(NotificationDeepLink.parse("/a b/c"), "unsafe owner")
        XCTAssertNil(NotificationDeepLink.parse("   "), "blank")
        XCTAssertNil(NotificationDeepLink.parse("/a%zz/b"), "malformed escape")
        XCTAssertNil(NotificationDeepLink.parse("/a%2Fb/c"), "an escaped slash is not a safe segment")
    }
}
