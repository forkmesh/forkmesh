import FMApi
import FMBase
import FMCrypto
import FMDiff
import FMNet
import XCTest

@testable import FMCode

@MainActor
final class FakeCollabTransport: CollabTransport {
    var requests: [(method: String, url: URL, body: JSONObject)] = []
    var responder: (String, URL, JSONObject) -> HTTPResponse = { _, _, _ in HTTPResponse(status: 200, text: "{\"ok\":true}") }

    func url(_ path: String, _ query: [(String, String)]) -> URL {
        var components = URLComponents(string: "https://app.test")!
        components.path = path
        if !query.isEmpty { components.queryItems = query.map { URLQueryItem(name: $0.0, value: $0.1) } }
        return components.url!
    }

    func rawRequest(_ method: String, _ url: URL, headers: HTTPHeaders, body: Data?) async throws -> HTTPResponse {
        let json = body.map { JSON.parseObject($0) } ?? [:]
        requests.append((method, url, json))
        return responder(method, url, json)
    }
}

final class ReviewComposerTests: XCTestCase {
    let patch = "diff --git a/src/a.swift b/src/a.swift\n--- a/src/a.swift\n+++ b/src/a.swift\n@@ -1,3 +1,3 @@\n let a = 1\n-let b = 2\n+let b = 3\n let c = 4\n"
    let patchset = String(repeating: "d", count: 40)

    func testDiffLinesMapToSidesLikeTheWeb() {
        let added = DiffLine(kind: .added, text: "x", oldNumber: 0, newNumber: 7)
        let removed = DiffLine(kind: .removed, text: "x", oldNumber: 5, newNumber: 0)
        let context = DiffLine(kind: .context, text: "x", oldNumber: 4, newNumber: 6)
        XCTAssertEqual(DiffLineSelection(path: "f", line: added), DiffLineSelection(path: "f", side: .new, startLine: 7))
        XCTAssertEqual(DiffLineSelection(path: "f", line: removed)?.side, .old)
        XCTAssertEqual(DiffLineSelection(path: "f", line: context)?.startLine, 6)
        XCTAssertNil(DiffLineSelection(path: "f", line: DiffLine(kind: .added, text: "", newNumber: 0)))
        let range = DiffLineSelection(path: "f", side: .new, startLine: 9).extended(to: added)
        XCTAssertEqual(range.lineStart, 7)
        XCTAssertEqual(range.lineEnd, 9)
        XCTAssertTrue(range.contains(DiffLine(kind: .added, text: "", newNumber: 8)))
        XCTAssertFalse(range.contains(removed))
        XCTAssertEqual(range.extended(to: removed), range)
        XCTAssertEqual(range.label, "f · new lines 7–9")
    }

    func testSingleLineCommentIsALineCommentWithItsAnchor() {
        let fields = ReviewComposer.inlineComment(
            DiffLineSelection(path: "src/a.swift", side: .new, startLine: 2), body: "Why 3?\n\n", patch: patch,
            patchset: patchset, newId: { "E1" })
        XCTAssertEqual(fields.type, "line-comment")
        XCTAssertEqual(fields.fields.str("id"), "E1")
        XCTAssertEqual(fields.fields.int("line"), 2)
        XCTAssertEqual(fields.fields.str("anchorLines"), "let b = 3")
        XCTAssertEqual(fields.fields.str("patchset"), patchset)
        XCTAssertEqual(fields.body, "Why 3?")
        XCTAssertNil(fields.fields["threadId"])
    }

    func testRangeCommentOpensAThreadNamedAfterItsEvent() {
        let fields = ReviewComposer.inlineComment(
            DiffLineSelection(path: "src/a.swift", side: .old, startLine: 3, endLine: 1), body: "old block", patch: patch,
            patchset: "", newId: { "T9" })
        XCTAssertEqual(fields.type, "thread-comment")
        XCTAssertEqual(fields.fields.str("threadId"), "T9")
        XCTAssertEqual(fields.fields.str("id"), "T9")
        XCTAssertEqual(fields.fields.int("lineStart"), 1)
        XCTAssertEqual(fields.fields.int("lineEnd"), 3)
        XCTAssertEqual(fields.fields.str("anchorLines"), "let a = 1\nlet b = 2\nlet c = 4")
        XCTAssertNil(fields.fields["patchset"])
    }

