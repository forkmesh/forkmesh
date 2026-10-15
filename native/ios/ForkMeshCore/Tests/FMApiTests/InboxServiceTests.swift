// The inbox contract, as in native/android core/src/test/.../api/InboxContractTest.kt. Every
// content layout and canonical string InboxService signs must match the Qt client
// (IssueStore/PullStore) and the Worker's verify_* functions byte for byte, or the relay rejects
// the submission with 401 bad_signature. The content hashes are the same cross-language vectors
// desktop/tests/test_crypto.cpp pins. As in Kotlin, the posted signatures are verified against the
// strings the Worker rebuilds.

import FMBase
import FMCrypto
import FMNet
import Foundation
import XCTest
@testable import FMApi

@MainActor
final class InboxServiceTests: XCTestCase {
    private struct Fixture {
        let identity: Identity
        let http: FakeHTTPClient
        let inbox: InboxService

        var author: String { identity.publicKeyB64url }

        func body(_ index: Int) -> JSONObject { JSON.parseObject(http.requests[index].bodyText) }

        func path(_ index: Int) -> String { http.requests[index].url.path }
    }

    private func fixture() throws -> Fixture {
        let identity = try Identity.fromSeed([UInt8](repeating: 7, count: 32))
        let http = FakeHTTPClient { _ in HTTPResponse(status: 200, json: ["ok": true]) }
        let inbox = InboxService(
            settings: StaticApiSettings(serverUrl: "wss://app.forkmesh.com/ws"), identity: identity, http: http,
            clock: FixedClock(epochMillis: svcEpochMillis))
        return Fixture(identity: identity, http: http, inbox: inbox)
    }

    private func sha256Hex(_ content: String) -> String { Sha256.hex(Array(content.utf8)) }

    private func verify(
        _ fx: Fixture, _ canonical: String, _ sig: JSONValue?, file: StaticString = #filePath, line: UInt = #line
    ) {
        guard let text = sig?.stringValue, let signature = try? B64.decodeUnpadded(text) else {
            return XCTFail("no signature for:\n\(canonical)", file: file, line: line)
        }
        XCTAssertTrue(
            Ed25519.verify(publicKey: fx.identity.publicKeyBytes, message: Array(canonical.utf8), signature: Array(signature)),
            "signature over:\n\(canonical)", file: file, line: line)
    }

    // MARK: inbox contract

    func testIssueEventContentHashesMatchTheQtCrossLanguageVectors() async throws {
        let fx = try fixture()
        // qt: IssueStore::canonicalString "open" vector.
        XCTAssertEqual(
            sha256Hex(fx.inbox.issueContent("open", [
                "title": "Hello", "body": "World", "attachments": ["attachments/x.png"],
            ])),
            "7b66aa5f19006b73598497bef2f5d6ed94700c48d479279df3ff5eb147fb0513")
        // qt: "title" (rename) vector — content is just the title.
        XCTAssertEqual(
            sha256Hex(fx.inbox.issueContent("title", ["title": "Renamed issue"])),
            "37f2b6516c608aafe45958eabd5bb4c20b9c6765d1eb39a5c27b4588e566ec97")
    }

