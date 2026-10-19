package com.forkmesh.nativeapp.navigation

data class RepoRef(val owner: String, val name: String) {
    val slug: String get() = "$owner/$name"

    override fun toString(): String = slug

    companion object {
        fun parse(slug: String?): RepoRef? {
            val parts = (slug ?: return null).split('/')
            if (parts.size != 2 || parts[0].isEmpty() || parts[1].isEmpty()) return null
            return RepoRef(parts[0], parts[1])
        }
    }
}

enum class RepoSection(val raw: String) {
    Code("code"), Issues("issues"), Pulls("pulls"), Discussions("discussions"), Actions("actions"),
    Agents("agents"), Worktrees("worktrees"), Releases("releases"), Branches("branches"), Commits("commits"),
    Projects("projects"), Security("security"), Insights("insights"), Quality("quality"), Mirrors("mirrors"),
    Artifacts("artifacts"), Settings("settings"), About("about");

    companion object {
        fun of(raw: String?): RepoSection? = entries.firstOrNull { it.raw == raw }
    }
}

enum class PullTab(val raw: String) {
    Conversation("conversation"), Commits("commits"), Files("files"), Checks("checks");

    companion object {
        fun of(raw: String?): PullTab? = entries.firstOrNull { it.raw == raw }
    }
}

enum class AgentFilter(val raw: String) {
    All("all"), Desktop("desktop"), Cloud("cloud"), NeedsYou("needsYou"), Running("running"), Queued("queued"),
    Recent("recent");

    companion object {
        fun of(raw: String?): AgentFilter? = entries.firstOrNull { it.raw == raw }
    }
}

enum class InboxFilter(val raw: String) {
    All("all"), Pings("pings"), Reviews("reviews"), Assigned("assigned"), Mentions("mentions"), Approvals("approvals");

    companion object {
        fun of(raw: String?): InboxFilter? = entries.firstOrNull { it.raw == raw }
    }
}

enum class RepoScope(val raw: String) {
    All("all"), Pinned("pinned"), Mine("mine"), Organizations("organizations"), Network("network"), OnDevice("onDevice");

    companion object {
        fun of(raw: String?): RepoScope? = entries.firstOrNull { it.raw == raw }
    }
}

enum class SearchScope(val raw: String) {
    Everything("everything"), Repos("repos"), Code("code"), Issues("issues"), Pulls("pulls"), People("people"),
    Notes("notes"), Commands("commands"), Chat("chat");

    companion object {
        fun of(raw: String?): SearchScope? = entries.firstOrNull { it.raw == raw }
    }
}

enum class CalendarMode(val raw: String) {
    Day("day"), Week("week"), Month("month"), Agenda("agenda");

    companion object {
        fun of(raw: String?): CalendarMode? = entries.firstOrNull { it.raw == raw }
    }
}

data class ComposeTarget(
    val repo: RepoRef? = null,
    val issue: Int? = null,
    val pull: Int? = null,
    val agentId: String? = null,
    val channel: String? = null,
    val text: String? = null,
)

enum class AppTab(val raw: String) {
    Agents("agents"), Inbox("inbox"), Code("code"), Chat("chat");

    companion object {
        fun of(raw: String?): AppTab? = entries.firstOrNull { it.raw == raw }
    }
}

sealed class Route {
    abstract val kind: Kind