    func testSuggestionsAlwaysOpenAThread() {
        let fields = ReviewComposer.inlineComment(
            DiffLineSelection(path: "src/a.swift", side: .new, startLine: 2), body: "try", patch: patch, patchset: patchset,
            suggestion: "@@ -2 +2 @@\n-let b = 3\n+let b = 4\n", newId: { "S1" })
        XCTAssertEqual(fields.type, "thread-comment")
        XCTAssertEqual(fields.fields.str("suggestionPatch"), "@@ -2 +2 @@\n-let b = 3\n+let b = 4\n")
    }

    func testRepliesAndResolutionTargetTheThread() {
        var thread = ReviewThread(id: "T1")
        thread.events = [ReviewEvent(type: "thread-comment", id: "T1"), ReviewEvent(type: "thread-reply", id: "R2")]
        let reply = ReviewComposer.reply(to: thread, body: "done")
        XCTAssertEqual(reply.type, "thread-reply")
        XCTAssertEqual(reply.fields.str("parentId"), "R2")
        XCTAssertEqual(ReviewComposer.setResolved(thread, resolved: true).fields.str("state"), "resolved")
        XCTAssertEqual(ReviewComposer.setResolved(thread, resolved: false).fields.str("state"), "open")
    }

    func testBatchedReviewSendsCommentsThenTheVerdict() {
        var book = ReviewDraftBook()
        XCTAssertNil(book.add(DiffLineSelection(path: "src/a.swift", side: .new, startLine: 2), body: "one", id: "d1"))
        XCTAssertEqual(book.add(DiffLineSelection(path: "src/a.swift", side: .new, startLine: 1), body: "  ", id: "d2"), ReviewComposer.emptyBody)
        XCTAssertNil(book.add(DiffLineSelection(path: "src/a.swift", side: .new, startLine: 1, endLine: 3), body: "two", id: "d3"))
        XCTAssertEqual(book.summary, "2 pending comments")
        var ids = ["A", "B"].makeIterator()
        let events = ReviewComposer.submission(
            verdict: .approve, body: "", drafts: book.drafts, patch: patch, patchset: patchset, newId: { ids.next()! })
        XCTAssertEqual(events.map(\.type), ["line-comment", "thread-comment", "review"])
        XCTAssertEqual(events.last?.fields.str("state"), "approve")
        XCTAssertEqual(ReviewComposer.submission(verdict: .comment, body: "", drafts: [], patch: "", patchset: "", newId: { "" }).count, 0)
        book.update("d1", body: "uno")
        XCTAssertEqual(book.drafts.first?.body, "uno")
        book.remove("d1")
        XCTAssertEqual(book.drafts(inFile: "src/a.swift").map(\.id), ["d3"])
    }

    func testReviewValidation() {
        XCTAssertEqual(ReviewComposer.validateReview(.requestChanges, body: "", drafts: []), ReviewComposer.requestChangesNeedsNote)
        XCTAssertNil(ReviewComposer.validateReview(.approve, body: "", drafts: []))
        XCTAssertEqual(ReviewComposer.validateReview(.comment, body: " ", drafts: []), ReviewComposer.emptyBody)
        let draft = ReviewDraft(id: "d", selection: DiffLineSelection(path: "f", side: .new, startLine: 1), body: "x")
        XCTAssertNil(ReviewComposer.validateReview(.requestChanges, body: "", drafts: [draft]))
        XCTAssertEqual(ReviewComposer.validate(String(repeating: "x", count: ReviewComposer.maxBodyBytes + 1)), ReviewComposer.tooLong)
    }
}

final class CollabSigningTests: XCTestCase {
    func sha(_ text: String) -> String { Sha256.hex(Array(text.utf8)) }

    func testContentMatchesTheCrossLanguageVectors() {
        XCTAssertEqual(sha(CollabContent.pull("comment", ["body": "Looks good"])),
                       "5fc87d339144090b0ad2e192e6a6fe58e98d3a5a062467c7e62049c7d8c3db01")
        XCTAssertEqual(sha(CollabContent.pull("review", ["state": "approved", "body": "LGTM"])),
                       "b863bbc11dd8fea92da94a7da47f815aceeaa9418483992d0a952273894a0731")
        XCTAssertEqual(sha(CollabContent.pull("line-comment", ["path": "src/x.cpp", "side": "new", "line": 42, "body": "needs a guard"])),
                       "a2574c2b392fd0db6b7e4b0d025d4094bb410691dca7686ddf095d63881f4985")
        XCTAssertEqual(sha(CollabContent.pull("line-comment", [
            "path": "src/x.cpp", "side": "new", "line": 7, "body": "needs a guard",
            "patchset": "0123456789abcdef0123456789abcdef01234567", "anchorLines": "if (!p)",
        ])), "de9b901105f5216175574abb59e8dd1e28e192c85864c7cd07e28a629adebdfa")
        XCTAssertEqual(sha(CollabContent.pull("thread-comment", [
            "threadId": "thread-1", "path": "src/x.cpp", "side": "new", "lineStart": 42, "lineEnd": 44,
            "body": "Use guard", "suggestionPatch": "@@ -1 +1 @@\n-old\n+new\n",
            "patchset": "0123456789abcdef0123456789abcdef01234567", "anchorLines": "int x = 1;\nreturn x;\n}",
        ])), "3e917b7d28e0e5ac2f551e30dc6d30f986325bc015c7b2dd6d1095cd11ebd894")
        XCTAssertEqual(sha(CollabContent.issue("open", ["title": "Hello", "body": "World", "attachments": ["attachments/x.png"]])),
                       "7b66aa5f19006b73598497bef2f5d6ed94700c48d479279df3ff5eb147fb0513")
        XCTAssertEqual(sha(CollabContent.issue("title", ["title": "Renamed issue"])),
                       "37f2b6516c608aafe45958eabd5bb4c20b9c6765d1eb39a5c27b4588e566ec97")
    }

