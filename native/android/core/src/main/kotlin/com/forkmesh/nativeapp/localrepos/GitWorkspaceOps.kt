package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.git.bytes.dartTrim
import com.forkmesh.nativeapp.git.bytes.utf8Decode
import com.forkmesh.nativeapp.git.bytes.utf8Encode
import com.forkmesh.nativeapp.git.crypto.GitSha1CollisionDetector
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.index.GitIndexEntry
import com.forkmesh.nativeapp.git.index.GitIndexMode
import com.forkmesh.nativeapp.git.index.GitIndexStore
import com.forkmesh.nativeapp.git.objects.GitCommit
import com.forkmesh.nativeapp.git.refs.GitFetchRefspec
import com.forkmesh.nativeapp.git.refs.GitPushRefspec
import com.forkmesh.nativeapp.git.refs.GitRefName
import com.forkmesh.nativeapp.git.refs.RefStore
import com.forkmesh.nativeapp.git.repository.GitAncestry
import com.forkmesh.nativeapp.git.repository.GitBranchName
import com.forkmesh.nativeapp.git.repository.GitChangeKind
import com.forkmesh.nativeapp.git.repository.GitCheckoutRequest
import com.forkmesh.nativeapp.git.repository.GitCloneRequest
import com.forkmesh.nativeapp.git.repository.GitCommitRequest
import com.forkmesh.nativeapp.git.repository.GitDeepen
import com.forkmesh.nativeapp.git.repository.GitFastForwardOutcome
import com.forkmesh.nativeapp.git.repository.GitFetchRequest
import com.forkmesh.nativeapp.git.repository.GitFetchResult
import com.forkmesh.nativeapp.git.repository.GitLogRequest
import com.forkmesh.nativeapp.git.repository.GitProgress
import com.forkmesh.nativeapp.git.repository.GitProgressPhase
import com.forkmesh.nativeapp.git.repository.GitProgressListener
import com.forkmesh.nativeapp.git.repository.GitPushRequest
import com.forkmesh.nativeapp.git.repository.GitRepository
import com.forkmesh.nativeapp.git.repository.GitRepositoryInit
import com.forkmesh.nativeapp.git.repository.GitRepositoryOpen
import com.forkmesh.nativeapp.git.repository.ancestry
import com.forkmesh.nativeapp.git.repository.isShallow
import com.forkmesh.nativeapp.git.security.GitResourceLimits
import com.forkmesh.nativeapp.git.transport.GitHttpTransport
import com.forkmesh.nativeapp.git.transport.GitRemoteUrl
import com.forkmesh.nativeapp.git.worktree.FileSystemEntityType
import com.forkmesh.nativeapp.git.worktree.GitWorktree
import com.forkmesh.nativeapp.git.worktree.GitWorktreePath
import com.forkmesh.nativeapp.git.worktree.entityType
import java.io.File
import java.io.IOException
import java.nio.file.Files

