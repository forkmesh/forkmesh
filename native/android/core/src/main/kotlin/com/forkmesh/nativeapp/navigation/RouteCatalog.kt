package com.forkmesh.nativeapp.navigation

val AppTab.title: String
    get() = when (this) {
        AppTab.Agents -> "Agents"
        AppTab.Inbox -> "Inbox"
        AppTab.Code -> "Code"
        AppTab.Chat -> "Chat"
    }

val AppTab.icon: String
    get() = when (this) {
        AppTab.Agents -> "auto_awesome"
        AppTab.Inbox -> "inbox"
        AppTab.Code -> "integration_instructions"
        AppTab.Chat -> "forum"
    }

val AppTab.Companion.barOrder: List<AppTab> get() = listOf(AppTab.Agents, AppTab.Inbox, AppTab.Code, AppTab.Chat)

val Route.title: String
    get() = when (this) {
        is Route.Agents -> when (filter) {
            AgentFilter.NeedsYou -> "Needs you"
            AgentFilter.Running -> "Running"
            AgentFilter.Queued -> "Queued"
            AgentFilter.Recent -> "Recent agents"
            AgentFilter.Desktop -> "Desktop agents"
            AgentFilter.Cloud -> "Cloud agents"
            AgentFilter.All -> "Agents"
        }
        is Route.Inbox -> if (filter == InboxFilter.All) "Inbox" else filter.label
        is Route.Repo -> section?.let { "${RouteCatalog.sectionTitle(it)} · ${repo.slug}" } ?: repo.slug
        is Route.Issue -> "${repo.slug} #$number"
        is Route.Pull -> "${repo.slug} #$number"
        is Route.Review -> "Review ${repo.slug} #$number"
        is Route.Discussion -> "${repo.slug} #$number"
        is Route.NewIssue -> "New issue · ${repo.slug}"
        is Route.Commit -> "Commit ${sha.take(7)}"
        is Route.File -> path.substringAfterLast('/').ifEmpty { "Code" }
        is Route.Tree -> path.substringAfterLast('/').ifEmpty { "Code" }
        is Route.Profile -> "@$name"
        is Route.Search -> if (query.isEmpty()) "Search" else "Search “$query”"
        is Route.Channel -> id
        is Route.Dm -> "@$user"
        is Route.Org -> name
        else -> RouteCatalog.title(kind)
    }

val InboxFilter.label: String
    get() = when (this) {
        InboxFilter.All -> "All"
        InboxFilter.Pings -> "Pings"
        InboxFilter.Reviews -> "Reviews"
        InboxFilter.Assigned -> "Assigned"
        InboxFilter.Mentions -> "Mentions"
        InboxFilter.Approvals -> "Approvals"
    }

val InboxFilter.emptyMessage: String
    get() = when (this) {
        InboxFilter.All -> "Nothing needs you right now."
        InboxFilter.Pings -> "No pings."
        InboxFilter.Reviews -> "No pull requests are waiting for you."
        InboxFilter.Assigned -> "Nothing is assigned to you."
        InboxFilter.Mentions -> "No one has mentioned you."
        InboxFilter.Approvals -> "No agent is waiting for your approval."
    }

