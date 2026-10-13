package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.refs.GitFetchRefspec
import com.forkmesh.nativeapp.git.refs.GitRefName
import com.forkmesh.nativeapp.git.refs.RefStore
import com.forkmesh.nativeapp.git.repository.GitAncestry
import com.forkmesh.nativeapp.git.repository.GitDeepen
import com.forkmesh.nativeapp.git.repository.GitRepository
import com.forkmesh.nativeapp.git.repository.ancestry
import com.forkmesh.nativeapp.git.security.GitCancellationToken

internal object ShallowPushProof {
    val schedule: List<GitDeepen> =
        listOf(GitDeepen.Relative(50), GitDeepen.Relative(250), GitDeepen.Relative(1000), GitDeepen.Unshallow)

    fun prove(
        repo: GitRepository,
        branch: String,
        remote: String,
        schedule: List<GitDeepen> = ShallowPushProof.schedule,
        fetch: (GitFetchRefspec, GitDeepen?) -> Unit,
        token: GitCancellationToken?,
    ) {
        val tracking = GitRefName.parse("refs/remotes/$remote/$branch")
        val refspec = GitFetchRefspec.parse("+refs/heads/$branch:${tracking.value}")
        val local = GitRefName.parse("refs/heads/$branch")
        val store = RefStore(repo.gitDirectory)
        bounded(branch) { fetch(refspec, null) }
        val steps = ArrayDeque(schedule)
        while (true) {
            token?.throwIfCancelled()
            val remoteTip = store.readDirect(tracking) ?: return
            val localTip = store.readDirect(local)
                ?: throw GitException(GitErrorCode.invalidRef, "local branch $branch does not exist")
            when (bounded(branch) { repo.ancestry(remoteTip, localTip, token) }) {
                GitAncestry.ancestor -> return
                GitAncestry.notAncestor -> throw diverged(branch)
                GitAncestry.unknown -> {
                    val step = steps.removeFirstOrNull() ?: throw historyLimit(branch)
                    bounded(branch) { fetch(refspec, step) }
                }
            }
        }
    }

    fun diverged(branch: String): GitWorkspaceException = GitWorkspaceException(
        "$branch on ForkMesh has commits that your $branch does not include, so pushing would overwrite them. " +
            "Push your work to a new branch and open a pull request, or fetch and redo your change on top of $branch.",
        code = GitErrorCode.nonFastForward,
        detail = "push_diverged",
    )

    fun historyLimit(branch: String): GitWorkspaceException = GitWorkspaceException(
        "This device holds only recent history of $branch, and checking that your push only adds commits " +
            "needs more history than it may download. Push your work to a new branch and open a pull request, " +
            "or push from a full clone on a computer.",
        code = GitErrorCode.resourceLimitExceeded,
        detail = "shallow_history_limit",
    )

    private fun <T> bounded(branch: String, body: () -> T): T = try {
        body()
    } catch (error: GitException) {
        if (error.code == GitErrorCode.resourceLimitExceeded) throw historyLimit(branch)
        throw error
    }
}
