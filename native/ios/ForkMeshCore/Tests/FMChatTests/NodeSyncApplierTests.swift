// The node sync applier over the in-memory workspace instead of a real LocalRepoStore (FMChat
// cannot reach FMCode's Git port; the protocol is the seam), including the byte formats and the
// discussion/pull paths.

import FMAccount
import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMChat

private let author = NodeSyncAuthor(name: "Alice Mobile", email: "alice@example.com")

/// `_issueOpen`.
private func issueOpen(
    id: Int = 11, owner: String = "alice", repo: String = "notes", title: String = "Crash on launch",
    extra: JSONObject = [:]
) -> NodeSyncItem {
    var data: JSONObject = [
        "titleIfNew": .string(title),
        "event": [
            "type": "open",
            "title": .string(title),
            "body": "It crashes every time.",
            "author": "F4rlozoS0ls2-Evs6Q7mLsSsZR1TVwMAsan0tdgaJoM",
            "authorName": "bob",
            "ts": 1_783_000_000_000,
        ],
    ]
    data.merge(extra)
    return NodeSyncItem(owner: owner, repo: repo, kind: .issues, id: id, data: data)
}

private func event(_ kind: NodeSyncKind, number: Int, id: Int = 40, _ event: JSONObject, extra: JSONObject = [:]) -> NodeSyncItem {
    var data: JSONObject = ["number": .int(Int64(number)), "event": .object(event)]
    data.merge(extra)
    return NodeSyncItem(owner: "alice", repo: "notes", kind: kind, id: id, data: data)
}

@MainActor
final class NodeSyncApplierTests: XCTestCase {
    private var store = InMemoryNodeSyncRepositorySource()
    private var prefs = InMemoryKeyValueStore([:])

    override func setUp() async throws {
        store = InMemoryNodeSyncRepositorySource()
        prefs = InMemoryKeyValueStore([:])
    }

    private func applierFor() -> LocalNodeSyncApplier {
        LocalNodeSyncApplier(repos: store, author: { author }, prefs: prefs)
    }

    @discardableResult
    private func published(_ name: String, _ publishedAs: String, branch: String = "main") -> InMemoryNodeSyncWorkspace {
        store.add(name: name, publishedAs: publishedAs, branch: branch)
    }

    // MARK: Core cases

    func testWritesTheDesktopIssueLayoutAndCommitsIt() async throws {
        let workspace = published("notes", "alice/notes")
        let applier = applierFor()
        defer { applier.dispose() }

        let ok = await applier.call(issueOpen())
        XCTAssertTrue(ok)

        // Path shape: .forkmesh/issues/open/<n>/issue-<n>.json
        let entries = try await workspace.listFiles(".forkmesh/issues/open")
        let number = try XCTUnwrap(entries.first?.name)
        let raw = try await workspace.readFile(".forkmesh/issues/open/\(number)/issue-\(number).json").text

        // Qt writes QJsonDocument::Indented (four spaces) over a QJsonObject (alphabetical).
        XCTAssertTrue(raw.hasSuffix("\n"))
        XCTAssertTrue(raw.components(separatedBy: "\n")[1].hasPrefix("    \""))
        let decoded = JSON.parseObject(raw)
        XCTAssertEqual(decoded.keys, decoded.keys.sorted())
        XCTAssertEqual(decoded["schema"], "forkmesh-issue-v1")
        XCTAssertEqual(decoded["title"], "Crash on launch")
        XCTAssertEqual(decoded["status"], "open")

        // The ack promise is a durable commit, so one must exist.
        let message = try XCTUnwrap(workspace.commits.first?.message)
        XCTAssertTrue(message.contains("issue #\(number): opened"), message)
        XCTAssertTrue(message.contains("from bob"), message)
        XCTAssertEqual(workspace.commits.first?.author, author)
        XCTAssertEqual(applier.appliedTotal, 1)
        XCTAssertEqual(applier.leftTotal, 0)
        XCTAssertEqual(store.refreshed, ["notes"])
        XCTAssertEqual(applier.recent.first?.outcome, .applied)
        XCTAssertEqual(applier.recent.first?.commitId, workspace.commits.first?.id)
    }

