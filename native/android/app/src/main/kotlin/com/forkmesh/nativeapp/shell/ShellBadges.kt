package com.forkmesh.nativeapp.shell

import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.account.projects.ProjectScope
import com.forkmesh.nativeapp.ci.ApiCiClient
import com.forkmesh.nativeapp.code.ApiRepoOverviewClient
import com.forkmesh.nativeapp.code.RepoHubSection
import com.forkmesh.nativeapp.code.RepoOverviewModel
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountEventChannel
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.code.CodeFeature
import com.forkmesh.nativeapp.feature.rssreader.RssReaderFeature
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.navigation.RepoRef
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.tasks.TaskBoard
import com.forkmesh.nativeapp.tasks.TasksApi
import com.forkmesh.nativeapp.tools.loadBugReportInbox
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.flow.updateAndGet
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.util.concurrent.atomic.AtomicBoolean

object ShellBadges {
    val repoSections: List<RepoHubSection> = listOf(
        RepoHubSection.Pulls,
        RepoHubSection.Issues,
        RepoHubSection.Commits,
        RepoHubSection.Branches,
        RepoHubSection.Releases,
        RepoHubSection.Actions,
        RepoHubSection.Discussions,
    )

    private val countsState = MutableStateFlow(ShellBadgeCounts())
    private val repoState = MutableStateFlow(ShellRepoCounts())
    private val rssState = MutableStateFlow(0)
    private val pendingState = MutableStateFlow<Set<String>>(emptySet())
    private val freshRepos = java.util.Collections.synchronizedSet(HashSet<String>())
    private val tasksLoading = AtomicBoolean(false)
    private var rssJob: Job? = null

    val counts: StateFlow<ShellBadgeCounts> = countsState.asStateFlow()

    val repoCounts: StateFlow<ShellRepoCounts> = repoState.asStateFlow()

    val rssUnread: StateFlow<Int> = rssState.asStateFlow()

    const val REPO = "repo"
    const val TASKS = "tasks"

    val pending: StateFlow<Set<String>> = pendingState.asStateFlow()

    fun install(app: App) {
        countsState.value = ShellBadgeCounts()
        repoState.value = ShellRepoCounts()
        pendingState.value = emptySet()
        freshRepos.clear()
        CodeFeature.currentRepo.value?.let(::restoreRepoCounts)
        rssState.value = 0
        ShellPullsWatch.reset()
        rssJob?.cancel()
        rssJob = AppLifecycle.scope.launch(Dispatchers.Default) {
            val service = runCatching { RssReaderFeature.service }.getOrNull() ?: return@launch
            service.version.collect { version ->
                if (version >= 0) rssState.value = service.library?.counts(System.currentTimeMillis())?.unread ?: 0
            }
        }
        val channel = Services.get<AccountEventChannel>() ?: return
        val previous = channel.onTopic
        channel.onTopic = { topic ->
            previous?.invoke(topic)
            if (topic == "pings") AppLifecycle.scope.launch { refreshTasks() }
            if (topic == "pulls") ShellPullsWatch.ring()
        }
        val previousConnected = channel.onConnected
        channel.onConnected = {
            previousConnected?.invoke()
            AppLifecycle.scope.launch { ShellPullsWatch.refresh() }
        }
    }

    fun adoptTasks(board: TaskBoard) {
        countsState.update { it.adoptTasks(board) }
    }

    fun adoptBugReports(counts: Map<String, Int>) {
        countsState.update { it.adoptBugReports(counts) }
    }

    fun adoptRepoCounts(repo: RepoRef, counts: Map<RepoHubSection, Int>) {
        val next = repoState.updateAndGet { it.adopt(repo, counts) }
        if (next.knows(repo)) prefs()?.putString(ShellRepoCounts.key(repo), ShellRepoCounts.encode(next.counts))
    }

    private fun prefs(): PrefStore? {
        val base = Services.get<PrefStore>() ?: return null
        return Services.get<ProjectScope>()?.prefs(base) ?: base
    }

