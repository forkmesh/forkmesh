package com.forkmesh.nativeapp.account

import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.model.NotificationDeepLink
import com.forkmesh.nativeapp.model.RepoDetailTab
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale

const val NOTIFICATION_LINE_ICON = "mingcute:notification_line"

fun notificationIconForKind(kind: String): String = when (kind.trim().lowercase()) {
    "mention" -> "alternate_email_rounded"
    "subscribed" -> "notifications_active_outlined"
    "issue_opened" -> "adjust"
    "pull_submitted" -> "call_merge_rounded"
    "issue_assigned" -> "assignment_ind_outlined"
    "repo_shared" -> "folder_special_outlined"
    "security_advisory" -> "gpp_maybe_outlined"
    "bounty_funded", "bounty_paid", "tip_sent" -> "payments_outlined"
    "release_published" -> "new_releases_outlined"
    "host_online" -> "cloud_done_outlined"
    "host_offline" -> "cloud_off_outlined"
    "credits_refilled" -> "account_balance_wallet_outlined"
    "pending_inbox" -> "inbox_outlined"
    "mirror_request" -> "sync_alt"
    "pending_reward" -> "account_balance_wallet_outlined"
    "org_succession" -> "account_tree_outlined"
    "repository_hosted" -> "cloud_done_outlined"
    "organization_task_started", "organization_task_activity", "organization_task_update" ->
        "check_circle_outline"
    "calendar_invitation", "calendar_reminder", "calendar_rsvp" -> "calendar_today_outlined"
    "chat_webhook", "channel_unread", "direct_unread" -> "chat_bubble_outline"
    "account_email_sent" -> "mark_email_read_outlined"
    "operational_alert" -> "warning_amber_outlined"
    "error_group" -> "error_outline"
    "plugin_activity" -> "extension_outlined"
    "timer_finished" -> "timer_outlined"
    "desktop_ping" -> "desktop_windows_outlined"
    "repo_activity" -> "commit_outlined"
    "code_push" -> "commit_outlined"
    "security_alert" -> "gpp_maybe_outlined"
    "device_added" -> "smartphone_outlined"
    "hackathon_squad" -> "groups_outlined"
    "hackathon_reward" -> "card_giftcard_outlined"
    "badge" -> "emoji_events_outlined"
    "package_review" -> "rate_review_outlined"
    "reaction" -> "add_reaction_outlined"
    "pull_attention" -> "rate_review_outlined"
    else -> NOTIFICATION_LINE_ICON
}

val NOTIFICATION_FILTERS: List<String> = listOf("All", "Unread", "Mentions", "Repo", "System")

fun filterNotifications(items: List<ForkNotification>, filter: String): List<ForkNotification> = items.filter { n ->
    when (filter) {
        "Unread" -> n.isUnread
        "Mentions" -> n.filterGroup == "Mentions"
        "Repo" -> n.filterGroup == "Repo"
        "System" -> n.filterGroup == "System"
        else -> true
    }
}

sealed class NotificationsBody {
    data object Loading : NotificationsBody()

    data class Error(val message: String) : NotificationsBody()

    data class Empty(val filter: String) : NotificationsBody() {
        val icon: String get() = if (filter == "Unread") "mark_email_read_outlined" else NOTIFICATION_LINE_ICON

        val title: String get() = if (filter == "Unread") "All caught up" else "No ${filter.lowercase()} notifications"

        val message: String
            get() = if (filter == "Unread") {
                "Unread issue, PR, discussion, host, release, and mention updates will appear here."
            } else {
                "Mainnode-backed ForkMesh updates will appear here when something needs your attention."
            }
    }

    data class Items(val filter: String, val items: List<ForkNotification>) : NotificationsBody()

    val stateKey: String
        get() = when (this) {
            Loading -> "loading"
            is Error -> "error"
            is Empty -> "empty-$filter"
            is Items -> "list-$filter"
        }
}

fun notificationsBody(
    loading: Boolean,
    error: String,
    all: List<ForkNotification>,
    filter: String,
): NotificationsBody {
    if (loading && all.isEmpty()) return NotificationsBody.Loading
    if (error.isNotEmpty() && all.isEmpty()) return NotificationsBody.Error(error)
    val visible = filterNotifications(all, filter)
    if (visible.isEmpty()) return NotificationsBody.Empty(filter)
    return NotificationsBody.Items(filter, visible)
}

const val NOTIFICATIONS_LOAD_ERROR_TITLE = "Could not load notifications"