object RouteCatalog {
    fun title(kind: Route.Kind): String = when (kind) {
        Route.Kind.Agents -> "Agents"
        Route.Kind.Agent, Route.Kind.LaunchedAgent -> "Agent"
        Route.Kind.Approval -> "Approval"
        Route.Kind.Compose -> "New agent"
        Route.Kind.AgentLogin -> "Provider sign-in"
        Route.Kind.AgentWatch -> "Watch"
        Route.Kind.AllAgents -> "All agents"
        Route.Kind.Teammates -> "Special Agents"
        Route.Kind.Teammate -> "Special Agent"
        Route.Kind.TeammateEditor -> "Edit special agent"
        Route.Kind.Shortcuts -> "Shortcuts"
        Route.Kind.Inbox -> "Inbox"
        Route.Kind.PingSettings -> "Ping settings"
        Route.Kind.Repos -> "Repositories"
        Route.Kind.Repo -> "Repository"
        Route.Kind.Tree -> "Code"
        Route.Kind.File -> "File"
        Route.Kind.FileHistory -> "File history"
        Route.Kind.CodeMap -> "Code map"
        Route.Kind.Commits -> "Commits"
        Route.Kind.Commit -> "Commit"
        Route.Kind.Range -> "Range"
        Route.Kind.Compare -> "Compare"
        Route.Kind.Conflicts -> "Conflicts"
        Route.Kind.Issue -> "Issue"
        Route.Kind.NewIssue -> "New issue"
        Route.Kind.Board -> "Board"
        Route.Kind.Pull -> "Pull request"
        Route.Kind.Review -> "Review"
        Route.Kind.Discussion -> "Discussion"
        Route.Kind.Run -> "Run"
        Route.Kind.Release -> "Release"
        Route.Kind.Project -> "Project"
        Route.Kind.Milestones -> "Milestones"
        Route.Kind.RepoSettings -> "Repository settings"
        Route.Kind.LocalRepos -> "On this device"
        Route.Kind.LocalRepo -> "Local repository"
        Route.Kind.QuickEdit -> "Quick edit"
        Route.Kind.Chat -> "Chat"
        Route.Kind.Channel -> "Channel"
        Route.Kind.Thread -> "Thread"
        Route.Kind.Dm -> "Direct message"
        Route.Kind.Discord -> "Discord"
        Route.Kind.Profile -> "Profile"
        Route.Kind.People -> "People"
        Route.Kind.Docs -> "Docs"
        Route.Kind.Search -> "Search"
        Route.Kind.Notes -> "Notes"
        Route.Kind.Note -> "Note"
        Route.Kind.Tasks -> "Tasks"
        Route.Kind.Task -> "Task"
        Route.Kind.Calendar -> "Calendar"
        Route.Kind.Event -> "Event"
        Route.Kind.Timers -> "Timers"
        Route.Kind.Desktops -> "Desktops"
        Route.Kind.Desktop -> "Desktop"
        Route.Kind.Hosts -> "Hosts & fleet"
        Route.Kind.Host -> "Host"
        Route.Kind.Provision -> "Provisioning"
        Route.Kind.Network -> "Network & nodes"
        Route.Kind.Node -> "Node"
        Route.Kind.Market -> "Market"
        Route.Kind.Package -> "Package"
        Route.Kind.Crm -> "CRM"
        Route.Kind.Contact -> "Contact"
        Route.Kind.CrmOrg -> "CRM organization"
        Route.Kind.Marketing -> "Marketing"
        Route.Kind.Analytics -> "Analytics"
        Route.Kind.Competitors -> "Competitors"
        Route.Kind.Ads -> "Ads"
        Route.Kind.Ledger -> "Ledger & tips"
        Route.Kind.Vault -> "Vault"
        Route.Kind.Coves -> "Coves"
        Route.Kind.Cove -> "Cove"
        Route.Kind.Calories -> "Calories"
        Route.Kind.GlassesAlert -> "Glasses Alert"
        Route.Kind.FaceChanger -> "Face Changer"
        Route.Kind.LocalAI -> "On-device AI"
        Route.Kind.Activity -> "Activity"
        Route.Kind.Stats -> "Stats"
        Route.Kind.Status -> "Status"
        Route.Kind.Admin -> "Admin"
        Route.Kind.Settings -> "Settings"
        Route.Kind.Orgs -> "Organizations"
        Route.Kind.Org -> "Organization"
        Route.Kind.InstalledApps -> "Installed apps"
        Route.Kind.Projects -> "Projects"
        Route.Kind.ProjectAccess -> "Agent access"
        Route.Kind.Tour -> "Tour"
        Route.Kind.UpdateRequired -> "Update required"
    }

