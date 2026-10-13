package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.model.RepoMirror
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.format.relativeTimeFrom
import com.forkmesh.nativeapp.model.auth.AuthSession
import java.util.Locale

fun localRepoRelativeTime(timeMillis: Long, nowMillis: Long = System.currentTimeMillis()): String =
    relativeTimeFrom(timeMillis, nowMillis)

fun localRepoAuthor(session: AuthSession?, override: Pair<String, String>? = null): GitAuthor {
    val account = GitAuthor.fromAccount(nodeName = session?.nodeName ?: "", email = session?.email ?: "")
    if (override == null) return account
    return GitAuthor(override.first.ifEmpty { account.name }, override.second.ifEmpty { account.email })
}

fun describeLocalRepoFailure(error: Throwable): String = when (error) {
    is GitWorkspaceException -> error.message
    is LocalRepoStoreException -> error.message
    else -> error.toString()
}

fun localMeshIdentities(records: List<LocalRepoRecord>): Set<String> =
    records.filter { it.isPublished }.mapTo(LinkedHashSet()) { it.publishedAs!!.lowercase() }

fun localRepoTileMeta(record: LocalRepoRecord, nowMillis: Long = System.currentTimeMillis()): String {
    val whenAt = record.lastCommitAt
    return listOf(
        if (record.isPublished) record.publishedAs!! else record.branch,
        if (whenAt != null) "committed ${localRepoRelativeTime(whenAt, nowMillis)}" else "no commits yet",
    ).joinToString(" · ")
}

fun slugifyImportName(raw: String): String {
    val cleaned = raw
        .replace(Regex("[^A-Za-z0-9._-]+"), "-")
        .replace(Regex("-{2,}"), "-")
        .replace(Regex("^[-.]+|[-.]+$"), "")
    if (cleaned.isEmpty()) return "imported"
    return if (cleaned.length > 100) cleaned.substring(0, 100) else cleaned
}

fun suggestedImportName(displayName: String, zip: Boolean): String {
    var suggested = if (zip) {
        displayName
    } else {
        displayName.split('/').lastOrNull { it.isNotEmpty() } ?: "imported"
    }
    if (zip && suggested.lowercase().endsWith(".zip")) suggested = suggested.substring(0, suggested.length - 4)
    return slugifyImportName(suggested)
}

fun suggestCloneName(records: List<LocalRepoRecord>, name: String): String {
    val base = name.replace(Regex("[^A-Za-z0-9._-]+"), "-")
    val taken = records.mapTo(HashSet()) { it.name }
    if (base !in taken) return base
    for (i in 2 until 100) {
        if ("$base-$i" !in taken) return "$base-$i"
    }
    return base
}

val cloneToDeviceNote: String
    get() = "Downloads the latest commit of the default branch from the repository's mirrors into app " +
        "storage. Older history is fetched only when a push needs it."

data class CloneToDeviceOutcome(val record: LocalRepoRecord?, val message: String, val existing: LocalRepoRecord? = null)

fun cloneToDevice(
    store: LocalRepoStore,
    publisher: PublishService,
    session: AuthSession?,
    owner: String,
    name: String,
    localName: String,
    description: String = "",
    onProgress: ((GitTransferProgress) -> Unit)? = null,
    mirrors: ((String, String) -> List<RepoMirror>)? = null,
    onStatus: ((String) -> Unit)? = null,
): CloneToDeviceOutcome {
    val repository = "$owner/$name"
    store.ensureLoaded()
    store.record(repository)?.let { return CloneToDeviceOutcome(null, LocalRepoStore.alreadyOnDevice(repository, it.name), existing = it) }
    val relay = publisher.credentials(
        owner = owner,
        repo = name,
        signed = session != null && session.nodeName.isNotEmpty(),
    )
    onStatus?.invoke("Finding mirrors of $repository")
    var found = emptyList<RepoMirror>()
    var lookupProblem: String? = null
    try {
        found = (mirrors ?: { o, n -> publisher.api.mirrorSet(o, n).mirrors })(owner, name)
    } catch (error: Exception) {
        lookupProblem = describeLocalRepoFailure(error)
    }
    val endpoints = CloneEndpoints.plan(owner, name, found, relay)
    var source: CloneEndpoint? = null
    return try {
        val record = CloneEndpoints.run(
            repository = repository,
            endpoints = endpoints,
            lookupProblem = lookupProblem,
            onAttempt = { endpoint, index ->
                source = endpoint
                onStatus?.invoke(CloneEndpoints.attemptLabel(endpoint, index, endpoints.size))
            },
        ) { endpoint ->
            val progress: (GitTransferProgress) -> Unit = { p ->
                onStatus?.invoke(CloneEndpoints.progressLabel(p, endpoint.label))
                onProgress?.invoke(p)
            }
            try {
                store.cloneFromMesh(
                    name = localName,
                    credentials = endpoint.credentials(relay),
                    description = description,
                    onProgress = progress,
                    depth = GitWorkspace.meshCloneDepth,
                )
            } catch (error: GitWorkspaceException) {
                if (!endpoint.isRelay || error.code != GitErrorCode.protocolUnsupported) throw error
                store.cloneFromMesh(
                    name = localName,
                    credentials = endpoint.credentials(relay),
                    description = description,
                    onProgress = progress,
                )
            }
        }
        val from = source?.let { " from ${it.label}" } ?: ""
        CloneToDeviceOutcome(record, "Cloned $repository to $localName$from.")
    } catch (error: CloneFailure) {
        CloneToDeviceOutcome(null, error.message)
    } catch (error: Exception) {
        CloneToDeviceOutcome(null, describeLocalRepoFailure(error))
    }
}

