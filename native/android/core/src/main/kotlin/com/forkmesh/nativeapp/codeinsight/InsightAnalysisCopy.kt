package com.forkmesh.nativeapp.codeinsight

import com.forkmesh.nativeapp.git.crypto.GitSha1CollisionDetector
import com.forkmesh.nativeapp.git.crypto.GitSha1DcDetector
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.objects.GitObjectId
import com.forkmesh.nativeapp.git.refs.GitRefName
import com.forkmesh.nativeapp.git.refs.RefStore
import com.forkmesh.nativeapp.git.repository.GitCloneRequest
import com.forkmesh.nativeapp.git.repository.GitDeepen
import com.forkmesh.nativeapp.git.repository.GitFetchRequest
import com.forkmesh.nativeapp.git.repository.GitProgressListener
import com.forkmesh.nativeapp.git.repository.GitRemoteAccess
import com.forkmesh.nativeapp.git.repository.GitRepository
import com.forkmesh.nativeapp.git.repository.GitRepositoryOpen
import com.forkmesh.nativeapp.git.repository.isShallow
import com.forkmesh.nativeapp.git.repository.shallowCommits
import com.forkmesh.nativeapp.git.security.GitCancellationToken
import com.forkmesh.nativeapp.git.transport.GitHttpTransport
import com.forkmesh.nativeapp.git.transport.GitRemoteUrl
import com.forkmesh.nativeapp.localrepos.CloneEndpoint
import com.forkmesh.nativeapp.localrepos.CloneEndpoints
import com.forkmesh.nativeapp.localrepos.CloneFailure
import com.forkmesh.nativeapp.localrepos.ForkMeshRemoteCredentials
import com.forkmesh.nativeapp.localrepos.GitTransferProgress
import com.forkmesh.nativeapp.localrepos.GitWorkspace
import com.forkmesh.nativeapp.localrepos.GitWorkspaceException
import com.forkmesh.nativeapp.localrepos.describeLocalRepoFailure
import com.forkmesh.nativeapp.localrepos.describeRemoteFailure
import com.forkmesh.nativeapp.localrepos.forkMeshRemoteAccess
import com.forkmesh.nativeapp.model.RepoMirror
import java.io.File
import java.nio.file.Files

data class InsightCopyState(val directory: File, val branch: String, val tip: String, val servedBy: String, val message: String)

class InsightCopyException(override val message: String) : RuntimeException(message) {
    override fun toString(): String = message
}

