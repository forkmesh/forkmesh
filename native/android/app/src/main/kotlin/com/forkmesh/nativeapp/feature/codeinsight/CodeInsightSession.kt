package com.forkmesh.nativeapp.feature.codeinsight

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.codeinsight.Insight
import com.forkmesh.nativeapp.codeinsight.InsightAnalysisCopy
import com.forkmesh.nativeapp.codeinsight.InsightGitSource
import com.forkmesh.nativeapp.codeinsight.InsightHistoryDepth
import com.forkmesh.nativeapp.codeinsight.InsightLabels
import com.forkmesh.nativeapp.codeinsight.InsightSnapshot
import com.forkmesh.nativeapp.codeinsight.InsightSnapshotCache
import com.forkmesh.nativeapp.codeinsight.InsightTarget
import com.forkmesh.nativeapp.codeinsight.InsightTrendPoint
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.localrepos.LocalReposFeature
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.security.GitCancellationToken
import com.forkmesh.nativeapp.localrepos.GitWorkspace
import com.forkmesh.nativeapp.localrepos.LocalRepoStore
import com.forkmesh.nativeapp.localrepos.PublishService
import com.forkmesh.nativeapp.localrepos.describeLocalRepoFailure
import com.forkmesh.nativeapp.model.RepoMirror
import java.io.File
import kotlin.coroutines.cancellation.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

data class CodeInsightState(
    val snapshot: InsightSnapshot? = null,
    val sourceLabel: String = "",
    val running: Boolean = false,
    val stage: String = "",
    val fraction: Float? = null,
    val error: String? = null,
    val warning: String? = null,
)

object CodeInsightSession {
    private class Entry(val target: InsightTarget) {
        val state = MutableStateFlow(CodeInsightState())
        var job: Job? = null
        var token: GitCancellationToken? = null
        var holders = 0
        var generation = 0
        var source: InsightGitSource? = null
        val trends = HashMap<String, Deferred<List<InsightTrendPoint>>>()
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val entries = HashMap<String, Entry>()
    private val cache = InsightSnapshotCache<String>()
    private val depthState = MutableStateFlow(InsightHistoryDepth.DEFAULT)

    @Volatile
    var cacheRoot: File? = null

    val depth: StateFlow<InsightHistoryDepth> get() = depthState

    fun install(cacheDir: File) {
        cacheRoot = File(cacheDir, "CodeInsight")
        depthState.value = InsightHistoryDepth.fromId(prefs()?.getString(InsightHistoryDepth.PREF_KEY))
    }

    private fun prefs(): PrefStore? = Services.get<PrefStore>()

    fun lastTarget(): InsightTarget? = InsightTarget.parse(prefs()?.getString(InsightTarget.LAST_KEY))

    @Synchronized
    private fun entry(target: InsightTarget): Entry = entries.getOrPut(target.key) { Entry(target) }

    fun state(target: InsightTarget): StateFlow<CodeInsightState> = entry(target).state

    @Synchronized
    fun acquire(target: InsightTarget) {
        val entry = entry(target)
        entry.holders++
        val running = entry.job?.isActive == true
        val fresh = cache.fresh(target.key)
        if (fresh != null && entry.state.value.snapshot === fresh) return
        if (fresh != null && !running) {
            entry.state.value = entry.state.value.copy(snapshot = fresh, sourceLabel = InsightLabels.sourceLabel(target, depthState.value))
            return
        }
        if (!running) start(entry)
    }

    @Synchronized
    fun release(target: InsightTarget) {
        val entry = entries[target.key] ?: return
        entry.holders = maxOf(0, entry.holders - 1)
        if (entry.holders == 0 && entry.job?.isActive == true) {
            entry.token?.cancel()
            entry.job?.cancel()
        }
    }

    @Synchronized
    fun reanalyze(target: InsightTarget) {
        val entry = entry(target)
        if (entry.job?.isActive == true) return
        cache.invalidate(target.key)
        start(entry)
    }

    fun setDepth(depth: InsightHistoryDepth, current: InsightTarget?) {
        if (depthState.value == depth) return
        depthState.value = depth
        prefs()?.putString(InsightHistoryDepth.PREF_KEY, depth.id)
        if (current != null) restart(current)
    }

    @Synchronized
    private fun restart(target: InsightTarget) {
        val entry = entry(target)
        entry.token?.cancel()
        entry.job?.cancel()
        cache.invalidate(target.key)
        start(entry)
    }

    suspend fun trend(target: InsightTarget, path: String): List<InsightTrendPoint> {
        val deferred = synchronized(this) {
            val entry = entry(target)
            val source = entry.source ?: return emptyList()
            entry.trends.getOrPut(path) {
                scope.async {
                    try {
                        source.trend(path)
                    } catch (error: CancellationException) {
                        throw error
                    } catch (_: Exception) {
                        emptyList()
                    }
                }
            }
        }
        return deferred.await()
    }

