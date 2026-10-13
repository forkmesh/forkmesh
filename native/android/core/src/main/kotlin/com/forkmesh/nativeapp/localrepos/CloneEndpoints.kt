package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.transport.GitRemoteUrl
import com.forkmesh.nativeapp.model.RepoMirror
import kotlin.coroutines.cancellation.CancellationException

data class CloneEndpoint(val url: String, val node: String = "", val healthy: Boolean = false, val isRelay: Boolean = false) {
    val host: String get() = forkMeshRemoteHost(url)

    val label: String
        get() = if (isRelay) "the relay ($host)" else host.ifEmpty { node }

    fun credentials(relay: ForkMeshRemoteCredentials): ForkMeshRemoteCredentials = if (isRelay) relay else relay.at(url)
}

data class CloneAttempt(val endpoint: CloneEndpoint, val message: String)

class CloneFailure(
    val repository: String,
    val attempts: List<CloneAttempt>,
    val lookupProblem: String? = null,
) : RuntimeException() {
    override val message: String
        get() {
            val mirrors = attempts.filter { !it.endpoint.isRelay }
            val lines = ArrayList<String>()
            if (mirrors.isEmpty()) {
                lines.add(
                    lookupProblem?.let { "Could not list the mirrors of $repository ($it)." }
                        ?: "No mirror is serving $repository right now.",
                )
            } else {
                lines.add("Could not clone $repository from any of its ${attempts.size} sources.")
            }
            for (attempt in attempts) lines.add("• ${attempt.endpoint.label}: ${attempt.message}")
            return lines.joinToString("\n")
        }
}

object CloneEndpoints {
    fun plan(owner: String, name: String, mirrors: List<RepoMirror>, relay: ForkMeshRemoteCredentials): List<CloneEndpoint> {
        val seen = HashSet<String>()
        val candidates = ArrayList<Pair<RepoMirror, String>>()
        for (mirror in mirrors) {
            if (mirror.abuseBlocked || !(mirror.cloneAvailable || mirror.online)) continue
            val url = mirrorUrl(mirror, owner, name) ?: continue
            if (seen.add(url.lowercase())) candidates.add(mirror to url)
        }
        fun latency(mirror: RepoMirror): Long = if (mirror.latencyMs > 0) mirror.latencyMs else Long.MAX_VALUE
        val sorted = candidates.sortedWith(
            compareBy<Pair<RepoMirror, String>> { !it.first.isHealthy }
                .thenBy { it.first.behind }
                .thenBy { latency(it.first) }
                .thenByDescending { it.first.lastSyncMs }
                .thenBy { it.second },
        )
        val out = sorted.map { (mirror, url) ->
            CloneEndpoint(url = url, node = mirror.node.ifEmpty { mirror.owner }, healthy = mirror.isHealthy)
        }.toMutableList()
        if (relay.origin.isNotEmpty() && seen.add(relay.relayUrl.lowercase())) {
            out.add(CloneEndpoint(url = relay.relayUrl, isRelay = true))
        }
        return out
    }

    fun mirrorUrl(mirror: RepoMirror, owner: String, name: String): String? {
        var url = mirror.mirrorUrl
        if (url.isEmpty() && mirror.endpoint.isNotEmpty()) {
            url = "${mirror.endpoint.trimEnd('/')}/$owner/$name.git"
        }
        if (url.isEmpty()) return null
        return try {
            GitRemoteUrl.parse(url)
            url
        } catch (error: Exception) {
            null
        }
    }

    fun isFinal(error: Throwable): Boolean {
        if (error is CancellationException || error is LocalRepoStoreException) return true
        if (error is GitWorkspaceException) {
            return error.code == GitErrorCode.cancelled || error.code == GitErrorCode.resourceLimitExceeded
        }
        if (error is GitException) {
            return error.code == GitErrorCode.cancelled || error.code == GitErrorCode.resourceLimitExceeded
        }
        return false
    }

    fun <T> run(
        repository: String,
        endpoints: List<CloneEndpoint>,
        lookupProblem: String? = null,
        onAttempt: (CloneEndpoint, Int) -> Unit = { _, _ -> },
        attempt: (CloneEndpoint) -> T,
    ): T {
        val attempts = ArrayList<CloneAttempt>()
        for ((index, endpoint) in endpoints.withIndex()) {
            onAttempt(endpoint, index)
            try {
                return attempt(endpoint)
            } catch (error: Exception) {
                if (isFinal(error)) throw error
                attempts.add(CloneAttempt(endpoint, describeLocalRepoFailure(error)))
            }
        }
        throw CloneFailure(repository, attempts, lookupProblem)
    }

    fun attemptLabel(endpoint: CloneEndpoint, index: Int, count: Int): String {
        val base = "Connecting to ${endpoint.label}"
        return if (count > 1) "$base (source ${index + 1} of $count)" else base
    }

    fun progressLabel(progress: GitTransferProgress, source: String): String =
        if (progress.phase == "connecting") "Connecting to $source" else "${progress.label} · $source"
}