    data class Agents(val filter: AgentFilter) : Route() { override val kind get() = Kind.Agents }
    data class Agent(val id: String) : Route() { override val kind get() = Kind.Agent }
    data class Approval(val id: String) : Route() { override val kind get() = Kind.Approval }
    data class Compose(val target: ComposeTarget) : Route() { override val kind get() = Kind.Compose }
    data class AgentLogin(val target: String) : Route() { override val kind get() = Kind.AgentLogin }
    data class AgentWatch(val filter: AgentFilter) : Route() { override val kind get() = Kind.AgentWatch }
    data object AllAgents : Route() { override val kind get() = Kind.AllAgents }
    data class LaunchedAgent(val repo: RepoRef, val sessionId: String) : Route() { override val kind get() = Kind.LaunchedAgent }
    data object Teammates : Route() { override val kind get() = Kind.Teammates }
    data class Teammate(val id: String) : Route() { override val kind get() = Kind.Teammate }
    data class TeammateEditor(val id: String?) : Route() { override val kind get() = Kind.TeammateEditor }
    data class Shortcuts(val repo: RepoRef?) : Route() { override val kind get() = Kind.Shortcuts }
    data class Inbox(val filter: InboxFilter) : Route() { override val kind get() = Kind.Inbox }
    data object PingSettings : Route() { override val kind get() = Kind.PingSettings }
    data class Repos(val scope: RepoScope) : Route() { override val kind get() = Kind.Repos }
    data class Repo(val repo: RepoRef, val section: RepoSection?) : Route() { override val kind get() = Kind.Repo }
    data class Tree(val repo: RepoRef, val ref: String, val path: String) : Route() { override val kind get() = Kind.Tree }
    data class File(val repo: RepoRef, val ref: String, val path: String) : Route() { override val kind get() = Kind.File }
    data class FileHistory(val repo: RepoRef, val ref: String, val path: String) : Route() { override val kind get() = Kind.FileHistory }
    data class CodeMap(val repo: RepoRef, val ref: String) : Route() { override val kind get() = Kind.CodeMap }
    data class Commits(val repo: RepoRef, val ref: String) : Route() { override val kind get() = Kind.Commits }
    data class Commit(val repo: RepoRef, val sha: String) : Route() { override val kind get() = Kind.Commit }
    data class Range(val repo: RepoRef, val base: String, val head: String) : Route() { override val kind get() = Kind.Range }
    data class Compare(val repo: RepoRef, val base: String, val head: String) : Route() { override val kind get() = Kind.Compare }
    data class Conflicts(val repo: RepoRef, val branch: String) : Route() { override val kind get() = Kind.Conflicts }
    data class Issue(val repo: RepoRef, val number: Int) : Route() { override val kind get() = Kind.Issue }
    data class NewIssue(val repo: RepoRef) : Route() { override val kind get() = Kind.NewIssue }
    data class Board(val repo: RepoRef) : Route() { override val kind get() = Kind.Board }
    data class Pull(val repo: RepoRef, val number: Int, val tab: PullTab) : Route() { override val kind get() = Kind.Pull }
    data class Review(val repo: RepoRef, val number: Int) : Route() { override val kind get() = Kind.Review }
    data class Discussion(val repo: RepoRef, val number: Int) : Route() { override val kind get() = Kind.Discussion }
    data class Run(val repo: RepoRef, val id: String) : Route() { override val kind get() = Kind.Run }
    data class Release(val repo: RepoRef, val tag: String) : Route() { override val kind get() = Kind.Release }
    data class Project(val repo: RepoRef, val id: String) : Route() { override val kind get() = Kind.Project }
    data class Milestones(val repo: RepoRef) : Route() { override val kind get() = Kind.Milestones }
    data class RepoSettings(val repo: RepoRef, val section: String?) : Route() { override val kind get() = Kind.RepoSettings }
    data object LocalRepos : Route() { override val kind get() = Kind.LocalRepos }
    data class LocalRepo(val id: String) : Route() { override val kind get() = Kind.LocalRepo }
    data class QuickEdit(val repoId: String, val path: String) : Route() { override val kind get() = Kind.QuickEdit }
    data object Chat : Route() { override val kind get() = Kind.Chat }
    data class Channel(val id: String) : Route() { override val kind get() = Kind.Channel }
    data class Thread(val channel: String, val id: String) : Route() { override val kind get() = Kind.Thread }
    data class Dm(val user: String) : Route() { override val kind get() = Kind.Dm }
    data class Discord(val guild: String, val channel: String?) : Route() { override val kind get() = Kind.Discord }
    data class Profile(val name: String) : Route() { override val kind get() = Kind.Profile }
    data object People : Route() { override val kind get() = Kind.People }
    data object Docs : Route() { override val kind get() = Kind.Docs }
    data class Search(val query: String, val scope: SearchScope) : Route() { override val kind get() = Kind.Search }
    data object Notes : Route() { override val kind get() = Kind.Notes }
    data class Note(val id: String) : Route() { override val kind get() = Kind.Note }
    data class Tasks(val filter: String?) : Route() { override val kind get() = Kind.Tasks }
    data class Task(val id: String) : Route() { override val kind get() = Kind.Task }
    data class Calendar(val date: String?, val mode: CalendarMode) : Route() { override val kind get() = Kind.Calendar }
    data class Event(val id: String) : Route() { override val kind get() = Kind.Event }
    data object Timers : Route() { override val kind get() = Kind.Timers }
    data object Desktops : Route() { override val kind get() = Kind.Desktops }
    data class Desktop(val id: String, val section: String?) : Route() { override val kind get() = Kind.Desktop }
    data object Hosts : Route() { override val kind get() = Kind.Hosts }
    data class Host(val name: String, val section: String?) : Route() { override val kind get() = Kind.Host }
    data class Provision(val id: String) : Route() { override val kind get() = Kind.Provision }
    data class Network(val section: String?) : Route() { override val kind get() = Kind.Network }
    data class Node(val name: String) : Route() { override val kind get() = Kind.Node }
    data class Market(val filter: String?) : Route() { override val kind get() = Kind.Market }
    data class Package(val id: String) : Route() { override val kind get() = Kind.Package }
    data class Crm(val pipeline: String?) : Route() { override val kind get() = Kind.Crm }
    data class Contact(val id: String) : Route() { override val kind get() = Kind.Contact }
    data class CrmOrg(val id: String) : Route() { override val kind get() = Kind.CrmOrg }
    data class Marketing(val section: String?) : Route() { override val kind get() = Kind.Marketing }
    data class Analytics(val section: String?) : Route() { override val kind get() = Kind.Analytics }
    data object Competitors : Route() { override val kind get() = Kind.Competitors }
    data object Ads : Route() { override val kind get() = Kind.Ads }
    data class Ledger(val section: String?) : Route() { override val kind get() = Kind.Ledger }
    data class Vault(val section: String?) : Route() { override val kind get() = Kind.Vault }
    data class Coves(val repo: RepoRef?) : Route() { override val kind get() = Kind.Coves }
    data class Cove(val id: String) : Route() { override val kind get() = Kind.Cove }
    data object Calories : Route() { override val kind get() = Kind.Calories }
    data object GlassesAlert : Route() { override val kind get() = Kind.GlassesAlert }
    data object PhoneSecurity : Route() { override val kind get() = Kind.PhoneSecurity }
    data class Ember(val repo: RepoRef?) : Route() { override val kind get() = Kind.Ember }
    data class Strata(val repo: RepoRef?) : Route() { override val kind get() = Kind.Strata }
    data class Synapse(val repo: RepoRef?) : Route() { override val kind get() = Kind.Synapse }
    data object OpenWrtRouter : Route() { override val kind get() = Kind.OpenWrtRouter }
    data object FaceChanger : Route() { override val kind get() = Kind.FaceChanger }
    data object AppStoreStatus : Route() { override val kind get() = Kind.AppStoreStatus }
    data object Usage : Route() { override val kind get() = Kind.Usage }
    data object RssReader : Route() { override val kind get() = Kind.RssReader }
    data class InstanceFeed(val post: String?) : Route() { override val kind get() = Kind.InstanceFeed }
    data object SecureDrive : Route() { override val kind get() = Kind.SecureDrive }
    data object Files : Route() { override val kind get() = Kind.Files }
    data object Workflows : Route() { override val kind get() = Kind.Workflows }
    data object LocalAI : Route() { override val kind get() = Kind.LocalAI }
    data object Activity : Route() { override val kind get() = Kind.Activity }
    data object Stats : Route() { override val kind get() = Kind.Stats }
    data object Status : Route() { override val kind get() = Kind.Status }
    data class Admin(val section: String?) : Route() { override val kind get() = Kind.Admin }
    data class Settings(val section: String?) : Route() { override val kind get() = Kind.Settings }
    data object Orgs : Route() { override val kind get() = Kind.Orgs }
    data class Org(val name: String) : Route() { override val kind get() = Kind.Org }
    data object InstalledApps : Route() { override val kind get() = Kind.InstalledApps }
    data object Projects : Route() { override val kind get() = Kind.Projects }
    data class ProjectAccess(val id: String) : Route() { override val kind get() = Kind.ProjectAccess }
    data object Tour : Route() { override val kind get() = Kind.Tour }
    data object UpdateRequired : Route() { override val kind get() = Kind.UpdateRequired }

