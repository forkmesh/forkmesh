import FMApi
import FMBase
import FMCrypto
import FMDiff
import FMNet
import XCTest

@testable import FMCode

final class BranchReviewTests: XCTestCase {
    static let base = String(repeating: "a", count: 40)
    static let head = String(repeating: "b", count: 40)
    static let patch = """
        diff --git a/src/app.txt b/src/app.txt
        --- a/src/app.txt
        +++ b/src/app.txt
        @@ -1,4 +1,6 @@
         one
         two
        -old
        +alpha
        +beta
        +gamma
         four
        """
    static let folder = ".forkmesh/branch-reviews/306395bddc1b0cac0297da67"

    @MainActor final class Reader: ReviewLedgerReading {
        var files: [String: String] = [:]
        var trees: [(path: String, ref: String)] = []
        var blobs: [String] = []

        func tree(_ owner: String, _ name: String, path: String, ref: String) async throws -> RepoTree {
            trees.append((path, ref))
            let names = files.keys.filter { $0.hasPrefix(path + "/") }
            if names.isEmpty { throw ApiException("not_found") }
            return RepoTree(path: path, entries: names.map {
                RepoTreeEntry(name: String($0.dropFirst(path.count + 1)), path: $0, type: "file")
            })
        }

        func blob(_ owner: String, _ name: String, _ path: String, ref: String) async throws -> RepoBlob {
            blobs.append(path)
            return RepoBlob(path: path, content: files[path] ?? "")
        }

        func compare(_ owner: String, _ name: String, base: String, head: String) async throws -> RepoComparison {
            RepoComparison()
        }
    }

    func vectorEvent() -> JSONObject {
        [
            "type": "thread-comment", "threadId": "thread-comment-ios-1", "path": "src/app.txt", "side": "new",
            "lineStart": .int(3), "lineEnd": .int(5), "body": "Rename these", "suggestionPatch": "", "patchset": "",
            "anchorLines": "alpha\nbeta\ngamma",
        ]
    }

    func testSigningVectorMatchesTheOtherClients() {
        let event = vectorEvent()
        XCTAssertEqual(
            Sha256.hex(Array(CollabContent.pull("thread-comment", event).utf8)),
            "02d13720c8a1e971d05805561c7378eba454f51998fd5c8f447bf6569efb64ce")
        XCTAssertEqual(
            CollabSigner.branchCanonical(
                owner: "alice", name: "demo", branch: "feature/review", type: "thread-comment", author: "AUTHORKEY",
                ts: 1_790_000_000_000, event: event),
            "forkmesh-branch-comment-v2\nalice/demo\nthread-comment\nfeature/review\nAUTHORKEY\n1790000000000\n"
                + "02d13720c8a1e971d05805561c7378eba454f51998fd5c8f447bf6569efb64ce")
        let reply: JSONObject = ["threadId": "thread-comment-ios-1", "parentId": "thread-comment-ios-1", "body": "Done"]
        XCTAssertEqual(
            Sha256.hex(Array(CollabContent.pull("thread-reply", reply).utf8)),
            "d9c05bb661d5f22bce769db007e4fa6831fb49282b9e041ead75f52ef517468a")
        let state: JSONObject = ["threadId": "thread-comment-ios-1", "state": "resolved", "body": ""]
        XCTAssertEqual(
            Sha256.hex(Array(CollabContent.pull("thread-state", state).utf8)),
            "ce3bf1a2fd60719b415f48680b213edf965f71ecc6154233b02dc3c82c6c2c5f")
        XCTAssertEqual(BranchReviewPaths.key("feature/review"), "306395bddc1b0cac0297da67")
        XCTAssertEqual(BranchReviewPaths.folder("feature/review"), Self.folder)
    }