class InsightAnalysisCopy(
    val root: File,
    private val detector: () -> GitSha1CollisionDetector? = { GitSha1DcDetector() },
    private val transport: () -> GitHttpTransport? = { null },
    private val clock: () -> Long = System::currentTimeMillis,
) {
    fun directoryFor(owner: String, name: String): File {
        val folder = InsightLabels.copyFolder(owner, name) ?: throw InsightCopyException("$owner/$name is not a repository name this phone can store.")
        return File(root, folder)
    }

    fun exists(owner: String, name: String): Boolean = File(directoryFor(owner, name), ".git").isDirectory

    fun sync(
        owner: String,
        name: String,
        relay: ForkMeshRemoteCredentials,
        mirrors: List<RepoMirror>,
        depth: Int,
        lookupProblem: String? = null,
        cancellation: GitCancellationToken = GitCancellationToken(),
        onStatus: (String) -> Unit = {},
    ): InsightCopyState {
        val repository = "$owner/$name"
        val directory = directoryFor(owner, name)
        val endpoints = CloneEndpoints.plan(owner, name, mirrors, relay)
        var source: CloneEndpoint? = null
        val existing = openExisting(directory)
        try {
            val state = CloneEndpoints.run(
                repository = repository,
                endpoints = endpoints,
                lookupProblem = lookupProblem,
                onAttempt = { endpoint, index ->
                    source = endpoint
                    onStatus(CloneEndpoints.attemptLabel(endpoint, index, endpoints.size))
                },
            ) { endpoint ->
                val credentials = endpoint.credentials(relay)
                val progress = GitProgressListener { sample ->
                    onStatus(CloneEndpoints.progressLabel(GitTransferProgress(sample.phase.name, sample.bytes, sample.objects, sample.refs, sample.total), endpoint.label))
                }
                remote {
                    if (existing != null) {
                        refresh(existing, credentials, depth, cancellation, progress)
                    } else {
                        clone(directory, credentials, depth, endpoint.isRelay, cancellation, progress)
                    }
                }
            }
            val from = source?.let { " from ${it.label}" } ?: ""
            val verb = if (existing != null) "Fetched" else "Downloaded"
            return state.copy(servedBy = source?.label ?: "", message = "$verb $repository$from.")
        } catch (failure: CloneFailure) {
            throw InsightCopyException(failure.message)
        }
    }

    private fun <T> remote(body: () -> T): T = try {
        body()
    } catch (error: GitException) {
        val failure = describeRemoteFailure(error)
        throw GitWorkspaceException(failure.message, code = error.code, detail = failure.code)
    }

    private fun openExisting(directory: File): GitRepository? {
        if (!directory.exists()) return null
        if (!File(directory, ".git").isDirectory) {
            if (directory.isDirectory && (directory.list()?.isEmpty() == true)) return null
            moveAside(directory)
            return null
        }
        return try {
            GitRepository.open(GitRepositoryOpen(directory, limits = GitWorkspace.deviceLimits(), detector = detector()))
        } catch (error: GitException) {
            moveAside(directory)
            null
        }
    }

    private fun moveAside(directory: File) {
        val target = File(directory.parentFile, "${directory.name}.unusable-${clock()}")
        try {
            Files.move(directory.toPath(), target.toPath())
        } catch (error: Exception) {
            throw InsightCopyException("The analysis copy in ${directory.path} cannot be opened or moved aside: ${describeLocalRepoFailure(error)}")
        }
    }

    private fun clone(
        directory: File,
        credentials: ForkMeshRemoteCredentials,
        depth: Int,
        relay: Boolean,
        cancellation: GitCancellationToken,
        progress: GitProgressListener,
    ): InsightCopyState {
        directory.parentFile?.let { Files.createDirectories(it.toPath()) }
        val access = forkMeshRemoteAccess(credentials, transport())
        val result = try {
            try {
                GitRepository.clone(cloneRequest(directory, credentials, depth, access, cancellation), progress).value
            } catch (error: GitException) {
                if (!relay || error.code != GitErrorCode.protocolUnsupported) throw error
                GitRepository.clone(cloneRequest(directory, credentials, null, access, cancellation), progress).value
            }
        } finally {
            access.transport?.close()
        }
        return stateOf(result.repository, directory)
    }

    private fun cloneRequest(
        directory: File,
        credentials: ForkMeshRemoteCredentials,
        depth: Int?,
        access: GitRemoteAccess,
        cancellation: GitCancellationToken,
    ) = GitCloneRequest(
        url = GitRemoteUrl.parse(credentials.remoteUrl),
        worktree = directory,
        depth = depth,
        singleBranch = true,
        access = access,
        limits = GitWorkspace.deviceLimits(),
        detector = detector(),
        cancellation = cancellation,
    )

    private fun refresh(
        repository: GitRepository,
        credentials: ForkMeshRemoteCredentials,
        depth: Int,
        cancellation: GitCancellationToken,
        progress: GitProgressListener,
    ): InsightCopyState {
        val existing = repository.readConfig().remotes["origin"]?.fetch
        repository.setRemote("origin", GitRemoteUrl.parse(credentials.remoteUrl), fetch = existing)
        fetch(repository, credentials, null, cancellation, progress)
        if (repository.isShallow && depth > 1) {
            val tip = tipOf(repository)
            val have = if (tip == null) 0 else reachable(repository, tip, depth)
            if (have < depth) {
                try {
                    fetch(repository, credentials, GitDeepen.Relative(depth - have), cancellation, progress)
                } catch (error: GitException) {
                    if (error.code != GitErrorCode.protocolUnsupported) throw error
                }
            }
        }
        return stateOf(repository, repository.worktree)
    }

    private fun fetch(
        repository: GitRepository,
        credentials: ForkMeshRemoteCredentials,
        deepen: GitDeepen?,
        cancellation: GitCancellationToken,
        progress: GitProgressListener,
    ) {
        val access = forkMeshRemoteAccess(credentials, transport())
        try {
            repository.fetch(GitFetchRequest(remote = "origin", deepen = deepen, access = access, cancellation = cancellation), progress).value
        } finally {
            access.transport?.close()
        }
    }

    private fun stateOf(repository: GitRepository, directory: File): InsightCopyState {
        val branch = branchOf(repository)
        val tip = tipOf(repository) ?: throw InsightCopyException("The repository has no commits yet.")
        return InsightCopyState(directory, branch, tip.hex, "", "")
    }

    companion object {
        fun branchOf(repository: GitRepository): String = try {
            repository.readHead().target.removePrefix("refs/heads/")
        } catch (_: GitException) {
            ""
        }

        fun trackingRef(branch: String): String? = if (branch.isEmpty()) null else "refs/remotes/origin/$branch"

        fun tipOf(repository: GitRepository): GitObjectId? {
            val tracking = trackingRef(branchOf(repository))
            val remote = tracking?.let {
                try {
                    RefStore(repository.gitDirectory).readDirect(GitRefName.parse(it))
                } catch (_: GitException) {
                    null
                }
            }
            return remote ?: repository.readHeadObjectId()
        }

        fun reachable(repository: GitRepository, tip: GitObjectId, limit: Int): Int {
            val shallow = repository.shallowCommits()
            val seen = HashSet<GitObjectId>()
            val stack = ArrayList<GitObjectId>()
            stack.add(tip)
            while (stack.isNotEmpty() && seen.size < limit) {
                val id = stack.removeAt(stack.size - 1)
                if (id in seen || !repository.hasObject(id)) continue
                seen.add(id)
                if (id in shallow) continue
                val commit = try {
                    repository.readCommit(id)
                } catch (_: GitException) {
                    continue
                }
                stack.addAll(commit.parents)
            }
            return seen.size
        }
    }
}