fun notificationsUnreadLabel(unread: Int): String = "$unread unread"

fun notificationsUpdatesLabel(count: Int): String = "$count ${if (count == 1) "update" else "updates"}"

fun canMarkAllRead(unread: Int): Boolean = unread > 0

const val NOTIFICATIONS_REFRESH_LABEL = "Refresh notifications"

const val NOTIFICATIONS_DELETE_ALL_LABEL = "Delete all notifications"

fun canDeleteAllNotifications(loaded: Int): Boolean = loaded > 0

fun notificationsDeleteAllTitle(total: Int): String =
    if (total == 1) "Delete 1 notification?" else "Delete all $total notifications?"

const val NOTIFICATIONS_DELETE_ALL_BODY: String =
    "This removes them from your ForkMesh inbox on every device, read and unread alike. " +
        "This cannot be undone."

const val NOTIFICATIONS_DELETE_ALL_CONFIRM = "Delete all"

const val NOTIFICATIONS_DELETED_TEXT = "Notifications deleted."

fun notificationsDeleteErrorText(error: Throwable?): String =
    "Could not delete notifications: ${error ?: "unknown error"}"

fun notificationTimeLabel(ms: Long, zone: ZoneId = ZoneId.systemDefault()): String {
    if (ms <= 0) return ""
    val time = Instant.ofEpochMilli(ms).atZone(zone)
    val meridiem = if (time.hour < 12) "AM" else "PM"
    val hour = if (time.hour % 12 == 0) 12 else time.hour % 12
    return "$hour:${time.minute.toString().padStart(2, '0')} $meridiem"
}

fun pingDetailWhen(ms: Long, zone: ZoneId = ZoneId.systemDefault()): String {
    if (ms <= 0) return ""
    val date = DateTimeFormatter.ofPattern("EEE, MMM d, yyyy", Locale.US).withZone(zone).format(Instant.ofEpochMilli(ms))
    return "$date · ${notificationTimeLabel(ms, zone)}"
}

fun notificationPreview(notification: ForkNotification): String =
    notification.body.ifEmpty { "Open context in ForkMesh to review this update." }

fun notificationPills(notification: ForkNotification): List<String> {
    val pills = arrayListOf(notification.kindLabel)
    if (notification.repo.isNotEmpty()) pills.add(notification.repo)
    if (notification.actor.isNotEmpty()) pills.add("@${notification.actor}")
    return pills
}

fun notificationContextLabel(notification: ForkNotification): String? =
    if (notification.href.isEmpty()) null else "Open context: ${notification.href}"

fun pingWhen(ms: Long, zone: ZoneId = ZoneId.systemDefault()): String {
    val t = Instant.ofEpochMilli(ms).atZone(zone)
    fun two(n: Int) = n.toString().padStart(2, '0')
    return "${t.year.toString().padStart(4, '0')}-${two(t.monthValue)}-${two(t.dayOfMonth)} ${two(t.hour)}:${two(t.minute)}"
}

fun pingAgeLabel(ms: Long, now: Long): String {
    if (ms <= 0) return ""
    val seconds = (now - ms) / 1000
    return when {
        seconds < 60 -> "just now"
        seconds < 3600 -> "${seconds / 60} min ago"
        seconds < 86400 -> "${seconds / 3600} h ago"
        seconds < 86400 * 30 -> "${seconds / 86400} d ago"
        else -> ""
    }
}

fun pingAgentPrompt(ping: ForkNotification, zone: ZoneId = ZoneId.systemDefault()): String {
    val head = arrayListOf<String>()
    val kind = ping.kindLabel.trim()
    if (kind.isNotEmpty()) head.add(kind)
    if (ping.ts > 0) head.add(pingWhen(ping.ts, zone))
    val lines = arrayListOf(if (head.isEmpty()) "ForkMesh ping" else "ForkMesh ping (${head.joinToString(" · ")})")
    val title = ping.title.trim()
    if (title.isNotEmpty()) lines.add(title)
    val body = ping.body.trim()
    if (body.isNotEmpty()) lines.addAll(listOf("", body))
    val context = arrayListOf<String>()
    val repo = ping.repo.trim()
    val actor = ping.actor.trim()
    val href = ping.href.trim()
    if (repo.isNotEmpty()) context.add("Repository: $repo")
    if (actor.isNotEmpty()) context.add("From: $actor")
    if (href.isNotEmpty()) context.add("Link: $href")
    if (context.isNotEmpty()) {
        lines.add("")
        lines.addAll(context)
    }
    return lines.joinToString("\n")
}