    func testLeavesAnItemForARepositoryThisPhoneDoesNotHold() async {
        let applier = applierFor()
        defer { applier.dispose() }

        let ok = await applier.call(issueOpen())
        XCTAssertFalse(ok)
        XCTAssertEqual(applier.appliedTotal, 0)
        XCTAssertEqual(applier.leftTotal, 1)
        XCTAssertEqual(applier.leftReasons.count, 1)
        XCTAssertTrue(applier.leftReasons[0].reason.contains("no local copy"))
    }

    func testMatchesOnTheMeshIdentityNeverABareLocalName() async {
        // A local repo called "notes" that was never published, plus an inbox row for a
        // stranger's alice/notes.
        store.add(name: "notes")
        let applier = applierFor()
        defer { applier.dispose() }

        let ok = await applier.call(issueOpen())
        XCTAssertFalse(ok)
        XCTAssertEqual(applier.leftReasons.count, 1)
        XCTAssertTrue(applier.leftReasons[0].reason.contains("no local copy"))
    }

    func testLeavesAnIssueThatCarriesFileAttachments() async {
        published("notes", "alice/notes")
        let applier = applierFor()
        defer { applier.dispose() }

        let withFiles = issueOpen(extra: ["attachmentData": [["name": "shot.png", "bytes": .string(B64.encode([1, 2, 3]))]]])

        let ok = await applier.call(withFiles)
        XCTAssertFalse(ok)
        XCTAssertEqual(applier.leftReasons.count, 1)
        XCTAssertTrue(applier.leftReasons[0].reason.contains("attachments"))
    }

    func testLeavesAMalformedEventUnapplied() async {
        published("notes", "alice/notes")
        let applier = applierFor()
        defer { applier.dispose() }

        let malformed = NodeSyncItem(owner: "alice", repo: "notes", kind: .issues, id: 12, data: ["event": [:]])

        let ok = await applier.call(malformed)
        XCTAssertFalse(ok)
        XCTAssertEqual(applier.appliedTotal, 0)
        XCTAssertEqual(applier.leftTotal, 1)
        XCTAssertEqual(applier.leftReasons.first?.reason, "issue event has no type")
    }

    func testLeavesAPullUnlessTheWorktreeIsOnTheLedgerBranch() async {
        published("notes", "alice/notes")
        let applier = applierFor()
        defer { applier.dispose() }

        let pull = NodeSyncItem(owner: "alice", repo: "notes", kind: .pulls, id: 21, data: [
            "titleIfNew": "Fix the crash",
            "event": ["type": "open", "title": "Fix the crash", "ts": 1_783_000_000_000],
        ])

        // Pull records live on forkmesh/pulls; a phone has one worktree and must not switch it
        // under the user, so the item goes back to the relay.
        let ok = await applier.call(pull)
        XCTAssertFalse(ok)
        XCTAssertEqual(applier.leftReasons.count, 1)
        XCTAssertTrue(applier.leftReasons[0].reason.contains(LocalNodeSyncApplier.pullLedgerBranch))
    }

    func testReOfferingAnAppliedItemAcksWithoutCommittingTwice() async {
        let workspace = published("notes", "alice/notes")
        let applier = applierFor()
        defer { applier.dispose() }

        let item = issueOpen()
        let first = await applier.call(item)
        XCTAssertTrue(first)
        let before = workspace.commits.count

        // Same row re-offered because its ack DELETE never landed.
        let second = await applier.call(item)
        XCTAssertTrue(second)
        XCTAssertEqual(workspace.commits.count, before)
        XCTAssertEqual(applier.recent.first?.outcome, .alreadyApplied)
        XCTAssertEqual(applier.appliedTotal, 1)
    }

    func testRanksTheReasonsItemsWereLeftMostCommonFirst() async {
        published("notes", "alice/notes")
        let applier = applierFor()
        defer { applier.dispose() }

        let applied = await applier.call(issueOpen())
        XCTAssertTrue(applied)
        let strangerA = await applier.call(issueOpen(id: 12, owner: "zed", repo: "other"))
        XCTAssertFalse(strangerA)
        let strangerB = await applier.call(issueOpen(id: 13, owner: "zed", repo: "other"))
        XCTAssertFalse(strangerB)
        let withFiles = await applier.call(issueOpen(
            id: 14, extra: ["attachmentData": [["name": "a.png", "bytes": .string(B64.encode([1]))]]]))
        XCTAssertFalse(withFiles)

        XCTAssertEqual(applier.appliedTotal, 1)
        XCTAssertEqual(applier.leftTotal, 3)
        let reasons = applier.leftReasons
        XCTAssertEqual(reasons.first?.count, 2)
        XCTAssertTrue(reasons.first?.reason.contains("no local copy of zed/other") ?? false)
        XCTAssertTrue(reasons.last?.reason.contains("attachments") ?? false)
    }