    func testIssueContentCoversEveryServerType() {
        XCTAssertEqual(CollabContent.issue("reaction", ["target": "e1", "reaction": "eyes"]), "e1\0eyes")
        XCTAssertEqual(CollabContent.issue("dates", ["startDate": 5, "endDate": .null]), "5\u{0}0")
        XCTAssertEqual(CollabContent.issue("claim", ["assignee": "mona", "claimState": "claimed"]), "mona\0claimed")
        XCTAssertEqual(CollabContent.issue("priority", ["priority": 3.9]), "3")
        XCTAssertEqual(CollabContent.issue("labels", ["labels": ["a", "b"]]), "a,b")
        XCTAssertEqual(CollabContent.issue("delete", ["target": "self"]), "self")
        XCTAssertEqual(CollabContent.issue("vote", [:]), "")
        XCTAssertEqual(CollabContent.pull("suggestion-state", ["threadId": "t1", "state": "applied", "appliedCommit": "abc"]), "t1\0applied\0abc\0")
        XCTAssertEqual(CollabContent.discussion("open", ["title": "T", "body": "B", "category": "ideas"]), "T\0B\0ideas")
    }

    func testSignedEventsVerifyAgainstTheCanonicalString() throws {
        let identity = try Identity.fromSeed([UInt8](repeating: 7, count: 32))
        let signer = CollabSigner(identity: identity, clock: FixedClock(epochMillis: 1_770_000_000_000), authorName: "mona")
        let fields = ReviewComposer.inlineComment(
            DiffLineSelection(path: "a", side: .new, startLine: 1), body: "b", patch: "", patchset: "", newId: { "L1" })
        let event = try signer.sign(.pull, owner: "o", name: "r", number: 4, type: fields.type, fields: fields.fields)
        XCTAssertEqual(event.str("id"), "L1")
        XCTAssertEqual(event.str("authorName"), "mona")
        XCTAssertEqual(event.int64("ts"), 1_770_000_000_000)
        let canonical = "forkmesh-pull-comment-v2\no/r\nline-comment\n4\n\(identity.publicKeyB64url)\n1770000000000\n"
            + sha(CollabContent.pull("line-comment", event))
        let signature = try B64.decodeUnpadded(event.str("sig"))
        XCTAssertTrue(Ed25519.verify(publicKey: identity.publicKeyBytes, message: Array(canonical.utf8), signature: Array(signature)))
        let issue = try signer.sign(.issue, owner: "o", name: "r", number: 9, type: "labels", fields: ["labels": ["bug"]])
        XCTAssertTrue(issue.str("id").hasPrefix("labels-ios-1770000000000-"))
        XCTAssertThrowsError(try signer.sign(.pull, owner: "o", name: "r", number: 1, type: "merge", fields: [:]))
        XCTAssertThrowsError(try signer.sign(.discussion, owner: "o", name: "r", number: 1, type: "status", fields: [:]))
    }

