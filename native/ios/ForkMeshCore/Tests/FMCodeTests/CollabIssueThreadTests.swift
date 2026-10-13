// The issue detail rules (timeline, parents, sub-issues, actions sheet, bounty card); ports
// native/android IssueThreadTest.kt and the issue assertions of RepoCollaborationTest.kt.

import FMApi
import XCTest
@testable import FMCode

let collabFixtureIssue = Issue(
    number: 12, title: "Stabilize signed inbox", status: "open",
    body: "Mobile should make signed collaboration clear.", author: "alice", labels: ["mobile", "signed"],
    milestone: "v2 mobile", priority: 3, assignees: ["mona", "kai"],
    events: [IssueEvent(type: "comment", body: "I can reproduce this on mobile.", authorName: "Mona")], votes: 2,
    bountyUsd: 150, bountyAddress: "escrow-fixture", bountyStatus: "funded", bountyRequiredLamports: 1_500_000_000,
    bountyReceivedLamports: 750_000_000, bountyAmountSol: 1.5, bountyPayUri: "solana:escrow-fixture?amount=1.5",
    bountyPayee: "maintainer-node", bountyPayoutSig: "payout-fixture")

final class CollabIssueThreadTests: XCTestCase {
    func testTimelineOrdersByTimeAndKeepsSignedEventsAheadOfMentionsOnTies() {
        let events = [
            IssueEvent(type: "comment", id: "late", ts: 30),
            IssueEvent(type: "status", id: "tie", status: "closed", ts: 10),
            IssueEvent(type: "comment", id: "first", ts: 5),
        ]
        let mentions = [
            IssueMention(sourceNumber: 9, sourceTitle: "Other", sourceStatus: "open", authorName: "bob", ts: 10),
            IssueMention(sourceNumber: 8, sourceTitle: "Early", sourceStatus: "closed", authorName: "", ts: 1),
        ]
        let order = IssueTimeline.entries(events, mentions).map { entry -> String in
            switch entry {
            case .event(let event): return event.id
            case .mention(let mention): return "#\(mention.sourceNumber)"
            }
        }
        XCTAssertEqual(order, ["#8", "first", "tie", "#9", "late"])
    }

    func testTimelineTitlesAndIcons() {
        XCTAssertEqual(IssueTimeline.title(IssueEvent(type: "status", status: "closed")), "Status changed to closed")
        XCTAssertEqual(IssueTimeline.title(IssueEvent(type: "status")), "Status changed to updated")
        XCTAssertEqual(IssueTimeline.title(IssueEvent(type: "vote")), "Vote added")
        XCTAssertEqual(IssueTimeline.title(IssueEvent(type: "comment")), "Comment")
        XCTAssertEqual(IssueTimeline.title(IssueEvent(type: "open")), "Comment")
        XCTAssertEqual(IssueTimeline.icon(IssueEvent(type: "status")), "sync_alt")
        XCTAssertEqual(IssueTimeline.icon(IssueEvent(type: "vote")), "chat_bubble_outline")
        XCTAssertEqual(IssueTimeline.tone(IssueEvent(type: "status")), .accent)
        XCTAssertEqual(IssueTimeline.tone(IssueEvent(type: "comment")), .tertiary)
        XCTAssertEqual(
            IssueTimeline.mentionTitle(
                IssueMention(sourceNumber: 3, sourceTitle: "Relay", sourceStatus: "closed", authorName: "ann", ts: 1)),
            "Mentioned in #3 Relay (closed)")
        XCTAssertEqual(
            IssueTimeline.mentionTitle(
                IssueMention(sourceNumber: 3, sourceTitle: "Relay", sourceStatus: "open", authorName: "ann", ts: 1)),
            "Mentioned in #3 Relay")
        XCTAssertEqual(IssueTimeline.empty, "No timeline events have been published yet.")
    }

    func testParentsNeverIncludeTheIssueItselfAndTheBodyDropsRelationLines() {
        let issue = Issue(number: 5, title: "t", body: "Parent issue: #5, #2\n\nDetails")
        XCTAssertEqual(IssueDetail.parents(issue), [2])
        XCTAssertEqual(IssueDetail.body(issue), "Details")
        XCTAssertEqual(IssueDetail.body(Issue(number: 1, title: "t", body: "Depends on: #4")), "No description provided.")
    }

    func testParentLinkAndStatusText() {
        XCTAssertEqual(issueStatusText(nil), "Not synced")
        XCTAssertEqual(issueParentLinkLabel(4, nil), "#4 · Not synced")
        XCTAssertEqual(issueParentLinkLabel(4, Issue(number: 4, title: "Parent")), "#4 Parent · Open")
        XCTAssertEqual(issueParentLinkLabel(4, Issue(number: 4, title: "", status: "closed")), "#4 · Closed")
    }

    func testSubIssuesTitleDetailsAndProgress() {
        let children: [Int: Issue] = [
            21: Issue(number: 21, title: "Child", status: "closed", priority: 2, progress: 40, assignees: ["mona", "kai"]),
            22: Issue(number: 22, title: "Other", progress: 25),
        ]
        let find: (Int) -> Issue? = { children[$0] }
        XCTAssertEqual(SubIssues.title([], find: find), "Sub-issues")
        XCTAssertEqual(SubIssues.title([21, 22, 24], find: find), "Sub-issues · 1 of 3 closed")
        XCTAssertEqual(SubIssues.details(children[21]), "Closed · 40% · P2 · mona, kai")
        XCTAssertEqual(SubIssues.details(children[22]), "Open · 25% · Unassigned")
        XCTAssertEqual(SubIssues.details(nil), "Not synced")
        XCTAssertEqual(SubIssues.rowTitle(24, nil), "#24")
        XCTAssertEqual(SubIssues.rowTitle(21, children[21]), "#21 Child")
        XCTAssertEqual(SubIssues.progress(children[21]!), 1)
        XCTAssertEqual(SubIssues.progress(children[22]!), 0.25)
    }