internal class GitWorkspaceOps(
    private val path: File,
    private val detector: GitSha1CollisionDetector?,
    private val inner: () -> GitHttpTransport? = { null },
) {
    private fun open(): GitRepository = GitRepository.open(GitRepositoryOpen(path, limits = GitWorkspace.deviceLimits(), detector = detector))

    fun init(context: GitWorkContext, files: Map<String, String>, message: String?, author: GitAuthor?, now: Long, branch: String = "main"): String? {
        val repo = GitRepository.init(GitRepositoryInit(path, detector = detector, initialBranch = branch))
        if (files.isEmpty()) return null
        val tree = GitWorktree(repo.worktree)
        val paths = ArrayList<GitWorktreePath>()
        for ((key, value) in files) {
            val target = GitWorktreePath(key)
            tree.writeFile(target, utf8Encode(value))
            paths.add(target)
        }
        repo.add(paths, context.cancellation)
        if (message == null || author == null) return null
        val identity = author.toIdentity(now)
        return repo.commit(GitCommitRequest(message, identity, identity), context.cancellation).hex
    }

    fun importDirectory(
        context: GitWorkContext,
        source: LocalImportDirectory,
        author: GitAuthor,
        message: String,
        now: Long,
    ): LocalImportResult {
        if (!source.isReadableDirectory()) throw GitWorkerException("The selected folder could not be read.")
        val repo = GitRepository.init(GitRepositoryInit(path, detector = detector))
        val tree = GitWorktree(repo.worktree)
        var imported = 0
        var skipped = 0
        var totalBytes = 0L
        val roots = LinkedHashSet<String>()

        fun walk(directory: LocalImportDirectory, prefix: List<String>) {
            if (prefix.size > maxTreeDepth) {
                throw GitException(GitErrorCode.resourceLimitExceeded, "import tree is too deep")
            }
            for (entry in directory.list()) {
                context.cancellation.throwIfCancelled()
                val name = entry.name
                if (prefix.isEmpty() && name == ".git") {
                    skipped += 1
                    continue
                }
                val components = prefix + name
                val target = try {
                    GitWorktreePath(components.joinToString("/"))
                } catch (error: GitException) {
                    skipped += 1
                    continue
                }
                when (entry.type) {
                    FileSystemEntityType.directory -> walk(entry.directory(), components)
                    FileSystemEntityType.file -> {
                        if (entry.size > maxImportFileBytes) {
                            throw GitException(GitErrorCode.resourceLimitExceeded, "a file exceeds the maximum object size")
                        }
                        val bytes = readBounded(entry, maxImportFileBytes)
                        if (bytes.size > maxImportFileBytes) {
                            throw GitException(GitErrorCode.resourceLimitExceeded, "a file exceeds the maximum object size")
                        }
                        totalBytes += bytes.size
                        imported += 1
                        if (imported > maxImportFiles || totalBytes > maxImportBytes) {
                            throw GitException(GitErrorCode.resourceLimitExceeded, "import exceeds file or byte limits")
                        }
                        tree.writeFile(target, bytes)
                        roots.add(components.first())
                    }
                    else -> skipped += 1
                }
            }
        }

        walk(source, emptyList())
        val commitId = commitImport(context, repo, roots, author, message, now, imported)
        return LocalImportResult(importedFiles = imported, skippedEntries = skipped, commitId = commitId)
    }

    fun importZip(context: GitWorkContext, zip: File, author: GitAuthor, message: String, now: Long): LocalImportResult {
        if (entityType(zip) != FileSystemEntityType.file) {
            throw GitWorkerException("The selected archive could not be read.")
        }
        if (zip.length() > maxZipBytes) {
            throw GitException(GitErrorCode.resourceLimitExceeded, "zip archive is too large")
        }
        val archive = try {
            ZipArchive.open(zip)
        } catch (error: Exception) {
            throw GitWorkerException("The archive is not a valid zip file.", error.toString())
        }
        archive.use {
            val candidates = ArrayList<ZipArchiveFile>()
            var declaredBytes = 0L
            for (file in archive.files) {
                if (!file.isFile || file.isSymbolicLink) continue
                val name = file.name
                if (name.startsWith("__MACOSX/") || name.endsWith("/.DS_Store") || name == ".DS_Store") continue
                if (file.size > maxImportFileBytes) {
                    throw GitException(GitErrorCode.resourceLimitExceeded, "a file exceeds the maximum object size")
                }
                declaredBytes += file.size
                candidates.add(file)
                if (candidates.size > maxImportFiles || declaredBytes > maxImportBytes) {
                    throw GitException(GitErrorCode.resourceLimitExceeded, "import exceeds file or byte limits")
                }
            }
            var sharedRoot: String? = null
            var allShareRoot = candidates.isNotEmpty()
            for (file in candidates) {
                val slash = file.name.indexOf('/')
                if (slash <= 0) {
                    allShareRoot = false
                    break
                }
                val root = file.name.substring(0, slash)
                if (sharedRoot == null) {
                    sharedRoot = root
                } else if (sharedRoot != root) {
                    allShareRoot = false
                    break
                }
            }
            val strip = if (allShareRoot && sharedRoot != null) "$sharedRoot/" else ""

            val repo = GitRepository.init(GitRepositoryInit(path, detector = detector))
            val tree = GitWorktree(repo.worktree)
            var imported = 0
            var skipped = 0
            var totalBytes = 0L
            val roots = LinkedHashSet<String>()
            for (file in candidates) {
                context.cancellation.throwIfCancelled()
                var name = file.name
                if (strip.isNotEmpty() && name.startsWith(strip)) name = name.substring(strip.length)
                while (name.startsWith("./")) name = name.substring(2)
                val target = try {
                    GitWorktreePath(name)
                } catch (error: GitException) {
                    skipped += 1
                    continue
                }
                if (target.components.first() == ".git" || target.components.size > maxTreeDepth) {
                    skipped += 1
                    continue
                }
                val bytes = archive.content(file, maxImportFileBytes.toLong())
                totalBytes += bytes.size
                if (bytes.size > maxImportFileBytes || totalBytes > maxImportBytes) {
                    throw GitException(GitErrorCode.resourceLimitExceeded, "import exceeds file or byte limits")
                }
                tree.writeFile(target, bytes)
                imported += 1
                roots.add(target.components.first())
            }
            val commitId = commitImport(context, repo, roots, author, message, now, imported)
            return LocalImportResult(importedFiles = imported, skippedEntries = skipped, commitId = commitId)
        }
    }

    private fun commitImport(
        context: GitWorkContext,
        repo: GitRepository,
        roots: Set<String>,
        author: GitAuthor,
        message: String,
        now: Long,
        imported: Int,
    ): String {
        if (imported == 0) throw GitWorkerException("Nothing importable was found.")
        repo.add(roots.map(::GitWorktreePath), context.cancellation)
        val identity = author.toIdentity(now)
        return repo.commit(GitCommitRequest(message, identity, identity), context.cancellation).hex
    }

    fun listFiles(context: GitWorkContext, target: String): List<LocalFileEntry> {
        val repo = open()
        val index = indexPaths(repo)
        val prefix = if (target.isEmpty()) emptyList() else GitWorktreePath(target).components
        var directory = repo.worktree
        for (component in prefix) {
            val next = File(directory, component)
            if (entityType(next) != FileSystemEntityType.directory) {
                throw GitException(GitErrorCode.pathEscapesWorktree, "directory is missing or is a symlink")
            }
            directory = next
        }
        val entries = ArrayList<LocalFileEntry>()
        Files.newDirectoryStream(directory.toPath()).use { stream ->
            for (child in stream) {
                context.cancellation.throwIfCancelled()
                val name = child.fileName.toString()
                if (prefix.isEmpty() && name == ".git") continue
                val relative = (prefix + name).joinToString("/")
                val kind = when (entityType(child.toFile())) {
                    FileSystemEntityType.directory -> LocalFileKind.directory
                    FileSystemEntityType.link -> LocalFileKind.symlink
                    else -> LocalFileKind.file
                }
                val size = if (kind == LocalFileKind.file) {
                    try {
                        Files.size(child)
                    } catch (error: IOException) {
                        0L
                    }
                } else {
                    0L
                }
                val tracked = if (kind == LocalFileKind.directory) {
                    index.any { it.startsWith("$relative/") }
                } else {
                    relative in index
                }
                entries.add(LocalFileEntry(name = name, path = relative, kind = kind, size = size, tracked = tracked))
            }
        }
        return entries.sortedWith { a, b ->
            if (a.isDirectory != b.isDirectory) {
                if (a.isDirectory) -1 else 1
            } else {
                a.name.lowercase().compareTo(b.name.lowercase())
            }
        }
    }

    fun readFile(target: String): LocalFileContent {
        val repo = open()
        val bytes = GitWorktree(repo.worktree).readFile(GitWorktreePath(target))
        if (bytes.size > LocalFileContent.maxEditableBytes) {
            return LocalFileContent(path = target, bytes = bytes.size, text = "", isBinary = false, isTooLarge = true)
        }
        val probe = minOf(bytes.size, 8000)
        for (index in 0 until probe) {
            if (bytes[index].toInt() == 0) {
                return LocalFileContent(path = target, bytes = bytes.size, text = "", isBinary = true, isTooLarge = false)
            }
        }
        return try {
            LocalFileContent(path = target, bytes = bytes.size, text = utf8Decode(bytes), isBinary = false, isTooLarge = false)
        } catch (error: IllegalArgumentException) {
            LocalFileContent(path = target, bytes = bytes.size, text = "", isBinary = true, isTooLarge = false)
        }
    }

    fun writeFile(target: String, text: String) {
        val repo = open()
        val worktreePath = GitWorktreePath(target)
        if (worktreePath.components.first() == ".git") {
            throw GitException(GitErrorCode.pathEscapesWorktree, "cannot write repository metadata")
        }
        val bytes = utf8Encode(text)
        if (bytes.size > repo.limits.maxObjectBytes) {
            throw GitException(GitErrorCode.resourceLimitExceeded, "file exceeds the maximum object size")
        }
        GitWorktree(repo.worktree).writeFile(worktreePath, bytes)
    }

    fun deleteFile(target: String) {
        val repo = open()
        val worktreePath = GitWorktreePath(target)
        if (worktreePath.components.first() == ".git") {
            throw GitException(GitErrorCode.pathEscapesWorktree, "cannot delete repository metadata")
        }
        GitWorktree(repo.worktree).deleteFile(worktreePath)
    }

    fun status(context: GitWorkContext): LocalStatus = status(open(), context)

    private fun status(repo: GitRepository, context: GitWorkContext): LocalStatus {
        val status = repo.status(context.cancellation)
        fun kindOf(kind: GitChangeKind): LocalChangeKind = when (kind) {
            GitChangeKind.added -> LocalChangeKind.added
            GitChangeKind.modified -> LocalChangeKind.modified
            GitChangeKind.deleted -> LocalChangeKind.deleted
            GitChangeKind.untracked -> LocalChangeKind.untracked
        }
        val staged = status.staged.map { LocalChange(path = it.path.value, kind = kindOf(it.kind), staged = true) }
        val unstaged = ArrayList<LocalChange>()
        val untracked = ArrayList<LocalChange>()
        for (entry in status.worktree) {
            val change = LocalChange(path = entry.path.value, kind = kindOf(entry.kind), staged = false)
            if (entry.kind == GitChangeKind.untracked) untracked.add(change) else unstaged.add(change)
        }
        return LocalStatus(staged = staged, unstaged = unstaged, untracked = untracked)
    }

    fun stage(context: GitWorkContext, targets: List<String>) = stagePaths(open(), targets, context)

    fun stageAll(context: GitWorkContext) {
        val repo = open()
        val status = status(repo, context)
        stagePaths(repo, status.unstaged.map { it.path } + status.untracked.map { it.path }, context)
    }

    private fun stagePaths(repo: GitRepository, targets: List<String>, context: GitWorkContext) {
        val adds = ArrayList<GitWorktreePath>()
        val removes = ArrayList<GitWorktreePath>()
        val tree = GitWorktree(repo.worktree)
        for (target in targets) {
            val worktreePath = GitWorktreePath(target)
            if (tree.typeOf(worktreePath) == FileSystemEntityType.notFound) {
                removes.add(worktreePath)
            } else {
                adds.add(worktreePath)
            }
        }
        if (removes.isNotEmpty()) repo.remove(removes)
        if (adds.isNotEmpty()) repo.add(adds, context.cancellation)
    }

    fun unstage(context: GitWorkContext, targets: Set<String>) = unstagePaths(open(), targets, context)

    fun unstageAll(context: GitWorkContext) {
        val repo = open()
        val status = status(repo, context)
        unstagePaths(repo, status.staged.map { it.path }.toSet(), context)
    }

    private fun unstagePaths(repo: GitRepository, targets: Set<String>, context: GitWorkContext) {
        if (targets.isEmpty()) return
        repo.resetPaths(targets.map(::GitWorktreePath), context.cancellation)
    }

    fun discard(context: GitWorkContext, targets: List<String>) {
        val repo = open()
        val index = GitIndexStore(repo.gitDirectory, repo.limits).read()
        val entries = LinkedHashMap<String, GitIndexEntry>()
        for (entry in index.entries) entries[utf8Decode(entry.path)] = entry
        val tree = GitWorktree(repo.worktree)
        for (target in targets) {
            context.cancellation.throwIfCancelled()
            val worktreePath = GitWorktreePath(target)
            if (worktreePath.components.first() == ".git") continue
            val entry = entries[target]
            if (entry == null) {
                tree.deleteFile(worktreePath)
                continue
            }
            val bytes = repo.readBlob(entry.objectId)
            if (entry.mode == GitIndexMode.symlink) {
                tree.writeSymlink(worktreePath, utf8Decode(bytes))
            } else {
                tree.writeFile(worktreePath, bytes)
            }
        }
    }

    fun commit(context: GitWorkContext, rawMessage: String, author: GitAuthor, now: Long): String {
        val repo = open()
        val message = rawMessage.dartTrim()
        if (message.isEmpty()) throw GitWorkerException("Enter a commit message.")
        val status = repo.status(context.cancellation)
        if (status.staged.isEmpty()) throw GitWorkerException("Nothing is staged. Stage changes first.")
        val identity = author.toIdentity(now)
        return repo.commit(
            GitCommitRequest(
                message = if (message.endsWith("\n")) message else "$message\n",
                author = identity,
                committer = identity,
            ),
            context.cancellation,
        ).hex
    }

    fun branches(): List<LocalBranch> {
        val repo = open()
        val current = try {
            repo.readHead().target.substring("refs/heads/".length)
        } catch (error: GitException) {
            null
        }
        val branches = repo.listBranches().map {
            LocalBranch(name = it.shortName, isCurrent = it.shortName == current, tipId = it.objectId.hex)
        }.toMutableList()
        if (current != null && branches.none { it.name == current }) {
            branches.add(0, LocalBranch(name = current, isCurrent = true, tipId = null))
        }
        return branches
    }

    fun createBranch(context: GitWorkContext, name: String, checkout: Boolean) {
        val repo = open()
        val branch = GitBranchName(name)
        repo.createBranch(branch)
        if (checkout) repo.checkout(GitCheckoutRequest.branch(branch), context.cancellation)
    }

    fun checkout(context: GitWorkContext, name: String) {
        val repo = open()
        repo.checkout(GitCheckoutRequest.branch(GitBranchName(name)), context.cancellation)
    }

    fun log(context: GitWorkContext, limit: Int): List<LocalCommit> {
        val repo = open()
        return repo.log(GitLogRequest(limit = limit), context.cancellation).map { toLocalCommit(it.id.hex, it.commit) }
    }

    fun summary(): LocalRepoSummary {
        val repo = open()
        val branch = try {
            repo.readHead().target.substring("refs/heads/".length)
        } catch (error: GitException) {
            "HEAD"
        }
        val head = repo.readHeadObjectId()
            ?: return LocalRepoSummary(branch = branch, headId = null, lastCommitSubject = "", lastCommitAt = null)
        val commit = toLocalCommit(head.hex, repo.readCommit(head))
        return LocalRepoSummary(
            branch = branch,
            headId = head.hex,
            lastCommitSubject = commit.subject,
            lastCommitAt = commit.time,
        )
    }

    private fun toLocalCommit(id: String, commit: GitCommit): LocalCommit {
        val message = String(commit.message, Charsets.UTF_8)
        val subject = message.split("\n").first().dartTrim()
        val author = commit.author
        val committer = commit.committer
        return LocalCommit(
            id = id,
            parents = commit.parents.map { it.hex },
            subject = subject,
            message = message,
            authorName = author?.name ?: commit.headers["author"]?.firstOrNull() ?: "",
            authorEmail = author?.email ?: "",
            time = (committer?.timeUtc ?: author?.timeUtc)?.toEpochMilli() ?: 0L,
        )
    }

    private fun indexPaths(repo: GitRepository): Set<String> =
        GitIndexStore(repo.gitDirectory, repo.limits).read().entries.mapTo(HashSet()) { utf8Decode(it.path) }

    private fun pipe(context: GitWorkContext) = GitProgressListener { sample: GitProgress ->
        context.progress(
            GitTransferProgress(
                phase = sample.phase.name,
                bytes = sample.bytes,
                objects = sample.objects,
                refs = sample.refs,
                total = sample.total,
            ),
        )
    }

    fun push(context: GitWorkContext, credentials: ForkMeshRemoteCredentials, remote: String, branch: String): LocalPushResult {
        try {
            return pushOnce(context, credentials, remote, branch)
        } catch (error: GitException) {
            if (error.code != GitErrorCode.nonFastForward) throw error
            val repo = GitRepository.open(
                GitRepositoryOpen(
                    path,
                    limits = GitWorkspace.deviceLimits(),
                    detector = detector,
                ),
            )
            if (!repo.isShallow) throw error
            ShallowPushProof.prove(
                repo,
                branch = branch,
                remote = remote,
                fetch = { refspec, deepen -> fetchInto(context, repo, credentials, remote, refspec, deepen, route = null) },
                token = context.cancellation,
            )
            return pushOnce(context, credentials, remote, branch)
        }
    }

    private fun pushOnce(
        context: GitWorkContext,
        credentials: ForkMeshRemoteCredentials,
        remote: String,
        branch: String,
    ): LocalPushResult {
        val repo = open()
        pointRemote(repo, remote, credentials.remoteUrl)
        val access = forkMeshRemoteAccess(credentials, inner())
        try {
            val result = repo.push(
                GitPushRequest(
                    remote = remote,
                    refspecs = listOf(GitPushRefspec(localBranch = branch)),
                    access = access,
                    cancellation = context.cancellation,
                ),
                pipe(context),
            ).value
            return LocalPushResult(
                refs = result.refs.map {
                    LocalPushRef(
                        remoteRef = it.remoteRef,
                        newId = it.newId.hex,
                        outcome = it.outcome.name,
                        message = it.message ?: "",
                    )
                },
                packBytes = result.packBytes,
                objects = result.objects,
            )
        } finally {
            access.transport?.close()
        }
    }

    internal fun fetchInto(
        context: GitWorkContext,
        repo: GitRepository,
        credentials: ForkMeshRemoteCredentials,
        remote: String,
        refspec: GitFetchRefspec?,
        deepen: GitDeepen?,
        route: GitRemoteRoute?,
    ): GitFetchResult {
        pointRemote(repo, remote, credentials.remoteUrl)
        val access = forkMeshRemoteAccess(credentials, inner(), route)
        try {
            return repo.fetch(
                GitFetchRequest(
                    remote = remote,
                    refspec = refspec,
                    deepen = deepen,
                    access = access,
                    cancellation = context.cancellation,
                ),
                pipe(context),
            ).value
        } finally {
            access.transport?.close()
        }
    }

    fun fetch(context: GitWorkContext, credentials: ForkMeshRemoteCredentials, remote: String): LocalFetchResult {
        val route = GitRemoteRoute()
        val result = fetchInto(context, open(), credentials, remote, refspec = null, deepen = null, route = route)
        return localFetchResult(result, credentials, route)
    }

    private fun localFetchResult(result: GitFetchResult, credentials: ForkMeshRemoteCredentials, route: GitRemoteRoute) =
        LocalFetchResult(
            updatedRefs = result.updates.filter { it.isChanged }.map { it.ref },
            packBytes = result.packBytes,
            objects = result.objects,
            remoteHead = result.remoteHead,
            servedBy = route.mirrorHost ?: if (credentials.endpointUrl.isEmpty()) null else credentials.host,
        )

    fun pull(context: GitWorkContext, credentials: ForkMeshRemoteCredentials, remote: String, branch: String): LocalPullResult {
        val repo = open()
        val route = GitRemoteRoute()
        val tracking = GitRefName.parse("refs/remotes/$remote/$branch")
        val refspec = GitFetchRefspec.parse("+refs/heads/$branch:${tracking.value}")
        val fetched = fetchInto(context, repo, credentials, remote, refspec = refspec, deepen = null, route = route)
        val fetch = localFetchResult(fetched, credentials, route)
        val store = RefStore(repo.gitDirectory)
        val branchName = GitBranchName(branch)
        val remoteTip = store.readDirect(tracking) ?: return LocalPullResult(branch, fetch, LocalPullOutcome.NoRemoteBranch)
        val localTip = store.readDirect(branchName.ref)
            ?: throw GitException(GitErrorCode.invalidRef, "local branch $branch does not exist")
        if (localTip != remoteTip && repo.ancestry(remoteTip, localTip, context.cancellation) == GitAncestry.ancestor) {
            return LocalPullResult(branch, fetch, LocalPullOutcome.Ahead)
        }
        val listener = pipe(context)
        val forwarded = try {
            repo.fastForward(branchName, remoteTip, context.cancellation) { written, total ->
                listener.onProgress(GitProgress(GitProgressPhase.checkingOut, objects = written, total = total))
            }
        } catch (error: GitException) {
            if (error.code != GitErrorCode.operationInterrupted) throw error
            throw GitWorkerException(syncGuardMessage(branch), detail = error.message)
        }
        val outcome = when (forwarded) {
            GitFastForwardOutcome.upToDate -> LocalPullOutcome.UpToDate
            GitFastForwardOutcome.fastForwarded -> LocalPullOutcome.FastForwarded(localTip.hex, remoteTip.hex)
            GitFastForwardOutcome.diverged -> LocalPullOutcome.Diverged
            GitFastForwardOutcome.undetermined -> LocalPullOutcome.Undetermined
        }
        return LocalPullResult(branch, fetch, outcome)
    }

    private fun pointRemote(repo: GitRepository, remote: String, url: String) {
        val existing = repo.readConfig().remotes[remote]?.fetch
        repo.setRemote(remote, GitRemoteUrl.parse(url), fetch = existing)
    }

    fun clone(
        context: GitWorkContext,
        credentials: ForkMeshRemoteCredentials,
        branch: String?,
        maxPackBytes: Int,
        depth: Int? = null,
    ): LocalCloneResult {
        val access = forkMeshRemoteAccess(credentials, inner())
        try {
            val result = GitRepository.clone(
                GitCloneRequest(
                    url = GitRemoteUrl.parse(credentials.remoteUrl),
                    worktree = path,
                    branch = branch,
                    depth = depth,
                    singleBranch = depth != null,
                    access = access,
                    limits = GitWorkspace.deviceLimits(maxPackBytes),
                    detector = detector,
                    cancellation = context.cancellation,
                ),
                pipe(context),
            ).value
            if (credentials.remoteUrl != credentials.relayUrl && credentials.origin.isNotEmpty()) {
                pointRemote(result.repository, "origin", credentials.relayUrl)
            }
            return LocalCloneResult(
                branch = result.checkedOutBranch,
                packBytes = result.fetch.packBytes,
                objects = result.fetch.objects,
            )
        } finally {
            access.transport?.close()
        }
    }

    fun syncState(context: GitWorkContext, remote: String, branch: String): LocalSyncState {
        val repo = open()
        if (branch.isEmpty()) return LocalSyncState.unknown(branch)
        val store = RefStore(repo.gitDirectory)
        val localRef = "refs/heads/$branch"
        val remoteRef = "refs/remotes/$remote/$branch"
        val local = store.readDirect(GitRefName.parse(localRef))
        val tracking = store.readDirect(GitRefName.parse(remoteRef))
        if (local == null || tracking == null) {
            return LocalSyncState(branch, local?.hex, tracking?.hex, ahead = null, behind = null)
        }
        if (local == tracking) return LocalSyncState(branch, local.hex, tracking.hex, ahead = 0, behind = 0)
        return LocalSyncState(
            branch = branch,
            localTip = local.hex,
            remoteTip = tracking.hex,
            ahead = distance(repo, localRef, tracking.hex, context),
            behind = distance(repo, remoteRef, local.hex, context),
        )
    }

    private fun distance(repo: GitRepository, from: String, target: String, context: GitWorkContext): Int? {
        val entries = repo.log(GitLogRequest(ref = from, limit = maxSyncWalk), context.cancellation)
        val index = entries.indexOfFirst { it.id.hex == target }
        return if (index < 0) null else index
    }

    companion object {
        const val maxSyncWalk = 500

        const val maxImportFiles = 100000
        const val maxImportBytes = 512L * 1024 * 1024
        const val maxImportFileBytes = GitResourceLimits.defaultMaxObjectBytes
        const val maxZipBytes = 256L * 1024 * 1024
        const val maxTreeDepth = 64
    }
}
