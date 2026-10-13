package com.forkmesh.nativeapp.git.repository

import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.objects.GitCommit
import com.forkmesh.nativeapp.git.objects.GitObjectId
import com.forkmesh.nativeapp.git.security.GitCancellationToken
import com.forkmesh.nativeapp.git.worktree.createFileExclusive
import com.forkmesh.nativeapp.git.worktree.fileExists
import com.forkmesh.nativeapp.git.worktree.readString
import com.forkmesh.nativeapp.git.worktree.renameFile
import com.forkmesh.nativeapp.git.worktree.writeStringFlushed
import java.io.File

internal object GitShallowFile {
    fun path(gitDirectory: File): File = File(gitDirectory, "shallow")

    fun read(gitDirectory: File): Set<GitObjectId> {
        val file = path(gitDirectory)
        if (!fileExists(file)) return emptySet()
        val ids = LinkedHashSet<GitObjectId>()
        for (line in readString(file).split("\n")) {
            val text = line.trim()
            if (text.isEmpty()) continue
            try {
                ids.add(GitObjectId.parseSha1(text))
            } catch (error: GitException) {
                throw GitException(GitErrorCode.invalidRepository, "shallow file is invalid")
            }
        }
        return ids
    }

    fun write(gitDirectory: File, ids: Set<GitObjectId>) {
        val file = path(gitDirectory)
        if (ids.isEmpty()) {
            if (fileExists(file)) file.delete()
            return
        }
        val lock = File("${file.path}.lock")
        if (!createFileExclusive(lock)) {
            throw GitException(GitErrorCode.refLocked, "shallow lock already exists")
        }
        try {
            writeStringFlushed(lock, ids.map { it.hex }.sorted().joinToString("") { "$it\n" })
            renameFile(lock, file)
        } finally {
            if (fileExists(lock)) lock.delete()
        }
    }
}

fun GitRepository.shallowCommits(): Set<GitObjectId> = GitShallowFile.read(gitDirectory)

val GitRepository.isShallow: Boolean get() = fileExists(GitShallowFile.path(gitDirectory))

internal fun walkParents(id: GitObjectId, commit: GitCommit, shallow: Set<GitObjectId>): List<GitObjectId> =
    if (shallow.contains(id)) emptyList() else commit.parents

fun GitRepository.ancestry(
    ancestor: GitObjectId,
    descendant: GitObjectId,
    cancellation: GitCancellationToken? = null,
): GitAncestry {
    if (ancestor == descendant) return GitAncestry.ancestor
    val shallow = shallowCommits()
    val below = boundedWalk(descendant, shallow, cancellation)
    if (below.visited.contains(ancestor)) return GitAncestry.ancestor
    if (below.edges.isEmpty() && below.missing.isEmpty()) return GitAncestry.notAncestor
    if (below.missing.isNotEmpty()) return GitAncestry.unknown
    val above = boundedWalk(ancestor, shallow, cancellation)
    return if (above.visited.containsAll(below.edges)) GitAncestry.notAncestor else GitAncestry.unknown
}

private class AncestryWalk(val visited: Set<GitObjectId>, val edges: Set<GitObjectId>, val missing: Set<GitObjectId>)

private fun GitRepository.boundedWalk(
    tip: GitObjectId,
    shallow: Set<GitObjectId>,
    cancellation: GitCancellationToken?,
): AncestryWalk {
    val visited = HashSet<GitObjectId>()
    val edges = HashSet<GitObjectId>()
    val missing = HashSet<GitObjectId>()
    val stack = ArrayList<GitObjectId>()
    stack.add(tip)
    while (stack.isNotEmpty()) {
        cancellation?.throwIfCancelled()
        val id = stack.removeAt(stack.size - 1)
        if (!visited.add(id)) continue
        if (visited.size > limits.maxCommitWalk) {
            throw GitException(GitErrorCode.resourceLimitExceeded, "ancestry walk exceeded the configured limit")
        }
        val commit = if (hasObject(id)) readCommitOrTagTarget(id) else null
        if (commit == null) {
            missing.add(id)
            continue
        }
        if (shallow.contains(id)) {
            edges.add(id)
            continue
        }
        stack.addAll(commit.parents)
    }
    return AncestryWalk(visited, edges, missing)
}