data class PingDetailFact(val label: String, val value: String)

fun pingMetaLabel(key: String): String {
    val words = key.trim()
        .replace(Regex("([a-z0-9])([A-Z])"), "$1 $2")
        .replace(Regex("[_\\-.]+"), " ")
        .trim()
        .lowercase()
    return words.replaceFirstChar { it.uppercaseChar() }
}

private fun pingMetaValue(value: Any?): String? = when (value) {
    is String -> value.trim().ifEmpty { null }
    is Boolean -> if (value) "Yes" else "No"
    is Double -> if (value % 1.0 == 0.0 && kotlin.math.abs(value) < 1e15) value.toLong().toString() else value.toString()
    is Float -> pingMetaValue(value.toDouble())
    is Number -> value.toString()
    else -> null
}

fun pingMetaFacts(meta: Map<String, Any?>): List<PingDetailFact> = meta.entries
    .sortedBy { it.key.lowercase() }
    .mapNotNull { (key, value) ->
        val label = pingMetaLabel(key)
        val text = pingMetaValue(value)
        if (label.isEmpty() || text == null) null else PingDetailFact(label, text)
    }

fun pingDetailFacts(ping: ForkNotification, now: Long, zone: ZoneId = ZoneId.systemDefault()): List<PingDetailFact> {
    val facts = arrayListOf(PingDetailFact("Kind", ping.kindLabel))
    if (ping.ts > 0) {
        val age = pingAgeLabel(ping.ts, now)
        facts.add(PingDetailFact("Received", if (age.isEmpty()) pingDetailWhen(ping.ts, zone) else "${pingDetailWhen(ping.ts, zone)} · $age"))
    }
    val status = when {
        ping.isUnread -> "Unread"
        else -> "Read · ${pingDetailWhen(ping.readAt, zone)}"
    }
    facts.add(PingDetailFact("Status", status))
    if (ping.repo.isNotBlank()) facts.add(PingDetailFact("Repository", ping.repo.trim()))
    if (ping.actor.isNotBlank()) facts.add(PingDetailFact("From", "@${ping.actor.trim()}"))
    if (ping.source.isNotBlank()) facts.add(PingDetailFact("Source", ping.source.trim()))
    if (ping.href.isNotBlank()) facts.add(PingDetailFact("Link", ping.href.trim()))
    facts.addAll(pingMetaFacts(ping.meta))
    if (ping.id.isNotBlank()) facts.add(PingDetailFact("ID", ping.id.trim()))
    return facts
}

fun pingWebLink(href: String): String? {
    val trimmed = href.trim()
    val uri = runCatching { java.net.URI(trimmed) }.getOrNull() ?: return null
    val scheme = uri.scheme?.lowercase() ?: return null
    if (scheme != "http" && scheme != "https") return null
    if (uri.host.isNullOrEmpty() || uri.userInfo != null) return null
    return trimmed
}

const val PING_DETAIL_TITLE = "Ping"
const val PING_DELETE_LABEL = "Delete ping"
const val PING_DELETE_TITLE = "Delete this ping?"
const val PING_DELETE_BODY = "It is removed from your ForkMesh inbox on every device. This cannot be undone."
const val PING_DELETED_TEXT = "Ping deleted."

fun pingDeleteErrorText(error: Throwable?): String = "Could not delete ping: ${error ?: "unknown error"}"

fun pingOpenLabel(link: NotificationDeepLink?, web: String?): String? {
    if (link == null) return if (web != null) "Open in browser" else null
    return when {
        link.initialTab == RepoDetailTab.Issues && link.number > 0 -> "Open issue #${link.number}"
        link.initialTab == RepoDetailTab.Pulls && link.number > 0 -> "Open pull request #${link.number}"
        link.initialTab == RepoDetailTab.Discussions && link.number > 0 -> "Open discussion #${link.number}"
        link.initialTab == RepoDetailTab.Commits && link.reference.isNotEmpty() -> "Open commit ${link.reference.take(7)}"
        link.initialTab == RepoDetailTab.Commits && link.branch.isNotEmpty() -> "Open commits on ${link.branch}"
        link.initialTab == RepoDetailTab.Agents && link.session > 0 -> "Open agent session"
        link.initialTab == RepoDetailTab.Actions && link.run > 0 -> "Open run #${link.run}"
        link.initialTab == RepoDetailTab.Releases -> "Open releases"
        else -> "Open ${link.repo.owner}/${link.repo.name}"
    }
}
