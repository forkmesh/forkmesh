package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.api.AccountSettings
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.code.RepoHubSection
import com.forkmesh.nativeapp.navigation.AppTab
import com.forkmesh.nativeapp.navigation.Route

object PackageGate {
    const val CUTOFF = "2026-10-11"
    const val LEGACY_KEY = "packages/legacyProject"
    const val PROJECT_INSTALLS_KEY = "packages/projectInstalls"
    const val STARTER_PACK_KEY = "packages/starterPack"

    const val AGENTS = "agents"
    const val INBOX = "inbox"
    const val CODE = "code"
    const val CHAT = "chat"
    const val BUG_REPORTS = "bug-reports"
    const val ISSUES = "issues"
    const val COMMITS = "commits"
    const val BRANCHES = "branches"
    const val PULL_REQUESTS = "pull-requests"
    const val RELEASES = "releases"
    const val ACTIONS = "actions"
    const val DISCUSSIONS = "discussions"
    const val SIGN_IN = "sign-in"
    const val CRM = "crm"
    const val TOUR = "tour"
    const val WORKFLOWS = "workflows"

    val ORGANIZATION_PACKAGES: Set<String> = setOf(CRM, "marketing", "analytics", "competitor")

    val STARTER: Set<String> = setOf(AGENTS, CHAT, INBOX, CODE, TOUR)

    val HUB_ITEMS: Map<String, String> = mapOf(
        "notes" to "notes", "tasks" to "tasks", "calendar" to "calendar", "timers" to "timers",
        "desktops" to "desktops", "allAgents" to AGENTS, "teammates" to "special-agents", "shortcuts" to "shortcuts",
        "hosts" to "hosts", "network" to "network",
        "crm" to CRM, "marketing" to "marketing", "analytics" to "analytics", "competitors" to "competitor",
        "ledger" to "ledger", "vault" to "vault", "coves" to "coves", "calories" to "calories",
        "glassesAlert" to "glasses-alert", "phoneSecurity" to "phone-security", "openWrtRouter" to "openwrt", "faceChanger" to "face-changer", "appStoreStatus" to "app-store-status", "rssReader" to "rss-reader", "secureDrive" to "secure-drive", "localAI" to "on-device-ai", "activity" to "activity",
        "stats" to "stats", "status" to "status-page", "orgs" to "organizations", "reportBug" to BUG_REPORTS,
        "tour" to "tour", "world" to "world", "people" to "people", "docs" to "docs", "designSystem" to "design-system",
        "signIn" to SIGN_IN, "signOut" to SIGN_IN, "workflows" to WORKFLOWS,
    )

    val ROUTE_KINDS: Map<Route.Kind, String> = buildMap {
        fun put(pkg: String, vararg kinds: Route.Kind) = kinds.forEach { put(it, pkg) }
        put(AGENTS, Route.Kind.Agents, Route.Kind.Agent, Route.Kind.AgentWatch, Route.Kind.AllAgents, Route.Kind.LaunchedAgent, Route.Kind.Compose)
        put(INBOX, Route.Kind.Inbox, Route.Kind.Approval)
        put(CHAT, Route.Kind.Chat, Route.Kind.Channel, Route.Kind.Thread, Route.Kind.Dm, Route.Kind.Discord)
        put(ISSUES, Route.Kind.Issue, Route.Kind.NewIssue, Route.Kind.Board)
        put(PULL_REQUESTS, Route.Kind.Pull, Route.Kind.Review)
        put(COMMITS, Route.Kind.Commits, Route.Kind.Commit)
        put(DISCUSSIONS, Route.Kind.Discussion)
        put(ACTIONS, Route.Kind.Run)
        put(RELEASES, Route.Kind.Release)
        put("notes", Route.Kind.Notes, Route.Kind.Note)
        put("tasks", Route.Kind.Tasks, Route.Kind.Task)
        put("calendar", Route.Kind.Calendar, Route.Kind.Event)
        put("timers", Route.Kind.Timers)
        put("desktops", Route.Kind.Desktops, Route.Kind.Desktop)
        put("special-agents", Route.Kind.Teammates, Route.Kind.Teammate, Route.Kind.TeammateEditor)
        put("shortcuts", Route.Kind.Shortcuts)
        put("hosts", Route.Kind.Hosts, Route.Kind.Host, Route.Kind.Provision)
        put("network", Route.Kind.Network, Route.Kind.Node)
        put(CRM, Route.Kind.Crm, Route.Kind.Contact, Route.Kind.CrmOrg)
        put("marketing", Route.Kind.Marketing)
        put("analytics", Route.Kind.Analytics)
        put("competitor", Route.Kind.Competitors)
        put("ledger", Route.Kind.Ledger)
        put("vault", Route.Kind.Vault)
        put("coves", Route.Kind.Coves, Route.Kind.Cove)
        put("calories", Route.Kind.Calories)
        put("glasses-alert", Route.Kind.GlassesAlert)
        put("phone-security", Route.Kind.PhoneSecurity)
        put("openwrt", Route.Kind.OpenWrtRouter)
        put("face-changer", Route.Kind.FaceChanger)
        put("app-store-status", Route.Kind.AppStoreStatus)
        put("rss-reader", Route.Kind.RssReader)
        put("secure-drive", Route.Kind.SecureDrive)
        put(WORKFLOWS, Route.Kind.Workflows)
        put("people", Route.Kind.People)
        put("docs", Route.Kind.Docs)
        put("on-device-ai", Route.Kind.LocalAI)
        put("activity", Route.Kind.Activity)
        put("stats", Route.Kind.Stats)
        put("status-page", Route.Kind.Status)
        put("organizations", Route.Kind.Orgs, Route.Kind.Org)
        put("tour", Route.Kind.Tour)
    }

