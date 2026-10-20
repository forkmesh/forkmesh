package com.forkmesh.nativeapp.shell

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.code.RepoHubSection
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.code.CodeFeature
import com.forkmesh.nativeapp.model.PublishedPull
import com.forkmesh.nativeapp.navigation.RepoRef
import com.forkmesh.nativeapp.platform.AppLifecycle
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

object ShellPullsWatch {
    private val stateFlow = MutableStateFlow(ShellPulls())
    private val ringFlow = MutableStateFlow(0)
    private val viewing = HashMap<String, Int>()
    private var loading = false
    private var queued = false

    val state: StateFlow<ShellPulls> = stateFlow.asStateFlow()

    val rings: StateFlow<Int> = ringFlow.asStateFlow()

    fun reset() {
        synchronized(this) {
            viewing.clear()
            queued = false
        }
        stateFlow.value = ShellPulls()
    }

    fun ring() {
        ringFlow.update { it + 1 }
        AppLifecycle.scope.launch { refresh() }
    }

    suspend fun refresh() {
        while (true) {
            val repo = CodeFeature.currentRepo.value ?: return
            val api = Services.get<ApiService>() ?: return
            if (!begin()) return
            val pulls = try {
                withContext(Dispatchers.IO) { runCatching { api.publishedPulls(repo.owner, repo.name) }.getOrNull() }
            } catch (cancelled: CancellationException) {
                end()
                throw cancelled
            }
            val again = end()
            if (pulls != null && CodeFeature.currentRepo.value == repo) adopt(repo, pulls)
            if (!again) return
        }
    }

    suspend fun poll(heat: () -> DeviceHeat, lowPower: () -> Boolean) {
        while (true) {
            val every = NewPullsMarker.pollEvery(heat(), lowPower())
            delay(every ?: NewPullsMarker.WARM_POLL_MS)
            if (every != null && AppLifecycle.foreground.value) refresh()
        }
    }

    fun isCurrent(repo: RepoRef): Boolean = CodeFeature.currentRepo.value?.slug.equals(repo.slug, ignoreCase = true)

    fun holds(repo: RepoRef): Boolean = stateFlow.value.holds(repo)

    fun opened(repo: RepoRef) {
        synchronized(this) { viewing.merge(repo.slug.lowercase(), 1, Int::plus) }
    }

    fun closed(repo: RepoRef) {
        val slug = repo.slug.lowercase()
        synchronized(this) {
            val left = (viewing[slug] ?: 0) - 1
            if (left > 0) viewing[slug] = left else viewing.remove(slug)
        }
    }

    fun viewed(repo: RepoRef, pulls: List<PublishedPull>) {
        marker().markSeen(repo.slug, pulls)
        if (isCurrent(repo)) adopt(repo, pulls)
    }

    private fun marker(): NewPullsMarker = NewPullsMarker(Services.get<PrefStore>())

    @Synchronized
    private fun begin(): Boolean {
        if (loading) {
            queued = true
            return false
        }
        loading = true
        return true
    }

    @Synchronized
    private fun end(): Boolean {
        loading = false
        val again = queued
        queued = false
        return again
    }

    private fun adopt(repo: RepoRef, pulls: List<PublishedPull>) {
        val slug = repo.slug.lowercase()
        val marker = marker()
        if (synchronized(this) { viewing.containsKey(slug) }) marker.markSeen(slug, pulls)
        val fresh = marker.newCount(slug, pulls)
        stateFlow.update { it.adopted(repo, pulls, fresh) }
        val known = ShellBadges.repoCounts.value
        if (known.knows(repo)) {
            val open = NewPullsMarker.openCount(pulls)
            if (known.of(repo, RepoHubSection.Pulls) != open) ShellBadges.adoptRepoCounts(repo, mapOf(RepoHubSection.Pulls to open))
        }
    }
}
