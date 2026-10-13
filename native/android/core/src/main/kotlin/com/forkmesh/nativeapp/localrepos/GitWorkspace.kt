package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.git.crypto.GitSha1CollisionDetector
import com.forkmesh.nativeapp.git.crypto.GitSha1DcDetector
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.security.GitResourceLimits
import com.forkmesh.nativeapp.git.transport.GitHttpTransport
import java.io.File
import kotlin.time.Duration
import kotlin.time.Duration.Companion.minutes

fun interface GitPerformanceMonitor {
    fun recordOperation(name: String, durationMillis: Long, details: Map<String, Any?>)
}

open class GitWorkspace(
    val worktree: File,
    private val performanceMonitor: GitPerformanceMonitor? = null,
    runner: GitIsolateRunner? = null,
    val detector: GitSha1CollisionDetector? = GitSha1DcDetector(),
    private val clock: () -> Long = System::currentTimeMillis,
) {
    private val runner: GitIsolateRunner = runner ?: GitIsolateRunner { name, elapsed, failed ->
        performanceMonitor?.recordOperation(
            "git.$name",
            elapsed,
            buildMap {
                put("repo", worktree.name)
                if (failed) put("failed", true)
            },
        )
    }

    @Volatile
    var transportFactory: (() -> GitHttpTransport)? = null

    private val ops = GitWorkspaceOps(worktree, detector) { transportFactory?.invoke() }

    open val isBusy: Boolean get() = runner.isBusy

    open fun cancel() = runner.cancel()

    open fun dispose() = runner.dispose()

    open fun init(files: Map<String, String> = emptyMap(), commitMessage: String? = null, author: GitAuthor? = null, branch: String = "main"): String? {
        val now = clock()
        return run("init") { ops.init(it, files, commitMessage, author, now, branch) }
    }

    open fun importFromDirectory(source: File, author: GitAuthor, message: String = "Import"): LocalImportResult =
        importFromTree(FileImportDirectory(source), author, message)

    open fun importFromTree(source: LocalImportDirectory, author: GitAuthor, message: String = "Import"): LocalImportResult {
        val now = clock()
        return run("import-directory") { ops.importDirectory(it, source, author, message, now) }
    }

    open fun importFromZip(zipPath: String, author: GitAuthor, message: String = "Import"): LocalImportResult {
        val now = clock()
        return run("import-zip") { ops.importZip(it, File(zipPath), author, message, now) }
    }

    open fun listFiles(directory: String = ""): List<LocalFileEntry> = run("list-files") { ops.listFiles(it, directory) }

    open fun readFile(path: String): LocalFileContent = run("read-file") { ops.readFile(path) }

    open fun writeFile(path: String, text: String) = run("write-file") { ops.writeFile(path, text) }

    open fun deleteFile(path: String) = run("delete-file") { ops.deleteFile(path) }

    open fun status(): LocalStatus = run("status") { ops.status(it) }

    open fun stage(paths: Iterable<String>) {
        val targets = paths.toList()
        run("stage") { ops.stage(it, targets) }
    }

    open fun stageAll() = run("stage-all") { ops.stageAll(it) }

    open fun unstage(paths: Iterable<String>) {
        val targets = paths.toList()
        run("unstage") { ops.unstage(it, targets.toSet()) }
    }

    open fun unstageAll() = run("unstage-all") { ops.unstageAll(it) }

    open fun discard(paths: Iterable<String>) {
        val targets = paths.toList()
        run("discard") { ops.discard(it, targets) }
    }

    open fun commit(message: String, author: GitAuthor): String {
        val now = clock()
        return run("commit") { ops.commit(it, message, author, now) }
    }

    open fun branches(): List<LocalBranch> = run("branches") { ops.branches() }

    open fun createBranch(name: String, checkout: Boolean = false) =
        run("create-branch") { ops.createBranch(it, name, checkout) }

    open fun checkout(name: String) = run("checkout") { ops.checkout(it, name) }

    open fun log(limit: Int = 200): List<LocalCommit> {
        val bounded = limit.coerceIn(1, maxLogEntries)
        return run("log") { ops.log(it, bounded) }
    }

    open fun summary(): LocalRepoSummary = run("summary") { ops.summary() }

    open fun push(
        credentials: ForkMeshRemoteCredentials,
        branch: String,
        remote: String = "origin",
        onProgress: ((GitTransferProgress) -> Unit)? = null,
    ): LocalPushResult = runRemote("push", onProgress, push = true) { ops.push(it, credentials, remote, branch) }

    open fun fetch(
        credentials: ForkMeshRemoteCredentials,
        remote: String = "origin",
        onProgress: ((GitTransferProgress) -> Unit)? = null,
    ): LocalFetchResult = runRemote("fetch", onProgress) { ops.fetch(it, credentials, remote) }

    open fun pull(
        credentials: ForkMeshRemoteCredentials,
        branch: String,
        remote: String = "origin",
        onProgress: ((GitTransferProgress) -> Unit)? = null,
    ): LocalPullResult = runRemote("pull", onProgress) { ops.pull(it, credentials, remote, branch) }

    open fun clone(
        credentials: ForkMeshRemoteCredentials,
        branch: String? = null,
        maxPackBytes: Int = maxCloneBytes,
        onProgress: ((GitTransferProgress) -> Unit)? = null,
        depth: Int? = null,
    ): LocalCloneResult = runRemote("clone", onProgress) { ops.clone(it, credentials, branch, maxPackBytes, depth) }

    open fun syncState(branch: String, remote: String = "origin"): LocalSyncState =
        run("sync-state") { ops.syncState(it, remote, branch) }

    private fun <T> runRemote(
        name: String,
        onProgress: ((GitTransferProgress) -> Unit)?,
        push: Boolean = false,
        body: (GitWorkContext) -> T,
    ): T {
        try {
            return runner.runWithProgress(
                name,
                body,
                onProgress?.let { listener -> { message: Any -> if (message is GitTransferProgress) listener(message) } },
            )
        } catch (error: GitException) {
            val failure = describeRemoteFailure(error, push = push)
            throw GitWorkspaceException(failure.message, code = error.code, detail = failure.code)
        } catch (error: GitWorkerException) {
            throw GitWorkspaceException(error.message, detail = error.detail)
        }
    }

    private fun <T> run(name: String, body: (GitWorkContext) -> T): T {
        try {
            return runner.run(name, body)
        } catch (error: GitException) {
            throw GitWorkspaceException(describe(error), code = error.code, detail = error.message)
        } catch (error: GitWorkerException) {
            throw GitWorkspaceException(error.message, detail = error.detail)
        }
    }

    companion object {
        const val maxLogEntries = 500

        const val maxCloneBytes = GitResourceLimits.hardMaxCompressedPackBytes

        const val meshCloneDepth = 1

        val cloneDeadline: Duration = 20.minutes

        fun deviceLimits(maxPackBytes: Int = maxCloneBytes): GitResourceLimits = GitResourceLimits(
            maxObjectBytes = GitResourceLimits.hardMaxObjectBytes,
            maxInflatedBytes = GitResourceLimits.hardMaxInflatedBytes,
            maxCompressedPackBytes = maxPackBytes,
            connectTimeout = GitResourceLimits.hardConnectTimeout,
            idleReadTimeout = GitResourceLimits.hardIdleReadTimeout,
            operationDeadline = cloneDeadline,
        )

        fun describe(error: Any?): String {
            if (error is GitWorkspaceException) return error.message
            if (error is GitException) {
                return when (error.code) {
                    GitErrorCode.cancelled -> "Operation cancelled."
                    GitErrorCode.resourceLimitExceeded -> "This content exceeds the on-device Git limits."
                    GitErrorCode.pathEscapesWorktree,
                    GitErrorCode.symlinkEscape,
                    -> "A path was rejected because it is not safe or portable."
                    GitErrorCode.refLocked -> "The repository is busy with another operation. Try again."
                    GitErrorCode.nonFastForward -> "The repository changed while committing. Try again."
                    GitErrorCode.operationInterrupted -> if (error.message.contains("local changes")) {
                        "Commit or discard your changes before switching branches."
                    } else {
                        "A previous operation was interrupted. Reopen the repository."
                    }
                    GitErrorCode.invalidRef -> when {
                        error.message.contains("does not exist") -> "That branch does not exist."
                        error.message.contains("unborn") -> "Make a first commit before creating branches."
                        else -> "That branch name is not valid."
                    }
                    GitErrorCode.invalidRepository -> if (error.message.contains("already exists")) {
                        "A repository already exists at this location."
                    } else {
                        "This is not a usable Git repository."
                    }
                    GitErrorCode.unsupportedRepositoryFormat ->
                        "This repository uses a format the mobile engine does not support."
                    GitErrorCode.indexCorrupt,
                    GitErrorCode.packCorrupt,
                    GitErrorCode.packChecksumMismatch,
                    GitErrorCode.objectHashMismatch,
                    GitErrorCode.invalidObject,
                    GitErrorCode.invalidTree,
                    GitErrorCode.deltaDepthExceeded,
                    -> "Repository data is corrupt (${error.code.name})."
                    GitErrorCode.sha1CollisionDetected,
                    GitErrorCode.hashScreeningUnavailable,
                    GitErrorCode.hashImplementationFailure,
                    GitErrorCode.quarantineRejected,
                    -> "Object hashing failed (${error.code.name})."
                    GitErrorCode.networkUnavailable,
                    GitErrorCode.tlsRejected,
                    GitErrorCode.authenticationFailed,
                    GitErrorCode.protocolUnsupported,
                    GitErrorCode.remoteRejected,
                    -> "Network operation failed (${error.code.name})."
                }
            }
            if (error is GitWorkerException) return error.message
            return error.toString()
        }
    }
}
