package com.forkmesh.nativeapp.collab

import com.forkmesh.nativeapp.model.Issue
import com.forkmesh.nativeapp.model.IssueEvent
import com.forkmesh.nativeapp.model.IssueMention
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class IssueThreadTest {
    fun testTimelineOrdersByTimeAndKeepsSignedEventsAheadOfMentionsOnTies() {
        val events = listOf(
            IssueEvent(type = "comment", id = "late", ts = 30),
            IssueEvent(type = "status", id = "tie", status = "closed", ts = 10),
            IssueEvent(type = "comment", id = "first", ts = 5),
        )
        val mentions = listOf(
            IssueMention(sourceNumber = 9, sourceTitle = "Other", sourceStatus = "open", authorName = "bob", ts = 10),
            IssueMention(sourceNumber = 8, sourceTitle = "Early", sourceStatus = "closed", authorName = "", ts = 1),
        )
        val order = IssueTimeline.entries(events, mentions).map {
            when (it) {
                is IssueTimelineEntry.Event -> it.event.id
                is IssueTimelineEntry.Mention -> "#${it.mention.sourceNumber}"
            }
        }
        assertEq(listOf("#8", "first", "tie", "#9", "late"), order)
    }

    fun testTimelineTitlesAndIcons() {
        assertEq("Status changed to closed", IssueTimeline.title(IssueEvent(type = "status", status = "closed")))
        assertEq("Status changed to updated", IssueTimeline.title(IssueEvent(type = "status")))
        assertEq("Vote added", IssueTimeline.title(IssueEvent(type = "vote")))
        assertEq("Comment", IssueTimeline.title(IssueEvent(type = "comment")))
        assertEq("Comment", IssueTimeline.title(IssueEvent(type = "open")))
        assertEq("sync_alt", IssueTimeline.icon(IssueEvent(type = "status")))
        assertEq("chat_bubble_outline", IssueTimeline.icon(IssueEvent(type = "vote")))
        assertEq(
            "Mentioned in #3 Relay (closed)",
            IssueTimeline.mentionTitle(IssueMention(3, "Relay", "closed", "ann", 1)),
        )
        assertEq("Mentioned in #3 Relay", IssueTimeline.mentionTitle(IssueMention(3, "Relay", "open", "ann", 1)))
    }

    fun testParentsNeverIncludeTheIssueItselfAndTheBodyDropsRelationLines() {
        val issue = Issue(number = 5, title = "t", body = "Parent issue: #5, #2\n\nDetails")
        assertEq(listOf(2), IssueDetail.parents(issue))
        assertEq("Details", IssueDetail.body(issue))
        assertEq("No description provided.", IssueDetail.body(Issue(number = 1, title = "t", body = "Depends on: #4")))
    }

    fun testParentLinkAndStatusText() {
        assertEq("Not synced", issueStatusText(null))
        assertEq("#4 · Not synced", issueParentLinkLabel(4, null))
        assertEq("#4 Parent · Open", issueParentLinkLabel(4, Issue(number = 4, title = "Parent")))
        assertEq("#4 · Closed", issueParentLinkLabel(4, Issue(number = 4, title = "", status = "closed")))
    }

    fun testSubIssuesTitleDetailsAndProgress() {
        val children = mapOf(
            21 to Issue(number = 21, title = "Child", status = "closed", progress = 40, priority = 2, assignees = listOf("mona", "kai")),
            22 to Issue(number = 22, title = "Other", progress = 25),
        )
        val find = { number: Int -> children[number] }
        assertEq("Sub-issues", SubIssues.title(emptyList(), find))
        assertEq("Sub-issues · 1 of 3 closed", SubIssues.title(listOf(21, 22, 24), find))
        assertEq("Closed · 40% · P2 · mona, kai", SubIssues.details(children[21]))
        assertEq("Open · 25% · Unassigned", SubIssues.details(children[22]))
        assertEq("Not synced", SubIssues.details(null))
        assertEq("#24", SubIssues.rowTitle(24, null))
        assertEq("#21 Child", SubIssues.rowTitle(21, children[21]))
        assertEq(1f, SubIssues.progress(children[21]!!))
        assertEq(.25f, SubIssues.progress(children[22]!!))
    }

    fun testIssueCardAndActionsSheet() {
        val open = Issue(number = 7, title = "Crash", author = "alice")
        assertEq("#7 · alice", IssueCard.subtitle(open))
        assertEq("#7", IssueCard.subtitle(open.copy(author = "")))
        assertEq("error_outline", IssueCard.icon(open))
        assertEq("check_circle_outline", IssueCard.icon(open.copy(status = "closed")))
        assertEq("#7 · Crash", IssueActions.header(open))
        assertEq("Comment on #7", IssueActions.commentTitle(open))
        assertEq("Close issue", IssueActions.statusLabel(open))
        assertEq("closed", IssueActions.nextStatus(open))
        assertEq("Reopen issue", IssueActions.statusLabel(open.copy(status = "closed")))
        assertEq("open", IssueActions.nextStatus(open.copy(status = "closed")))
        assertEq("refresh", IssueActions.statusIcon(open.copy(status = "closed")))
    }

    fun testFormatUsdMatchesToStringAsFixed() {
        assertEq("$150", formatUsd(150.0))
        assertEq("$12.50", formatUsd(12.5))
        assertEq("$1.00", formatUsd(1.005))
        assertEq("$0.13", formatUsd(0.125))
        expect(formatUsd(0.1).startsWith("$0.10"))
    }
}
