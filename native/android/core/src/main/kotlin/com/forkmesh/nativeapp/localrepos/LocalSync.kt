package com.forkmesh.nativeapp.localrepos

sealed class LocalPullOutcome {
    data object UpToDate : LocalPullOutcome()

    data class FastForwarded(val from: String, val to: String) : LocalPullOutcome()

    data object Ahead : LocalPullOutcome()

    data object Diverged : LocalPullOutcome()

    data object Undetermined : LocalPullOutcome()

    data object NoRemoteBranch : LocalPullOutcome()
}

data class LocalPullResult(val branch: String, val fetch: LocalFetchResult, val outcome: LocalPullOutcome) {
    val changedWorktree: Boolean get() = outcome is LocalPullOutcome.FastForwarded

    val needsAttention: Boolean
        get() = outcome == LocalPullOutcome.Diverged || outcome == LocalPullOutcome.Undetermined

    val title: String
        get() = when (outcome) {
            LocalPullOutcome.UpToDate, LocalPullOutcome.Ahead -> "Up to date"
            is LocalPullOutcome.FastForwarded -> "Synced"
            LocalPullOutcome.Diverged, LocalPullOutcome.Undetermined -> "Sync needs attention"
            LocalPullOutcome.NoRemoteBranch -> "Nothing to sync"
        }

    val message: String
        get() = when (outcome) {
            LocalPullOutcome.UpToDate -> "$branch is already up to date with ForkMesh."
            is LocalPullOutcome.FastForwarded -> "Synced $branch with ForkMesh."
            LocalPullOutcome.Ahead ->
                "Nothing to sync: $branch here already contains ForkMesh's. Push to share your commits."
            LocalPullOutcome.Diverged ->
                "$branch changed both here and on ForkMesh, and this device only syncs fast-forwards. " +
                    "Push your commits to a new branch and open a pull request."
            LocalPullOutcome.Undetermined ->
                "This device's history of $branch is too short to tell whether ForkMesh's builds on yours, " +
                    "so $branch was left as it was. The fetched commits are on origin/$branch."
            LocalPullOutcome.NoRemoteBranch -> "ForkMesh has no branch named $branch yet. Push it first."
        }
}

fun pullResultMessage(result: LocalPullResult): String =
    result.message + (result.fetch.servedBy?.let { " (from $it)" } ?: "")

fun syncGuardMessage(branch: String): String = "Commit or discard your changes before syncing $branch."

fun syncableRecords(records: List<LocalRepoRecord>): List<LocalRepoRecord> = records.filter { it.isPublished }

fun syncAllLabel(records: List<LocalRepoRecord>): String {
    val syncable = syncableRecords(records)
    return if (syncable.count { it.isCloned } == syncable.size) "Sync clones" else "Sync all"
}

fun localSyncSummary(
    results: List<Pair<LocalRepoRecord, LocalPullResult>>,
    failures: List<Pair<LocalRepoRecord, String>>,
): String {
    if (results.size + failures.size == 1) {
        results.firstOrNull()?.let { (record, result) -> return "${record.name}: ${result.message}" }
        failures.firstOrNull()?.let { (record, message) -> return "${record.name}: $message" }
    }
    val parts = mutableListOf("Synced ${results.size} of ${results.size + failures.size} repositories")
    val updated = results.count { it.second.changedWorktree }
    if (updated > 0) parts.add("$updated updated")
    val attention = results.filter { it.second.needsAttention }.map { it.first.name }
    if (attention.isNotEmpty()) parts.add("check ${attention.joinToString(", ")}")
    if (failures.isNotEmpty()) parts.add("failed: ${failures.joinToString(", ") { it.first.name }}")
    return parts.joinToString(" · ")
}

class LocalRepoSyncRun(private val store: LocalRepoStore, private val publisher: PublishService) {
    @Volatile
    private var stopped = false

    fun cancel() {
        stopped = true
        store.cancelActive()
    }

    fun run(targets: List<LocalRepoRecord>, onStatus: ((String) -> Unit)? = null): String {
        stopped = false
        val results = ArrayList<Pair<LocalRepoRecord, LocalPullResult>>()
        val failures = ArrayList<Pair<LocalRepoRecord, String>>()
        for ((index, record) in targets.withIndex()) {
            if (stopped) break
            val step = if (targets.size > 1) " (${index + 1} of ${targets.size})" else ""
            val title = "Syncing ${record.name}$step"
            onStatus?.invoke(title)
            try {
                val result = store.sync(record, publisher) { progress ->
                    if (!stopped) onStatus?.invoke("$title · ${progress.label}")
                }
                results.add(record to result)
            } catch (error: Exception) {
                if (stopped) break
                failures.add(record to describeLocalRepoFailure(error))
            }
        }
        if (stopped) return "Sync cancelled."
        return localSyncSummary(results, failures)
    }
}