    func testEventFileNamesFollowTheLedgerPattern() {
        XCTAssertTrue(BranchReviewPaths.isEventFile("0001-thread-comment.md"))
        XCTAssertTrue(BranchReviewPaths.isEventFile("0012-thread-state.md"))
        XCTAssertFalse(BranchReviewPaths.isEventFile("branch.md"))
        XCTAssertFalse(BranchReviewPaths.isEventFile("001-thread-comment.md"))
        XCTAssertFalse(BranchReviewPaths.isEventFile("0001-Thread.md"))
        XCTAssertFalse(BranchReviewPaths.isEventFile("0001-.md"))
        XCTAssertFalse(BranchReviewPaths.isEventFile("0001-thread-comment.txt"))
        XCTAssertFalse(BranchReviewPaths.isEventFile("0001_thread.md"))
    }

    func testSignedBranchEventsVerifyAndRefuseOtherTypes() throws {
        let identity = try Identity.fromSeed([UInt8](repeating: 7, count: 32))
        let signer = CollabSigner(identity: identity, clock: FixedClock(epochMillis: 1_790_000_000_000), authorName: "mona")
        let event = try signer.signBranch(
            owner: "Alice", name: "Demo", branch: "feature/review", type: "thread-comment", fields: vectorEvent())
        XCTAssertEqual(event.str("author"), identity.publicKeyB64url)
        XCTAssertEqual(event.str("authorName"), "mona")
        XCTAssertEqual(event.int64("ts"), 1_790_000_000_000)
        XCTAssertTrue(event.str("id").hasPrefix("thread-comment-ios-1790000000000-"))
        let canonical = "forkmesh-branch-comment-v2\nalice/demo\nthread-comment\nfeature/review\n\(identity.publicKeyB64url)\n"
            + "1790000000000\n02d13720c8a1e971d05805561c7378eba454f51998fd5c8f447bf6569efb64ce"
        let signature = Array(try B64.decodeUnpadded(event.str("sig")))
        XCTAssertTrue(Ed25519.verify(publicKey: identity.publicKeyBytes, message: Array(canonical.utf8), signature: signature))
        XCTAssertThrowsError(try signer.signBranch(owner: "o", name: "r", branch: "b", type: "line-comment", fields: [:]))
        XCTAssertThrowsError(try signer.signBranch(owner: "o/x", name: "r", branch: "b", type: "thread-reply", fields: [:]))
        XCTAssertThrowsError(try signer.signBranch(owner: "o", name: "r", branch: "", type: "thread-reply", fields: [:]))
    }

    @MainActor func testWriterPostsTheBranchAndEventWithoutANumber() async throws {
        let transport = FakeCollabTransport()
        transport.responder = { _, _, _ in HTTPResponse(status: 201, text: "{\"ok\":true,\"queued\":true}") }
        let signer = CollabSigner(identity: try Identity.fromSeed([UInt8](repeating: 3, count: 32)), clock: FixedClock(epochMillis: 4))
        let writer = CollabRecordWriter(transport: transport, signer: signer, owner: "alice", name: "demo")
        let fields = ReviewEventFields(type: "thread-reply", fields: ["threadId": "t1", "parentId": "t1", "body": "Done"])
        let event = try await writer.sendBranch("feature/review", fields)
        XCTAssertEqual(transport.requests.count, 1)
        let request = transport.requests[0]
        XCTAssertEqual(request.method, "POST")
        XCTAssertEqual(request.url.path, "/api/repo/alice/demo/pulls")
        XCTAssertEqual(request.body.str("branch"), "feature/review")
        XCTAssertNil(request.body["number"])
        XCTAssertEqual(request.body.obj("event").str("sig"), event.str("sig"))
        XCTAssertEqual(event.str("parentId"), "t1")
        let unsigned = CollabRecordWriter(transport: transport, signer: nil, owner: "alice", name: "demo")
        do {
            _ = try await unsigned.sendBranch("feature/review", fields)
            XCTFail("expected an error")
        } catch {}
    }