    fun icon(kind: Route.Kind): String = when (kind) {
        Route.Kind.Agents, Route.Kind.Agent, Route.Kind.LaunchedAgent, Route.Kind.AllAgents, Route.Kind.Compose ->
            "auto_awesome"
        Route.Kind.Approval -> "verified_user"
        Route.Kind.AgentLogin -> "terminal"
        Route.Kind.AgentWatch -> "graphic_eq"
        Route.Kind.Teammates, Route.Kind.Teammate, Route.Kind.TeammateEditor -> "group"
        Route.Kind.Shortcuts -> "keyboard_command_key"
        Route.Kind.Inbox -> "inbox"
        Route.Kind.PingSettings -> "notifications_active"
        Route.Kind.Repos, Route.Kind.Repo, Route.Kind.LocalRepos, Route.Kind.LocalRepo -> "folder"
        Route.Kind.Tree, Route.Kind.File, Route.Kind.FileHistory, Route.Kind.CodeMap, Route.Kind.QuickEdit -> "description"
        Route.Kind.Commits, Route.Kind.Commit, Route.Kind.Range -> "history"
        Route.Kind.Compare, Route.Kind.Conflicts -> "call_split"
        Route.Kind.Issue, Route.Kind.NewIssue -> "adjust"
        Route.Kind.Board -> "view_week"
        Route.Kind.Pull, Route.Kind.Review -> "merge"
        Route.Kind.Discussion -> "forum"
        Route.Kind.Run -> "play_circle"
        Route.Kind.Release -> "inventory_2"
        Route.Kind.Project, Route.Kind.Milestones -> "flag"
        Route.Kind.RepoSettings, Route.Kind.Settings -> "settings"
        Route.Kind.Chat, Route.Kind.Channel, Route.Kind.Thread -> "chat"
        Route.Kind.Dm -> "mail"
        Route.Kind.Discord -> "sports_esports"
        Route.Kind.Profile -> "account_circle"
        Route.Kind.People -> "groups"
        Route.Kind.Docs -> "library_books"
        Route.Kind.Search -> "search"
        Route.Kind.Notes, Route.Kind.Note -> "sticky_note_2"
        Route.Kind.Tasks, Route.Kind.Task -> "task_alt"
        Route.Kind.Calendar, Route.Kind.Event -> "calendar_month"
        Route.Kind.Timers -> "timer"
        Route.Kind.Desktops, Route.Kind.Desktop -> "desktop_windows"
        Route.Kind.Hosts, Route.Kind.Host, Route.Kind.Provision -> "dns"
        Route.Kind.Network, Route.Kind.Node -> "hub"
        Route.Kind.Market, Route.Kind.Package -> "shopping_bag"
        Route.Kind.Crm, Route.Kind.Contact, Route.Kind.CrmOrg -> "contacts"
        Route.Kind.Marketing -> "campaign"
        Route.Kind.Analytics -> "show_chart"
        Route.Kind.Competitors -> "travel_explore"
        Route.Kind.Ads -> "ads_click"
        Route.Kind.Ledger -> "account_balance_wallet"
        Route.Kind.Vault -> "key"
        Route.Kind.Coves, Route.Kind.Cove -> "shield"
        Route.Kind.Calories -> "restaurant"
        Route.Kind.GlassesAlert -> "visibility"
        Route.Kind.FaceChanger -> "face"
        Route.Kind.LocalAI -> "memory"
        Route.Kind.Activity -> "bolt"
        Route.Kind.Stats -> "bar_chart"
        Route.Kind.Status -> "monitor_heart"
        Route.Kind.Admin -> "build"
        Route.Kind.Orgs, Route.Kind.Org -> "apartment"
        Route.Kind.InstalledApps -> "extension"
        Route.Kind.Projects, Route.Kind.ProjectAccess -> "layers"
        Route.Kind.Tour -> "tour"
        Route.Kind.UpdateRequired -> "system_update"
    }

    fun sectionTitle(section: RepoSection): String = when (section) {
        RepoSection.Pulls -> "Pull requests"
        else -> section.raw.replaceFirstChar { it.uppercase() }
    }
}

data class HubItem(
    val id: String,
    val title: String,
    val icon: String,
    val action: Action,
    val guestAction: String? = null,
    val needsRegistration: Boolean = false,
    val adminOnly: Boolean = false,
) {
    sealed class Action {
        data class Open(val route: Route) : Action()
        data object World : Action()
        data object DesignSystem : Action()
        data object Workflows : Action()
        data object ReportBug : Action()
        data object SetStatus : Action()
        data object SignIn : Action()
        data object SignOut : Action()
    }

    val route: Route? get() = (action as? Action.Open)?.route

    companion object {
        const val WORKFLOWS_PATH = "/dashboard/workflows"

        fun destination(
            route: Route,
            title: String? = null,
            guestAction: String? = null,
            needsRegistration: Boolean = false,
            adminOnly: Boolean = false,
        ): HubItem = HubItem(
            id = route.kind.raw,
            title = title ?: RouteCatalog.title(route.kind),
            icon = RouteCatalog.icon(route.kind),
            action = Action.Open(route),
            guestAction = guestAction,
            needsRegistration = needsRegistration,
            adminOnly = adminOnly,
        )
    }
}

data class HubGroup(val id: String, val title: String, val items: List<HubItem>)

object HubCatalog {
    const val MAX_RECENTS = 5

    val workspace: List<HubItem> = listOf(
        HubItem.destination(Route.Notes, guestAction = "use notes"),
        HubItem.destination(Route.Tasks(null), guestAction = "use tasks"),
        HubItem.destination(Route.Calendar(null, CalendarMode.Month), needsRegistration = true),
        HubItem.destination(Route.Timers, needsRegistration = true),
    )