    // MARK: Byte formats

    func testTheIssueRecordIsByteIdenticalToTheDesktopFormat() async throws {
        let workspace = published("notes", "alice/notes")
        let applier = applierFor()
        defer { applier.dispose() }

        let item = NodeSyncItem(owner: "alice", repo: "notes", kind: .issues, id: 3, data: [
            "event": ["type": "open", "id": "e1", "title": "T", "body": "b", "author": "AAAA", "authorName": "bob", "ts": 5],
        ])
        let ok = await applier.call(item)
        XCTAssertTrue(ok)

        let expected = """
            {
                "assignees": [],
                "author": "AAAA",
                "authorName": "bob",
                "bountyAddress": "",
                "bountyStatus": "",
                "bountyUsd": 0,
                "createdAt": 5,
                "endDate": 0,
                "events": [
                    {
                        "author": "AAAA",
                        "authorName": "bob",
                        "body": "b",
                        "id": "e1",
                        "title": "T",
                        "ts": 5,
                        "type": "open"
                    }
                ],
                "labels": [],
                "milestone": "",
                "number": 1,
                "priority": 0,
                "progress": 0,
                "schema": "forkmesh-issue-v1",
                "startDate": 0,
                "status": "open",
                "title": "T",
                "updatedAt": 5,
                "votes": 0
            }

            """
        XCTAssertEqual(workspace.files[".forkmesh/issues/open/1/issue-1.json"], expected)
        XCTAssertEqual(workspace.commits.first?.message, "issue #1: opened (from bob)")
    }

    func testFrontMatterDocumentsMatchTheDesktopWriters() async {
        let text = LocalNodeSyncApplier.frontMatterDocument(fields: [("type", "comment"), ("title", "a\r\nb\nc\rd")], body: "Body")
        XCTAssertEqual(text, "---\ntype: comment\ntitle: a b c d\n---\n\nBody\n")
        XCTAssertEqual(LocalNodeSyncApplier.pad4(2), "0002")
        XCTAssertEqual(LocalNodeSyncApplier.pad4(12345), "12345")
        XCTAssertTrue(LocalNodeSyncApplier.isCommentFile("0002-comment.md"))
        XCTAssertFalse(LocalNodeSyncApplier.isCommentFile("002-comment.md"))
        XCTAssertTrue(LocalNodeSyncApplier.isPullEventFile("0003-request-changes.md"))
        XCTAssertFalse(LocalNodeSyncApplier.isPullEventFile("0003-Review.md"))
        XCTAssertFalse(LocalNodeSyncApplier.isPullEventFile("0003-.md"))
        XCTAssertEqual(LocalNodeSyncApplier.authorLabel(["author": "F4rlozoS0ls2"]), "F4rlozoS")
        XCTAssertEqual(LocalNodeSyncApplier.short(String(repeating: "x", count: 130)), String(repeating: "x", count: 119) + "\u{2026}")
    }

    // MARK: Issues

    func testAFollowUpEventIsAppendedAndAStatusChangeMovesTheRecord() async throws {
        let workspace = published("notes", "alice/notes")
        let applier = applierFor()
        defer { applier.dispose() }
        let opened = await applier.call(issueOpen())
        XCTAssertTrue(opened)

        let comment = await applier.call(event(.issues, number: 1, id: 12, [
            "type": "comment", "id": "c1", "body": "Same here", "author": "K3y", "ts": 1_783_000_000_500,
        ]))
        XCTAssertTrue(comment)
        var record = JSON.parseObject(workspace.files[".forkmesh/issues/open/1/issue-1.json"] ?? "")
        XCTAssertEqual(record["events"]?.arrayValue?.count, 2)
        XCTAssertEqual(record["updatedAt"], .int(1_783_000_000_500))
        XCTAssertEqual(workspace.commits.first?.message, "issue #1: comment (from K3y)")

        let closed = await applier.call(event(.issues, number: 1, id: 13, [
            "type": "status", "id": "s1", "status": "closed", "authorName": "carol", "ts": 1_783_000_001_000,
        ]))
        XCTAssertTrue(closed)
        XCTAssertNil(workspace.files[".forkmesh/issues/open/1/issue-1.json"])
        record = JSON.parseObject(workspace.files[".forkmesh/issues/closed/1/issue-1.json"] ?? "")
        XCTAssertEqual(record["status"], "closed")
        XCTAssertEqual(workspace.commits.first?.message, "issue #1: status (from carol)")
        XCTAssertEqual(workspace.commits.count, 3)
    }

