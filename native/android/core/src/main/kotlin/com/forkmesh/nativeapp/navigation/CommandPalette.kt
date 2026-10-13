package com.forkmesh.nativeapp.navigation

sealed class PaletteCommand {
    data class Compose(val target: ComposeTarget) : PaletteCommand()
    data class Open(val route: Route) : PaletteCommand()
    data class SelectTab(val tab: AppTab) : PaletteCommand()
    data object SwitchProject : PaletteCommand()
    data class SearchEverywhere(val query: String) : PaletteCommand()
}

data class PaletteEntry(val id: String, val title: String, val subtitle: String = "", val icon: String, val command: PaletteCommand)

data class PaletteSection(val id: String, val title: String, val entries: List<PaletteEntry>)

data class PaletteContext(
    val target: ComposeTarget = ComposeTarget(),
    val tab: AppTab = AppTab.Agents,
    val pendingApprovals: Int = 0,
    val recents: List<Route> = emptyList(),
    val guest: Boolean = false,
    val hidden: Set<Route.Kind> = emptySet(),
)

object CommandPalette {
    const val MAX_DESTINATIONS = 8

    fun suggestions(context: PaletteContext): List<PaletteEntry> = buildList {
        val target = context.target
        val hidden = context.hidden
        when {
            target.channel != null ->
                if (Route.Kind.Chat !in hidden) add(PaletteEntry("compose", "Message ${target.channel}", icon = "chat", command = PaletteCommand.Compose(target)))
            Route.Kind.Compose in hidden -> Unit
            target.repo != null ->
                add(PaletteEntry("compose", "New agent here", target.repo.slug, "auto_awesome", PaletteCommand.Compose(target)))
            else -> add(PaletteEntry("compose", "New agent", icon = "auto_awesome", command = PaletteCommand.Compose(target)))
        }
        target.repo?.takeIf { Route.Kind.NewIssue !in hidden }?.let {
            add(PaletteEntry("newIssue", "New issue", it.slug, RouteCatalog.icon(Route.Kind.NewIssue), PaletteCommand.Open(Route.NewIssue(it))))
        }
        if (context.pendingApprovals > 0 && Route.Kind.Inbox !in hidden) {
            val count = context.pendingApprovals
            add(
                PaletteEntry(
                    "approvals", "Review $count pending ${if (count == 1) "approval" else "approvals"}",
                    icon = RouteCatalog.icon(Route.Kind.Approval), command = PaletteCommand.Open(Route.Inbox(InboxFilter.Approvals)),
                ),
            )
        }
        if (Route.Kind.Agents !in hidden) {
            add(PaletteEntry("running", "Running agents", icon = "play_circle", command = PaletteCommand.Open(Route.Agents(AgentFilter.Running))))
        }
        if (!context.guest && Route.Kind.Note !in hidden) {
            add(PaletteEntry("newNote", "New note", icon = "edit_note", command = PaletteCommand.Open(Route.Note(""))))
        }
        if (Route.Kind.Timers !in hidden) add(PaletteEntry("timer", "Start a timer", icon = "timer", command = PaletteCommand.Open(Route.Timers)))
        add(PaletteEntry("switchProject", "Switch project", icon = "layers", command = PaletteCommand.SwitchProject))
    }

    val tabEntries: List<PaletteEntry>
        get() = tabEntries(emptySet())

    fun tabEntries(hidden: Set<Route.Kind>): List<PaletteEntry> = AppTab.barOrder.filter { it.root.kind !in hidden }
        .map { PaletteEntry("tab:${it.raw}", "Go to ${it.title}", icon = it.icon, command = PaletteCommand.SelectTab(it)) }

    fun destinationEntries(hidden: Set<Route.Kind> = emptySet()): List<PaletteEntry> =
        DestinationsSearchSource.routes(hidden).filter { route -> AppTab.barOrder.none { it.root == route } }.map(::entry)

    fun entry(route: Route): PaletteEntry =
        PaletteEntry("route:${route.kind.raw}:${route.title}", route.title, icon = RouteCatalog.icon(route.kind), command = PaletteCommand.Open(route))