    val TABS: Map<AppTab, String> = mapOf(AppTab.Agents to AGENTS, AppTab.Inbox to INBOX, AppTab.Chat to CHAT)

    val REPO_SECTIONS: Map<RepoHubSection, String> = mapOf(
        RepoHubSection.Pulls to PULL_REQUESTS, RepoHubSection.MergeQueue to PULL_REQUESTS,
        RepoHubSection.Issues to ISSUES, RepoHubSection.Board to ISSUES, RepoHubSection.Backlog to ISSUES,
        RepoHubSection.Commits to COMMITS, RepoHubSection.Branches to BRANCHES, RepoHubSection.Actions to ACTIONS,
        RepoHubSection.Releases to RELEASES, RepoHubSection.Discussions to DISCUSSIONS, RepoHubSection.Agents to AGENTS,
    )

    val GATED: Set<String> = HUB_ITEMS.values.toSet() + ROUTE_KINDS.values + TABS.values + REPO_SECTIONS.values + CODE

    val EVIDENCE_KEYS: List<String> = listOf(
        AccountPackagesStore.STATE_KEY,
        AccountPackagesStore.CATALOG_KEY,
        StatusMonitorsStore.CACHE_KEY,
        "nav/tour.v1",
        "nav/archive.v1",
        AccountSettings.DESIGN_SYSTEM_ON_KEY,
        AccountSettings.TOUR_ON_KEY,
        AccountSettings.DISPLAY_NAME_KEY,
        AccountSettings.ROOM_KEY,
        AccountSettings.SERVE_AS_NODE_KEY,
    )

    fun legacyProject(prefs: PrefStore?, earlierUse: () -> Boolean = { false }): Boolean {
        if (prefs == null) return true
        prefs.getBool(LEGACY_KEY)?.let { return it }
        val legacy = EVIDENCE_KEYS.any(prefs::contains) || earlierUse()
        prefs.putBool(LEGACY_KEY, legacy)
        return legacy
    }

    fun markNewProject(prefs: PrefStore) = prefs.putBool(LEGACY_KEY, false)

    fun organizationInstalled(states: List<OrgPackageState>): Set<String> =
        states.flatMap { state -> state.plugins.filter { it.installed }.map { it.id } }.filter { it in ORGANIZATION_PACKAGES }.toSet()

    fun starterPack(prefs: PrefStore?): StarterPack = StarterPack.fromId(prefs?.getString(STARTER_PACK_KEY))

    fun chooseStarterPack(pack: StarterPack, prefs: PrefStore?) {
        prefs?.putString(STARTER_PACK_KEY, pack.id)
    }

    fun needsStarterPack(prefs: PrefStore?, legacy: Boolean): Boolean =
        prefs != null && !legacy && prefs.getString(STARTER_PACK_KEY) == null

    fun fallback(id: String, entry: AccountCatalogEntry?, legacy: Boolean, pack: StarterPack = StarterPack.FALLBACK): Boolean = when {
        !legacy && id in pack.packages -> true
        entry == null -> legacy
        entry.isDefault -> true
        entry.installedBefore.isNotEmpty() -> legacy
        else -> false
    }
}

enum class StarterPack(val id: String, val label: String, val summary: String) {
    MINIMAL("minimal", "Minimal", "Just your code. No agents, chat or inbox; add more from the Market later."),
    STANDARD("standard", "Standard", "Agents, Chat, Inbox, Code and the tour. A good place to start."),
    EVERYTHING("everything", "Everything", "Every package on the phone, for people who want it all at once.");

    val packages: Set<String>
        get() = when (this) {
            MINIMAL -> setOf(PackageGate.CODE)
            STANDARD -> PackageGate.STARTER
            EVERYTHING -> PackageGate.GATED - PackageGate.ORGANIZATION_PACKAGES
        }

    companion object {
        val FALLBACK = STANDARD

        fun fromId(id: String?): StarterPack = values().firstOrNull { it.id == id } ?: FALLBACK
    }
}

data class PackageSurfaces(val removed: Set<String> = emptySet()) {
    fun shows(packageId: String?): Boolean = packageId == null || packageId !in removed

    val hubItems: Set<String> get() = PackageGate.HUB_ITEMS.filterValues { it in removed }.keys

    val routeKinds: Set<Route.Kind> get() = PackageGate.ROUTE_KINDS.filterValues { it in removed }.keys

    val tabs: Set<AppTab> get() = PackageGate.TABS.filterValues { it in removed }.keys

    val repoSections: Set<RepoHubSection> get() = PackageGate.REPO_SECTIONS.filterValues { it in removed }.keys

    fun showsTab(tab: AppTab): Boolean = shows(PackageGate.TABS[tab])

    fun showsRoute(route: Route): Boolean = shows(PackageGate.ROUTE_KINDS[route.kind])

    fun showsRepoSection(section: RepoHubSection): Boolean = shows(PackageGate.REPO_SECTIONS[section])

    val agents: Boolean get() = shows(PackageGate.AGENTS)

    val code: Boolean get() = shows(PackageGate.CODE)

    val signIn: Boolean get() = shows(PackageGate.SIGN_IN)

    companion object {
        val ALL = PackageSurfaces()

        fun of(installed: (String) -> Boolean): PackageSurfaces = PackageSurfaces(PackageGate.GATED.filterNot(installed).toSet())
    }
}