    func testAnEventAlreadyInTheRecordCountsAsAppliedWithoutACommit() async {
        let workspace = published("notes", "alice/notes")
        let applier = applierFor()
        defer { applier.dispose() }
        let opened = await applier.call(issueOpen())
        XCTAssertTrue(opened)
        let vote: JSONObject = ["type": "vote", "id": "v1", "ts": 1]
        let first = await applier.call(event(.issues, number: 1, id: 20, vote))
        XCTAssertTrue(first)
        let commits = workspace.commits.count

        // A different relay row carrying the same signed event (the fingerprint differs by row
        // only when there is no id, so clear the ledger to reach the record check).
        let fresh = LocalNodeSyncApplier(repos: store, author: { author }, prefs: InMemoryKeyValueStore([:]))
        defer { fresh.dispose() }
        let again = await fresh.call(event(.issues, number: 1, id: 21, vote))
        XCTAssertTrue(again)
        XCTAssertEqual(workspace.commits.count, commits)
        XCTAssertEqual(JSON.parseObject(workspace.files[".forkmesh/issues/open/1/issue-1.json"] ?? "")["votes"], .int(1))
    }

    func testAnEventForAnUnknownIssueWritesTheDesktopStandIn() async {
        let workspace = published("notes", "alice/notes")
        let applier = applierFor()
        defer { applier.dispose() }

        let ok = await applier.call(event(.issues, number: 7, [
            "type": "labels", "id": "l1", "labels": ["bug", " ", "ui "], "ts": 9,
        ], extra: ["titleIfNew": "Remote title"]))
        XCTAssertTrue(ok)
        let record = JSON.parseObject(workspace.files[".forkmesh/issues/open/7/issue-7.json"] ?? "")
        XCTAssertEqual(record["title"], "Remote title")
        XCTAssertEqual(record["labels"], ["bug", "ui"])
        XCTAssertEqual(record["events"]?[0]?["id"], "open-7")
        XCTAssertEqual(record["events"]?[0]?["sig"], "")
    }

    func testAnOpenOnATakenSlotTakesTheNextNumber() async {
        let workspace = published("notes", "alice/notes")
        workspace.files[".forkmesh/issues/closed/4/issue-4.json"] = "{}\n"
        let applier = applierFor()
        defer { applier.dispose() }

        let ok = await applier.call(issueOpen(extra: ["number": 4]))
        XCTAssertTrue(ok)
        XCTAssertNotNil(workspace.files[".forkmesh/issues/open/5/issue-5.json"])
        XCTAssertEqual(workspace.commits.first?.message, "issue #5: opened (from bob)")
    }

    func testABinaryRecordIsNeverOverwritten() async {
        let workspace = published("notes", "alice/notes")
        workspace.files[".forkmesh/issues/open/2/issue-2.json"] = ""
        workspace.binaryPaths = [".forkmesh/issues/open/2/issue-2.json"]
        let applier = applierFor()
        defer { applier.dispose() }

        let ok = await applier.call(event(.issues, number: 2, ["type": "comment", "id": "c9", "ts": 1]))
        XCTAssertFalse(ok)
        XCTAssertEqual(applier.leftReasons.first?.reason, ".forkmesh/issues/open/2/issue-2.json is not readable as text")
    }

    // MARK: Discussions

