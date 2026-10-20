package com.forkmesh.nativeapp.shell

import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.api.ApiService
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
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger

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
    private val loadingState = MutableStateFlow(false)
    private val busy = AtomicInteger(0)
    private val tasksLoading = AtomicBoolean(false)
    private var rssJob: Job? = null

    val counts: StateFlow<ShellBadgeCounts> = countsState.asStateFlow()

    val repoCounts: StateFlow<ShellRepoCounts> = repoState.asStateFlow()

    val rssUnread: StateFlow<Int> = rssState.asStateFlow()

    val loading: StateFlow<Boolean> = loadingState.asStateFlow()

    fun install(app: App) {
        countsState.value = ShellBadgeCounts()
        repoState.value = ShellRepoCounts()
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
        repoState.update { it.adopt(repo, counts) }
    }

    private suspend fun tracked(block: suspend () -> Unit) {
        busy.incrementAndGet()
        loadingState.value = true
        try {
            block()
        } finally {
            if (busy.decrementAndGet() == 0) loadingState.value = false
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
            val board = withContext(Dispatchers.IO) { runCatching { TasksApi(api).board() }.getOrNull() } ?: return
            adoptTasks(board)
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

    suspend fun refreshRepoCounts() {
        val repo = CodeFeature.currentRepo.value ?: return
        val api = Services.get<ApiService>() ?: return
        tracked {
            if (!repoState.value.knows(repo)) {
                val overview = RepoOverviewModel(Repository(repo.owner, repo.name), ApiRepoOverviewClient(api))
                val found = coroutineScope {
                    RepoHubSection.counted.map { section -> async(Dispatchers.IO) { section to overview.count(section) } }.awaitAll()
                }.mapNotNull { (section, count) -> count?.let { section to it } }.toMap()
                if (CodeFeature.currentRepo.value == repo) adoptRepoCounts(repo, found)
            }
            val known = repoState.value
            if (known.of(repo, RepoHubSection.Actions) == null || known.of(repo, RepoHubSection.Discussions) == null) {
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
    }

    suspend fun refreshHome(rssShown: Boolean) {
        tracked {
            coroutineScope {
                launch { refreshTasks() }
                launch { refreshBugReports() }
                if (rssShown) runCatching { RssReaderFeature.service.open() }
            }
        }
    }
}