    enum class Kind(val raw: String, val owner: String) {
        Agents("agents", "WS10"), Agent("agent", "WS10"), Approval("approval", "WS10"), Compose("compose", "WS3"),
        AgentLogin("agentLogin", "WS10"), AgentWatch("agentWatch", "WS4"),
        AllAgents("allAgents", "WS7"), LaunchedAgent("launchedAgent", "WS8"),
        Teammates("teammates", "WS17"), Teammate("teammate", "WS17"), TeammateEditor("teammateEditor", "WS17"),
        Shortcuts("shortcuts", "WS17"),
        Inbox("inbox", "WS9"), PingSettings("pingSettings", "WS2"),
        Repos("repos", "WS11"), Repo("repo", "WS11"), Tree("tree", "WS11"), File("file", "WS11"),
        FileHistory("fileHistory", "WS11"), CodeMap("codeMap", "WS11"), Commits("commits", "WS11"),
        Commit("commit", "WS11"), Range("range", "WS11"), Compare("compare", "WS11"), Conflicts("conflicts", "WS11"),
        Issue("issue", "WS11"), NewIssue("newIssue", "WS11"), Board("board", "WS11"), Pull("pull", "WS11"),
        Review("review", "WS11"), Discussion("discussion", "WS11"), Run("run", "WS14"), Release("release", "WS14"),
        Project("project", "WS15"), Milestones("milestones", "WS15"), RepoSettings("repoSettings", "WS11"),
        LocalRepos("localRepos", "WS5"), LocalRepo("localRepo", "WS5"), QuickEdit("quickEdit", "WS5"),
        Chat("chat", "WS13"), Channel("channel", "WS13"), Thread("thread", "WS13"), Dm("dm", "WS13"),
        Discord("discord", "WS13"),
        Profile("profile", "WS18"), People("people", "WS18"), Docs("docs", "existing"), Search("search", "WS8"),
        Notes("notes", "WS15"), Note("note", "WS15"), Tasks("tasks", "WS15"), Task("task", "WS15"),
        Calendar("calendar", "WS15"), Event("event", "WS15"), Timers("timers", "WS15"),
        Desktops("desktops", "WS16"), Desktop("desktop", "WS16"), Hosts("hosts", "WS7"), Host("host", "WS7"),
        Provision("provision", "WS16"), Network("network", "WS20"), Node("node", "WS16"),
        Market("market", "WS19"), Package("package", "WS19"), Crm("crm", "WS19"), Contact("contact", "WS19"),
        CrmOrg("crmOrg", "WS19"), Marketing("marketing", "WS19"), Analytics("analytics", "WS19"),
        Competitors("competitors", "WS19"), Ads("ads", "WS19"), Ledger("ledger", "WS19"),
        Vault("vault", "WS21"), Coves("coves", "WS21"), Cove("cove", "WS21"), Calories("calories", "existing"),
        GlassesAlert("glassesAlert", "existing"), PhoneSecurity("phoneSecurity", "existing"), Ember("ember", "existing"), Strata("strata", "existing"), Synapse("synapse", "existing"), OpenWrtRouter("openWrtRouter", "existing"), FaceChanger("faceChanger", "existing"), AppStoreStatus("appStoreStatus", "WS20"), Usage("usage", "WS10"), RssReader("rssReader", "WS20"), InstanceFeed("instanceFeed", "WS20"), SecureDrive("secureDrive", "WS23"), Files("files", "existing"), Workflows("workflows", "existing"),
        LocalAI("localAI", "existing"), Activity("activity", "WS20"),
        Stats("stats", "WS20"), Status("status", "WS20"), Admin("admin", "WS20"),
        Settings("settings", "WS18"), Orgs("orgs", "existing"), Org("org", "existing"), InstalledApps("installedApps", "WS22"),
        Projects("projects", "WS1"),
        ProjectAccess("projectAccess", "WS1"), Tour("tour", "WS8"), UpdateRequired("updateRequired", "WS8");