    func testADiscussionOpensAndCommentsNumberFrom0002() async throws {
        let workspace = published("notes", "alice/notes")
        let applier = applierFor()
        defer { applier.dispose() }

        let opened = await applier.call(event(.discussions, number: 0, id: 31, [
            "type": "open", "id": "d1", "title": "Roadmap", "category": "Ideas", "body": "What next?",
            "author": "K3y", "authorName": "dave", "ts": 100, "sig": "SIG",
        ]))
        XCTAssertTrue(opened)
        XCTAssertEqual(workspace.files[".forkmesh/discussions/1/discussion.md"], """
            ---
            schema: forkmesh-discussion-v1
            number: 1
            title: Roadmap
            category: Ideas
            status: open
            createdAt: 100
            author: K3y
            authorName: dave
            type: open
            id: d1
            ts: 100
            sig: SIG
            ---

            What next?

            """)
        XCTAssertEqual(workspace.commits.first?.message, "discussion #1: opened (from dave)")

        for (index, id) in ["c1", "c2"].enumerated() {
            let ok = await applier.call(event(.discussions, number: 1, id: 32 + index, [
                "type": "comment", "id": .string(id), "body": "Reply", "authorName": "erin", "ts": 200,
            ]))
            XCTAssertTrue(ok)
        }
        XCTAssertEqual(workspace.files[".forkmesh/discussions/1/0002-comment.md"],
                       "---\ntype: comment\nid: c1\nauthor: \nauthorName: erin\nts: 200\nsig: \n---\n\nReply\n")
        XCTAssertNotNil(workspace.files[".forkmesh/discussions/1/0003-comment.md"])
        XCTAssertEqual(workspace.commits.first?.message, "discussion #1: comment (from erin)")

        // The same comment through a fresh ledger is found in the files, not written twice.
        let fresh = LocalNodeSyncApplier(repos: store, author: { author }, prefs: InMemoryKeyValueStore([:]))
        defer { fresh.dispose() }
        let again = await fresh.call(event(.discussions, number: 1, id: 99, ["type": "comment", "id": "c1", "ts": 200]))
        XCTAssertTrue(again)
        XCTAssertNil(workspace.files[".forkmesh/discussions/1/0004-comment.md"])
    }

    func testACommentOnAnUnknownDiscussionWritesTheStandIn() async {
        let workspace = published("notes", "alice/notes")
        let applier = applierFor()
        defer { applier.dispose() }

        let ok = await applier.call(event(.discussions, number: 5, [
            "type": "comment", "id": "c1", "body": "Hi", "ts": 1,
        ], extra: ["titleIfNew": "Remote"]))
        XCTAssertTrue(ok)
        let root = workspace.files[".forkmesh/discussions/5/discussion.md"] ?? ""
        XCTAssertTrue(root.contains("\ncategory: Ideas\n"), root)
        XCTAssertTrue(root.contains("\nid: open-5\n"), root)
        XCTAssertTrue(root.contains("\ntitle: Remote\n"), root)
        XCTAssertNotNil(workspace.files[".forkmesh/discussions/5/0002-comment.md"])
    }

    func testAnUnsupportedDiscussionEventIsLeft() async {
        published("notes", "alice/notes")
        let applier = applierFor()
        defer { applier.dispose() }
        let ok = await applier.call(event(.discussions, number: 1, ["type": "lock", "id": "x"]))
        XCTAssertFalse(ok)
        XCTAssertEqual(applier.leftReasons.first?.reason, "unsupported discussion event \"lock\"")
    }

    // MARK: Pulls

    func testOnTheLedgerBranchAPullAndItsReviewsAreWritten() async throws {
        let workspace = published("notes", "alice/notes", branch: LocalNodeSyncApplier.pullLedgerBranch)
        let applier = applierFor()
        defer { applier.dispose() }

        let opened = await applier.call(NodeSyncItem(owner: "alice", repo: "notes", kind: .pulls, id: 21, data: [
            "pull": [
                "number": 9, "title": "Fix the crash", "base": "main", "head": "fix", "ts": 7,
                "author": "K3yK3yK3y", "sig": "S", "patch": "diff --git a b\n", "commits": "From x\n",
                "description": "Fixes #4", "mergeBase": "abc",
            ],
        ]))
        XCTAssertTrue(opened)
        let expectedPull = [
            "---", "schema: forkmesh-pull-v1", "number: 9", "title: Fix the crash", "base: main", "head: fix",
            "status: open", "mergeBase: abc", "ts: 7", "author: K3yK3yK3y", "authorName: ", "sig: S", "---", "",
            "Fixes #4", "",
        ].joined(separator: "\n")
        XCTAssertEqual(workspace.files[".forkmesh/pulls/9/pull.md"], expectedPull)
        XCTAssertEqual(workspace.files[".forkmesh/pulls/9/changes.patch"], "diff --git a b")
        XCTAssertEqual(workspace.files[".forkmesh/pulls/9/commits.mbox"], "From x")
        XCTAssertEqual(workspace.commits.first?.message, "pull #9: opened (from K3yK3yK3)")

        let review = await applier.call(event(.pulls, number: 9, id: 22, [
            "type": "review", "id": "r1", "state": "approve", "body": "LGTM", "authorName": "erin", "ts": 8,
        ]))
        XCTAssertTrue(review)
        XCTAssertEqual(workspace.files[".forkmesh/pulls/9/0002-review.md"],
                       "---\ntype: review\nid: r1\nauthor: \nauthorName: erin\nts: 8\nstate: approve\nsig: \n---\n\nLGTM\n")
        XCTAssertEqual(workspace.commits.first?.message, "pull #9: review (from erin)")

        let missing = await applier.call(event(.pulls, number: 3, id: 23, ["type": "comment", "id": "c1"]))
        XCTAssertFalse(missing)
        XCTAssertEqual(applier.leftReasons.first?.reason, "pull #3 is not in this repository")
    }

