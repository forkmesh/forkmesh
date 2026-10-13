package com.forkmesh.nativeapp.collab

import com.forkmesh.nativeapp.model.Issue
import com.forkmesh.nativeapp.model.IssueBounty
import com.forkmesh.nativeapp.model.IssueEvent
import com.forkmesh.nativeapp.model.IssueMention
import com.forkmesh.nativeapp.model.issueBodyWithoutRelationLines
import com.forkmesh.nativeapp.model.issueRelationNumbers
import java.math.BigDecimal
import java.math.RoundingMode

data class CollabChip(val icon: String, val label: String)

fun formatUsd(value: Double): String {
    if (value == Math.rint(value)) return "$" + value.toLong()
    return "$" + BigDecimal(value).setScale(2, RoundingMode.HALF_UP).toPlainString()
}

object IssueCard {
    fun icon(issue: Issue): String = if (issue.isOpen) "error_outline" else "check_circle_outline"

    fun subtitle(issue: Issue): String = "#${issue.number}" + if (issue.author.isNotEmpty()) " · ${issue.author}" else ""
}

fun issueStatusText(issue: Issue?): String = when {
    issue == null -> "Not synced"
    issue.isOpen -> "Open"
    else -> "Closed"
}

object IssueDetail {
    const val NO_DESCRIPTION = "No description provided."
    const val FUNDING_GATE_COPY =
        "Custodial funding is disabled. Do not fund a legacy deposit address. New bounty transfers require an owner-controlled external wallet, program, or multisig."
    const val NON_CUSTODIAL_COPY =
        "Non-custodial boundary: live Worker creation, funding, signing, and payout of bounty wallets are disabled. Historical addresses and signatures are migration-only public status. Wallet keys must stay in an owner-controlled external wallet."
    const val BOUNTY_REFRESHED = "Historical migration status refreshed."
    const val BOUNTY_NOT_FOUND = "No historical bounty migration record was found."

    fun parents(issue: Issue): List<Int> = issueRelationNumbers(issue.body).filter { it != issue.number }

    fun body(issue: Issue): String = issueBodyWithoutRelationLines(issue.body).ifEmpty { NO_DESCRIPTION }

    fun chips(issue: Issue, repoFullName: String): List<CollabChip> = buildList {
        add(CollabChip("label_outline", issue.status))
        if (issue.author.isNotEmpty()) add(CollabChip("person_outline", issue.author))
        for (label in issue.labels) add(CollabChip("sell_outlined", label))
        if (issue.priority > 0) add(CollabChip("priority_high_outlined", "priority ${issue.priority}"))
        if (issue.milestone.isNotEmpty()) add(CollabChip("flag_outlined", issue.milestone))
        for (assignee in issue.assignees) add(CollabChip("assignment_ind_outlined", assignee))
        if (issue.votes > 0) add(CollabChip("how_to_vote_outlined", "${issue.votes} ${if (issue.votes == 1) "vote" else "votes"}"))
        if (issue.bountyUsd > 0) add(CollabChip("attach_money", "${formatUsd(issue.bountyUsd)} bounty"))
        add(CollabChip("folder_outlined", repoFullName))
    }

    fun bountyRefreshed(issue: Issue, bounty: IssueBounty): Pair<Issue, String> =
        if (bounty.hasFunding) issue.copyWithBounty(bounty) to BOUNTY_REFRESHED else issue to BOUNTY_NOT_FOUND

    fun bountyRefreshFailed(error: Throwable): String = "Refresh failed: ${dartErrorText(error)}"

    fun bountyMessageIsError(message: String): Boolean =
        message.startsWith("Refresh failed") || message.startsWith("Exception")

    fun pledgeText(issue: Issue): String? = if (issue.bountyUsd > 0) "${formatUsd(issue.bountyUsd)} pledged" else null
}

fun issueParentLinkLabel(number: Int, parent: Issue?): String {
    val title = if (parent == null || parent.title.isEmpty()) "" else " ${parent.title}"
    return "#$number$title · ${issueStatusText(parent)}"
}

object SubIssues {
    fun title(numbers: List<Int>, find: (Int) -> Issue?): String {
        if (numbers.isEmpty()) return "Sub-issues"
        val closed = numbers.count { find(it)?.isOpen == false }
        return "Sub-issues · $closed of ${numbers.size} closed"
    }

    fun rowTitle(number: Int, child: Issue?): String = "#$number ${child?.title ?: ""}".trim()

    fun details(child: Issue?): String = buildList {
        add(issueStatusText(child))
        if (child != null) add("${child.progress}%")
        if (child != null && child.priority > 0) add("P${child.priority}")
        if (child != null) add(if (child.assignees.isEmpty()) "Unassigned" else child.assignees.joinToString(", "))
    }.joinToString(" · ")

    fun progress(child: Issue): Float = if (child.isOpen) child.progress / 100f else 1f
}

sealed interface IssueTimelineEntry {
    val ts: Long

    data class Event(val event: IssueEvent) : IssueTimelineEntry {
        override val ts: Long get() = event.ts
    }

    data class Mention(val mention: IssueMention) : IssueTimelineEntry {
        override val ts: Long get() = mention.ts
    }
}

object IssueTimeline {
    const val EMPTY = "No timeline events have been published yet."

    fun entries(events: List<IssueEvent>, mentions: List<IssueMention>): List<IssueTimelineEntry> =
        (events.map { IssueTimelineEntry.Event(it) } + mentions.map { IssueTimelineEntry.Mention(it) }).sortedBy { it.ts }

    fun isStatus(event: IssueEvent): Boolean = event.type == "status"

    fun title(event: IssueEvent): String = when {
        isStatus(event) -> "Status changed to ${event.status.ifEmpty { "updated" }}"
        event.type == "vote" -> "Vote added"
        else -> "Comment"
    }

    fun icon(event: IssueEvent): String = if (isStatus(event)) "sync_alt" else "chat_bubble_outline"

    fun mentionTitle(mention: IssueMention): String =
        "Mentioned in #${mention.sourceNumber} ${mention.sourceTitle}" + if (mention.sourceStatus == "closed") " (closed)" else ""
}

object IssueActions {
    const val VOTE_SUBMITTED = "Vote submitted to the inbox."

    fun header(issue: Issue): String = "#${issue.number} · ${issue.title}"

    fun commentTitle(issue: Issue): String = "Comment on #${issue.number}"

    fun statusLabel(issue: Issue): String = if (issue.isOpen) "Close issue" else "Reopen issue"

    fun statusIcon(issue: Issue): String = if (issue.isOpen) "check_circle_outline" else "refresh"

    fun nextStatus(issue: Issue): String = if (issue.isOpen) "closed" else "open"
}