    val operate: List<HubItem> = listOf(
        HubItem.destination(Route.Desktops, needsRegistration = true),
        HubItem.destination(Route.AllAgents, guestAction = "see your agents"),
        HubItem.destination(Route.Teammates, needsRegistration = true),
        HubItem.destination(Route.Shortcuts(null), needsRegistration = true),
        HubItem.destination(Route.Hosts, guestAction = "manage your hosts"),
        HubItem.destination(Route.Network(null), needsRegistration = true),
    )

    val business: List<HubItem> = listOf(
        HubItem.destination(Route.Market(null), needsRegistration = true),
        HubItem.destination(Route.Crm(null), needsRegistration = true, guestAction = "use the CRM"),
        HubItem.destination(Route.Marketing(null), needsRegistration = true, guestAction = "plan marketing"),
        HubItem.destination(Route.Analytics(null), needsRegistration = true, guestAction = "see product analytics"),
        HubItem.destination(Route.Competitors, needsRegistration = true, guestAction = "keep a competitor watchlist"),
        HubItem.destination(Route.Ads, needsRegistration = true, guestAction = "manage your ads"),
        HubItem.destination(Route.Ledger(null), needsRegistration = true),
    )

    val personal: List<HubItem> = listOf(
        HubItem.destination(Route.Vault(null), guestAction = "open your password vault"),
        HubItem.destination(Route.Coves(null), needsRegistration = true, guestAction = "open your coves"),
        HubItem.destination(Route.Calories, guestAction = "use the calorie counter"),
        HubItem.destination(Route.GlassesAlert),
        HubItem.destination(Route.FaceChanger),
        HubItem.destination(Route.LocalAI, needsRegistration = true),
        HubItem.destination(Route.Activity),
    )

    val insights: List<HubItem> = listOf(
        HubItem.destination(Route.Stats, needsRegistration = true),
        HubItem.destination(Route.Status, needsRegistration = true),
        HubItem.destination(Route.Admin(null), needsRegistration = true, adminOnly = true),
    )

    fun app(guest: Boolean): List<HubItem> = buildList {
        add(HubItem.destination(Route.Settings(null)))
        add(HubItem.destination(Route.Orgs))
        add(HubItem.destination(Route.People))
        add(HubItem.destination(Route.Docs))
        add(HubItem.destination(Route.Projects))
        add(HubItem("setStatus", "Set status", "emoji_emotions", HubItem.Action.SetStatus))
        add(HubItem("reportBug", "Report a bug", "bug_report", HubItem.Action.ReportBug))
        add(HubItem.destination(Route.Tour))
        add(HubItem("world", "World", "public", HubItem.Action.World))
        add(HubItem("designSystem", "Design", "palette", HubItem.Action.DesignSystem))
        add(HubItem("workflows", "Workflows", "account_tree", HubItem.Action.Workflows))
        if (guest) {
            add(HubItem("signIn", "Sign in", "login", HubItem.Action.SignIn))
        } else {
            add(HubItem("signOut", "Sign out", "logout", HubItem.Action.SignOut))
        }
    }

    fun groups(
        guest: Boolean,
        admin: Boolean = false,
        hidden: Set<String> = emptySet(),
        isRegistered: (Route.Kind) -> Boolean,
    ): List<HubGroup> {
        val all = listOf(
            HubGroup("workspace", "Workspace", workspace),
            HubGroup("operate", "Operate", operate),
            HubGroup("business", "Business", business),
            HubGroup("personal", "Personal", personal),
            HubGroup("insights", "Insights", insights),
            HubGroup("app", "App", app(guest)),
        )
        return all.mapNotNull { group ->
            val items = group.items.filter { item ->
                if (item.adminOnly && !admin) return@filter false
                if (item.id in hidden) return@filter false
                if (guest && item.action == HubItem.Action.SetStatus) return@filter false
                val route = item.route
                !item.needsRegistration || route == null || isRegistered(route.kind)
            }
            if (items.isEmpty()) null else HubGroup(group.id, group.title, items)
        }
    }

    fun pages(groups: List<HubGroup>): List<HubGroup> = groups.mapNotNull { group ->
        val items = group.items.filter {
            it.action is HubItem.Action.Open || it.action == HubItem.Action.World || it.action == HubItem.Action.DesignSystem ||
                it.action == HubItem.Action.Workflows
        }
        if (items.isEmpty()) null else group.copy(items = items)
    }

    val routes: List<Route>
        get() = (workspace + operate + business + personal + insights + app(false)).mapNotNull { it.route }

    fun guestAction(route: Route): String? =
        (workspace + operate + business + personal + insights).firstOrNull { it.route?.kind == route.kind }?.guestAction
}