    @MainActor func testLedgerReadsTheBranchFolderOnThePullsRef() async {
        let reader = Reader()
        let ledger = BranchReviewLedger(reader: reader, owner: "alice", name: "demo")
        let missing = await ledger.load("feature/review")
        XCTAssertEqual(missing, [])
        reader.files = [
            "\(Self.folder)/branch.md": "---\nbranch: feature/review\n---\n",
            "\(Self.folder)/0002-thread-reply.md": "---\ntype: thread-reply\nid: r1\nthreadId: t1\nparentId: t1\nauthor: B\n---\nok",
            "\(Self.folder)/0001-thread-comment.md":
                "---\ntype: thread-comment\nid: t1\nthreadId: t1\nauthor: A\npath: src/app.txt\nside: new\nlineStart: 3\nlineEnd: 5\nanchorLinesB64: \(ReviewEvent.encodeBlob("alpha\nbeta\ngamma"))\n---\nRename these",
            "\(Self.folder)/0003-review.md": "---\ntype: review\nid: v1\nstate: approve\n---\n",
            "\(Self.folder)/notes.txt": "x",
        ]
        let events = await ledger.load("feature/review")
        XCTAssertEqual(reader.trees.last?.path, Self.folder)
        XCTAssertEqual(reader.trees.last?.ref, "forkmesh/pulls")
        XCTAssertEqual(events.map(\.id), ["t1", "r1"])
        XCTAssertEqual(events[0].anchorLines, "alpha\nbeta\ngamma")
        XCTAssertEqual(events[0].body, "Rename these")
        XCTAssertFalse(reader.blobs.contains("\(Self.folder)/branch.md"))
        XCTAssertFalse(reader.blobs.contains("\(Self.folder)/notes.txt"))
    }

    @MainActor func makeModel(
        _ reader: Reader, _ transport: FakeCollabTransport, signed: Bool = true, headOid: String = BranchReviewTests.head
    ) throws -> BranchDiffModel {
        let api = FakeCollabAPI()
        api.comparison = RepoComparison(baseOid: Self.base, headOid: headOid, mergeBaseOid: Self.base, commitCount: 1, patch: Self.patch)
        let signer = signed
            ? CollabSigner(identity: try Identity.fromSeed([UInt8](repeating: 5, count: 32)), clock: FixedClock(epochMillis: 9))
            : nil
        let threads = BranchReviewSession(
            owner: "alice", name: "demo", branch: "feature/review", reader: reader,
            writer: CollabRecordWriter(transport: transport, signer: signer, owner: "alice", name: "demo"),
            clock: FixedClock(epochMillis: 9))
        return BranchDiffModel(repo: Repository(owner: "alice", name: "demo"), head: "feature/review", api: api, threads: threads)
    }

