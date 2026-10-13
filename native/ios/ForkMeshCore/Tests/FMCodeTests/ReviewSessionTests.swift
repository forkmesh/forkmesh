import FMApi
import FMBase
import FMCrypto
import FMDiff
import FMNet
import XCTest

@testable import FMCode

final class ReviewSessionTests: XCTestCase {
    static let head = String(repeating: "a", count: 40)
    static let patch = "diff --git a/f.txt b/f.txt\n--- a/f.txt\n+++ b/f.txt\n@@ -1,2 +1,3 @@\n one\n+two\n three"

    @MainActor final class Reader: ReviewLedgerReading {
        var files: [String: String] = [
            ".forkmesh/pulls/2/pull.md": "---\ntitle: T\nauthor: A\ncreationHeadOid: \(ReviewSessionTests.head)\n---\n",
            ".forkmesh/pulls/2/changes.patch": ReviewSessionTests.patch,
            ".forkmesh/pulls/2/0001-thread-comment.md":
                "---\ntype: thread-comment\nid: t1\nthreadId: t1\nauthor: R\npath: f.txt\nside: new\nlineStart: 2\nlineEnd: 2\npatchset: \(ReviewSessionTests.head)\n---\nhm",
        ]

        func tree(_ owner: String, _ name: String, path: String, ref: String) async throws -> RepoTree {
            RepoTree(path: path, entries: files.keys.filter { $0.hasPrefix(path + "/") }.map {
                RepoTreeEntry(name: String($0.dropFirst(path.count + 1)), path: $0, type: "file")
            })
        }

        func blob(_ owner: String, _ name: String, _ path: String, ref: String) async throws -> RepoBlob {
            RepoBlob(path: path, content: files[path] ?? "")
        }

        func compare(_ owner: String, _ name: String, base: String, head: String) async throws -> RepoComparison {
            RepoComparison()
        }
    }

    @MainActor func session(_ transport: FakeCollabTransport) throws -> PullReviewSession {
        let signer = CollabSigner(identity: try Identity.fromSeed([UInt8](repeating: 5, count: 32)), clock: FixedClock(epochMillis: 9))
        return PullReviewSession(
            owner: "o", name: "r", number: 2, reader: Reader(),
            writer: CollabRecordWriter(transport: transport, signer: signer, owner: "o", name: "r"), clock: FixedClock(epochMillis: 9))
    }

    @MainActor func testLoadsThreadsAndSelectsLines() async throws {
        let session = try session(FakeCollabTransport())
        await session.load()
        XCTAssertNil(session.loadError)
        XCTAssertEqual(session.files.map(\.path), ["f.txt"])
        XCTAssertEqual(session.threadCount("f.txt"), 1)
        XCTAssertEqual(session.snapshot.thread("t1")?.placement, .current)
        let lines = session.file("f.txt")!.hunks[0].lines
        session.tap(path: "f.txt", line: lines[0])
        XCTAssertEqual(session.selection?.startLine, 1)
        session.tap(path: "f.txt", line: lines[2])
        XCTAssertEqual(session.selection?.lineEnd, 3)
        session.tap(path: "f.txt", line: lines[1])
        XCTAssertEqual(session.selection, DiffLineSelection(path: "f.txt", side: .new, startLine: 2))
        session.tap(path: "f.txt", line: lines[1])
        XCTAssertNil(session.selection)
        session.toggleViewed("f.txt")
        XCTAssertEqual(session.viewedSummary, "1 of 1 viewed")
    }

    @MainActor func testCommentsRepliesAndReviewsAppearPending() async throws {
        let transport = FakeCollabTransport()
        let session = try session(transport)
        await session.load()
        let added = session.file("f.txt")!.hunks[0].lines[1]
        session.tap(path: "f.txt", line: added)
        let notice = await session.commentNow(body: "nice")
        XCTAssertFalse(notice.isError)
        XCTAssertNil(session.selection)
        let sent = transport.requests[0].body.obj("event")
        XCTAssertEqual(sent.str("type"), "line-comment")
        XCTAssertEqual(sent.str("anchorLines"), "two")
        XCTAssertEqual(sent.str("patchset"), Self.head)
        XCTAssertEqual(session.snapshot.threads.count, 2)
        let thread = session.snapshot.thread("t1")!
        _ = await session.reply(thread, body: "agreed")
        _ = await session.setResolved(thread, true)
        XCTAssertTrue(session.snapshot.thread("t1")!.resolved)
        XCTAssertEqual(session.snapshot.thread("t1")?.events.count, 3)
        session.tap(path: "f.txt", line: added)
        XCTAssertNil(session.addDraft(body: "draft one"))
        XCTAssertEqual(session.addDraft(body: "x"), "Pick a line first.")
        let review = await session.submit(.requestChanges, body: "")
        XCTAssertFalse(review.isError)
        XCTAssertTrue(session.drafts.isEmpty)
        XCTAssertEqual(transport.requests.suffix(2).map { $0.body.obj("event").str("type") }, ["line-comment", "review"])
        XCTAssertEqual(session.snapshot.reviewSummary, "changes_requested")
        XCTAssertTrue(session.isPending(session.snapshot.thread("t1")!.events[1]))
    }

    @MainActor func testFailedWritesKeepDraftsAndSayWhy() async throws {
        let transport = FakeCollabTransport()
        transport.responder = { _, _, _ in HTTPResponse(status: 429, text: "{\"error\":\"inbox_full\"}") }
        let session = try session(transport)
        await session.load()
        session.tap(path: "f.txt", line: session.file("f.txt")!.hunks[0].lines[1])
        XCTAssertNil(session.addDraft(body: "keep me"))
        let notice = await session.submit(.approve, body: "")
        XCTAssertTrue(notice.isError)
        XCTAssertTrue(notice.message.contains("inbox is full"))
        XCTAssertEqual(session.drafts.count, 1)
        let empty = await session.commentNow(body: "")
        XCTAssertEqual(empty.message, ReviewComposer.emptyBody)
    }
}

final class IssueBoardSessionTests: XCTestCase {
    @MainActor func testMovesSignLabelAndStatusEditsAndUpdateLocally() async throws {
        let transport = FakeCollabTransport()
        let signer = CollabSigner(identity: try Identity.fromSeed([UInt8](repeating: 6, count: 32)), clock: FixedClock(epochMillis: 1))
        let store = InMemoryKeyValueStore()
        let board = IssueBoardSession(
            owner: "o", name: "r", store: store,
            writer: CollabRecordWriter(transport: transport, signer: signer, owner: "o", name: "r"),
            load: { [Issue(number: 1, title: "Crash", labels: ["bug"]), Issue(number: 2, title: "Docs")] })
        await board.refresh()
        XCTAssertEqual(board.layout[0].issues.count, 2)
        board.query = "crash"
        XCTAssertEqual(board.layout[0].issues.map(\.number), [1])
        let notice = await board.move(board.issues[0], to: "Done")
        XCTAssertEqual(notice?.message, "Moved #1 to Done.")
        XCTAssertEqual(transport.requests.map { $0.body.obj("event").str("type") }, ["status"])
        XCTAssertEqual(board.issues[0].status, "closed")
        let again = await board.move(board.issues[0], to: "Done")
        XCTAssertNil(again)
        XCTAssertEqual(board.saveColumns(["x"]), IssueBoard.tooFewColumns)
        XCTAssertNil(board.saveColumns(["Now", "Later"]))
        XCTAssertEqual(board.columns, ["Now", "Later"])
    }
}
