package com.forkmesh.nativeapp.codeinsight

import com.forkmesh.nativeapp.git.crypto.GitSha1CollisionDetector
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.objects.GitCommit
import com.forkmesh.nativeapp.git.objects.GitObjectId
import com.forkmesh.nativeapp.git.objects.GitTreeMode
import com.forkmesh.nativeapp.git.refs.GitRefName
import com.forkmesh.nativeapp.git.refs.RefStore
import com.forkmesh.nativeapp.git.repository.GitRepository
import com.forkmesh.nativeapp.git.repository.GitRepositoryOpen
import com.forkmesh.nativeapp.git.repository.shallowCommits
import com.forkmesh.nativeapp.git.security.GitResourceLimits
import java.io.File
import java.util.PriorityQueue
import kotlin.coroutines.cancellation.CancellationException

class InsightGitSource(
    val repository: GitRepository,
    override val name: String,
    override val description: String,
    val commitId: GitObjectId?,
    override val branch: String,
    private val workingTree: File? = null,
    private val clock: () -> Long = { System.currentTimeMillis() / 1000 },
) : InsightSource {
    private class Blob(val id: GitObjectId, val mode: GitTreeMode)

    private class Change(val path: String, val old: Blob?, val new: Blob?)

    private class Pending(val id: GitObjectId, val commit: GitCommit, val time: Long, val order: Long)

    override val head: String = commitId?.hex ?: ""

    override val git: Boolean get() = commitId != null

    private val shallow: Set<GitObjectId> by lazy { repository.shallowCommits() }

    private val blobs: Map<String, Blob> by lazy {
        val out = LinkedHashMap<String, Blob>()
        val id = commitId
        if (id != null) flatten(repository.readCommit(id).tree, "", out, 0)
        out
    }

    override fun entries(): List<InsightEntry> = blobs.map { (path, blob) ->
        val working = workingFile(path)
        InsightEntry(path, if (working != null) working.length() else if (blob.mode == GitTreeMode.symlink) 0 else -1)
    }

    override fun read(path: String, limit: Long): ByteArray? {
        val working = workingFile(path)
        if (working != null) {
            if (working.length() > limit) return ByteArray(0)
            return try {
                working.readBytes()
            } catch (_: Exception) {
                null
            }
        }
        val blob = blobs[path] ?: return null
        return try {
            repository.readBlob(blob.id)
        } catch (_: GitException) {
            null
        }
    }

    private fun workingFile(path: String): File? {
        val root = workingTree ?: return null
        val file = File(root, path)
        if (!file.isFile) return null
        val canonical = try {
            file.canonicalFile
        } catch (_: Exception) {
            return null
        }
        if (!canonical.path.startsWith(root.canonicalPath + File.separator)) return null
        return canonical
    }

    override fun history(options: InsightOptions, isCancelled: () -> Boolean): List<InsightCommit>? {
        val start = commitId ?: return null
        val cutoff = clock() - options.historyDays.toLong() * 86400L
        val out = ArrayList<InsightCommit>()
        walk(start, isCancelled, stopBefore = cutoff) { id, commit, parents ->
            if (parents.size > 1) return@walk true
            val parentTree = parents.firstOrNull()?.let { repository.readCommit(it).tree }
            out.add(record(id, commit, parentTree))
            out.size < options.maxCommits
        }
        return out
    }

    fun versions(path: String, samples: Int = 16, maxCommits: Int = 400, isCancelled: () -> Boolean = { false }): List<InsightVersion> {
        val start = commitId ?: return emptyList()
        val touching = ArrayList<Pair<GitObjectId, GitCommit>>()
        walk(start, isCancelled, stopBefore = null, follow = { commit, parents -> followFor(path, commit, parents) }) { id, commit, parents ->
            if (parents.size > 1) return@walk true
            val mine = blobAt(commit.tree, path)
            val before = parents.firstOrNull()?.let { blobAt(repository.readCommit(it).tree, path) }
            if (mine?.id != before?.id || (mine != null && before != null && mine.mode != before.mode)) touching.add(id to commit)
            touching.size < maxCommits
        }
        val versions = ArrayList<InsightVersion>()
        for (index in Insight.sampleIndexes(touching.size, samples).asReversed()) {
            if (isCancelled()) throw CancellationException("Complexity trend was canceled.")
            val (id, commit) = touching[index]
            val blob = blobAt(commit.tree, path) ?: continue
            val bytes = try {
                repository.readBlob(blob.id)
            } catch (_: GitException) {
                continue
            }
            val author = commit.author
            versions.add(InsightVersion(author?.timestampSeconds ?: 0L, id.hex, author?.name ?: "", bytes))
        }
        return versions
    }

    fun trend(path: String, samples: Int = 16, isCancelled: () -> Boolean = { false }): List<InsightTrendPoint> =
        Insight.trend(path, versions(path, samples, isCancelled = isCancelled))

    private fun followFor(path: String, commit: GitCommit, parents: List<GitObjectId>): List<GitObjectId> {
        if (parents.size < 2) return parents
        val mine = blobAt(commit.tree, path)
        for (parent in parents) {
            val theirs = blobAt(repository.readCommit(parent).tree, path)
            if (theirs?.id == mine?.id) return listOf(parent)
        }
        return parents
    }

    private fun walk(
        start: GitObjectId,
        isCancelled: () -> Boolean,
        stopBefore: Long?,
        follow: (GitCommit, List<GitObjectId>) -> List<GitObjectId> = { _, parents -> parents },
        visit: (GitObjectId, GitCommit, List<GitObjectId>) -> Boolean,
    ) {
        val queue = PriorityQueue<Pending>(compareByDescending<Pending> { it.time }.thenBy { it.order })
        val seen = HashSet<GitObjectId>()
        var order = 0L
        fun enqueue(id: GitObjectId) {
            if (!seen.add(id)) return
            if (seen.size > MAX_WALK) throw GitException(GitErrorCode.resourceLimitExceeded, "history walk exceeded the configured limit")
            val commit = readCommitOrNull(id) ?: return
            queue.add(Pending(id, commit, commitTime(commit), order++))
        }
        enqueue(start)
        while (queue.isNotEmpty()) {
            if (isCancelled()) throw CancellationException("Reading history was canceled.")
            val next = queue.poll()
            if (stopBefore != null && next.time < stopBefore) continue
            val parents = parentsOf(next.id, next.commit) ?: continue
            for (parent in follow(next.commit, parents)) enqueue(parent)
            if (!visit(next.id, next.commit, parents)) return
        }
    }

    private fun parentsOf(id: GitObjectId, commit: GitCommit): List<GitObjectId>? {
        if (id in shallow) return null
        for (parent in commit.parents) if (!hasCommit(parent)) return null
        return commit.parents
    }

    private fun hasCommit(id: GitObjectId): Boolean = try {
        repository.hasObject(id)
    } catch (_: GitException) {
        false
    }

    private fun readCommitOrNull(id: GitObjectId): GitCommit? = try {
        if (repository.hasObject(id)) repository.readCommit(id) else null
    } catch (error: GitException) {
        if (error.code == GitErrorCode.invalidObject) null else throw error
    }

    private fun commitTime(commit: GitCommit): Long =
        commit.committer?.timestampSeconds ?: commit.author?.timestampSeconds ?: 0L

    private fun record(id: GitObjectId, commit: GitCommit, parentTree: GitObjectId?): InsightCommit {
        val changes = ArrayList<Change>()
        diffTrees(parentTree, commit.tree, "", changes)
        val files = ArrayList<InsightCommitFile>()
        for (change in changes) {
            if (Insight.languageForPath(change.path).isEmpty() || Insight.isVendoredPath(change.path)) continue
            val old = change.old?.let { repository.readBlob(it.id) } ?: ByteArray(0)
            val new = change.new?.let { repository.readBlob(it.id) } ?: ByteArray(0)
            val counts = InsightLineDiff.count(old, new)
            files.add(InsightCommitFile(change.path, counts?.added, counts?.deleted))
        }
        val author = commit.author
        return InsightCommit(
            hash = id.hex,
            author = author?.name ?: "",
            time = author?.timestampSeconds ?: 0L,
            subject = subjectOf(commit.message),
            files = files,
        )
    }

    private fun diffTrees(old: GitObjectId?, new: GitObjectId?, prefix: String, out: MutableList<Change>) {
        if (old == new) return
        val before = old?.let { entriesOf(it) } ?: emptyMap()
        val after = new?.let { entriesOf(it) } ?: emptyMap()
        val names = (before.keys + after.keys).sortedBy { gitOrder(it, before, after) }
        for (name in names) {
            val path = if (prefix.isEmpty()) name else "$prefix/$name"
            val left = before[name]
            val right = after[name]
            val leftTree = left?.mode == GitTreeMode.directory
            val rightTree = right?.mode == GitTreeMode.directory
            if (left != null && right != null && left.id == right.id && left.mode == right.mode) continue
            when {
                leftTree && rightTree -> diffTrees(left!!.id, right!!.id, path, out)
                leftTree -> {
                    diffTrees(left!!.id, null, path, out)
                    if (right != null) out.add(Change(path, null, right))
                }
                rightTree -> {
                    if (left != null) out.add(Change(path, left, null))
                    diffTrees(null, right!!.id, path, out)
                }
                else -> out.add(Change(path, left, right))
            }
        }
    }

    private fun gitOrder(name: String, before: Map<String, Blob>, after: Map<String, Blob>): String {
        val directory = before[name]?.mode == GitTreeMode.directory || after[name]?.mode == GitTreeMode.directory
        return if (directory) "$name/" else name
    }

    private fun entriesOf(tree: GitObjectId): Map<String, Blob> {
        val out = LinkedHashMap<String, Blob>()
        for (entry in repository.readTree(tree).entries) out[String(entry.rawName, Charsets.UTF_8)] = Blob(entry.objectId, entry.mode)
        return out
    }

    private fun blobAt(tree: GitObjectId, path: String): Blob? {
        var current = tree
        val parts = path.split('/')
        for ((index, part) in parts.withIndex()) {
            val entry = repository.readTree(current).entries.firstOrNull { String(it.rawName, Charsets.UTF_8) == part } ?: return null
            if (index == parts.lastIndex) return if (entry.mode == GitTreeMode.directory) null else Blob(entry.objectId, entry.mode)
            if (entry.mode != GitTreeMode.directory) return null
            current = entry.objectId
        }
        return null
    }

    private fun flatten(tree: GitObjectId, prefix: String, out: MutableMap<String, Blob>, depth: Int) {
        if (depth > MAX_TREE_DEPTH) return
        for (entry in repository.readTree(tree).entries) {
            val name = String(entry.rawName, Charsets.UTF_8)
            val path = if (prefix.isEmpty()) name else "$prefix/$name"
            if (entry.mode == GitTreeMode.directory) flatten(entry.objectId, path, out, depth + 1) else out[path] = Blob(entry.objectId, entry.mode)
        }
    }

    companion object {
        const val MAX_WALK = 1_000_000
        const val MAX_TREE_DEPTH = 64

        fun subjectOf(message: ByteArray): String {
            val lines = String(message, Charsets.UTF_8).split('\n')
            val parts = ArrayList<String>()
            var started = false
            for (line in lines) {
                val trimmed = line.trimEnd()
                if (trimmed.isBlank()) {
                    if (started) break
                    continue
                }
                started = true
                parts.add(trimmed.trimStart())
            }
            return parts.joinToString(" ")
        }

        fun open(
            directory: File,
            name: String,
            description: String,
            ref: String? = null,
            readWorkingFiles: Boolean = false,
            detector: GitSha1CollisionDetector? = null,
            limits: GitResourceLimits? = null,
            clock: () -> Long = { System.currentTimeMillis() / 1000 },
        ): InsightGitSource {
            val repository = GitRepository.open(GitRepositoryOpen(directory, limits = limits, detector = detector))
            val branch = try {
                repository.readHead().target.removePrefix("refs/heads/")
            } catch (_: GitException) {
                ""
            }
            val tip = if (ref != null) {
                RefStore(repository.gitDirectory).readDirect(GitRefName.parse(ref)) ?: repository.readHeadObjectId()
            } else {
                repository.readHeadObjectId()
            }
            return InsightGitSource(
                repository = repository,
                name = name,
                description = description,
                commitId = tip,
                branch = branch,
                workingTree = if (readWorkingFiles) directory else null,
                clock = clock,
            )
        }
    }
}