    func testPullCommentContentHashesMatchTheQtCrossLanguageVectors() async throws {
        let fx = try fixture()
        // qt: PullStore::canonicalString "comment" vector.
        XCTAssertEqual(
            sha256Hex(fx.inbox.pullCommentContent("comment", ["body": "Looks good"])),
            "5fc87d339144090b0ad2e192e6a6fe58e98d3a5a062467c7e62049c7d8c3db01")
        // qt: "review" vector — state and body, NUL-joined.
        XCTAssertEqual(
            sha256Hex(fx.inbox.pullCommentContent("review", ["state": "approved", "body": "LGTM"])),
            "b863bbc11dd8fea92da94a7da47f815aceeaa9418483992d0a952273894a0731")
        // qt: "line-comment" vector — path, side, line, body.
        XCTAssertEqual(
            sha256Hex(fx.inbox.pullCommentContent("line-comment", [
                "path": "src/x.cpp", "side": "new", "line": 42, "body": "needs a guard",
            ])),
            "a2574c2b392fd0db6b7e4b0d025d4094bb410691dca7686ddf095d63881f4985")
        // qt: anchored "line-comment" vector — the patchset (head commit) and the commented
        // text join the signed content when either is present.
        XCTAssertEqual(
            sha256Hex(fx.inbox.pullCommentContent("line-comment", [
                "path": "src/x.cpp", "side": "new", "line": 7, "body": "needs a guard",
                "patchset": "0123456789abcdef0123456789abcdef01234567", "anchorLines": "if (!p)",
            ])),
            "de9b901105f5216175574abb59e8dd1e28e192c85864c7cd07e28a629adebdfa")
        // qt: anchored "thread-comment" vector.
        XCTAssertEqual(
            sha256Hex(fx.inbox.pullCommentContent("thread-comment", [
                "threadId": "thread-1", "path": "src/x.cpp", "side": "new", "lineStart": 42, "lineEnd": 44,
                "body": "Use guard", "suggestionPatch": "@@ -1 +1 @@\n-old\n+new\n",
                "patchset": "0123456789abcdef0123456789abcdef01234567", "anchorLines": "int x = 1;\nreturn x;\n}",
            ])),
            "3e917b7d28e0e5ac2f551e30dc6d30f986325bc015c7b2dd6d1095cd11ebd894")
    }

    func testANewPullSignsTheFiveFieldContentEvenWithNoCommits() async throws {
        // verify_pull_event builds ["\0".join(fields + [commits])] and only adds the legacy
        // four-field candidate when commits is empty. Signing the five-field form is therefore
        // correct in both cases, and with an empty mbox it is the four-field bytes plus one
        // trailing NUL, not the same string.
        let title = "Fix the install command"
        let base = "main"
        let head = "kaif:mobile-edit-readme"
        let patch = "diff --git a/README.md b/README.md\n"
        XCTAssertEqual([title, base, head, patch, ""].joined(separator: "\0"), "\(title)\0\(base)\0\(head)\0\(patch)\0")
        XCTAssertNotEqual([title, base, head, patch, ""].joined(separator: "\0"), [title, base, head, patch].joined(separator: "\0"))

        let fx = try fixture()
        try await fx.inbox.submitNewPull("o", "r", title: title, base: base, head: head, patch: patch)
        let pull = fx.body(0).obj("pull")
        verify(fx, "forkmesh-pull-event-v2\no/r\n\(fx.author)\n1770000000000\n\(sha256Hex("\(title)\0\(base)\0\(head)\0\(patch)\0"))", pull["sig"])
    }

    // MARK: InboxContractTest.kt

    func testContentLayoutsPerType() async throws {
        let fx = try fixture()
        XCTAssertEqual(fx.inbox.issueContent("comment", ["body": "b"]), "b\0")
        XCTAssertEqual(fx.inbox.issueContent("labels", ["labels": ["a", "b"]]), "a,b")
        XCTAssertEqual(fx.inbox.issueContent("priority", ["priority": 3.9]), "3")
        XCTAssertEqual(fx.inbox.issueContent("progress", [:]), "0")
        XCTAssertEqual(fx.inbox.issueContent("vote", [:]), "")
        XCTAssertEqual(fx.inbox.pullCommentContent("thread-reply", ["threadId": "t1", "parentId": "p1", "body": "b"]), "t1\0p1\0b")
        XCTAssertEqual(
            fx.inbox.pullCommentContent("suggestion-state", ["threadId": "t1", "state": "applied", "appliedCommit": "abc"]),
            "t1\0applied\0abc\0")
        XCTAssertEqual(InboxService.discussionContent("open", ["title": "T", "body": "B", "category": "ideas"]), "T\0B\0ideas")
    }