    func testIssueCardAndActionsSheet() {
        var open = Issue(number: 7, title: "Crash", author: "alice")
        XCTAssertEqual(IssueCard.subtitle(open), "#7 · alice")
        XCTAssertEqual(IssueCard.icon(open), "error_outline")
        XCTAssertEqual(IssueCard.tone(open), .success)
        XCTAssertEqual(IssueActions.header(open), "#7 · Crash")
        XCTAssertEqual(IssueActions.commentTitle(open), "Comment on #7")
        XCTAssertEqual(IssueActions.statusLabel(open), "Close issue")
        XCTAssertEqual(IssueActions.nextStatus(open), "closed")
        XCTAssertEqual(IssueActions.statusIcon(open), "check_circle_outline")
        var closed = open
        closed.status = "closed"
        XCTAssertEqual(IssueCard.icon(closed), "check_circle_outline")
        XCTAssertEqual(IssueCard.tone(closed), .accentEdge)
        XCTAssertEqual(IssueActions.statusLabel(closed), "Reopen issue")
        XCTAssertEqual(IssueActions.nextStatus(closed), "open")
        XCTAssertEqual(IssueActions.statusIcon(closed), "refresh")
        open.author = ""
        XCTAssertEqual(IssueCard.subtitle(open), "#7")
    }

    func testFormatUsdMatchesToStringAsFixed() {
        XCTAssertEqual(formatUsd(150), "$150")
        XCTAssertEqual(formatUsd(12.5), "$12.50")
        XCTAssertEqual(formatUsd(1.005), "$1.00")
        XCTAssertEqual(formatUsd(0.125), "$0.13")
        XCTAssertTrue(formatUsd(0.1).hasPrefix("$0.10"))
    }

    func testIssueDetailSurfacesSignedCollaborationState() {
        let labels = IssueDetail.chips(collabFixtureIssue, repoFullName: "owner/repo").map(\.label)
        for expected in ["mobile", "signed", "2 votes", "$150 bounty", "priority 3", "v2 mobile", "mona", "kai"] {
            XCTAssertEqual(labels.filter { $0 == expected }.count, 1, expected)
        }
        XCTAssertEqual(labels.first, "open")
        XCTAssertEqual(labels.last, "owner/repo")
        var one = collabFixtureIssue
        one.votes = 1
        XCTAssertEqual(
            IssueDetail.chips(one, repoFullName: "o/r").first { $0.icon == "how_to_vote_outlined" }?.label, "1 vote")
        XCTAssertTrue(CollabCopy.pendingNote.contains("pending the repo owner applying"))
    }

    func testIssueDetailShowsMigrationOnlyBountyState() {
        XCTAssertEqual(IssueDetail.pledgeText(collabFixtureIssue), "$150 pledged")
        var unpledged = collabFixtureIssue
        unpledged.bountyUsd = 0
        XCTAssertNil(IssueDetail.pledgeText(unpledged))
        XCTAssertEqual(IssueDetail.statusLine(collabFixtureIssue), "Status: funded")
        XCTAssertEqual(collabFixtureIssue.bountyProgressLabel, "750000000 / 1500000000 lamports")
        XCTAssertTrue(IssueDetail.nonCustodialCopy.contains("live Worker creation, funding, signing, and payout"))
    }

    func testIssueBountyCardShowsCopyableTransactionLinks() {
        let rows = BountyFunding.rows(collabFixtureIssue)
        XCTAssertEqual(rows.map(\.value), [
            "escrow-fixture",
            "https://explorer.solana.com/address/escrow-fixture",
            "payout-fixture",
            "https://explorer.solana.com/tx/payout-fixture",
        ])
        XCTAssertEqual(rows.map(\.key), [
            "issue-bounty-copy-address",
            "issue-bounty-copy-address-explorer",
            "issue-bounty-copy-payout-sig",
            "issue-bounty-copy-payout-explorer",
        ])
        XCTAssertEqual(rows.first?.label, "Legacy address (do not fund)")
        // The pay URI is never offered.
        XCTAssertFalse(rows.contains { $0.value.hasPrefix("solana:") })
        XCTAssertEqual(BountyFunding.rows(Issue(number: 1, title: "t")), [])
    }

    func testBountyRefreshWithoutALegacyRecordKeepsTheIssue() {
        let (kept, message) = IssueDetail.bountyRefreshed(collabFixtureIssue, IssueBounty())
        XCTAssertEqual(kept, collabFixtureIssue)
        XCTAssertEqual(message, "No historical bounty migration record was found.")
        XCTAssertFalse(IssueDetail.bountyMessageIsError(message))

        let refreshed = IssueDetail.bountyRefreshed(
            Issue(number: 1, title: "t"), IssueBounty(address: "escrow-fixture", status: "funded", amountUsd: 150))
        XCTAssertEqual(refreshed.0.bountyAddress, "escrow-fixture")
        XCTAssertEqual(refreshed.1, "Historical migration status refreshed.")

        let failed = IssueDetail.bountyRefreshFailed(ApiException("HTTP 500"))
        XCTAssertEqual(failed, "Refresh failed: Exception: HTTP 500")
        XCTAssertTrue(IssueDetail.bountyMessageIsError(failed))
    }
}
