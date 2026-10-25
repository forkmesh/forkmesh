package com.forkmesh.nativeapp.push

data class LinkProject(
    val id: String,
    val label: String,
    val signedIn: Boolean,
    val enabled: Boolean,
)

data class LinkConditions(
    val notificationsAllowed: Boolean = true,
    val networkAvailable: Boolean = true,
    val deviceIdle: Boolean = false,
)

data class LinkPlan(
    val runService: Boolean,
    val projects: List<LinkProject>,
    val live: Boolean,
    val mesh: Boolean = false,
    val callsOnly: Boolean = false,
) {
    val projectIds: List<String> get() = projects.map { it.id }

    val title: String get() = when {
        projects.isEmpty() && mesh -> MESH_TITLE
        callsOnly -> CALLS_TITLE
        else -> "Connected for pings"
    }

    fun statusText(connected: Set<String>): String {
        if (projects.isEmpty()) return if (mesh) MESH_STATUS else ""
        if (!live) return "Waiting for a network connection"
        val names = projects.map { it.label.ifEmpty { it.id } }
        val up = projects.count { it.id in connected }
        return when {
            up == projects.size -> "Listening for ${if (callsOnly) "calls" else "pings"} on ${names.joinToString(" and ")}"
            up == 0 -> "Connecting to ${names.joinToString(" and ")}"
            else -> "Connected to $up of ${projects.size} projects"
        }
    }

    companion object {
        val OFF = LinkPlan(false, emptyList(), false)
        const val MESH_TITLE = "Listening on #mesh"
        const val MESH_STATUS = "Nearby #mesh messages arrive with the app closed"
        const val CALLS_TITLE = "Listening for calls"

        fun of(projects: List<LinkProject>, conditions: LinkConditions): LinkPlan {
            val wanted = projects.filter { it.signedIn && it.enabled }.distinctBy { it.id }
            if (!conditions.notificationsAllowed) {
                if (wanted.isEmpty()) return OFF
                return LinkPlan(true, wanted, conditions.networkAvailable && !conditions.deviceIdle, callsOnly = true)
            }
            val mesh = projects.none { it.signedIn && !it.enabled }
            if (wanted.isEmpty()) return if (mesh) LinkPlan(true, emptyList(), true, mesh = true) else OFF
            return LinkPlan(true, wanted, conditions.networkAvailable && !conditions.deviceIdle, mesh)
        }
    }
}

object BackgroundLinkKeys {
    fun enabled(projectId: String): String = "push/background/${projectId.ifEmpty { "_" }}"

    fun ledger(projectId: String): String = "push/ledger/${projectId.ifEmpty { "_" }}"

    const val DEFAULT_ENABLED = true
}

data class LinkDiff(val open: List<String>, val close: List<String>, val keep: List<String>)

fun diffLinks(running: Set<String>, plan: LinkPlan): LinkDiff {
    val wanted = plan.projectIds
    return LinkDiff(
        open = wanted.filter { it !in running },
        close = running.filter { it !in wanted }.sorted(),
        keep = wanted.filter { it in running },
    )
}