    func testNewIssueIsSignedWithTheIssueEventTemplate() async throws {
        let fx = try fixture()
        try await fx.inbox.submitNewIssue(
            "owner", "repo", title: "Crash", body: "Steps", labels: ["bug"], milestone: "v2", priority: 2, assignees: ["mona"])
        XCTAssertEqual(fx.http.requests.count, 1)
        let request = fx.http.requests[0]
        XCTAssertEqual(request.method, "POST")
        XCTAssertEqual(request.url.absoluteString, "https://app.forkmesh.com/api/repo/owner/repo/issues")
        XCTAssertEqual(request.headers["Content-Type"], "application/json")
        // Posted without a session: the signature is the authority.
        XCTAssertNil(request.headers["Authorization"])
        let body = fx.body(0)
        XCTAssertEqual(body.keys, ["number", "titleIfNew", "event", "meta"])
        XCTAssertEqual(body["number"], 0)
        XCTAssertEqual(body["titleIfNew"], "Crash")
        XCTAssertEqual(body["meta"], ["labels": ["bug"], "milestone": "v2", "priority": 2, "assignees": ["mona"]])
        let event = body.obj("event")
        XCTAssertEqual(event.keys, ["type", "title", "body", "attachments", "author", "ts", "sig"])
        XCTAssertEqual(event["attachments"], [])
        XCTAssertEqual(event["author"], .string(fx.author))
        XCTAssertEqual(event["ts"], .int(1_770_000_000_000))
        let hash = sha256Hex("Crash\0Steps\0")
        verify(fx, "forkmesh-issue-event-v2\nowner/repo\nopen\n0\n\(fx.author)\n1770000000000\n\(hash)", event["sig"])
    }

    func testIssueCommentVoteAndStatusAreSigned() async throws {
        let fx = try fixture()
        try await fx.inbox.commentOnIssue("o", "r", 12, "me too")
        try await fx.inbox.voteOnIssue("o", "r", 12)
        try await fx.inbox.setIssueStatus("o", "r", 12, "closed")
        let author = fx.author
        let comment = fx.body(0)
        let vote = fx.body(1)
        let status = fx.body(2)
        XCTAssertEqual(comment.keys, ["number", "event"])
        XCTAssertEqual(comment["number"], 12)
        XCTAssertEqual(comment.obj("event")["body"], "me too")
        verify(fx, "forkmesh-issue-event-v2\no/r\ncomment\n12\n\(author)\n1770000000000\n\(sha256Hex("me too\0"))", comment.obj("event")["sig"])
        XCTAssertEqual(vote.obj("event").keys, ["type", "author", "ts", "sig"])
        verify(fx, "forkmesh-issue-event-v2\no/r\nvote\n12\n\(author)\n1770000000000\n\(sha256Hex(""))", vote.obj("event")["sig"])
        XCTAssertEqual(status.obj("event")["status"], "closed")
        verify(fx, "forkmesh-issue-event-v2\no/r\nstatus\n12\n\(author)\n1770000000000\n\(sha256Hex("closed"))", status.obj("event")["sig"])
        XCTAssertEqual(fx.path(2), "/api/repo/o/r/issues")
    }

    func testNewPullIsSignedOverTitleBaseHeadPatchCommits() async throws {
        let fx = try fixture()
        try await fx.inbox.submitNewPull(
            "o", "r", title: "Fix", base: "main", head: "fix/x", patch: "diff --git a b", commits: "From abc",
            description: "Why", authorName: "Mona")
        XCTAssertEqual(fx.http.requests.count, 1)
        XCTAssertEqual(fx.path(0), "/api/repo/o/r/pulls")
        let pull = fx.body(0).obj("pull")
        // 'commits', 'description' and 'authorName' are the fields the owner's
        // node needs to attribute and replay the work.
        XCTAssertEqual(pull.keys, ["title", "base", "head", "patch", "commits", "description", "author", "authorName", "ts", "sig"])
        XCTAssertEqual(pull["commits"], "From abc")
        XCTAssertEqual(pull["description"], "Why")
        XCTAssertEqual(pull["authorName"], "Mona")
        XCTAssertEqual(pull["ts"], .int(1_770_000_000_000))
        let hash = sha256Hex("Fix\0main\0fix/x\0diff --git a b\0From abc")
        verify(fx, "forkmesh-pull-event-v2\no/r\n\(fx.author)\n1770000000000\n\(hash)", pull["sig"])
    }