    fun directEntries(query: String): List<PaletteEntry> = buildList {
        val trimmed = query.trim()
        val parts = trimmed.split("#", limit = 2)
        val repo = RepoRef.parse(parts[0])
        if (repo != null) {
            val number = parts.getOrNull(1)?.toIntOrNull()
            if (parts.size == 2 && number != null && number > 0) {
                add(entry(Route.Issue(repo, number)))
                add(
                    PaletteEntry(
                        "route:pull:${repo.slug}#$number", "Pull request ${repo.slug} #$number",
                        icon = RouteCatalog.icon(Route.Kind.Pull), command = PaletteCommand.Open(Route.Pull(repo, number, PullTab.Conversation)),
                    ),
                )
            } else if (parts.size == 1) {
                add(entry(Route.Repo(repo, null)))
            }
        }
        if (trimmed.startsWith("@") && trimmed.length > 1) add(entry(Route.Profile(trimmed.drop(1))))
    }

    fun sections(query: String, context: PaletteContext): List<PaletteSection> {
        val trimmed = query.trim()
        if (trimmed.isEmpty()) {
            return buildList {
                add(PaletteSection("suggested", "Suggested", suggestions(context)))
                val recents = context.recents.filter { it.kind !in context.hidden }
                if (recents.isNotEmpty()) add(PaletteSection("recent", "Recent", recents.map(::entry)))
                add(PaletteSection("tabs", "Tabs", tabEntries(context.hidden)))
            }
        }
        return buildList {
            val direct = directEntries(trimmed)
            val actions = SearchMatch.rank(suggestions(context) + tabEntries(context.hidden), trimmed) { listOf(it.title, it.subtitle) }
            if ((direct + actions).isNotEmpty()) add(PaletteSection("actions", "Actions", direct + actions))
            val destinations = SearchMatch.rank(destinationEntries(context.hidden), trimmed) { listOf(it.title) }
            if (destinations.isNotEmpty()) add(PaletteSection("destinations", "Destinations", destinations.take(MAX_DESTINATIONS)))
            add(
                PaletteSection(
                    "everywhere", "Search",
                    listOf(PaletteEntry("everywhere", "Search everywhere for “$trimmed”", icon = "search", command = PaletteCommand.SearchEverywhere(trimmed))),
                ),
            )
        }
    }
}

enum class ShellQuickAction(val id: String, val title: String, val icon: String) {
    NewAgent("new-agent", "New agent", "auto_awesome"),
    Inbox("inbox", "Inbox", "inbox"),
    Search("search", "Search", "search"),
    NewNote("new-note", "New note", "edit_note"),
    RecentRepo("recent-repo", "Repository", "folder"),
    RecentAgent("recent-agent", "Agent", "play_circle");

    fun route(payload: String = ""): Route? = when (this) {
        NewAgent -> Route.Compose(ComposeTarget())
        Inbox -> Route.Inbox(InboxFilter.All)
        Search -> Route.Search("", SearchScope.Everything)
        NewNote -> Route.Note("")
        RecentRepo -> RepoRef.parse(payload)?.let { Route.Repo(it, null) }
        RecentAgent -> if (payload.isEmpty()) null else Route.Agent(payload)
    }

    data class DynamicItem(val action: ShellQuickAction, val title: String, val payload: String)

    companion object {
        val staticActions: List<ShellQuickAction> = listOf(NewAgent, Inbox, Search, NewNote)

        fun visible(recents: List<Route>, hidden: Set<Route.Kind>): List<DynamicItem> =
            (staticActions.map { DynamicItem(it, it.title, "") } + dynamicItems(recents.filter { it.kind !in hidden }))
                .filter { item -> item.action.route(item.payload)?.kind?.let { it !in hidden } ?: true }

        fun of(id: String?): ShellQuickAction? = entries.firstOrNull { it.id == id }

        fun dynamicItems(recents: List<Route>): List<DynamicItem> = buildList {
            recents.firstNotNullOfOrNull { it.repoRef }?.let { add(DynamicItem(RecentRepo, it.slug, it.slug)) }
            recents.firstNotNullOfOrNull { (it as? Route.Agent)?.id }?.let { add(DynamicItem(RecentAgent, "Agent $it", it)) }
        }
    }
}
