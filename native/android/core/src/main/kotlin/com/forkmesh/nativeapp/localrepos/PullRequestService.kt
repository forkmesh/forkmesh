package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.git.objects.GitObjectId
import com.forkmesh.nativeapp.git.repository.GitRepository
import com.forkmesh.nativeapp.git.repository.GitRepositoryOpen

@Suppress("EnumEntryName")
enum class PullRequestBlocker {
    notPublished,

    missingBranch,

    noCommits,

    noChanges,

    binaryChanges,
}

class PullRequestPreparation private constructor(
    val message: String,
    val blocker: PullRequestBlocker? = null,
    val owner: String = "",
    val repo: String = "",
    val base: String = "",
    val head: String = "",
    val patch: String = "",
    val changedPaths: List<String> = emptyList(),
    val binaryPaths: List<String> = emptyList(),
) {
    val isReady: Boolean get() = blocker == null

    val summary: String
        get() = if (isReady) {
            "${changedPaths.size} ${if (changedPaths.size == 1) "file" else "files"} · $head → $base"
        } else {
            message
        }

    companion object {
        fun blocked(blocker: PullRequestBlocker, message: String, binaryPaths: List<String> = emptyList()) =
            PullRequestPreparation(message = message, blocker = blocker, binaryPaths = binaryPaths)

        internal fun ready(owner: String, repo: String, base: String, head: String, patch: String, changedPaths: List<String>) =
            PullRequestPreparation(
                message = "Ready to open a pull request.",
                owner = owner,
                repo = repo,
                base = base,
                head = head,
                patch = patch,
                changedPaths = changedPaths,
            )
    }
}

fun interface PushBranch {
    fun push(workspace: GitWorkspace, owner: String, repo: String, branch: String): LocalPushResult
}

fun interface SubmitPull {
    fun submit(owner: String, name: String, title: String, base: String, head: String, patch: String)
}

class PullRequestService(val pushBranch: PushBranch, val submitPull: SubmitPull) {
    fun prepare(
        workspace: GitWorkspace,
        record: LocalRepoRecord,
        headBranch: String,
        baseBranch: String,
    ): PullRequestPreparation {
        val published = record.publishedAs?.trim() ?: ""
        if (published.isEmpty() || !published.contains('/')) {
            return PullRequestPreparation.blocked(
                PullRequestBlocker.notPublished,
                "This repository is only on this device. Publish it to ForkMesh first, then its branches can " +
                    "become pull requests.",
            )
        }
        val owner = published.split('/').first()
        val repo = published.split('/').last()

        val branches = workspace.branches()
        val head = branches.firstOrNull { it.name == headBranch }
        val base = branches.firstOrNull { it.name == baseBranch }
        if (head?.tipId == null || base?.tipId == null) {
            return PullRequestPreparation.blocked(
                PullRequestBlocker.missingBranch,
                "Could not find both $headBranch and $baseBranch in this repository.",
            )
        }

        val repository = GitRepository.open(GitRepositoryOpen(workspace.worktree, limits = GitWorkspace.deviceLimits(), detector = workspace.detector))
        val headId = GitObjectId.parseSha1(head.tipId)
        val baseId = GitObjectId.parseSha1(base.tipId)
        val divergedAt = mergeBase(repository, baseId, headId)

        if (divergedAt != null && divergedAt.hex == headId.hex) {
            return PullRequestPreparation.blocked(
                PullRequestBlocker.noCommits,
                "$headBranch has no commits that $baseBranch does not already have.",
            )
        }

        val result = PatchBuilder.between(
            repository = repository,
            base = divergedAt ?: baseId,
            head = headId,
        )

        if (result.hasBinaryChanges) {
            val names = result.binaryPaths.take(3).joinToString(", ")
            val more = if (result.binaryPaths.size > 3) " and ${result.binaryPaths.size - 3} more" else ""
            return PullRequestPreparation.blocked(
                PullRequestBlocker.binaryChanges,
                "This branch changes binary files ($names$more), which a pull request from mobile cannot carry yet. " +
                    "The commits are safe on $headBranch — open the pull request from a desktop node instead.",
                binaryPaths = result.binaryPaths,
            )
        }
        if (result.isEmpty) {
            return PullRequestPreparation.blocked(
                PullRequestBlocker.noChanges,
                "The commits on $headBranch do not change any files.",
            )
        }

        return PullRequestPreparation.ready(
            owner = owner,
            repo = repo,
            base = baseBranch,
            head = headBranch,
            patch = result.patch,
            changedPaths = result.changedPaths,
        )
    }

    fun submit(
        workspace: GitWorkspace,
        preparation: PullRequestPreparation,
        title: String,
        onProgress: ((String) -> Unit)? = null,
    ) {
        check(preparation.isReady) { "This branch is not ready: ${preparation.message}" }
        val subject = if (title.trim().isEmpty()) "Changes from ${preparation.head}" else title.trim()

        onProgress?.invoke("Pushing ${preparation.head}…")
        val push = pushBranch.push(
            workspace = workspace,
            owner = preparation.owner,
            repo = preparation.repo,
            branch = preparation.head,
        )
        check(!push.isRejected) {
            "The mesh rejected the push of ${preparation.head}, so the pull request was not opened. Fetch and retry."
        }

        onProgress?.invoke("Opening the pull request…")
        submitPull.submit(
            owner = preparation.owner,
            name = preparation.repo,
            title = subject,
            base = preparation.base,
            head = preparation.head,
            patch = preparation.patch,
        )
    }

    companion object {
        const val maxAncestorWalk = 2000

        private fun mergeBase(repository: GitRepository, a: GitObjectId, b: GitObjectId): GitObjectId? {
            val seen = HashSet<String>()
            var frontier = listOf(a)
            var visited = 0
            while (frontier.isNotEmpty() && visited < maxAncestorWalk) {
                val next = ArrayList<GitObjectId>()
                for (id in frontier) {
                    if (!seen.add(id.hex)) continue
                    visited++
                    try {
                        next.addAll(repository.readCommit(id).parents)
                    } catch (error: Exception) {
                    }
                }
                frontier = next
            }

            val queue = ArrayDeque(listOf(b))
            val walked = HashSet<String>()
            var steps = 0
            while (queue.isNotEmpty() && steps < maxAncestorWalk) {
                val id = queue.removeFirst()
                if (!walked.add(id.hex)) continue
                steps++
                if (id.hex in seen) return id
                try {
                    queue.addAll(repository.readCommit(id).parents)
                } catch (error: Exception) {
                }
            }
            return null
        }
    }
}