    func testOpeningAPullAnswersItsNumber() async throws {
        let identity = try Identity.fromSeed([UInt8](repeating: 7, count: 32))
        let http = FakeHTTPClient { _ in HTTPResponse(status: 200, json: ["ok": true, "number": 31]) }
        let inbox = InboxService(
            settings: StaticApiSettings(serverUrl: "wss://app.forkmesh.com/ws"), identity: identity, http: http,
            clock: FixedClock(epochMillis: svcEpochMillis))
        let number = try await inbox.openNewPull(
            "o", "r", title: "Fix", base: "main", head: "agent/x", patch: "diff --git a b", commits: "",
            description: "Why", authorName: "")
        XCTAssertEqual(number, 31)
        XCTAssertEqual(http.requests.first?.url.path, "/api/repo/o/r/pulls")
        let fx = try fixture()
        let unnumbered = try await fx.inbox.openNewPull("o", "r", title: "Fix", base: "main", head: "agent/x", patch: "p")
        XCTAssertEqual(unnumbered, 0)
    }

    func testPullCommentsAndReviewsUseThePullCommentTemplate() async throws {
        let fx = try fixture()
        try await fx.inbox.commentOnPull("o", "r", 7, "Looks good")
        try await fx.inbox.reviewPull("o", "r", 7, "approve", "LGTM")
        let author = fx.author
        XCTAssertEqual(fx.path(0), "/api/repo/o/r/pulls")
        XCTAssertEqual(fx.body(0)["number"], 7)
        let comment = fx.body(0).obj("event")
        verify(fx, "forkmesh-pull-comment-v2\no/r\ncomment\n7\n\(author)\n1770000000000\n\(sha256Hex("Looks good"))", comment["sig"])
        let review = fx.body(1).obj("event")
        XCTAssertEqual(review["state"], "approve")
        verify(fx, "forkmesh-pull-comment-v2\no/r\nreview\n7\n\(author)\n1770000000000\n\(sha256Hex("approve\0LGTM"))", review["sig"])
    }

    func testDiscussionsUseTheDiscussionEventTemplate() async throws {
        let fx = try fixture()
        try await fx.inbox.submitNewDiscussion("o", "r", title: "Idea", body: "Body", category: "ideas")
        try await fx.inbox.commentOnDiscussion("o", "r", 3, "Reply")
        let author = fx.author
        let open = fx.body(0)
        XCTAssertEqual(fx.path(0), "/api/repo/o/r/discussions")
        XCTAssertEqual(open["number"], 0)
        XCTAssertEqual(open["titleIfNew"], "Idea")
        verify(
            fx, "forkmesh-discussion-event-v2\no/r\nopen\n0\n\(author)\n1770000000000\n\(sha256Hex("Idea\0Body\0ideas"))",
            open.obj("event")["sig"])
        let reply = fx.body(1)
        verify(fx, "forkmesh-discussion-event-v2\no/r\ncomment\n3\n\(author)\n1770000000000\n\(sha256Hex("Reply"))", reply.obj("event")["sig"])
    }

