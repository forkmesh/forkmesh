package com.forkmesh.nativeapp.push

import com.forkmesh.nativeapp.account.projects.ProjectDefaults
import com.forkmesh.nativeapp.inbox.InboxRoutes
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.model.NotificationDeepLink
import com.forkmesh.nativeapp.navigation.Route

data class PushTarget(val projectId: String, val pingId: String, val href: String, val agent: String = "", val ping: ForkNotification? = null) {
    val link: NotificationDeepLink? get() = NotificationDeepLink.parse(href)

    fun notificationId(): Int = (projectId + "\n" + pingId).hashCode() and 0x7fffffff

    fun rowExtra(): String? {
        val row = ping ?: return null
        val text = runCatching { Json.stringify(row.toJson()) }.getOrNull() ?: return null
        return text.takeIf { it.length <= MAX_ROW_CHARS }
    }

    companion object {
        const val EXTRA_PROJECT = "com.forkmesh.push.project"
        const val EXTRA_PING = "com.forkmesh.push.ping"
        const val EXTRA_HREF = "com.forkmesh.push.href"
        const val EXTRA_ROW = "com.forkmesh.push.row"
        const val EXTRA_AGENT = "com.forkmesh.push.agent"
        const val MAX_ROW_CHARS = 32_000

        fun parseRow(text: String?): ForkNotification? {
            if (text.isNullOrBlank()) return null
            val map = runCatching { Json.parse(text) }.getOrNull() as? Map<*, *> ?: return null
            val row = ForkNotification.fromJson(map.entries.associate { (key, value) -> key.toString() to value })
            return row.takeIf { it.id.isNotEmpty() }
        }

        fun forPing(projectId: String, ping: ForkNotification): PushTarget {
            val agent = InboxRoutes.route(ping.kind, ping.href, ping.meta, ping.repo) as? Route.Agent
            return PushTarget(projectId, ping.id, ping.href, agent?.id.orEmpty(), ping)
        }

        fun from(project: String?, ping: String?, href: String?, agent: String? = null, row: String? = null): PushTarget? {
            val cleanProject = ProjectDefaults.canonicalId(project?.trim().orEmpty())
            val parsed = parseRow(row)
            val cleanPing = ping?.trim().orEmpty().ifEmpty { parsed?.id.orEmpty() }
            val cleanHref = href?.trim().orEmpty().ifEmpty { parsed?.href?.trim().orEmpty() }
            val cleanAgent = agent?.trim().orEmpty()
            if (cleanPing.isEmpty() && cleanHref.isEmpty() && cleanAgent.isEmpty()) return null
            return PushTarget(cleanProject, cleanPing, cleanHref, cleanAgent, parsed?.takeIf { it.id == cleanPing })
        }
    }
}

fun pingNotificationTitle(title: String, body: String, projectLabel: String, multipleProjects: Boolean): Pair<String, String> {
    val cleanTitle = title.trim().ifEmpty { "New ping" }
    val cleanBody = body.trim()
    return if (multipleProjects && projectLabel.isNotEmpty()) cleanTitle to listOf(cleanBody, projectLabel).filter { it.isNotEmpty() }.joinToString(" · ") else cleanTitle to cleanBody
}
