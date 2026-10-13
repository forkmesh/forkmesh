package com.forkmesh.nativeapp.inbox

import com.forkmesh.nativeapp.model.NotificationDeepLink
import com.forkmesh.nativeapp.navigation.ComposeTarget
import com.forkmesh.nativeapp.navigation.InboxFilter
import com.forkmesh.nativeapp.navigation.PullTab
import com.forkmesh.nativeapp.navigation.RepoRef
import com.forkmesh.nativeapp.navigation.Route

enum class NotificationActionId(val id: String, val title: String) {
    Open("fm.open", "Open"),
    MarkRead("fm.markRead", "Mark read"),
    Reply("fm.reply", "Reply"),
    Approve("fm.approve", "Approve"),
    Deny("fm.deny", "Deny"),
    Merge("fm.merge", "Merge & clean up"),
    ContinueRun("fm.continue", "Continue"),
    Retry("fm.retry", "Retry"),
    SendToAgent("fm.sendToAgent", "Send to agent"),
    ApprovePull("fm.approvePull", "Approve PR"),
    OpenLog("fm.openLog", "Open log");

    val requiresAuthentication: Boolean get() = this == Approve || this == Merge || this == ApprovePull
    val isDestructive: Boolean get() = this == Merge || this == Deny
    val opensApp: Boolean get() = this == Open || this == OpenLog || this == SendToAgent || requiresAuthentication
    val isTextInput: Boolean get() = this == Reply || this == ContinueRun

    companion object {
        fun of(id: String?): NotificationActionId? = entries.firstOrNull { it.id == id }
    }
}

enum class NotificationCategoryId(val id: String) {
    AgentApproval("agent-approval"),
    AgentFinished("agent-finished"),
    AgentFailed("agent-failed"),
    PullReview("pr-review"),
    Mention("mention"),
    Dm("dm"),
    CiFailed("ci-failed"),
    Ping("ping");

    val actions: List<NotificationActionId>
        get() = when (this) {
            AgentApproval -> listOf(NotificationActionId.Approve, NotificationActionId.Deny, NotificationActionId.Open)
            AgentFinished -> listOf(NotificationActionId.Open, NotificationActionId.ContinueRun, NotificationActionId.Merge)
            AgentFailed -> listOf(NotificationActionId.Open, NotificationActionId.Retry, NotificationActionId.SendToAgent)
            PullReview -> listOf(NotificationActionId.Open, NotificationActionId.ApprovePull)
            Mention, Dm -> listOf(NotificationActionId.Reply, NotificationActionId.MarkRead)
            CiFailed -> listOf(NotificationActionId.OpenLog, NotificationActionId.SendToAgent)
            Ping -> listOf(NotificationActionId.Open, NotificationActionId.MarkRead)
        }

    val isTimeSensitive: Boolean get() = this == AgentApproval

    companion object {
        fun of(id: String?): NotificationCategoryId? = entries.firstOrNull { it.id == id }

        fun forPing(kind: String, topic: String = ""): NotificationCategoryId {
            when (topic.lowercase()) {
                "direct-messages", "dm" -> return Dm
                "chat" -> return Mention
            }
            return when (kind.lowercase()) {
                "agent_approval", "agent-approval" -> AgentApproval
                "agent_finished", "agent-finished" -> AgentFinished
                "agent_failed", "agent-failed" -> AgentFailed
                "pull_submitted", "review_requested", "pr-review" -> PullReview
                "mention" -> Mention
                "ci_failed", "ci-failed", "action_failed" -> CiFailed
                else -> Ping
            }
        }
    }
}

data class NotificationPayload(
    val category: NotificationCategoryId,
    val alertId: String = "",
    val href: String = "",
    val topic: String = "",
    val sessionId: String = "",
    val approvalId: String = "",
    val repo: String = "",
    val number: Int = 0,
    val channel: String = "",
    val runId: String = "",
) {
    companion object {
        val keys = listOf("kind", "topic", "alertId", "href", "sessionId", "approvalId", "repo", "number", "channel", "runId")

        fun from(category: String?, fields: Map<String, String?>): NotificationPayload {
            fun value(key: String) = fields[key]?.trim() ?: ""
            return NotificationPayload(
                category = NotificationCategoryId.of(category) ?: NotificationCategoryId.forPing(value("kind"), value("topic")),
                alertId = value("alertId"),
                href = value("href"),
                topic = value("topic"),
                sessionId = value("sessionId"),
                approvalId = value("approvalId"),
                repo = value("repo"),
                number = value("number").toIntOrNull() ?: 0,
                channel = value("channel"),
                runId = value("runId"),
            )
        }
    }

    fun fields(kind: String = ""): Map<String, String> = linkedMapOf(
        "category" to category.id, "kind" to kind, "topic" to topic, "alertId" to alertId, "href" to href,
        "sessionId" to sessionId, "approvalId" to approvalId, "repo" to repo, "number" to number.toString(),
        "channel" to channel, "runId" to runId,
    ).filterValues { it.isNotEmpty() && it != "0" }
}