    func testABranchBackedPullCarriesNoPatch() async {
        let workspace = published("notes", "alice/notes", branch: LocalNodeSyncApplier.pullLedgerBranch)
        let applier = applierFor()
        defer { applier.dispose() }

        let ok = await applier.call(NodeSyncItem(owner: "alice", repo: "notes", kind: .pulls, id: 21, data: [
            "number": 2,
            "pull": ["title": "Branch", "derive": "branch", "patch": "diff", "authorName": "bob"],
        ]))
        XCTAssertTrue(ok)
        XCTAssertTrue(workspace.files[".forkmesh/pulls/2/pull.md"]?.contains("\nderive: branch\n") ?? false)
        XCTAssertNil(workspace.files[".forkmesh/pulls/2/changes.patch"])
    }

    // MARK: Git

    func testNothingStagedCountsAsDurablyHeld() async {
        let workspace = published("notes", "alice/notes")
        workspace.commitError = NodeSyncWorkspaceError.nothingStaged
        let applier = applierFor()
        defer { applier.dispose() }

        let ok = await applier.call(issueOpen())
        XCTAssertTrue(ok)
        XCTAssertEqual(applier.recent.first?.commitId, "")

        // An adapter that surfaces only the bare message is recognised too.
        struct GitWorkerError: Error, CustomStringConvertible {
            var description: String { "Nothing is staged. Stage changes first." }
        }
        workspace.commitError = GitWorkerError()
        let second = await applier.call(issueOpen(id: 12, title: "Another"))
        XCTAssertTrue(second)
    }

    func testACommitFailureLeavesTheItemWithAShortReason() async {
        let workspace = published("notes", "alice/notes")
        workspace.commitError = NodeSyncWorkspaceError.failed("disk full\nno space left on device")
        let applier = applierFor()
        defer { applier.dispose() }

        let ok = await applier.call(issueOpen())
        XCTAssertFalse(ok)
        XCTAssertEqual(applier.leftReasons.first?.reason, "commit failed: disk full no space left on device")
        XCTAssertEqual(applier.recent.first?.outcome, .left)
    }

    func testARefreshFailureDoesNotUndoACommittedApply() async {
        published("notes", "alice/notes")
        store.refreshError = NodeSyncWorkspaceError.failed("index busy")
        let applier = applierFor()
        defer { applier.dispose() }

        let ok = await applier.call(issueOpen())
        XCTAssertTrue(ok)
        XCTAssertEqual(applier.appliedTotal, 1)
    }

    func testTheLedgerIsPersistedAndCapped() async {
        let workspace = published("notes", "alice/notes")
        let applier = applierFor()
        defer { applier.dispose() }
        let ok = await applier.call(issueOpen())
        XCTAssertTrue(ok)
        XCTAssertEqual(prefs.stringList(forKey: LocalNodeSyncApplier.appliedPrefsKey), ["alice/notes|issues|row:11"])

        // A new applier (app restart) reads the ledger back.
        let restarted = applierFor()
        defer { restarted.dispose() }
        let again = await restarted.call(issueOpen())
        XCTAssertTrue(again)
        XCTAssertEqual(workspace.commits.count, 1)

        let full = InMemoryKeyValueStore(["nodeSync/applied": JSONValue((0..<500).map { "x|issues|\($0)" })])
        let capped = LocalNodeSyncApplier(repos: store, author: { author }, prefs: full)
        defer { capped.dispose() }
        let applied = await capped.call(issueOpen(id: 77, title: "Capped"))
        XCTAssertTrue(applied)
        let stored = full.stringList(forKey: LocalNodeSyncApplier.appliedPrefsKey) ?? []
        XCTAssertEqual(stored.count, LocalNodeSyncApplier.appliedLedgerLimit)
        XCTAssertEqual(stored.first, "x|issues|1")
        XCTAssertEqual(stored.last, "alice/notes|issues|row:77")
    }

