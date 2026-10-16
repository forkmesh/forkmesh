package com.forkmesh.nativeapp.model

import com.forkmesh.nativeapp.json.jsonMaps
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str

data class NotificationPage(
    val notifications: List<ForkNotification>,
    val unread: Int,
    val total: Int = notifications.size,
) {
    fun toJson(): Map<String, Any?> = linkedMapOf(
        "notifications" to notifications.map { it.toJson() },
        "unread" to unread.toLong(),
        "total" to total.toLong(),
    )

    companion object {
        fun fromJson(json: Map<String, Any?>): NotificationPage {
            val raw = when {
                json["notifications"] is List<*> -> json["notifications"]
                json["items"] is List<*> -> json["items"]
                else -> emptyList<Any?>()
            }
            val items = jsonMaps(raw).map { ForkNotification.fromJson(LinkedHashMap(it)) }
            return NotificationPage(
                notifications = items,
                unread = modelInt(json["unread"]).toInt(),
                total = if (json.containsKey("total")) modelInt(json["total"]).toInt() else items.size,
            )
        }
    }
}

data class FederatedReply(
    val remoteId: String,
    val author: String,
    val body: String,
    val authorName: String = "",
    val authorUrl: String = "",
    val backlink: String = "",
    val parentRemoteId: String = "",
    val sourceInstance: String = "",
    val sourceSoftware: String = "activitypub",
    val lifecycle: String = "active",
    val depth: Int = 0,
    val timestamp: Long = 0,
    val nativeEvent: Boolean = false,
) {
    val edited: Boolean get() = lifecycle == "edited"

    val tombstone: Boolean get() = lifecycle == "tombstoned"

    val moderated: Boolean get() = lifecycle == "moderated" || lifecycle == "awaiting-redelivery"

    val displayBody: String
        get() = when {
            tombstone -> "Deleted on the remote instance"
            moderated -> "Hidden by remote moderation"
            else -> body
        }

    companion object {
        fun fromJson(json: Map<String, Any?>): FederatedReply {
            val provenance = json.obj("provenance")
            val lifecycle = json["lifecycle"] ?: when {
                json["tombstone"] == true -> "tombstoned"
                json["moderated"] == true -> "moderated"
                json["edited"] == true -> "edited"
                else -> "active"
            }
            return FederatedReply(
                remoteId = jsonText(json["remoteId"] ?: json["id"]),
                parentRemoteId = json.str("parentRemoteId"),
                author = json.str("author"),
                authorName = json.str("authorName"),
                authorUrl = json.str("authorUrl"),
                body = json.str("body"),
                backlink = jsonText(json["url"] ?: json["backlink"] ?: provenance["backlink"] ?: json["remoteId"]),
                sourceInstance = jsonText(json["sourceInstance"] ?: provenance["instance"]),
                sourceSoftware = jsonText(json["sourceSoftware"] ?: provenance["software"] ?: "activitypub"),
                lifecycle = jsonText(lifecycle),
                depth = modelInt(json["depth"]).coerceIn(0L, 8L).toInt(),
                timestamp = modelInt(json["ts"]),
                nativeEvent = false,
            )
        }
    }
}

data class ForkNotification(
    val id: String,
    val kind: String,
    val title: String,
    val body: String = "",
    val repo: String = "",
    val href: String = "",
    val actor: String = "",
    val source: String = "",
    val ts: Long = 0,
    val readAt: Long = 0,
    val meta: Map<String, Any?> = emptyMap(),
) {
    val isUnread: Boolean get() = readAt <= 0

    val kindLabel: String
        get() = when (kind) {
            "mention" -> "Mention"
            "subscribed" -> "Subscribed"
            "issue_opened" -> "New issue"
            "pull_submitted" -> "Pull request"
            "issue_assigned" -> "Assignment"
            "repo_shared" -> "Repo share"
            "bounty_funded", "bounty_paid" -> "Bounty"
            "tip_sent" -> "Tip"
            "release_published" -> "Release"
            "host_online", "host_offline" -> "Host"
            "credits_refilled" -> "Credits"
            "pending_inbox" -> "Inbox"
            "mirror_request" -> "Mirror request"
            "pending_reward" -> "Reward"
            "org_succession" -> "Organization"
            "repository_hosted" -> "Hosting"
            "organization_task_started", "organization_task_activity", "organization_task_update" -> "Task"
            "calendar_invitation", "calendar_reminder", "calendar_rsvp" -> "Calendar"
            "chat_webhook", "channel_unread", "direct_unread" -> "Chat"
            "account_email_sent" -> "Email"
            "operational_alert" -> "Alert"
            "security_alert" -> "Security"
            "error_group" -> "Error"
            "device_added" -> "New device"
            "plugin_activity" -> "Package"
            "package_review" -> "Package review"
            "hackathon_squad", "hackathon_reward" -> "Hackathon"
            "badge" -> "Badge"
            "timer_finished" -> "Timer"
            "desktop_ping" -> "Desktop"
            "repo_activity" -> "Repo activity"
            "code_push" -> "Code push"
            else -> "Notification"
        }

    val filterGroup: String
        get() {
            if (kind == "mention") return "Mentions"
            if (kind == "host_online" ||
                kind == "host_offline" ||
                kind == "release_published" ||
                kind == "bounty_funded" ||
                kind == "bounty_paid" ||
                kind == "credits_refilled" ||
                kind == "timer_finished" ||
                kind == "desktop_ping"
            ) {
                return "System"
            }
            return "Repo"
        }

    fun copyWith(readAt: Long? = null): ForkNotification = copy(readAt = readAt ?: this.readAt)

    fun toJson(): Map<String, Any?> = linkedMapOf(
        "id" to id,
        "kind" to kind,
        "title" to title,
        "body" to body,
        "repo" to repo,
        "href" to href,
        "actor" to actor,
        "source" to source,
        "ts" to ts,
        "readAt" to readAt,
        "meta" to meta,
    )

    companion object {
        fun fromJson(json: Map<String, Any?>): ForkNotification = ForkNotification(
            id = json.str("id"),
            kind = json.str("kind", "pending_inbox"),
            title = json.str("title", "Notification"),
            body = json.str("body"),
            repo = json.str("repo"),
            href = json.str("href"),
            actor = json.str("actor"),
            source = json.str("source"),
            ts = modelInt(json["ts"]),
            readAt = modelInt(json["readAt"] ?: json["read_at"]),
            meta = if (json["meta"] is Map<*, *>) LinkedHashMap(json.obj("meta")) else emptyMap(),
        )
    }
}
