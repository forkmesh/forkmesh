package com.forkmesh.nativeapp.navigation

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str

enum class RoutePlacementKind { Automatic, CurrentTab, Tab }

data class RoutePlacement(val kind: RoutePlacementKind, val tab: AppTab? = null) {
    companion object {
        val Automatic = RoutePlacement(RoutePlacementKind.Automatic)
        val CurrentTab = RoutePlacement(RoutePlacementKind.CurrentTab)
        fun tab(tab: AppTab) = RoutePlacement(RoutePlacementKind.Tab, tab)
    }
}

sealed class LinkOutcome {
    data object Opened : LinkOutcome()
    data object WaitingForSignIn : LinkOutcome()
    data class SwitchProject(val project: String) : LinkOutcome()
    data class UnknownProject(val project: String) : LinkOutcome()
    data object Ignored : LinkOutcome()
}

data class TabNavigationState(
    val selectedTab: AppTab = AppTab.Agents,
    val paths: Map<AppTab, List<Route>> = emptyMap(),
    val recents: List<Route> = emptyList(),
    val savedAtMs: Long = 0,
)

data class ShowRequest(val tab: AppTab, val path: List<Route>, val replace: Boolean)

class RouterState(
    project: String = "",
    accountReady: Boolean = true,
    private val nowMs: () -> Long = System::currentTimeMillis,
) {
    companion object {
        const val RESTORE_WINDOW_MS: Long = 8L * 60 * 60 * 1000
        const val MAX_DEPTH = 32
        const val MAX_RECENTS = HubCatalog.MAX_RECENTS

        fun targetTab(route: Route, placement: RoutePlacement, selected: AppTab): AppTab = when (placement.kind) {
            RoutePlacementKind.Automatic -> route.homeTab ?: selected
            RoutePlacementKind.CurrentTab -> selected
            RoutePlacementKind.Tab -> placement.tab ?: selected
        }

        fun plan(stack: List<Route>, placement: RoutePlacement, selected: AppTab, current: List<Route>): ShowRequest? {
            val last = stack.lastOrNull() ?: return null
            val tab = targetTab(last, placement, selected)
            var trimmed = stack.filter { !it.isModal }
            if (trimmed.firstOrNull() == tab.root) trimmed = trimmed.drop(1)
            if (last.homeTab == null && placement.kind == RoutePlacementKind.Automatic) {
                if (current.size >= trimmed.size && current.takeLast(trimmed.size) == trimmed) return ShowRequest(tab, current, false)
                return ShowRequest(tab, (current + trimmed).takeLast(MAX_DEPTH), false)
            }
            return ShowRequest(tab, trimmed.takeLast(MAX_DEPTH), true)
        }
    }

    var project: String = project
        private set
    var accountReady: Boolean = accountReady
        private set
    var selectedTab: AppTab = AppTab.Agents
        private set
    var recents: List<Route> = emptyList()
        private set
    var pendingLink: ParsedLink? = null
        private set
    private val archive = LinkedHashMap<String, TabNavigationState>()
    private var paths: Map<AppTab, List<Route>> = emptyMap()

    fun path(tab: AppTab): List<Route> = paths[tab] ?: emptyList()

    fun select(tab: AppTab) {
        selectedTab = tab
    }

    fun setPath(tab: AppTab, path: List<Route>) {
        paths = paths + (tab to path.takeLast(MAX_DEPTH))
    }

    fun remember(route: Route) {
        if (route.isModal) return
        recents = (listOf(route) + recents.filter { it != route }).take(MAX_RECENTS)
    }

    fun open(link: ParsedLink, knownProjects: Set<String> = emptySet()): LinkOutcome {
        if (link.stack.isEmpty()) return LinkOutcome.Ignored
        var effective = link
        val target = link.project
        if (!target.isNullOrEmpty() && target != project) {
            if (target in knownProjects) {
                pendingLink = link
                return LinkOutcome.SwitchProject(target)
            }
            if (link.explicitProject) return LinkOutcome.UnknownProject(target)
            effective = link.copy(project = null)
        }
        if (!accountReady) {
            pendingLink = effective
            return LinkOutcome.WaitingForSignIn
        }
        pendingLink = null
        return LinkOutcome.Opened
    }

    fun setAccountReady(ready: Boolean): ParsedLink? {
        val was = accountReady
        accountReady = ready
        return if (ready && !was) takePending() else null
    }

    fun activate(id: String): ParsedLink? {
        if (id != project) {
            if (project.isNotEmpty()) archive[project] = state()
            project = id
            apply(archive[id])
        }
        return takePending()
    }

    fun takePending(): ParsedLink? {
        val link = pendingLink ?: return null
        if (!accountReady) return null
        val target = link.project
        if (!target.isNullOrEmpty() && target != project) return null
        pendingLink = null
        return link
    }

    fun discardPending() {
        pendingLink = null
    }

    fun state(): TabNavigationState =
        TabNavigationState(selectedTab, paths.filterValues { it.isNotEmpty() }, recents, nowMs())

    private fun apply(state: TabNavigationState?) {
        if (state == null) {
            selectedTab = AppTab.Agents
            paths = emptyMap()
            recents = emptyList()
            return
        }
        val restored = state.paths.mapValues { it.value.takeLast(MAX_DEPTH) }.toMutableMap()
        recents = state.recents.take(MAX_RECENTS)
        if (nowMs() - state.savedAtMs > RESTORE_WINDOW_MS) {
            restored.remove(AppTab.Agents)
            selectedTab = AppTab.Agents
        } else {
            selectedTab = state.selectedTab
        }
        paths = restored
    }

    fun load(text: String?) {
        archive.clear()
        archive.putAll(decodeArchive(text))
        apply(archive[project])
    }

    fun archiveText(): String {
        if (project.isNotEmpty()) archive[project] = state()
        return encodeArchive(archive)
    }

    private fun encodeArchive(projects: Map<String, TabNavigationState>): String = Json.stringify(
        mapOf(
            "projects" to projects.mapValues { (_, s) ->
                linkedMapOf(
                    "selectedTab" to s.selectedTab.raw,
                    "paths" to s.paths.entries.associate { it.key.raw to it.value.map(RouteCodec::toMap) },
                    "recents" to s.recents.map(RouteCodec::toMap),
                    "savedAt" to s.savedAtMs,
                )
            },
        ),
    )

    private fun decodeArchive(text: String?): Map<String, TabNavigationState> {
        if (text.isNullOrEmpty()) return emptyMap()
        val root = try {
            Json.parse(text).asJsonMap()
        } catch (_: Exception) {
            return emptyMap()
        }
        return root.obj("projects").mapValues { (_, raw) ->
            val m = raw.asJsonMap()
            val paths = m.obj("paths").mapNotNull { (key, value) ->
                val tab = AppTab.of(key) ?: return@mapNotNull null
                tab to (value as? List<*>).orEmpty().mapNotNull { RouteCodec.fromMap(it.asJsonMap()) }
            }.toMap()
            TabNavigationState(
                selectedTab = AppTab.of(m.str("selectedTab")) ?: AppTab.Agents,
                paths = paths,
                recents = (m["recents"] as? List<*>).orEmpty().mapNotNull { RouteCodec.fromMap(it.asJsonMap()) },
                savedAtMs = m.long("savedAt"),
            )
        }
    }
}