    func testDiscussionAnswersSignTheirTargetAndCarryTheSession() async throws {
        let fx = try fixture()
        try await fx.inbox.markDiscussionAnswer("o", "r", 3, target: "reply-1", sessionToken: "tok")
        try await fx.inbox.markDiscussionAnswer("o", "r", 3, target: "", sessionToken: "")
        let author = fx.author
        let marked = fx.body(0)
        XCTAssertEqual(fx.path(0), "/api/repo/o/r/discussions")
        XCTAssertEqual(marked["number"], 3)
        XCTAssertEqual(marked["sessionToken"], "tok")
        XCTAssertEqual(marked.obj("event")["type"], "answer")
        XCTAssertEqual(marked.obj("event")["target"], "reply-1")
        verify(
            fx, "forkmesh-discussion-event-v2\no/r\nanswer\n3\n\(author)\n1770000000000\n\(sha256Hex("reply-1"))",
            marked.obj("event")["sig"])
        XCTAssertNil(fx.body(1)["sessionToken"])
        verify(
            fx, "forkmesh-discussion-event-v2\no/r\nanswer\n3\n\(author)\n1770000000000\n\(sha256Hex(""))",
            fx.body(1).obj("event")["sig"])
        XCTAssertEqual(InboxService.discussionContent("answer", ["target": "reply-1"]), "reply-1")
    }

    func testEventSigningPrefixNamesTheRepository() async throws {
        XCTAssertEqual(EventSigning.prefix("issue-event", owner: " Alice ", name: "Project"), "forkmesh-issue-event-v2\nalice/project\n")
        XCTAssertEqual(EventSigning.repoKey("a/b", "c"), "")
        XCTAssertEqual(EventSigning.repoKey("a", "c\n"), "a/c")
        XCTAssertEqual(EventSigning.repoKey("a", "c\nd"), "")
        XCTAssertEqual(EventSigning.repoKey("", "c"), "")
        XCTAssertEqual(EventSigning.prefix("pull-event", owner: "a/b", name: "c"), "forkmesh-pull-event-v1\n")
    }

    func testInboxSignsV2StringsNamingTheRepository() async throws {
        let fx = try fixture()
        try await fx.inbox.submitNewIssue("Owner", "Repo", title: "Crash", body: "Steps")
        try await fx.inbox.commentOnIssue("o", "r", 12, "me too")
        try await fx.inbox.voteOnIssue("o", "r", 12)
        try await fx.inbox.setIssueStatus("o", "r", 12, "closed")
        try await fx.inbox.submitNewPull("o", "r", title: "Fix", base: "main", head: "fix/x", patch: "diff --git a b", commits: "From abc")
        try await fx.inbox.commentOnPull("o", "r", 7, "Looks good")
        try await fx.inbox.reviewPull("o", "r", 7, "approve", "LGTM")
        try await fx.inbox.submitNewDiscussion("o", "r", title: "Idea", body: "Body", category: "ideas")
        try await fx.inbox.commentOnDiscussion("o", "r", 3, "Reply")
        let a = fx.author
        let t = "1770000000000"
        verify(fx, "forkmesh-issue-event-v2\nowner/repo\nopen\n0\n\(a)\n\(t)\n\(sha256Hex("Crash\0Steps\0"))", fx.body(0).obj("event")["sig"])
        verify(fx, "forkmesh-issue-event-v2\no/r\ncomment\n12\n\(a)\n\(t)\n\(sha256Hex("me too\0"))", fx.body(1).obj("event")["sig"])
        verify(fx, "forkmesh-issue-event-v2\no/r\nvote\n12\n\(a)\n\(t)\n\(sha256Hex(""))", fx.body(2).obj("event")["sig"])
        verify(fx, "forkmesh-issue-event-v2\no/r\nstatus\n12\n\(a)\n\(t)\n\(sha256Hex("closed"))", fx.body(3).obj("event")["sig"])
        verify(
            fx, "forkmesh-pull-event-v2\no/r\n\(a)\n\(t)\n\(sha256Hex("Fix\0main\0fix/x\0diff --git a b\0From abc"))",
            fx.body(4).obj("pull")["sig"])
        verify(fx, "forkmesh-pull-comment-v2\no/r\ncomment\n7\n\(a)\n\(t)\n\(sha256Hex("Looks good"))", fx.body(5).obj("event")["sig"])
        verify(fx, "forkmesh-pull-comment-v2\no/r\nreview\n7\n\(a)\n\(t)\n\(sha256Hex("approve\0LGTM"))", fx.body(6).obj("event")["sig"])
        verify(fx, "forkmesh-discussion-event-v2\no/r\nopen\n0\n\(a)\n\(t)\n\(sha256Hex("Idea\0Body\0ideas"))", fx.body(7).obj("event")["sig"])
        verify(fx, "forkmesh-discussion-event-v2\no/r\ncomment\n3\n\(a)\n\(t)\n\(sha256Hex("Reply"))", fx.body(8).obj("event")["sig"])
        let v1 = "forkmesh-pull-comment-v1\ncomment\n7\n\(a)\n\(t)\n\(sha256Hex("Looks good"))"
        let sig = try B64.decodeUnpadded(fx.body(5).obj("event").str("sig"))
        XCTAssertFalse(Ed25519.verify(publicKey: fx.identity.publicKeyBytes, message: Array(v1.utf8), signature: Array(sig)))
        let other = "forkmesh-pull-comment-v2\nmallory/other\ncomment\n7\n\(a)\n\(t)\n\(sha256Hex("Looks good"))"
        XCTAssertFalse(Ed25519.verify(publicKey: fx.identity.publicKeyBytes, message: Array(other.utf8), signature: Array(sig)))
    }