    @MainActor func testBranchDiffLoadsThreadsAndPostsComments() async throws {
        let reader = Reader()
        reader.files = [
            "\(Self.folder)/0001-thread-comment.md":
                "---\ntype: thread-comment\nid: t1\nthreadId: t1\nauthor: A\npath: src/app.txt\nside: new\nlineStart: 3\nlineEnd: 4\nanchorLinesB64: \(ReviewEvent.encodeBlob("alpha\nbeta"))\n---\nRename",
            "\(Self.folder)/0002-thread-comment.md":
                "---\ntype: thread-comment\nid: f1\nthreadId: f1\nauthor: A\npath: src/app.txt\nside: new\nlineStart: 0\nlineEnd: 0\n---\nWhole file",
            "\(Self.folder)/0003-thread-comment.md":
                "---\ntype: thread-comment\nid: o1\nthreadId: o1\nauthor: A\npath: src/app.txt\nside: new\nlineStart: 9\nlineEnd: 9\nanchorLinesB64: \(ReviewEvent.encodeBlob("gone"))\n---\nOld",
        ]
        let transport = FakeCollabTransport()
        let model = try makeModel(reader, transport)
        await model.start()
        let threads = try XCTUnwrap(model.threads)
        XCTAssertTrue(threads.loaded)
        XCTAssertTrue(threads.canComment)
        XCTAssertEqual(threads.snapshot.threads.count, 3)
        XCTAssertEqual(threads.threads(path: "src/app.txt", at: [(.new, 4)]).map(\.id), ["t1"])
        XCTAssertEqual(threads.threads(path: "src/app.txt", at: [(.new, 3), (.old, 3)]), [])
        XCTAssertEqual(threads.topThreads(path: "src/app.txt").map(\.id), ["f1", "o1"])
        XCTAssertEqual(threads.summary, "3 open threads")

        let lines = try XCTUnwrap(model.comparison?.files.first?.hunks.first?.lines)
        let alpha = lines[3]
        let gamma = lines[5]
        XCTAssertEqual(alpha.text, "alpha")
        threads.tap(path: "src/app.txt", line: alpha)
        threads.tap(path: "src/app.txt", line: gamma)
        XCTAssertEqual(threads.selection, DiffLineSelection(path: "src/app.txt", side: .new, startLine: 3, endLine: 5))
        XCTAssertTrue(threads.isSelected(path: "src/app.txt", line: lines[4]))
        XCTAssertFalse(threads.isSelected(path: "other.txt", line: lines[4]))
        XCTAssertEqual(threads.selectionPreview, "alpha\nbeta\ngamma")

        let empty = await threads.comment(body: " ")
        XCTAssertEqual(empty.message, ReviewComposer.emptyBody)
        let notice = await threads.comment(body: "Rename these\n")
        XCTAssertFalse(notice.isError)
        XCTAssertNil(threads.selection)
        let sent = transport.requests[0].body
        XCTAssertEqual(sent.str("branch"), "feature/review")
        XCTAssertNil(sent["number"])
        let event = sent.obj("event")
        XCTAssertEqual(event.str("type"), "thread-comment")
        XCTAssertEqual(event.str("threadId"), event.str("id"))
        XCTAssertEqual(event.int("lineStart"), 3)
        XCTAssertEqual(event.int("lineEnd"), 5)
        XCTAssertEqual(event.str("side"), "new")
        XCTAssertEqual(event.str("body"), "Rename these")
        XCTAssertEqual(event.str("anchorLines"), "alpha\nbeta\ngamma")
        XCTAssertNil(event["patchset"])
        XCTAssertNil(event["suggestionPatch"])
        let signature = Array(try B64.decodeUnpadded(event.str("sig")))
        let canonical = CollabSigner.branchCanonical(
            owner: "alice", name: "demo", branch: "feature/review", type: "thread-comment", author: event.str("author"),
            ts: 9, event: event)
        let key = try Identity.fromSeed([UInt8](repeating: 5, count: 32)).publicKeyBytes
        XCTAssertTrue(Ed25519.verify(publicKey: key, message: Array(canonical.utf8), signature: signature))

        let mine = try XCTUnwrap(threads.snapshot.thread(event.str("id")))
        XCTAssertEqual(mine.placement, .current)
        XCTAssertEqual(threads.threads(path: "src/app.txt", at: [(.new, 5)]).map(\.id), [mine.id])
        XCTAssertTrue(threads.isPending(mine.events[0]))

        let t1 = try XCTUnwrap(threads.snapshot.thread("t1"))
        _ = await threads.reply(t1, body: "Done")
        let reply = transport.requests[1].body.obj("event")
        XCTAssertEqual(reply.str("type"), "thread-reply")
        XCTAssertEqual(reply.str("parentId"), "t1")
        _ = await threads.setResolved(t1, true)
        XCTAssertEqual(transport.requests[2].body.obj("event").str("state"), "resolved")
        XCTAssertTrue(threads.snapshot.thread("t1")?.resolved ?? false)
        _ = await threads.setResolved(try XCTUnwrap(threads.snapshot.thread("t1")), false)
        XCTAssertEqual(transport.requests[3].body.obj("event").str("state"), "unresolved")
        XCTAssertFalse(threads.snapshot.thread("t1")?.resolved ?? true)
        XCTAssertEqual(threads.pending.count, 4)

        reader.files["\(Self.folder)/0004-thread-comment.md"] =
            "---\ntype: thread-comment\nid: \(mine.id)\nthreadId: \(mine.id)\nauthor: \(event.str("author"))\npath: src/app.txt\nside: new\nlineStart: 3\nlineEnd: 5\n---\nRename these"
        await threads.load()
        XCTAssertEqual(threads.pending.count, 3)
        XCTAssertEqual(threads.snapshot.thread(mine.id)?.events.count, 1)
    }