object ComposeContext {
    fun target(path: List<Route>, tab: AppTab, conversation: String = ""): ComposeTarget {
        for (route in path.asReversed()) {
            when (route) {
                is Route.Agent -> return ComposeTarget(agentId = route.id)
                is Route.LaunchedAgent -> return ComposeTarget(repo = route.repo, agentId = route.sessionId)
                is Route.Pull -> return ComposeTarget(repo = route.repo, pull = route.number)
                is Route.Review -> return ComposeTarget(repo = route.repo, pull = route.number)
                is Route.Issue -> return ComposeTarget(repo = route.repo, issue = route.number)
                is Route.Channel -> return ComposeTarget(channel = route.id)
                is Route.Thread -> return ComposeTarget(channel = route.channel)
                is Route.Dm -> return ComposeTarget(channel = "@${route.user}")
                else -> route.repoRef?.let { return ComposeTarget(repo = it) }
            }
        }
        if (tab == AppTab.Chat && conversation.isNotEmpty()) return ComposeTarget(channel = conversation)
        return ComposeTarget()
    }

    fun startsAgent(target: ComposeTarget): Boolean = target.channel == null
}

data class UpdateRequirement(val running: String, val minimum: String, val storeUrl: String?) {
    val summary: String
        get() = "ForkMesh ${running.trim().ifEmpty { "(unknown build)" }} is older than the minimum supported version $minimum"

    companion object {
        const val ADVICE = "Update ForkMesh to keep using it."
    }
}

object ClientVersionGate {
    const val PLATFORM_KEY = "android"
    const val DEFAULT_STORE_URL = "https://forkmesh.com/mobile"

    fun versionNumbers(version: String): List<Int>? {
        var text = version.trim()
        if (text.startsWith("v") || text.startsWith("V")) text = text.drop(1)
        val release = text.takeWhile { it.isDigit() || it == '.' }
        val parts = release.split('.')
        if (parts.isEmpty() || parts.size > 4) return null
        return parts.map { it.toIntOrNull() ?: return null }
    }

    fun isTooOld(running: String, minimum: String): Boolean {
        val have = versionNumbers(running) ?: return false
        val floor = versionNumbers(minimum) ?: return false
        for (index in 0 until maxOf(have.size, floor.size)) {
            val left = have.getOrElse(index) { 0 }
            val right = floor.getOrElse(index) { 0 }
            if (left != right) return left < right
        }
        return false
    }

    fun minimum(body: Map<String, Any?>): Pair<String, String?>? {
        val store = body.str("androidDownloadUrl").ifEmpty { null }
        val fromMap = body.obj("minimumClientVersions").str(PLATFORM_KEY)
        if (fromMap.isNotEmpty()) return fromMap to store
        val legacy = body.str("minimumAndroidVersion")
        if (legacy.isNotEmpty()) return legacy to store
        return null
    }

    fun requirement(body: Map<String, Any?>, running: String): UpdateRequirement? {
        val (floor, store) = minimum(body) ?: return null
        if (!isTooOld(running, floor)) return null
        return UpdateRequirement(running, floor, store ?: DEFAULT_STORE_URL)
    }

    fun requirement(status: Int, running: String): UpdateRequirement? =
        if (status == 426) UpdateRequirement(running, "a newer version", DEFAULT_STORE_URL) else null
}