    func testSignerSignsV2StringsForTheWriterRepository() throws {
        let identity = try Identity.fromSeed([UInt8](repeating: 7, count: 32))
        let signer = CollabSigner(identity: identity, clock: FixedClock(epochMillis: 1_770_000_000_000))
        let fields = ReviewComposer.inlineComment(
            DiffLineSelection(path: "a", side: .new, startLine: 1), body: "b", patch: "", patchset: "", newId: { "L1" })
        let event = try signer.sign(.pull, owner: "Owner", name: "Repo", number: 4, type: fields.type, fields: fields.fields)
        let body = "line-comment\n4\n\(identity.publicKeyB64url)\n1770000000000\n" + sha(CollabContent.pull("line-comment", event))
        let signature = Array(try B64.decodeUnpadded(event.str("sig")))
        let key = identity.publicKeyBytes
        XCTAssertTrue(Ed25519.verify(publicKey: key, message: Array(("forkmesh-pull-comment-v2\nowner/repo\n" + body).utf8), signature: signature))
        XCTAssertFalse(Ed25519.verify(publicKey: key, message: Array(("forkmesh-pull-comment-v1\n" + body).utf8), signature: signature))
        XCTAssertFalse(Ed25519.verify(publicKey: key, message: Array(("forkmesh-pull-comment-v2\nmallory/repo\n" + body).utf8), signature: signature))
        XCTAssertEqual(
            signer.canonical(.issue, owner: "o", name: "r", type: "vote", number: 2, ts: 5, event: [:]),
            "forkmesh-issue-event-v2\no/r\nvote\n2\n\(identity.publicKeyB64url)\n5\n\(sha(""))")
        XCTAssertEqual(
            signer.canonical(.discussion, owner: "o", name: "r", type: "comment", number: 3, ts: 5, event: ["body": "x"]),
            "forkmesh-discussion-event-v2\no/r\ncomment\n3\n\(identity.publicKeyB64url)\n5\n\(sha("x"))")
        XCTAssertEqual(
            signer.canonical(.issue, owner: "a/b", name: "r", type: "vote", number: 2, ts: 5, event: [:]),
            "forkmesh-issue-event-v1\nvote\n2\n\(identity.publicKeyB64url)\n5\n\(sha(""))")
    }

    @MainActor func testWriterBindsItsOwnRepository() async throws {
        let transport = FakeCollabTransport()
        let identity = try Identity.fromSeed([UInt8](repeating: 3, count: 32))
        let writer = CollabRecordWriter(
            transport: transport, signer: CollabSigner(identity: identity, clock: FixedClock(epochMillis: 1)),
            owner: "Ann", name: "Tool")
        try await writer.commentOnDiscussion(4, body: "hi")
        let event = transport.requests[0].body.obj("event")
        let canonical = "forkmesh-discussion-event-v2\nann/tool\ncomment\n4\n\(identity.publicKeyB64url)\n1\n\(sha("hi"))"
        XCTAssertTrue(Ed25519.verify(
            publicKey: identity.publicKeyBytes, message: Array(canonical.utf8), signature: Array(try B64.decodeUnpadded(event.str("sig")))))
    }

    @MainActor func testWriterPostsSignedEventsToTheRecordInbox() async throws {
        let transport = FakeCollabTransport()
        let identity = try Identity.fromSeed([UInt8](repeating: 3, count: 32))
        let writer = CollabRecordWriter(
            transport: transport, signer: CollabSigner(identity: identity, clock: FixedClock(epochMillis: 1)), owner: "o", name: "r")
        let sent = try await writer.submitReview(7, [ReviewComposer.comment("hi"), ReviewComposer.review(.approve, body: "")])
        XCTAssertEqual(sent, 2)
        XCTAssertEqual(transport.requests.map(\.url.path), ["/api/repo/o/r/pulls", "/api/repo/o/r/pulls"])
        XCTAssertEqual(transport.requests[1].body.int("number"), 7)
        XCTAssertEqual(transport.requests[1].body.obj("event").str("state"), "approve")
        try await writer.applyIssue(3, [.title("New"), .status(open: false)])
        XCTAssertEqual(transport.requests.last?.url.path, "/api/repo/o/r/issues")
        XCTAssertEqual(transport.requests.last?.body.obj("event").str("status"), "closed")
        try await writer.openDiscussion(title: "Q", body: "B", category: "ideas")
        XCTAssertEqual(transport.requests.last?.body.str("titleIfNew"), "Q")
        XCTAssertEqual(transport.requests.last?.body.int("number"), 0)
        transport.responder = { _, _, _ in HTTPResponse(status: 401, text: "{\"error\":\"bad_signature\"}") }
        do {
            try await writer.commentOnDiscussion(1, body: "x")
            XCTFail("expected a refusal")
        } catch {
            XCTAssertTrue("\(error)".contains("could not verify"))
        }
        let unsigned = CollabRecordWriter(transport: transport, signer: nil, owner: "o", name: "r")
        do {
            try await unsigned.sendIssue(1, .comment("x"))
            XCTFail("expected no identity")
        } catch {
            XCTAssertTrue(error is CollabInboxUnavailable)
        }
    }
}
