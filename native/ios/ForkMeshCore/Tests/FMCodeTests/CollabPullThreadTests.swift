// The pull request rules: the Pulls row, ship controls, desktop request checks, review
// composers, timeline, parsePatchFiles and the diff viewer's line kinds. Ports the pull
// assertions of native/android RepoCollaborationTest.kt and PatchFilesTest.kt.

import FMApi
import FMBase
import XCTest
@testable import FMCode

let collabFixturePull = PublishedPull(
    number: 7, title: "Add signed review flow", status: "open", body: "Adds mobile review controls.", base: "main",
    head: "review/mobile", patch: "diff --git a/lib/a.dart b/lib/a.dart\n+review", signed: true,
    events: [
        PullEvent(type: "comment", body: "Review changes before merge.", authorName: "Kai"),
        PullEvent(type: "review", body: "LGTM from mobile.", authorName: "Mona", state: "approve"),
        PullEvent(type: "review", body: "Please add a regression test.", authorName: "Lin", state: "changes_requested"),
    ])

final class CollabPullThreadTests: XCTestCase {
    private let pull = collabFixturePull

    func testPullDetailQueuesSignedDesktopMergeRequests() throws {
        XCTAssertEqual(
            try DesktopCommand.ownerAccount(
                CollabAccount(nodeName: "Owner ", pubkey: "key"), identityPublicKey: "key"),
            "owner")
        XCTAssertTrue(PullDetail.canRequestMerge(pull))
        XCTAssertTrue(PullDetail.shipControlsCopy(pull).contains("desktop node will verify mergeability"))

        let commands = PullShipCommands.all(pull)
        XCTAssertEqual(
            commands.map(\.command),
            ["pull.merge", "pull.merge_delete", "pull.fix_agent", "pull.close", "pull.reopen", "pull.delete"])
        XCTAssertEqual(commands.map(\.label), [
            "Request merge on desktop",
            "Request merge + delete branch",
            "Request fix with agent",
            "Request close on desktop",
            "Request reopen on desktop",
            "Request delete on desktop",
        ])
        XCTAssertEqual(commands.map(\.needsOpen), [true, true, false, false, false, false])
        XCTAssertEqual(commands.map(\.filled), [true, false, false, false, false, false])
        XCTAssertEqual(PullDetail.commandTarget(pull), "pull:7")
        XCTAssertEqual(PullDetail.commandPayload(pull), ["pullNumber": 7, "base": "main", "head": "review/mobile"])
        XCTAssertEqual(JSON.encode(.object(PullDetail.commandPayload(pull))), #"{"pullNumber":7,"base":"main","head":"review/mobile"}"#)
        var bare = pull
        bare.base = ""
        bare.head = ""
        XCTAssertEqual(PullDetail.commandPayload(bare), ["pullNumber": 7])
        XCTAssertEqual(
            DesktopCommand.canonical(owner: "owner", name: "repo", command: "pull.merge", target: "pull:7", ts: "1700000000000"),
            "forkmesh-desktop-command-v1\nowner\nrepo\npull.merge\npull:7\n1700000000000")
        XCTAssertEqual(
            commands.first?.successMessage,
            "Merge queued. Approve it in the ForkMesh desktop app to merge pull request #7.")
    }

    func testOnlyTheDesktopRequestsThatFitThePullsStateAreOffered() {
        XCTAssertEqual(
            PullShipCommands.offered(pull).map(\.command),
            ["pull.merge", "pull.merge_delete", "pull.fix_agent", "pull.close", "pull.delete"])
        XCTAssertEqual(
            PullShipCommands.offered(pull).map(\.chipLabel),
            ["Merge on desktop", "Merge + delete branch", "Fix with agent", "Close", "Delete"])
        var patchOnly = pull
        patchOnly.head = ""
        XCTAssertEqual(
            PullShipCommands.offered(patchOnly).map(\.command), ["pull.merge", "pull.fix_agent", "pull.close", "pull.delete"])
        var closed = pull
        closed.status = "closed"
        XCTAssertEqual(PullShipCommands.offered(closed).map(\.command), ["pull.reopen", "pull.delete"])
        var merged = pull
        merged.status = "merged"
        XCTAssertEqual(PullShipCommands.offered(merged).map(\.command), ["pull.delete"])
    }

    func testOnlyAnOpenBranchBackedPullOffersItsHeadForEditing() {
        XCTAssertFalse(PullDetail.canEditHead(pull))
        var branch = pull
        branch.branchBacked = true
        XCTAssertTrue(PullDetail.canEditHead(branch))
        branch.status = "merged"
        XCTAssertFalse(PullDetail.canEditHead(branch))
    }

    func testDesktopRequestsNeedTheOwnersPairedKey() {
        func message(_ account: CollabAccount, _ key: String?) -> String {
            do {
                _ = try DesktopCommand.ownerAccount(account, identityPublicKey: key)
                return "no error"
            } catch {
                return dartErrorText(error)
            }
        }
        XCTAssertEqual(
            message(.signedOut, "key"), "Exception: Sign in with the repo owner account to request desktop control.")
        XCTAssertEqual(
            message(CollabAccount(nodeName: "owner", pubkey: "key"), nil),
            "Exception: Sign in with the repo owner account to request desktop control.")
        XCTAssertEqual(message(CollabAccount(nodeName: "mona", pubkey: "key"), "key"), "no error")
        XCTAssertEqual(
            message(CollabAccount(nodeName: "owner", pubkey: "other"), "key"),
            "Exception: Pair this mobile device with the owner key before requesting desktop control.")
        var merged = pull
        merged.status = "merged"
        XCTAssertFalse(PullDetail.canRequestMerge(merged))
        XCTAssertEqual(
            PullDetail.shipControlsCopy(merged),
            "This pull request is merged; merge/review-changing requests may be rejected by the desktop node if the local state does not support them.")
    }

    func testThreadAlertsSignWithThePairedAccountNode() {
        XCTAssertEqual(
            ThreadSubscription.signedAccountNode(CollabAccount(nodeName: "Preview-Node", pubkey: "key"), identityPublicKey: "key"),
            "preview-node")
        XCTAssertEqual(ThreadSubscription.signedAccountNode(.signedOut, identityPublicKey: "key"), "")
        XCTAssertEqual(
            ThreadSubscription.signedAccountNode(CollabAccount(nodeName: "preview-node", pubkey: "key"), identityPublicKey: nil),
            "")
        XCTAssertEqual(
            ThreadSubscription.signedAccountNode(CollabAccount(nodeName: "  ", pubkey: "key"), identityPublicKey: "key"), "")
        XCTAssertEqual(
            ThreadSubscription.signedAccountNode(CollabAccount(nodeName: "preview-node", pubkey: "other"), identityPublicKey: "key"),
            "")

        XCTAssertEqual(ThreadSubscription.label(guest: false, canSign: true, subscribed: false), "Subscribe thread")
        XCTAssertEqual(ThreadSubscription.label(guest: false, canSign: true, subscribed: true), "Unsubscribe thread")
        XCTAssertEqual(ThreadSubscription.label(guest: false, canSign: false, subscribed: false), "Thread alerts unavailable")
        XCTAssertEqual(ThreadSubscription.label(guest: true, canSign: true, subscribed: true), "Sign in for thread alerts")
        XCTAssertEqual(ThreadSubscription.icon(guest: false, canSign: false, subscribed: false), "lock_outline")
        XCTAssertEqual(ThreadSubscription.icon(guest: false, canSign: true, subscribed: true), "notifications_off_outlined")
        XCTAssertEqual(ThreadSubscription.icon(guest: false, canSign: true, subscribed: false), "notifications_active_outlined")
        XCTAssertEqual(ThreadSubscription.toggled(true), "Subscribed to thread notifications.")
        XCTAssertEqual(ThreadSubscription.toggled(false), "Unsubscribed from thread notifications.")
    }

    func testReviewComposersTitleAndValidate() {
        XCTAssertEqual(PullReview.composerTitle("approve", 7), "Approve PR #7")
        XCTAssertTrue(PullReview.composerHelper("approve").hasPrefix("Approval is signed and sent to the pull inbox"))
        XCTAssertNil(PullReview.validationError("approve", ""))

        XCTAssertEqual(PullReview.composerTitle("request-changes", 7), "Request changes on PR #7")
        XCTAssertEqual(PullReview.validationError("request-changes", "  "), "A note is required to request changes.")
        XCTAssertNil(PullReview.validationError("request-changes", "Please cover the empty state before owner apply."))

        XCTAssertEqual(PullReview.composerTitle("comment", 7), "Review comment on PR #7")
        XCTAssertEqual(PullReview.subtitle(pull), "review/mobile → main · open")
        XCTAssertEqual(
            PullReview.subtitle(PullRequest(number: 3, title: "t", status: "open", baseBranch: "main", headBranch: "f")),
            "f → main · open")
        XCTAssertEqual(PullReview.header(7, "Add signed review flow"), "#7 · Add signed review flow")
        XCTAssertEqual(PullReview.commentTitle(7), "Comment on PR #7")
    }

    func testPullDetailRendersSignedCommentAndReviewTimeline() {
        XCTAssertEqual(pull.events.map(\.displayTitle), ["Comment", "Approved", "Requested changes"])
        XCTAssertEqual(pull.events.map(PullTimeline.tone), [.tertiary, .success, .danger])
        XCTAssertEqual(
            pull.events.map(PullTimeline.icon), ["rate_review_outlined", "check_circle_outline", "cancel_outlined"])
        XCTAssertEqual(PullCard.subtitle(pull), "#7 · review/mobile → main")
        XCTAssertEqual(PullCard.badge(pull), "signed")
        var merged = pull
        merged.signed = false
        merged.status = "merged"
        XCTAssertEqual(PullCard.badge(merged), "merged")
        XCTAssertEqual(PullCard.tone(merged), .accentEdge)
        var closed = pull
        closed.status = "closed"
        XCTAssertEqual(PullCard.tone(closed), .danger)
        XCTAssertEqual(PullCard.tone(pull), .success)
        var bare = pull
        bare.head = ""
        bare.base = ""
        XCTAssertEqual(PullCard.subtitle(bare), "#7")
        XCTAssertEqual(PullDetail.chips(pull).map(\.label), ["open", "signed", "review/mobile → main"])
        XCTAssertEqual(PullDetail.chips(bare).map(\.label), ["open", "signed"])
        XCTAssertEqual(PullDetail.diffTitle(pull), "Pull #7")
        XCTAssertEqual(PullDetail.title(7), "PR #7")
    }

    func testPullRowShowsAuthorAgeActivityAndSnippet() {
        var row = pull
        row.authorName = "jett"
        row.createdMs = 1_000_000_000_000
        row.body = "\n## Summary\nAdds mobile review controls.\n"
        row.patch = "diff --git a/a b/a\n--- a/a\n+++ b/a\n+one\n+two\n-three\ndiff --git a/b b/b\n+four"
        let now = Date(timeIntervalSince1970: (1_000_000_000_000 + 3 * 86_400_000) / 1000)
        XCTAssertEqual(PullCard.meta(row, now: now), "#7 · jett · 3d ago · 3 comments · +3 −1 in 2 files")
        XCTAssertEqual(PullCard.snippet(row), "Summary")
        XCTAssertEqual(PullCard.branches(row), "review/mobile → main")
        XCTAssertEqual(PullCard.review(row), "Requested changes")
        row.authorName = ""
        row.author = "w0F5vYijNonu8kkqLSa"
        XCTAssertEqual(PullCard.author(row), "w0F5vYij…")
        var bare = PublishedPull(number: 3, title: "t")
        XCTAssertEqual(PullCard.meta(bare), "#3")
        XCTAssertNil(PullCard.review(bare))
        bare.body = "```diff\n+x\n```"
        XCTAssertEqual(PullCard.snippet(bare), "+x")
    }

    func testPullStatusFilterCountsMergedAsClosed() {
        let statuses = ["open", "merged", "closed", "reopened"]
        let pulls = statuses.enumerated().map { PublishedPull(number: $0.offset, title: "t", status: $0.element) }
        XCTAssertEqual(pulls.filter { PullStatusFilter.open.matches($0) }.map(\.status), ["open", "reopened"])
        XCTAssertEqual(pulls.filter { PullStatusFilter.closed.matches($0) }.map(\.status), ["merged", "closed"])
        XCTAssertEqual(pulls.filter { PullStatusFilter.all.matches($0) }.count, 4)
        XCTAssertEqual(PullStatusFilter.allCases.map(\.label), ["Open", "Closed", "All"])
    }

    func testRowBadgesTakeTheirToneFromTheirWords() {
        XCTAssertEqual(collabBadgeTone("open"), .success)
        XCTAssertEqual(collabBadgeTone("signed"), .success)
        XCTAssertEqual(collabBadgeTone("closed"), .danger)
        XCTAssertEqual(collabBadgeTone("merged"), .accent)
        XCTAssertEqual(collabBadgeTone("ideas"), .secondary)
    }

    func testFailureNoticesReadLikeDart() {
        XCTAssertEqual(CollabCopy.runFailure(ApiException("bad_signature")), "Failed: Exception: bad_signature")
        XCTAssertEqual(
            CollabCopy.runFailure(ApiArgumentError(value: "\"\"", name: "node", message: "expected account node name")),
            "Failed: Invalid argument (node): expected account node name: \"\"")
        XCTAssertEqual(CollabCopy.runFailure(ApiUnsupportedError("retired")), "Failed: Unsupported operation: retired")
    }

    // MARK: Patch files (PatchFilesTest.kt)

    func testParsesFilesKindsAndCounts() {
        let diff = [
            "diff --git a/lib/a.dart b/lib/a.dart",
            "index 1..2 100644",
            "--- a/lib/a.dart",
            "+++ b/lib/a.dart",
            "@@ -1 +1,2 @@",
            "-old",
            "+new",
            "+more",
            "diff --git a/new.txt b/new.txt",
            "new file mode 100644",
            "--- /dev/null",
            "+++ b/new.txt",
            "+hello",
            "diff --git a/gone.txt b/gone.txt",
            "deleted file mode 100644",
            "--- a/gone.txt",
            "+++ /dev/null",
            "-bye",
            "diff --git a/old/name.kt b/new/name.kt",
            "similarity index 90%",
            "rename from old/name.kt",
            "rename to new/name.kt",
        ].joined(separator: "\r\n") + "\n"
        XCTAssertEqual(parsePatchFiles(diff), [
            PatchFile("lib/a.dart", .modified, adds: 2, dels: 1),
            PatchFile("new.txt", .added, adds: 1, dels: 0),
            PatchFile("gone.txt", .deleted, adds: 0, dels: 1),
            PatchFile("new/name.kt", .renamed, adds: 0, dels: 0),
        ])
    }

    func testTheCollaborationFixtureAndEmptyDiffs() {
        XCTAssertEqual(parsePatchFiles(pull.patch), [PatchFile("lib/a.dart", .modified, adds: 1, dels: 0)])
        XCTAssertEqual(parsePatchFiles("  \n"), [])
        XCTAssertEqual(parsePatchFiles("+orphan line"), [])
        let totals = patchFileTotals([PatchFile("a", .added, adds: 2, dels: 1), PatchFile("b", .modified, adds: 3)])
        XCTAssertEqual(totals.adds, 5)
        XCTAssertEqual(totals.dels, 1)
        XCTAssertEqual(PatchFile("lib/src/a.dart", .modified).directory, "lib/src/")
        XCTAssertEqual(PatchFile("lib/src/a.dart", .modified).name, "a.dart")
        XCTAssertEqual(PatchFile("README", .modified).directory, "")
        XCTAssertEqual(PatchFile("README", .modified).name, "README")
    }

    func testLineSplittingAndTitles() {
        XCTAssertEqual(dartLines("a\r\nb\n\rc"), ["a", "b", "", "c"])
        XCTAssertEqual(dartLines("a\n\n"), ["a", ""])
        XCTAssertEqual(dartLines(""), [])
        XCTAssertEqual(changedFilesTitle(1), "1 changed file")
        XCTAssertEqual(changedFilesTitle(3), "3 changed files")
        XCTAssertEqual(PatchPreviewLineKind(line: "+x"), .added)
        XCTAssertEqual(PatchPreviewLineKind(line: "+++ b/x"), .context)
        XCTAssertEqual(PatchPreviewLineKind(line: "-x"), .removed)
        XCTAssertEqual(PatchPreviewLineKind(line: "--- a/x"), .context)
        XCTAssertEqual(PatchPreviewLineKind(line: "@@ -1 +1 @@"), .hunk)
        XCTAssertEqual(PatchPreviewLineKind(line: " same"), .context)
    }

    func testNewPullNeedsATitleAndAPatch() {
        XCTAssertTrue(NewPullComposer.ready(title: "Fix", patch: "diff"))
        XCTAssertFalse(NewPullComposer.ready(title: " ", patch: "diff"))
        XCTAssertFalse(NewPullComposer.ready(title: "Fix", patch: "\n"))
    }
}