sealed class NotificationCommand {
    data class Open(val route: Route?, val href: String) : NotificationCommand()
    data class MarkRead(val alertId: String) : NotificationCommand()
    data class Reply(val channel: String, val text: String) : NotificationCommand()
    data class Approve(val sessionId: String, val approvalId: String) : NotificationCommand()
    data class Deny(val sessionId: String, val approvalId: String) : NotificationCommand()
    data class Merge(val sessionId: String) : NotificationCommand()
    data class ContinueRun(val sessionId: String, val text: String) : NotificationCommand()
    data class Retry(val sessionId: String) : NotificationCommand()
    data class Compose(val target: ComposeTarget) : NotificationCommand()
    data class ApprovePull(val repo: RepoRef, val number: Int) : NotificationCommand()
    data class Ignore(val reason: String) : NotificationCommand()
}

object NotificationActionRouter {
    const val MAX_TEXT = 4000

    fun route(payload: NotificationPayload): Route? {
        when (payload.category) {
            NotificationCategoryId.AgentApproval ->
                if (payload.sessionId.isNotEmpty() && payload.approvalId.isNotEmpty()) {
                    return Route.Approval("${payload.sessionId}/${payload.approvalId}")
                } else if (payload.sessionId.isNotEmpty()) {
                    return Route.Agent(payload.sessionId)
                }
            NotificationCategoryId.AgentFinished, NotificationCategoryId.AgentFailed ->
                if (payload.sessionId.isNotEmpty()) return Route.Agent(payload.sessionId)
            NotificationCategoryId.CiFailed -> {
                val repo = RepoRef.parse(payload.repo)
                if (repo != null && payload.runId.isNotEmpty()) return Route.Run(repo, payload.runId)
            }
            NotificationCategoryId.Dm -> if (payload.channel.isNotEmpty()) return Route.Dm(payload.channel)
            NotificationCategoryId.Mention ->
                if (payload.channel.isNotEmpty() && payload.href.isEmpty()) return Route.Channel(payload.channel)
            else -> Unit
        }
        NotificationDeepLink.parse(payload.href)?.let { return InboxRoutes.route(it) }
        RepoRef.parse(payload.repo)?.let {
            return if (payload.number > 0) Route.Pull(it, payload.number, PullTab.Conversation) else Route.Repo(it, null)
        }
        return if (payload.category == NotificationCategoryId.Ping) Route.Inbox(InboxFilter.All) else null
    }

    fun command(actionId: String?, payload: NotificationPayload, text: String?): NotificationCommand {
        val action = NotificationActionId.of(actionId) ?: return NotificationCommand.Open(route(payload), payload.href)
        val session = payload.sessionId
        val typed = text.orEmpty().trim()
        return when (action) {
            NotificationActionId.Open, NotificationActionId.OpenLog -> NotificationCommand.Open(route(payload), payload.href)
            NotificationActionId.MarkRead ->
                if (payload.alertId.isEmpty()) NotificationCommand.Ignore("no alert id") else NotificationCommand.MarkRead(payload.alertId)
            NotificationActionId.Reply -> when {
                payload.channel.isEmpty() -> NotificationCommand.Ignore("no conversation")
                typed.isEmpty() -> NotificationCommand.Ignore("empty reply")
                else -> NotificationCommand.Reply(payload.channel, typed.take(MAX_TEXT))
            }
            NotificationActionId.Approve, NotificationActionId.Deny ->
                if (session.isEmpty() || payload.approvalId.isEmpty()) {
                    NotificationCommand.Ignore("no approval")
                } else if (action == NotificationActionId.Approve) {
                    NotificationCommand.Approve(session, payload.approvalId)
                } else {
                    NotificationCommand.Deny(session, payload.approvalId)
                }
            NotificationActionId.Merge -> if (session.isEmpty()) NotificationCommand.Ignore("no session") else NotificationCommand.Merge(session)
            NotificationActionId.ContinueRun ->
                if (session.isEmpty()) NotificationCommand.Ignore("no session") else NotificationCommand.ContinueRun(session, typed.take(MAX_TEXT))
            NotificationActionId.Retry -> if (session.isEmpty()) NotificationCommand.Ignore("no session") else NotificationCommand.Retry(session)
            NotificationActionId.SendToAgent -> {
                val prompt = if (payload.category == NotificationCategoryId.CiFailed) {
                    "A CI run failed${if (payload.repo.isEmpty()) "" else " in ${payload.repo}"}. Find the cause and fix it."
                } else {
                    "The agent run $session failed. Look at what went wrong and try again."
                }
                NotificationCommand.Compose(
                    ComposeTarget(repo = RepoRef.parse(payload.repo), agentId = session.ifEmpty { null }, text = prompt),
                )
            }
            NotificationActionId.ApprovePull -> {
                val repo = RepoRef.parse(payload.repo)
                if (repo == null || payload.number <= 0) NotificationCommand.Ignore("no pull request") else NotificationCommand.ApprovePull(repo, payload.number)
            }
        }
    }
}