    func testThreadSubscriptionSignsTheSubscribeTemplate() async throws {
        let fx = try fixture()
        try await fx.inbox.setThreadSubscription("owner", "repo", node: " Mona ", source: "pull", number: 7, subscribed: true)
        try await fx.inbox.setThreadSubscription("owner", "repo", node: "mona", source: "issue", number: 12, subscribed: false)
        let on = fx.body(0)
        XCTAssertEqual(fx.path(0), "/api/repo/owner/repo/subscribe")
        XCTAssertEqual(on.keys, ["node", "source", "number", "subscribed", "ts", "sig"])
        XCTAssertEqual(on["node"], "mona")
        XCTAssertEqual(on["subscribed"], true)
        XCTAssertEqual(on["ts"], "1770000000000")
        verify(fx, "forkmesh-subscribe-v1\nowner/repo\npull\n7\n1\n1770000000000", on["sig"])
        let off = fx.body(1)
        verify(fx, "forkmesh-subscribe-v1\nowner/repo\nissue\n12\n0\n1770000000000", off["sig"])

        let blank = await svcThrown { try await fx.inbox.setThreadSubscription("o", "r", node: " ", source: "issue", number: 1, subscribed: true) }
        XCTAssertTrue(blank is ApiArgumentError, "\(String(describing: blank))")
        let source = await svcThrown { try await fx.inbox.setThreadSubscription("o", "r", node: "a", source: "commit", number: 1, subscribed: true) }
        XCTAssertTrue(source is ApiArgumentError, "\(String(describing: source))")
        let number = await svcThrown { try await fx.inbox.setThreadSubscription("o", "r", node: "a", source: "issue", number: 0, subscribed: true) }
        XCTAssertTrue(number is ApiArgumentError, "\(String(describing: number))")
        XCTAssertEqual(fx.http.requests.count, 2)
    }

    func testRelayRefusalsSurfaceTheErrorCode() async throws {
        let fx = try fixture()
        fx.http.enqueue(status: 401, json: ["error": "bad_signature"])
        let refused = await svcThrown { try await fx.inbox.commentOnIssue("o", "r", 1, "x") }
        XCTAssertEqual(dartErrorText(refused!), "Exception: bad_signature")
        fx.http.enqueue(status: 500, text: "nope")
        let broken = await svcThrown { try await fx.inbox.voteOnIssue("o", "r", 1) }
        XCTAssertEqual((broken as? ApiException)?.message, "HTTP 500")
        fx.http.enqueue(status: 202, json: ["ok": true])
        let accepted = await svcThrown { try await fx.inbox.voteOnIssue("o", "r", 1) }
        XCTAssertEqual((accepted as? ApiException)?.message, "HTTP 202")
        fx.http.enqueue(status: 201, json: ["ok": true])
        try await fx.inbox.voteOnIssue("o", "r", 1)
    }
}
