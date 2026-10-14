package com.forkmesh.nativeapp.model

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str

data class PingKind(val kind: String, val label: String, val email: Boolean)

data class PingKindGroup(val key: String, val label: String, val kinds: List<PingKind>)

data class PingQuietHours(
    val enabled: Boolean = false,
    val start: Int = PING_QUIET_DEFAULT_START,
    val end: Int = PING_QUIET_DEFAULT_END,
    val days: List<Int> = PING_QUIET_ALL_DAYS,
    val timezone: String = "UTC",
) {
    fun toJson(): Map<String, Any?> = linkedMapOf(
        "enabled" to enabled,
        "start" to start,
        "end" to end,
        "days" to days,
        "timezone" to timezone,
    )

    companion object {
        fun fromJson(json: JsonMap?): PingQuietHours {
            if (json.isNullOrEmpty()) return PingQuietHours()
            val rawDays = json["days"]
            val days = if (rawDays is List<*>) {
                rawDays.mapNotNull { (it as? Number)?.toInt() ?: jsonText(it).toIntOrNull() }
                    .filter { it in 0..6 }
                    .distinct()
                    .sorted()
            } else {
                PING_QUIET_ALL_DAYS
            }
            return PingQuietHours(
                enabled = json["enabled"] == true,
                start = minuteOr(json["start"], PING_QUIET_DEFAULT_START),
                end = minuteOr(json["end"], PING_QUIET_DEFAULT_END),
                days = days,
                timezone = json.str("timezone").trim().ifEmpty { "UTC" },
            )
        }

        private fun minuteOr(value: Any?, fallback: Int): Int {
            val minute = jsonLong(value, -1L)
            return if (minute in 0L..1439L) minute.toInt() else fallback
        }
    }
}

data class StatusMonitorAlert(
    val id: String,
    val label: String,
    val pings: Boolean,
    val emails: Boolean,
    val continual: Boolean,
) {
    companion object {
        fun fromJson(json: JsonMap): StatusMonitorAlert {
            val id = json.str("id")
            return StatusMonitorAlert(
                id = id,
                label = json.str("label").ifEmpty { id },
                pings = json["pings"] == true,
                emails = json["emails"] == true,
                continual = json["continual"] == true,
            )
        }
    }
}

data class PingSettings(
    val emailNotifications: Boolean = true,
    val notificationPreferences: Map<String, Boolean> = emptyMap(),
    val notificationDelivery: String = "digest",
    val notificationPauseUntil: Long = 0,
    val pingMutedKinds: Set<String> = emptySet(),
    val notificationQuietHours: PingQuietHours = PingQuietHours(),
    val notificationQuietUntil: Long = 0,
    val pingKindCatalog: List<PingKindGroup> = PING_KIND_FALLBACK_CATALOG,
    val statusMonitorAlerts: List<StatusMonitorAlert>? = null,
    val codePushes: Boolean = true,
) {
    companion object {
        fun fromJson(json: JsonMap): PingSettings {
            val prefs = LinkedHashMap<String, Boolean>()
            for ((key, value) in json.obj("notificationPreferences")) {
                if (value is Boolean) prefs[key] = value
            }
            val muted = when (val raw = json["pingMutedKinds"]) {
                is List<*> -> raw.map(::jsonText).filter { it.isNotEmpty() }.toSet()
                is Map<*, *> -> raw.entries.filter { it.value == false }.map { jsonText(it.key) }.toSet()
                else -> emptySet()
            }
            val catalog = json.maps("pingKindCatalog").mapNotNull { group ->
                val kinds = group.maps("kinds").mapNotNull { kind ->
                    val key = kind.str("kind").trim()
                    if (key.isEmpty()) null else PingKind(key, kind.str("label").ifEmpty { key }, kind["email"] == true)
                }
                val key = group.str("key")
                if (kinds.isEmpty()) null else PingKindGroup(key, group.str("label").ifEmpty { key }, kinds)
            }
            val monitors = if (json["statusMonitorAlerts"] is List<*>) {
                json.maps("statusMonitorAlerts").map(StatusMonitorAlert::fromJson).filter { it.id.isNotEmpty() }
            } else {
                null
            }
            val delivery = json.str("notificationDelivery").trim().lowercase()
            return PingSettings(
                emailNotifications = json["emailNotifications"] != false,
                notificationPreferences = prefs,
                notificationDelivery = if (delivery == "instant") "instant" else "digest",
                notificationPauseUntil = jsonLong(json["notificationPauseUntil"]).coerceAtLeast(0),
                pingMutedKinds = muted,
                notificationQuietHours = PingQuietHours.fromJson(json.obj("notificationQuietHours")),
                notificationQuietUntil = jsonLong(json["notificationQuietUntil"]).coerceAtLeast(0),
                pingKindCatalog = catalog.ifEmpty { PING_KIND_FALLBACK_CATALOG },
                statusMonitorAlerts = monitors,
                codePushes = json["codePushes"] != false,
            )
        }
    }
}

const val PING_QUIET_DEFAULT_START = 22 * 60
const val PING_QUIET_DEFAULT_END = 7 * 60
val PING_QUIET_ALL_DAYS: List<Int> = listOf(0, 1, 2, 3, 4, 5, 6)

val PING_KIND_FALLBACK_CATALOG: List<PingKindGroup> = listOf(
    PingKindGroup(
        "conversations",
        "Conversations",
        listOf(
            PingKind("mention", "Mentions", true),
            PingKind("subscribed", "Activity on things you follow", true),
            PingKind("chat_webhook", "Chat webhook posts", false),
            PingKind("channel_unread", "Unread channel messages", true),
            PingKind("direct_unread", "Unread direct messages", true),
            PingKind("calendar_invitation", "Calendar invitations", true),
            PingKind("calendar_reminder", "Calendar reminders", true),
            PingKind("calendar_rsvp", "Calendar replies", true),
        ),
    ),
    PingKindGroup(
        "repositories",
        "Repositories",
        listOf(
            PingKind("issue_opened", "New issues", true),
            PingKind("pull_submitted", "New pull requests", true),
            PingKind("issue_assigned", "Issue assignments", true),
            PingKind("pending_inbox", "Inbox updates", true),
            PingKind("repo_activity", "Pushes and merges", false),
            PingKind("release_published", "Releases", true),
            PingKind("repo_shared", "Repositories shared with you", true),
            PingKind("mirror_request", "Mirror requests", true),
            PingKind("repository_hosted", "Imports finished hosting", false),
        ),
    ),
    PingKindGroup(
        "organizations",
        "Organizations",
        listOf(
            PingKind("org_succession", "Organization succession", true),
            PingKind("organization_task_update", "Tasks", false),
        ),
    ),
    PingKindGroup(
        "rewards",
        "Money & rewards",
        listOf(
            PingKind("bounty_funded", "Bounties funded", true),
            PingKind("bounty_paid", "Bounties paid", true),
            PingKind("pending_reward", "Pending rewards", true),
            PingKind("credits_refilled", "Credits refilled", true),
            PingKind("tip_sent", "Tips", true),
            PingKind("hackathon_squad", "Hackathon squad progress", false),
            PingKind("hackathon_reward", "Hackathon rewards", false),
        ),
    ),
    PingKindGroup(
        "devices",
        "Devices & apps",
        listOf(
            PingKind("host_online", "Host came online", true),
            PingKind("host_offline", "Host went offline", true),
            PingKind("device_added", "New device signed in", false),
            PingKind("timer_finished", "Timers and alarms", false),
            PingKind("desktop_ping", "Pings from your desktop", false),
            PingKind("plugin_activity", "App package activity", false),
        ),
    ),
)
