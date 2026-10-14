package com.forkmesh.nativeapp.push

const val PING_DEFAULT_ICON = "🔔"
const val PING_CHAT_ICON = "💬"
const val PING_DIRECT_MESSAGE_ICON = "📨"

val PING_KIND_ICONS: Map<String, String> = mapOf(
    "mention" to "📣",
    "hackathon_squad" to "🏁",
    "hackathon_reward" to "🏆",
    "subscribed" to "👀",
    "issue_opened" to "🐛",
    "pull_submitted" to "🔀",
    "issue_assigned" to "📌",
    "repo_shared" to "🤝",
    "bounty_funded" to "💰",
    "bounty_paid" to "💸",
    "release_published" to "🚀",
    "host_online" to "🟢",
    "host_offline" to "🔴",
    "credits_refilled" to "🔋",
    "pending_inbox" to "📥",
    "mirror_request" to "🪞",
    "pending_reward" to "🎁",
    "org_succession" to "👑",
    "repository_hosted" to "🏠",
    "organization_task_update" to "✅",
    "calendar_invitation" to "📅",
    "calendar_reminder" to "⏰",
    "calendar_rsvp" to "🙋",
    "chat_webhook" to "🪝",
    "channel_unread" to PING_CHAT_ICON,
    "direct_unread" to PING_CHAT_ICON,
    "general_chat" to PING_CHAT_ICON,
    "account_email_sent" to "📧",
    "operational_alert" to "📟",
    "tip_sent" to "🪙",
    "error_group" to "💥",
    "device_added" to "📱",
    "plugin_activity" to "🧩",
    "timer_finished" to "⌛",
    "desktop_ping" to "💻",
    "repo_activity" to "📦",
    "code_push" to "📤",
    "security_alert" to "🚨",
    "package_review" to "⭐",
)

val PING_DESKTOP_ALARM_ICONS: Map<String, String> = mapOf(
    PING_ALARM_POWER to "🪫",
)

val PING_DESKTOP_CATEGORY_ICONS: Map<String, String> = mapOf(
    "agent-finished" to "🤖",
    "agent-failed" to "⚠️",
)

fun pingIcon(kind: String, meta: Map<String, Any?> = emptyMap()): String {
    val clean = kind.trim()
    if (clean == "desktop_ping") {
        PING_DESKTOP_ALARM_ICONS[pingAlarm(clean, meta)]?.let { return it }
        PING_DESKTOP_CATEGORY_ICONS[(meta["category"] as? String)?.trim().orEmpty()]?.let { return it }
    }
    return PING_KIND_ICONS[clean] ?: PING_DEFAULT_ICON
}

fun withPingIcon(title: String, icon: String): String {
    val cleanTitle = title.trim()
    val cleanIcon = icon.trim()
    if (cleanTitle.isEmpty() || cleanIcon.isEmpty() || cleanTitle.startsWith(cleanIcon)) return cleanTitle
    return "$cleanIcon $cleanTitle"
}