    private fun start(entry: Entry) {
        val token = GitCancellationToken()
        entry.token = token
        val generation = ++entry.generation
        fun update(change: (CodeInsightState) -> CodeInsightState) {
            synchronized(entry) { if (entry.generation == generation) entry.state.value = change(entry.state.value) }
        }
        val depth = depthState.value
        val target = entry.target
        val label = InsightLabels.sourceLabel(target, depth)
        update { it.copy(running = true, stage = "Preparing…", fraction = null, error = null, warning = null, sourceLabel = label) }
        entry.job = scope.launch {
            try {
                var warning: String? = null
                val source = when (target) {
                    is InsightTarget.Local -> openLocal(target)
                    is InsightTarget.Account -> {
                        val opened = openCopy(target, depth, token) { stage -> update { it.copy(stage = stage, fraction = null) } }
                        warning = opened.second
                        opened.first
                    }
                }
                val context = currentCoroutineContext()
                val snapshot = Insight.analyze(
                    source,
                    depth.options,
                    progress = { text, fraction -> update { it.copy(stage = text, fraction = fraction.toFloat()) } },
                    isCancelled = { token.isCancelled || !context.isActive },
                )
                if (entry.generation != generation) return@launch
                synchronized(this@CodeInsightSession) {
                    entry.source = source
                    entry.trends.clear()
                }
                if (snapshot.error.isEmpty()) {
                    cache.put(target.key, snapshot)
                    prefs()?.putString(InsightTarget.LAST_KEY, target.key)
                }
                val done = CodeInsightState(
                    snapshot = snapshot,
                    sourceLabel = label,
                    error = snapshot.error.ifEmpty { null },
                    warning = listOfNotNull(warning, snapshot.warning.ifEmpty { null }).joinToString("\n").ifEmpty { null },
                )
                update { done }
            } catch (error: CancellationException) {
                update { it.copy(running = false, stage = "", fraction = null) }
            } catch (error: Throwable) {
                update { it.copy(running = false, stage = "", fraction = null, error = describe(error)) }
            }
        }
    }

    private fun openLocal(target: InsightTarget.Local): InsightGitSource {
        val store = Services.get<LocalRepoStore>() ?: throw IllegalStateException("Local repositories are not available in this project.")
        store.ensureLoaded()
        val record = store.find(target.name) ?: throw IllegalStateException("${target.name} is no longer on this phone.")
        return InsightGitSource.open(File(record.path), record.name, record.path, readWorkingFiles = true)
    }

    private fun openCopy(
        target: InsightTarget.Account,
        depth: InsightHistoryDepth,
        token: GitCancellationToken,
        onStage: (String) -> Unit,
    ): Pair<InsightGitSource, String?> {
        val root = cacheRoot ?: throw IllegalStateException("The analysis cache is not ready.")
        val copies = InsightAnalysisCopy(root)
        val directory = copies.directoryFor(target.owner, target.name)
        var warning: String? = null
        val publisher = Services.get<PublishService>() ?: throw IllegalStateException("Sign in to analyze ${target.slug}.")
        val session = LocalReposFeature.session()
        val relay = publisher.credentials(target.owner, target.name, signed = session != null && session.nodeName.isNotEmpty())
        onStage("Finding mirrors of ${target.slug}")
        var mirrors = emptyList<RepoMirror>()
        var lookupProblem: String? = null
        try {
            mirrors = publisher.api.mirrorSet(target.owner, target.name).mirrors
        } catch (error: Exception) {
            lookupProblem = describe(error)
        }
        try {
            copies.sync(target.owner, target.name, relay, mirrors, depth.commits, lookupProblem, token, onStage)
        } catch (error: CancellationException) {
            throw error
        } catch (error: Exception) {
            if (token.isCancelled) throw CancellationException("Analysis of ${target.slug} was canceled.")
            if (!copies.exists(target.owner, target.name)) throw error
            warning = "Could not fetch ${target.slug}, so this is the copy fetched earlier. ${describe(error)}"
        }
        val probe = InsightGitSource.open(directory, target.name, target.slug)
        val source = InsightGitSource(probe.repository, target.name, target.slug, InsightAnalysisCopy.tipOf(probe.repository), probe.branch)
        return source to warning
    }

    fun describe(error: Throwable): String = when (error) {
        is GitException -> GitWorkspace.describe(error)
        is IllegalStateException -> error.message ?: error.toString()
        else -> describeLocalRepoFailure(error)
    }
}