const val switchBranchGuardMessage = "Commit or discard your changes before switching branches."

fun publishHeadOf(log: List<LocalCommit>, branchCount: Int): PublishHead {
    if (log.isEmpty()) return PublishHead()
    return PublishHead(
        commit = log.first().id,
        rootCommit = if (log.last().parents.isEmpty()) log.last().id else "",
        subject = log.first().subject,
        authorName = log.first().authorName,
        at = log.first().time,
        commitCount = log.size,
        branchCount = branchCount,
    )
}

fun pushResultMessage(result: LocalPushResult, branch: String, target: String): String {
    val rejected = result.refs.filter { it.isRejected }
    if (rejected.isNotEmpty()) {
        val first = rejected.first()
        return if (first.message.isEmpty()) "ForkMesh refused ${first.branch}." else "ForkMesh refused ${first.branch}: ${first.message}"
    }
    return if (result.isUpToDate) "ForkMesh already has $branch." else "Pushed $branch to $target."
}

fun fetchResultMessage(result: LocalFetchResult): String {
    val from = result.servedBy?.let { " from $it" } ?: ""
    val count = result.updatedRefs.size
    return if (result.isUpToDate) {
        "Already up to date with ForkMesh$from."
    } else {
        "Fetched $count branch${if (count == 1) "" else "es"}$from."
    }
}

fun committedMessage(id: String): String = "Committed ${id.substring(0, minOf(7, id.length))}"

fun meshBannerText(publishedAs: String?, origin: LocalRepoOrigin): String =
    if (origin == LocalRepoOrigin.cloned) "$publishedAs · cloned to this device" else "$publishedAs · published from this device"

fun committingAsText(author: GitAuthor): String = "Committing as ${author.name} <${author.email}>"

fun commitButtonLabel(stagedCount: Int): String = if (stagedCount == 0) "Commit" else "Commit $stagedCount"

fun localRepoHumanSize(bytes: Long): String {
    if (bytes < 1024) return "$bytes B"
    if (bytes < 1024 * 1024) return "${String.format(Locale.ROOT, "%.1f", bytes / 1024.0)} KB"
    return "${String.format(Locale.ROOT, "%.1f", bytes / (1024.0 * 1024.0))} MB"
}

fun localFileMeta(entry: LocalFileEntry): String = if (entry.isDirectory) {
    if (entry.tracked) "Folder" else "Folder · untracked"
} else {
    "${localRepoHumanSize(entry.size)}${if (entry.tracked) "" else " · untracked"}"
}

fun localChangeLabel(change: LocalChange): String = if (change.kind == LocalChangeKind.untracked) {
    "Untracked"
} else {
    change.kind.name.replaceFirstChar { it.uppercaseChar() }
}

data class LocalBreadcrumb(val label: String, val target: String, val current: Boolean)

fun localBreadcrumbs(path: String): List<LocalBreadcrumb> {
    val segments = path.split('/').filter { it.isNotEmpty() }
    val crumbs = mutableListOf(LocalBreadcrumb("Root", "", segments.isEmpty()))
    var prefix = ""
    segments.forEachIndexed { index, segment ->
        prefix = if (prefix.isEmpty()) segment else "$prefix/$segment"
        crumbs.add(LocalBreadcrumb(segment, prefix, index == segments.lastIndex))
    }
    return crumbs
}

fun editorFooterText(path: String, text: String, dirty: Boolean): String {
    val lines = text.count { it == '\n' } + 1
    return "$path · $lines ${if (lines == 1) "line" else "lines"}${if (dirty) " · unsaved" else ""}"
}

fun commitRowSubtitle(commit: LocalCommit, nowMillis: Long = System.currentTimeMillis()): String =
    "${commit.shortId} · ${commit.authorName} · ${localRepoRelativeTime(commit.time, nowMillis)}"