    private fun restoreRepoCounts(repo: RepoRef) {
        val saved = prefs()?.getString(ShellRepoCounts.key(repo)) ?: return
        repoState.update { it.restored(repo, saved) }
    }

    private suspend fun tracked(key: String, block: suspend () -> Unit) {
        pendingState.update { it + key }
        try {
            block()
        } finally {
            pendingState.update { it - key }
        }
    }

    private fun signedIn(): Boolean {
        val auth = Services.get<AuthService>() ?: return false
        return !auth.isGuest.value && auth.isAuthenticated
    }

    suspend fun refreshTasks() {
        val api = Services.get<ApiService>() ?: return
        if (!signedIn() || !tasksLoading.compareAndSet(false, true)) return
        try {
            tracked(TASKS) {
                val board = withContext(Dispatchers.IO) { runCatching { TasksApi(api).board() }.getOrNull() } ?: return@tracked
                adoptTasks(board)
            }
        } finally {
            tasksLoading.set(false)
        }
    }

    suspend fun refreshBugReports() {
        val api = Services.get<ApiService>() ?: return
        if (Services.get<AuthService>()?.session?.value?.isAdmin != true) return
        val inbox = withContext(Dispatchers.IO) { loadBugReportInbox(api, "new", 0, 1) }
        if (inbox.ok) adoptBugReports(inbox.counts)
    }

    suspend fun refreshRepoCounts(force: Boolean = false) {
        val repo = CodeFeature.currentRepo.value ?: return
        val api = Services.get<ApiService>() ?: return
        restoreRepoCounts(repo)
        val slug = repo.slug.lowercase()
        if (force) freshRepos.remove(slug)
        if (slug in freshRepos) return
        tracked(REPO) {
            val overview = RepoOverviewModel(Repository(repo.owner, repo.name), ApiRepoOverviewClient(api))
            val found = coroutineScope {
                RepoHubSection.counted.map { section -> async(Dispatchers.IO) { section to overview.count(section) } }.awaitAll()
            }.mapNotNull { (section, count) -> count?.let { section to it } }.toMap()
            if (CodeFeature.currentRepo.value != repo) return@tracked
            if (found.isNotEmpty()) {
                adoptRepoCounts(repo, found)
                freshRepos.add(slug)
            }
            val more = coroutineScope {
                val runs = async(Dispatchers.IO) {
                    runCatching { ApiCiClient(api).actionsRuns(com.forkmesh.nativeapp.ci.RepoRef(repo.owner, repo.name)).runs.size }.getOrNull()
                }
                val discussions = async(Dispatchers.IO) { runCatching { api.publishedDiscussions(repo.owner, repo.name).size }.getOrNull() }
                listOfNotNull(runs.await()?.let { RepoHubSection.Actions to it }, discussions.await()?.let { RepoHubSection.Discussions to it }).toMap()
            }
            if (CodeFeature.currentRepo.value == repo) adoptRepoCounts(repo, more)
        }
    }

    suspend fun refreshHome(rssShown: Boolean) {
        coroutineScope {
            launch { refreshTasks() }
            launch { refreshBugReports() }
            if (rssShown) runCatching { RssReaderFeature.service.open() }
        }
    }

    suspend fun refreshEverything(rssShown: Boolean) {
        coroutineScope {
            launch { refreshTasks() }
            launch { refreshBugReports() }
            launch { refreshRepoCounts(force = true) }
            launch { ShellPullsWatch.refresh() }
            launch { com.forkmesh.nativeapp.feature.market.PackageGateFeature.refreshOrganizations() }
            launch { com.forkmesh.nativeapp.feature.insights.StatusPageFeature.refreshAccount() }
            launch { runCatching { com.forkmesh.nativeapp.feature.inbox.InboxFeature.refresh() } }
            if (rssShown) runCatching { RssReaderFeature.service.refresh(force = true) }
        }
    }
}