    func testFingerprintsPreferTheEventIdThenTheSignatureThenTheRow() async {
        let applier = applierFor()
        defer { applier.dispose() }
        XCTAssertEqual(applier.fingerprintOf(event(.issues, number: 1, id: 5, ["id": "e1", "sig": "s"])), "alice/notes|issues|e1")
        XCTAssertEqual(applier.fingerprintOf(event(.issues, number: 1, id: 5, ["sig": "s"])), "alice/notes|issues|s")
        XCTAssertEqual(applier.fingerprintOf(NodeSyncItem(owner: "a", repo: "b", kind: .pulls, id: 0, data: ["pull": ["id": "p"]])), "a/b|pulls|p")
        XCTAssertEqual(applier.fingerprintOf(event(.issues, number: 1, id: 5, [:])), "alice/notes|issues|row:5")
        XCTAssertEqual(applier.fingerprintOf(NodeSyncItem(owner: "a", repo: "b", kind: .issues, id: 0, data: [:])), "")
    }

    func testAppliesRunOneAtATime() async {
        let workspace = published("notes", "alice/notes")
        let applier = applierFor()
        defer { applier.dispose() }

        async let first = applier.call(issueOpen(id: 1, title: "One"))
        async let second = applier.call(issueOpen(id: 2, title: "Two"))
        let results = await [first, second]
        XCTAssertEqual(results, [true, true])
        // Interleaved applies would both have claimed slot 1.
        XCTAssertNotNil(workspace.files[".forkmesh/issues/open/1/issue-1.json"])
        XCTAssertNotNil(workspace.files[".forkmesh/issues/open/2/issue-2.json"])
        XCTAssertEqual(workspace.commits.count, 2)
    }

    func testTheRecentLedgerKeepsTheNewestTwenty() async {
        let applier = applierFor()
        defer { applier.dispose() }
        for id in 1...25 {
            _ = await applier.call(issueOpen(id: id, owner: "zed", repo: "r\(id)"))
        }
        XCTAssertEqual(applier.recent.count, LocalNodeSyncApplier.recentLimit)
        XCTAssertEqual(applier.recent.first?.repoFullName, "zed/r25")
        XCTAssertEqual(applier.leftTotal, 25)
    }

    func testADisposedApplierAppliesNothing() async {
        published("notes", "alice/notes")
        let applier = applierFor()
        applier.dispose()
        let ok = await applier.call(issueOpen())
        XCTAssertFalse(ok)
    }

    // MARK: Wired to the service

    func testInstallOnAcksExactlyWhatWasCommitted() async throws {
        let workspace = published("notes", "alice/notes")
        let applier = applierFor()
        defer { applier.dispose() }
        let body: JSONValue = [
            "repos": [
                [
                    "owner": "alice", "name": "notes",
                    "issues": [
                        ["id": 11, "number": 0, "event": ["type": "open", "id": "e1", "title": "Hi", "authorName": "bob", "ts": 1]],
                        ["id": 12, "number": 3, "event": ["type": "open", "title": "Files", "ts": 1], "attachmentData": [["name": "a"]]],
                    ],
                ],
            ],
        ]
        let h = try NodeSyncHarness(handler: { request in
            request.method == "DELETE" ? HTTPResponse(status: 200, json: ["ok": true]) : HTTPResponse(status: 200, json: body)
        })
        defer { h.node.dispose() }
        applier.installOn(h.node)

        await h.node.syncNow()

        let deletes = h.requests.filter { $0.method == "DELETE" }
        XCTAssertEqual(deletes.count, 1)
        XCTAssertEqual(nodeSyncQuery(deletes.first?.url)["ids"], "11")
        XCTAssertEqual(h.node.ackedTotal, 1)
        XCTAssertEqual(workspace.commits.count, 1)
        XCTAssertEqual(applier.leftTotal, 1)
    }
}