        companion object {
            fun of(raw: String?): Kind? = entries.firstOrNull { it.raw == raw }
        }
    }

    val homeTab: AppTab?
        get() = when (kind) {
            Kind.Agents, Kind.Agent, Kind.AgentWatch, Kind.AllAgents, Kind.LaunchedAgent -> AppTab.Agents
            Kind.Approval, Kind.Inbox -> AppTab.Inbox
            Kind.Repos, Kind.Repo, Kind.Tree, Kind.File, Kind.FileHistory, Kind.CodeMap, Kind.Commits, Kind.Commit,
            Kind.Range, Kind.Compare, Kind.Conflicts, Kind.Issue, Kind.NewIssue, Kind.Board, Kind.Pull, Kind.Review,
            Kind.Discussion, Kind.Run, Kind.Release, Kind.Project, Kind.Milestones, Kind.RepoSettings, Kind.LocalRepos,
            Kind.LocalRepo, Kind.QuickEdit -> AppTab.Code
            Kind.Chat, Kind.Channel, Kind.Thread, Kind.Dm, Kind.Discord -> AppTab.Chat
            else -> null
        }

    val isModal: Boolean
        get() = kind == Kind.Compose || kind == Kind.UpdateRequired || kind == Kind.AgentLogin

    val repoRef: RepoRef?
        get() = when (this) {
            is Repo -> repo
            is Tree -> repo
            is File -> repo
            is FileHistory -> repo
            is CodeMap -> repo
            is Commits -> repo
            is Commit -> repo
            is Range -> repo
            is Compare -> repo
            is Conflicts -> repo
            is Issue -> repo
            is NewIssue -> repo
            is Board -> repo
            is Pull -> repo
            is Review -> repo
            is Discussion -> repo
            is Run -> repo
            is Release -> repo
            is Project -> repo
            is Milestones -> repo
            is RepoSettings -> repo
            is LaunchedAgent -> repo
            is Coves -> repo
            is Shortcuts -> repo
            is Ember -> repo
            is Strata -> repo
            is Synapse -> repo
            is Compose -> target.repo
            else -> null
        }
}

val AppTab.root: Route
    get() = when (this) {
        AppTab.Agents -> Route.Agents(AgentFilter.All)
        AppTab.Inbox -> Route.Inbox(InboxFilter.All)
        AppTab.Code -> Route.Repos(RepoScope.All)
        AppTab.Chat -> Route.Chat
    }