    @MainActor func testNoCommentingWithoutABranchHeadOrASigningKey() async throws {
        let reader = Reader()
        let transport = FakeCollabTransport()
        let same = try makeModel(reader, transport, headOid: Self.base)
        await same.start()
        let line = try XCTUnwrap(same.comparison?.files.first?.hunks.first?.lines[3])
        XCTAssertFalse(same.threads?.canComment ?? true)
        same.threads?.tap(path: "src/app.txt", line: line)
        XCTAssertNil(same.threads?.selection)

        let unsigned = try makeModel(reader, transport, signed: false)
        await unsigned.start()
        XCTAssertFalse(unsigned.threads?.canWrite ?? true)
        XCTAssertFalse(unsigned.threads?.canComment ?? true)
        unsigned.threads?.tap(path: "src/app.txt", line: line)
        XCTAssertNil(unsigned.threads?.selection)
        XCTAssertEqual(unsigned.threads?.snapshot.threads.count, 0)
        XCTAssertTrue(transport.requests.isEmpty)
    }

    func testThreadAnchorsCoverBothSidesOfContextLines() {
        let context = DiffLine(kind: .context, text: "c", oldNumber: 4, newNumber: 6)
        let removed = DiffLine(kind: .removed, text: "r", oldNumber: 5, newNumber: 0)
        let added = DiffLine(kind: .added, text: "a", oldNumber: 0, newNumber: 7)
        XCTAssertEqual(DiffThreadAnchors.unified(context).map { "\($0.0.rawValue)\($0.1)" }, ["new6", "old4"])
        XCTAssertEqual(DiffThreadAnchors.unified(removed).map { "\($0.0.rawValue)\($0.1)" }, ["old5"])
        XCTAssertEqual(
            DiffThreadAnchors.split(DiffSplitRow(left: removed, right: added)).map { "\($0.0.rawValue)\($0.1)" }, ["old5", "new7"])
        XCTAssertEqual(
            DiffThreadAnchors.split(DiffSplitRow(left: context, right: context)).map { "\($0.0.rawValue)\($0.1)" }, ["old4", "new6"])
        XCTAssertEqual(DiffThreadAnchors.split(DiffSplitRow(left: nil, right: added)).map { "\($0.0.rawValue)\($0.1)" }, ["new7"])
    }

    func testTapRulesMatchPullReview() {
        let a = DiffLine(kind: .added, text: "a", oldNumber: 0, newNumber: 3)
        let b = DiffLine(kind: .added, text: "b", oldNumber: 0, newNumber: 6)
        let gone = DiffLine(kind: .removed, text: "x", oldNumber: 4, newNumber: 0)
        let one = DiffLineSelection.tapped(nil, path: "f", line: a)
        XCTAssertEqual(one, DiffLineSelection(path: "f", side: .new, startLine: 3))
        let range = DiffLineSelection.tapped(one, path: "f", line: b)
        XCTAssertEqual(range?.lineEnd, 6)
        XCTAssertEqual(DiffLineSelection.tapped(range, path: "f", line: a), one)
        XCTAssertNil(DiffLineSelection.tapped(one, path: "f", line: a))
        XCTAssertEqual(DiffLineSelection.tapped(one, path: "f", line: gone)?.side, .old)
        XCTAssertEqual(DiffLineSelection.tapped(one, path: "g", line: b), DiffLineSelection(path: "g", side: .new, startLine: 6))
    }
}
