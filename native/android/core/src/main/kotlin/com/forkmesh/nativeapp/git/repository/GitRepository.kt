package com.forkmesh.nativeapp.git.repository

import com.forkmesh.nativeapp.git.bytes.DartFormatException
import com.forkmesh.nativeapp.git.bytes.dartTrim
import com.forkmesh.nativeapp.git.bytes.utf8Decode
import com.forkmesh.nativeapp.git.bytes.utf8Encode
import com.forkmesh.nativeapp.git.crypto.GitSha1CollisionDetector
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.index.GitIndexEntry
import com.forkmesh.nativeapp.git.index.GitIndexMode
import com.forkmesh.nativeapp.git.index.GitIndexStore
import com.forkmesh.nativeapp.git.index.GitIndexV2
import com.forkmesh.nativeapp.git.objects.GitAnnotatedTag
import com.forkmesh.nativeapp.git.objects.GitCommit
import com.forkmesh.nativeapp.git.objects.GitObjectFrame
import com.forkmesh.nativeapp.git.objects.GitObjectId
import com.forkmesh.nativeapp.git.objects.GitObjectType
import com.forkmesh.nativeapp.git.objects.GitTree
import com.forkmesh.nativeapp.git.objects.GitTreeEntry
import com.forkmesh.nativeapp.git.objects.GitTreeMode
import com.forkmesh.nativeapp.git.objects.LooseObjectStore
import com.forkmesh.nativeapp.git.pack.GitPackObjectStore
import com.forkmesh.nativeapp.git.pack.GitPackWriteObject
import com.forkmesh.nativeapp.git.pack.GitPackWriter
import com.forkmesh.nativeapp.git.pack.GitWrittenPack
import com.forkmesh.nativeapp.git.refs.GitFetchRefspec
import com.forkmesh.nativeapp.git.refs.GitPushRefspec
import com.forkmesh.nativeapp.git.refs.GitRefName
import com.forkmesh.nativeapp.git.refs.GitRefUpdateResult
import com.forkmesh.nativeapp.git.refs.GitSymbolicRef
import com.forkmesh.nativeapp.git.refs.RefStore
import com.forkmesh.nativeapp.git.security.GitCancellationToken
import com.forkmesh.nativeapp.git.security.GitObjectQuarantine
import com.forkmesh.nativeapp.git.security.GitOperationJournal
import com.forkmesh.nativeapp.git.security.GitResourceLimits
import com.forkmesh.nativeapp.git.transport.GitRemoteUrl
import com.forkmesh.nativeapp.git.worktree.FileSystemEntityType
import com.forkmesh.nativeapp.git.worktree.GitWorktree
import com.forkmesh.nativeapp.git.worktree.GitWorktreePath
import com.forkmesh.nativeapp.git.worktree.createDirectories
import com.forkmesh.nativeapp.git.worktree.createFileExclusive
import com.forkmesh.nativeapp.git.worktree.deleteRecursive
import com.forkmesh.nativeapp.git.worktree.directoryExists
import com.forkmesh.nativeapp.git.worktree.entityType
import com.forkmesh.nativeapp.git.worktree.fileExists
import com.forkmesh.nativeapp.git.worktree.listEntries
import com.forkmesh.nativeapp.git.worktree.readString
import com.forkmesh.nativeapp.git.worktree.renameFile
import com.forkmesh.nativeapp.git.worktree.walkEntries
import com.forkmesh.nativeapp.git.worktree.writeStringFlushed
import java.io.File
import java.io.IOException
import java.nio.file.Files
import java.nio.file.attribute.BasicFileAttributes
import java.nio.file.attribute.PosixFilePermission
import kotlin.time.Duration
import kotlin.time.Duration.Companion.minutes

class GitRepositoryInit(
    val worktree: File,
    val limits: GitResourceLimits? = null,
    val detector: GitSha1CollisionDetector? = null,
    val initialBranch: String = "main",
)

class GitRepositoryOpen(
    val worktree: File,
    val limits: GitResourceLimits? = null,
    val detector: GitSha1CollisionDetector? = null,
)

@Suppress("EnumEntryName")
enum class GitChangeKind { added, modified, deleted, untracked }

@Suppress("EnumEntryName")
enum class GitFastForwardOutcome { upToDate, fastForwarded, diverged, undetermined }

class GitStatusEntry(val path: GitWorktreePath, val kind: GitChangeKind) {
    override fun toString(): String = "${kind.name} ${path.value}"
}

class GitStatus(val staged: List<GitStatusEntry>, val worktree: List<GitStatusEntry>) {
    val isClean: Boolean get() = staged.isEmpty() && worktree.isEmpty()
}

class GitIdentity(
    val name: String,
    val email: String,
    val timestampSeconds: Long,
    val timezoneOffsetMinutes: Int,
) {
    init {
        if (name.isEmpty() ||
            email.isEmpty() ||
            !isSafeIdentityField(name) ||
            !isSafeIdentityField(email) ||
            timezoneOffsetMinutes < -14 * 60 ||
            timezoneOffsetMinutes > 14 * 60
        ) {
            throw GitException(GitErrorCode.invalidObject, "commit identity is invalid")
        }
    }

    val gitText: String
        get() {
            val absoluteMinutes = Math.abs(timezoneOffsetMinutes)
            val sign = if (timezoneOffsetMinutes < 0) "-" else "+"
            val hours = absoluteMinutes / 60
            val minutes = absoluteMinutes % 60
            return "$name <$email> $timestampSeconds $sign${hours.toString().padStart(2, '0')}" +
                minutes.toString().padStart(2, '0')
        }
}

class GitCommitRequest(val message: String, val author: GitIdentity, val committer: GitIdentity) {
    init {
        if (message.indexOf(0.toChar()) >= 0) {
            throw GitException(GitErrorCode.invalidObject, "commit message contains NUL")
        }
    }
}

class GitBranchName(val value: String) {
    init {
        if (value.isEmpty() || value.startsWith("/") || value.endsWith("/")) {
            throw GitException(GitErrorCode.invalidRef, "branch name is invalid")
        }
        GitRefName.branch("refs/heads/$value")
    }

    val ref: GitRefName get() = GitRefName.branch("refs/heads/$value")

    override fun equals(other: Any?): Boolean = other is GitBranchName && other.value == value

    override fun hashCode(): Int = value.hashCode()

    override fun toString(): String = value
}

class GitCheckoutRequest private constructor(val branch: GitBranchName) {
    companion object {
        fun branch(branch: GitBranchName): GitCheckoutRequest = GitCheckoutRequest(branch)
    }
}

class GitLogRequest(
    val ref: String? = null,
    val limit: Int = 200,
) {
    init {
        if (limit <= 0 || limit > maxLimit) {
            throw IllegalArgumentException("Invalid argument (limit): must be in 1..$maxLimit: $limit")
        }
        if (ref != null) GitRefName.parse(ref)
    }

    companion object {
        const val maxLimit = 10000
    }
}

class GitLogEntry(val id: GitObjectId, val commit: GitCommit)

class GitRefEntry(
    val name: GitRefName,
    val objectId: GitObjectId,
) {
    val shortName: String
        get() {
            val value = name.value
            val second = value.indexOf('/', "refs/".length)
            return value.substring(second + 1)
        }
}

class GitRepository private constructor(
    val worktree: File,
    val gitDirectory: File,
    val limits: GitResourceLimits,
    val detector: GitSha1CollisionDetector?,
) {
    private val objects = LooseObjectStore(gitDirectory, limits, detector = detector)
    private val index = GitIndexStore(gitDirectory, limits)
    internal val packedObjects = GitPackObjectStore(gitDirectory, limits, detector = detector)

    companion object {
        fun init(request: GitRepositoryInit): GitRepository {
            createDirectories(request.worktree)
            val gitDirectory = File(request.worktree, ".git")
            if (directoryExists(gitDirectory)) {
                throw GitException(GitErrorCode.invalidRepository, "repository already exists")
            }
            createDirectories(File(gitDirectory, "objects"))
            createDirectories(File(File(gitDirectory, "refs"), "heads"))
            createDirectories(File(File(gitDirectory, "refs"), "tags"))
            createDirectories(File(File(gitDirectory, "refs"), "remotes"))
            writeStringFlushed(File(gitDirectory, "HEAD"), "ref: refs/heads/${request.initialBranch.ifEmpty { "main" }}\n")
            writeStringFlushed(
                File(gitDirectory, "config"),
                "[core]\n\trepositoryformatversion = 0\n\tbare = false\n",
            )
            return open(GitRepositoryOpen(request.worktree, limits = request.limits, detector = request.detector))
        }

        fun open(request: GitRepositoryOpen): GitRepository {
            val gitDirectory = File(request.worktree, ".git")
            if (!directoryExists(gitDirectory)) {
                throw GitException(GitErrorCode.invalidRepository, "missing .git directory")
            }
            val config = File(gitDirectory, "config")
            if (!fileExists(config)) {
                throw GitException(GitErrorCode.invalidRepository, "missing config")
            }
            RepositoryConfig.load(config)
            GitOperationJournal.recover(gitDirectory)
            GitObjectQuarantine.recover(gitDirectory)
            recoverStaleLocksIn(gitDirectory)
            val head = File(gitDirectory, "HEAD")
            if (!fileExists(head)) {
                throw GitException(GitErrorCode.invalidRepository, "HEAD is invalid")
            }
            val headValue = readString(head).dartTrim()
            try {
                if (headValue.startsWith("ref: ")) {
                    GitRefName.branch(headValue.substring(5))
                } else {
                    GitObjectId.parseSha1(headValue)
                }
            } catch (error: GitException) {
                throw GitException(GitErrorCode.invalidRepository, "HEAD is invalid")
            }
            return GitRepository(
                request.worktree,
                gitDirectory,
                request.limits ?: GitResourceLimits(),
                request.detector,
            )
        }

        fun clone(request: GitCloneRequest, progress: GitProgressListener? = null): GitOperation<GitCloneResult> {
            val worktree = request.worktree
            val existed = directoryExists(worktree)
            if (existed && listEntries(worktree).isNotEmpty()) {
                throw GitException(GitErrorCode.invalidRepository, "clone destination is not empty")
            }
            return GitOperation.start(cancellation = request.cancellation, progress = progress) { cancellation, sink ->
                try {
                    val repository = init(
                        GitRepositoryInit(worktree, limits = request.limits, detector = request.detector),
                    )
                    repository.setRemote(request.remoteName, request.url)
                    val fetched = RemoteOperations(repository).fetch(
                        url = request.url,
                        remoteName = request.remoteName,
                        refspec = GitFetchRefspec.standard(request.remoteName),
                        access = request.access,
                        cancellation = cancellation,
                        sink = sink,
                        writeFetchHead = false,
                        shape = FetchShape(
                            depth = request.depth,
                            singleBranch = request.singleBranch,
                            branch = request.branch,
                        ),
                    )
                    cancellation.throwIfCancelled()
                    val remoteHead = fetched.remoteHead
                    val branchName = request.branch
                        ?: if (remoteHead != null && remoteHead.startsWith("refs/heads/")) {
                            remoteHead.substring("refs/heads/".length)
                        } else {
                            null
                        }
                    var checkedOut: String? = null
                    if (branchName != null) {
                        val branch = GitBranchName(branchName)
                        val tracking = GitRefName.parse("refs/remotes/${request.remoteName}/$branchName")
                        val store = RefStore(repository.gitDirectory)
                        val target = store.readDirect(tracking)
                            ?: throw GitException(GitErrorCode.invalidRef, "remote has no branch $branchName")
                        val created = store.update(branch.ref, expectedOld = null, next = target)
                        if (created != GitRefUpdateResult.updated) {
                            throw GitException(GitErrorCode.refLocked, "clone branch already exists")
                        }
                        if (request.singleBranch) {
                            repository.setRemote(
                                request.remoteName,
                                request.url,
                                fetch = GitFetchRefspec.parse(
                                    "+refs/heads/$branchName:refs/remotes/${request.remoteName}/$branchName",
                                ),
                            )
                        }
                        repository.setBranchUpstream(
                            branch,
                            remote = request.remoteName,
                            merge = GitRefName.branch("refs/heads/$branchName"),
                        )
                        sink.add(GitProgress(GitProgressPhase.checkingOut))
                        cancellation.throwIfCancelled()
                        repository.initialCheckout(branch, target, cancellation) { written, total ->
                            sink.add(GitProgress(GitProgressPhase.checkingOut, objects = written, total = total))
                        }
                        checkedOut = branchName
                    }
                    GitCloneResult(repository = repository, checkedOutBranch = checkedOut, fetch = fetched)
                } catch (error: Throwable) {
                    discardClone(worktree, existed)
                    throw error
                }
            }
        }

        private fun discardClone(worktree: File, existed: Boolean) {
            try {
                if (!existed) {
                    if (directoryExists(worktree)) deleteRecursive(worktree)
                    return
                }
                for (entity in listEntries(worktree)) {
                    deleteRecursive(entity)
                }
            } catch (error: IOException) {
            }
        }

        private fun recoverStaleLocksIn(gitDirectory: File, olderThan: Duration = 15.minutes): Int {
            if (olderThan < 1.minutes) {
                throw IllegalArgumentException("Invalid argument (olderThan): must be at least one minute: $olderThan")
            }
            val cutoff = System.currentTimeMillis() - olderThan.inWholeMilliseconds
            var removed = 0
            fun removeIfStale(lock: File) {
                val attributes = try {
                    Files.readAttributes(lock.toPath(), BasicFileAttributes::class.java)
                } catch (error: IOException) {
                    return
                }
                if (!attributes.isRegularFile || attributes.lastModifiedTime().toMillis() >= cutoff) {
                    return
                }
                try {
                    Files.delete(lock.toPath())
                    removed += 1
                } catch (error: IOException) {
                }
            }
            for (name in listOf("HEAD.lock", "index.lock", "config.lock", "FETCH_HEAD.lock", "packed-refs.lock")) {
                removeIfStale(File(gitDirectory, name))
            }
            val refs = File(gitDirectory, "refs")
            if (directoryExists(refs)) {
                var visited = 0
                walkEntries(refs) { entity, type ->
                    visited += 1
                    if (visited > GitResourceLimits.hardMaxRefCount) {
                        throw GitException(GitErrorCode.resourceLimitExceeded, "refs directory exceeds the configured limit")
                    }
                    if (type == FileSystemEntityType.file && entity.path.endsWith(".lock")) {
                        removeIfStale(entity)
                    }
                }
            }
            return removed
        }
    }

    fun recoverStaleLocks(olderThan: Duration = 15.minutes): Int = recoverStaleLocksIn(gitDirectory, olderThan)

    fun listBranches(): List<GitRefEntry> = listRefs("refs/heads/")

    fun listTags(): List<GitRefEntry> = listRefs("refs/tags/")

    private fun listRefs(prefix: String): List<GitRefEntry> {
        val refs = RefStore(gitDirectory).listDirect(prefix, limits)
        return refs.keys.sorted().map { name -> GitRefEntry(GitRefName.parse(name), refs.getValue(name)) }
    }

    fun log(request: GitLogRequest, cancellation: GitCancellationToken? = null): List<GitLogEntry> {
        val ref = request.ref
        val start: GitObjectId? = if (ref == null) {
            readHeadObjectId()
        } else {
            RefStore(gitDirectory).readDirect(GitRefName.parse(ref))
                ?: throw GitException(GitErrorCode.invalidRef, "ref does not exist")
        }
        if (start == null) return emptyList()
        val shallow = shallowCommits()
        val seen = HashSet<GitObjectId>()
        val pending = ArrayList<Pair<GitLogEntry, Long>>()
        val output = ArrayList<GitLogEntry>()

        fun commitTime(commit: GitCommit): Long =
            commit.committer?.timestampSeconds ?: commit.author?.timestampSeconds ?: 0

        fun enqueue(id: GitObjectId) {
            cancellation?.throwIfCancelled()
            if (!seen.add(id)) return
            if (seen.size > limits.maxCommitWalk) {
                throw GitException(GitErrorCode.resourceLimitExceeded, "log walk exceeded the configured limit")
            }
            val commit = readCommitOrTagTarget(id)
                ?: throw GitException(GitErrorCode.invalidObject, "log start is not a commit")
            pending.add(Pair(GitLogEntry(id, commit), commitTime(commit)))
        }

        enqueue(start)
        while (pending.isNotEmpty() && output.size < request.limit) {
            cancellation?.throwIfCancelled()
            pending.sortWith { left, right -> right.second.compareTo(left.second) }
            val next = pending.removeAt(0).first
            output.add(next)
            for (parent in walkParents(next.id, next.commit, shallow)) {
                enqueue(parent)
            }
        }
        return output
    }

    fun resetPaths(paths: Iterable<GitWorktreePath>, cancellation: GitCancellationToken? = null) {
        val journal = GitOperationJournal.begin(gitDirectory, "reset")
        try {
            val headEntries = headEntries()
            val existing = index.read()
            val entries = LinkedHashMap<String, GitIndexEntry>()
            for (entry in existing.entries) entries[utf8Decode(entry.path)] = entry
            for (path in paths) {
                cancellation?.throwIfCancelled()
                val headEntry = headEntries[path.value]
                if (headEntry == null) {
                    entries.remove(path.value)
                } else {
                    entries[path.value] = headEntry
                }
            }
            index.write(GitIndexV2(entries.values))
        } finally {
            journal.complete()
        }
    }

    fun writeBlob(bytes: ByteArray): GitObjectId = objects.write(GitObjectType.blob, bytes)

    fun readBlob(id: GitObjectId): ByteArray {
        val frame = readObject(id)
        if (frame.type != GitObjectType.blob) {
            throw GitException(GitErrorCode.invalidObject, "expected blob object")
        }
        return frame.body
    }

    fun readHead(): GitSymbolicRef {
        val value = readString(File(gitDirectory, "HEAD")).dartTrim()
        if (!value.startsWith("ref: ")) {
            throw GitException(GitErrorCode.invalidRef, "HEAD is detached")
        }
        val target = value.substring(5)
        GitRefName.branch(target)
        return GitSymbolicRef(target)
    }

    fun readHeadObjectId(): GitObjectId? {
        val head = headTarget()
        val ref = head.ref ?: return head.direct
        return RefStore(gitDirectory).readDirect(ref)
    }

    fun readTree(id: GitObjectId): GitTree {
        val frame = readObject(id)
        if (frame.type != GitObjectType.tree) {
            throw GitException(GitErrorCode.invalidObject, "expected tree object")
        }
        return GitTree.parse(frame.body)
    }

    fun readCommit(id: GitObjectId): GitCommit {
        val frame = readObject(id)
        if (frame.type != GitObjectType.commit) {
            throw GitException(GitErrorCode.invalidObject, "expected commit object")
        }
        return GitCommit.parse(frame.body)
    }

    internal fun readObject(id: GitObjectId): GitObjectFrame = try {
        objects.read(id)
    } catch (error: GitException) {
        if (error.code != GitErrorCode.invalidObject) throw error
        packedObjects.read(id)
    }

    fun writePack(
        roots: Iterable<GitObjectId>,
        exclude: Set<GitObjectId> = emptySet(),
        cancellation: GitCancellationToken? = null,
    ): GitWrittenPack {
        val visited = HashSet<GitObjectId>(exclude)
        val output = ArrayList<GitPackWriteObject>()
        val shallow = shallowCommits()
        for (root in roots) {
            collectPackObject(root, shallow, visited, output, cancellation)
        }
        return GitPackWriter.write(output, cancellation = cancellation)
    }

    private fun collectPackObject(
        root: GitObjectId,
        shallow: Set<GitObjectId>,
        visited: MutableSet<GitObjectId>,
        output: MutableList<GitPackWriteObject>,
        cancellation: GitCancellationToken?,
    ) {
        val stack = ArrayDeque<Iterator<GitObjectId>>()
        visitPackObject(root, shallow, visited, output, cancellation)?.let { stack.addLast(it) }
        while (stack.isNotEmpty()) {
            val children = stack.last()
            if (!children.hasNext()) {
                stack.removeLast()
                continue
            }
            visitPackObject(children.next(), shallow, visited, output, cancellation)?.let { stack.addLast(it) }
        }
    }

    private fun visitPackObject(
        id: GitObjectId,
        shallow: Set<GitObjectId>,
        visited: MutableSet<GitObjectId>,
        output: MutableList<GitPackWriteObject>,
        cancellation: GitCancellationToken?,
    ): Iterator<GitObjectId>? {
        cancellation?.throwIfCancelled()
        if (!visited.add(id)) return null
        if (output.size >= limits.maxCommitWalk * 8) {
            throw GitException(GitErrorCode.resourceLimitExceeded, "pack object walk exceeded the configured limit")
        }
        val frame = readObject(id)
        output.add(GitPackWriteObject(frame.type, frame.body))
        return when (frame.type) {
            GitObjectType.blob -> null
            GitObjectType.tree -> GitTree.parse(frame.body).entries.map { it.objectId }.iterator()
            GitObjectType.commit -> {
                val commit = GitCommit.parse(frame.body)
                (listOf(commit.tree) + walkParents(id, commit, shallow)).iterator()
            }
            GitObjectType.tag -> listOf(GitAnnotatedTag.parse(frame.body).target).iterator()
        }
    }

    internal fun hasObject(id: GitObjectId): Boolean {
        if (fileExists(objects.fileFor(id))) return true
        return packedObjects.contains(id)
    }

    internal fun readCommitOrTagTarget(id: GitObjectId): GitCommit? {
        var current = id
        for (hops in 0 until 8) {
            val frame = readObject(current)
            when (frame.type) {
                GitObjectType.commit -> return GitCommit.parse(frame.body)
                GitObjectType.tag -> current = GitAnnotatedTag.parse(frame.body).target
                GitObjectType.tree, GitObjectType.blob -> return null
            }
        }
        throw GitException(GitErrorCode.invalidObject, "tag chain is too deep")
    }

    internal fun isAncestor(
        ancestor: GitObjectId,
        descendant: GitObjectId,
        cancellation: GitCancellationToken?,
    ): Boolean {
        if (ancestor == descendant) return true
        val shallow = shallowCommits()
        val visited = HashSet<GitObjectId>()
        val queue = ArrayList<GitObjectId>()
        queue.add(descendant)
        while (queue.isNotEmpty()) {
            cancellation?.throwIfCancelled()
            val id = queue.removeAt(queue.size - 1)
            if (!visited.add(id)) continue
            if (visited.size > limits.maxCommitWalk) {
                throw GitException(GitErrorCode.resourceLimitExceeded, "ancestry walk exceeded the configured limit")
            }
            val commit = readCommitOrTagTarget(id) ?: continue
            for (parent in walkParents(id, commit, shallow)) {
                if (parent == ancestor) return true
                queue.add(parent)
            }
        }
        return false
    }

    internal fun reachableCommits(tips: Iterable<GitObjectId>, cancellation: GitCancellationToken?): Set<GitObjectId> {
        val shallow = shallowCommits()
        val visited = LinkedHashSet<GitObjectId>()
        val queue = ArrayList<GitObjectId>()
        queue.addAll(tips)
        while (queue.isNotEmpty()) {
            cancellation?.throwIfCancelled()
            val id = queue.removeAt(queue.size - 1)
            if (!visited.add(id)) continue
            if (visited.size > limits.maxCommitWalk) {
                throw GitException(
                    GitErrorCode.resourceLimitExceeded,
                    "reachability walk exceeded the configured limit",
                )
            }
            val commit = readCommitOrTagTarget(id) ?: continue
            queue.addAll(walkParents(id, commit, shallow))
        }
        return visited
    }

    internal fun collectTreeClosure(
        tree: GitObjectId,
        output: MutableSet<GitObjectId>,
        cancellation: GitCancellationToken?,
    ) {
        val stack = ArrayList<GitObjectId>()
        stack.add(tree)
        while (stack.isNotEmpty()) {
            cancellation?.throwIfCancelled()
            val id = stack.removeAt(stack.size - 1)
            if (!output.add(id)) continue
            if (output.size > limits.maxCommitWalk * 8) {
                throw GitException(GitErrorCode.resourceLimitExceeded, "tree walk exceeded the configured limit")
            }
            val frame = readObject(id)
            if (frame.type != GitObjectType.tree) continue
            for (entry in GitTree.parse(frame.body).entries) {
                stack.add(entry.objectId)
            }
        }
    }

    private val configFile: File get() = File(gitDirectory, "config")

    fun readConfig(): RepositoryConfig = RepositoryConfig.load(configFile)

    fun setRemote(name: String, url: GitRemoteUrl, fetch: GitFetchRefspec? = null) =
        RepositoryConfig.writeRemote(configFile, name, url = url, fetch = fetch ?: GitFetchRefspec.standard(name))

    fun setBranchUpstream(branch: GitBranchName, remote: String, merge: GitRefName) =
        RepositoryConfig.writeBranchUpstream(configFile, branch.value, remote = remote, merge = merge)

    fun fetch(request: GitFetchRequest, progress: GitProgressListener? = null): GitOperation<GitFetchResult> {
        val config = readConfig()
        val remote = config.remotes[request.remote]
        val url = request.url ?: remote?.url
            ?: throw GitException(GitErrorCode.invalidRepository, "remote ${request.remote} is not configured")
        val refspec = request.refspec ?: remote?.fetch ?: GitFetchRefspec.standard(request.remote)
        return GitOperation.start(cancellation = request.cancellation, progress = progress) { cancellation, sink ->
            RemoteOperations(this).fetch(
                url = url,
                remoteName = request.remote,
                refspec = refspec,
                access = request.access,
                cancellation = cancellation,
                sink = sink,
                shape = FetchShape(deepen = request.deepen),
            )
        }
    }

    fun push(request: GitPushRequest, progress: GitProgressListener? = null): GitOperation<GitPushResult> {
        val config = readConfig()
        val remote = config.remotes[request.remote]
            ?: throw GitException(GitErrorCode.invalidRepository, "remote ${request.remote} is not configured")
        var refspecs = request.refspecs
        if (refspecs.isEmpty()) {
            val head = readHead()
            val branch = head.target.substring("refs/heads/".length)
            val upstream = config.branches[branch]
            val remoteBranch = if (upstream != null && upstream.remote == request.remote) {
                upstream.merge.value.substring("refs/heads/".length)
            } else {
                branch
            }
            refspecs = listOf(GitPushRefspec(localBranch = branch, remoteBranch = remoteBranch))
        }
        return GitOperation.start(cancellation = request.cancellation, progress = progress) { cancellation, sink ->
            RemoteOperations(this).push(
                url = remote.url,
                remoteName = request.remote,
                refspecs = refspecs,
                access = request.access,
                cancellation = cancellation,
                sink = sink,
            )
        }
    }

    fun add(paths: Iterable<GitWorktreePath>, cancellation: GitCancellationToken? = null) {
        val journal = GitOperationJournal.begin(gitDirectory, "add")
        try {
            val existing = index.read()
            val entries = LinkedHashMap<String, GitIndexEntry>()
            for (entry in existing.entries) entries[utf8Decode(entry.path)] = entry
            for (path in paths) {
                cancellation?.throwIfCancelled()
                if (path.components.first() == ".git") {
                    throw GitException(GitErrorCode.pathEscapesWorktree, "cannot stage repository metadata")
                }
                stagePath(path, entries, cancellation)
            }
            cancellation?.throwIfCancelled()
            index.write(GitIndexV2(entries.values))
        } finally {
            journal.complete()
        }
    }

    fun remove(paths: Iterable<GitWorktreePath>) {
        val journal = GitOperationJournal.begin(gitDirectory, "remove")
        try {
            val existing = index.read()
            val entries = LinkedHashMap<String, GitIndexEntry>()
            for (entry in existing.entries) entries[utf8Decode(entry.path)] = entry
            for (path in paths) {
                entries.remove(path.value)
            }
            index.write(GitIndexV2(entries.values))
        } finally {
            journal.complete()
        }
    }

    fun status(cancellation: GitCancellationToken? = null): GitStatus {
        val indexed = index.read()
        val indexEntries = LinkedHashMap<String, GitIndexEntry>()
        for (entry in indexed.entries) indexEntries[utf8Decode(entry.path)] = entry
        val headEntries = headEntries()
        val staged = ArrayList<GitStatusEntry>()
        val allPaths = LinkedHashSet<String>().apply {
            addAll(indexEntries.keys)
            addAll(headEntries.keys)
        }.sorted()
        for (path in allPaths) {
            cancellation?.throwIfCancelled()
            val indexEntry = indexEntries[path]
            val headEntry = headEntries[path]
            if (indexEntry == null) {
                staged.add(GitStatusEntry(GitWorktreePath(path), GitChangeKind.deleted))
            } else if (headEntry == null) {
                staged.add(GitStatusEntry(GitWorktreePath(path), GitChangeKind.added))
            } else if (indexEntry.objectId != headEntry.objectId || indexEntry.mode != headEntry.mode) {
                staged.add(GitStatusEntry(GitWorktreePath(path), GitChangeKind.modified))
            }
        }
        val worktreeChanges = ArrayList<GitStatusEntry>()
        for (entry in indexEntries.values) {
            cancellation?.throwIfCancelled()
            val change = worktreeChange(entry)
            if (change != null) {
                worktreeChanges.add(GitStatusEntry(GitWorktreePath(utf8Decode(entry.path)), change))
            }
        }
        collectUntracked(worktree, emptyList(), indexEntries, worktreeChanges, cancellation)
        worktreeChanges.sortWith { left, right -> left.path.value.compareTo(right.path.value) }
        return GitStatus(staged = staged, worktree = worktreeChanges)
    }

    fun commit(request: GitCommitRequest, cancellation: GitCancellationToken? = null): GitObjectId {
        val journal = GitOperationJournal.begin(gitDirectory, "commit")
        try {
            val indexed = index.read()
            val tree = writeTree(indexed.entries, cancellation)
            cancellation?.throwIfCancelled()
            val parent = readHeadObjectId()
            val body = encodeCommit(request, tree, parent)
            val commit = objects.write(GitObjectType.commit, body)
            val head = headTarget()
            val headRef = head.ref ?: throw GitException(
                GitErrorCode.unsupportedRepositoryFormat,
                "committing from detached HEAD is unsupported",
            )
            val result = RefStore(gitDirectory).update(headRef, expectedOld = parent, next = commit)
            if (result != GitRefUpdateResult.updated) {
                throw GitException(GitErrorCode.nonFastForward, "HEAD changed while creating commit")
            }
            return commit
        } finally {
            journal.complete()
        }
    }

    fun createBranch(branch: GitBranchName) {
        val head = readHeadObjectId()
            ?: throw GitException(GitErrorCode.invalidRef, "cannot create a branch from an unborn HEAD")
        val result = RefStore(gitDirectory).update(branch.ref, expectedOld = null, next = head)
        if (result != GitRefUpdateResult.updated) {
            throw GitException(GitErrorCode.refLocked, "branch already exists")
        }
    }

    internal fun initialCheckout(
        branch: GitBranchName,
        target: GitObjectId,
        cancellation: GitCancellationToken? = null,
        progress: ((written: Int, total: Int) -> Unit)? = null,
    ) {
        val journal = GitOperationJournal.begin(gitDirectory, "checkout")
        try {
            val targetEntries = entriesForCommit(target)
            materializeCheckout(emptyMap(), targetEntries, cancellation, progress)
            index.write(GitIndexV2(targetEntries.values))
            writeHead("ref: ${branch.ref.value}\n")
        } finally {
            journal.complete()
        }
    }

    fun checkout(request: GitCheckoutRequest, cancellation: GitCancellationToken? = null) {
        val journal = GitOperationJournal.begin(gitDirectory, "checkout")
        try {
            val target = RefStore(gitDirectory).readDirect(request.branch.ref)
                ?: throw GitException(GitErrorCode.invalidRef, "branch does not exist")
            if (readHeadObjectId() == target) {
                writeHead("ref: ${request.branch.ref.value}\n")
                return
            }
            val before = status(cancellation = cancellation)
            if (!before.isClean) {
                throw GitException(GitErrorCode.operationInterrupted, "cannot checkout with local changes")
            }
            val currentEntries = headEntries()
            val targetEntries = entriesForCommit(target)
            materializeCheckout(currentEntries, targetEntries, cancellation)
            cancellation?.throwIfCancelled()
            index.write(GitIndexV2(targetEntries.values))
            writeHead("ref: ${request.branch.ref.value}\n")
        } finally {
            journal.complete()
        }
    }

    fun fastForward(
        branch: GitBranchName,
        target: GitObjectId,
        cancellation: GitCancellationToken? = null,
        progress: ((written: Int, total: Int) -> Unit)? = null,
    ): GitFastForwardOutcome {
        val journal = GitOperationJournal.begin(gitDirectory, "checkout")
        try {
            val store = RefStore(gitDirectory)
            val current = store.readDirect(branch.ref)
                ?: throw GitException(GitErrorCode.invalidRef, "branch does not exist")
            if (current == target) return GitFastForwardOutcome.upToDate
            if (!hasObject(target)) throw GitException(GitErrorCode.invalidRef, "fast-forward target is not present")
            when (ancestry(current, target, cancellation)) {
                GitAncestry.ancestor -> {}
                GitAncestry.notAncestor -> return GitFastForwardOutcome.diverged
                GitAncestry.unknown -> return GitFastForwardOutcome.undetermined
            }
            val checkedOut = runCatching { readHead().target == branch.ref.value }.getOrDefault(false)
            if (checkedOut) {
                if (!status(cancellation = cancellation).isClean) {
                    throw GitException(GitErrorCode.operationInterrupted, "cannot fast-forward with local changes")
                }
                val targetEntries = entriesForCommit(target)
                materializeCheckout(headEntries(), targetEntries, cancellation, progress)
                cancellation?.throwIfCancelled()
                index.write(GitIndexV2(targetEntries.values))
            }
            if (store.update(branch.ref, expectedOld = current, next = target) != GitRefUpdateResult.updated) {
                throw GitException(GitErrorCode.refLocked, "branch changed during fast-forward")
            }
            return GitFastForwardOutcome.fastForwarded
        } finally {
            journal.complete()
        }
    }

    private fun stagePath(
        path: GitWorktreePath,
        entries: MutableMap<String, GitIndexEntry>,
        cancellation: GitCancellationToken?,
    ) {
        cancellation?.throwIfCancelled()
        val tree = GitWorktree(worktree)
        when (tree.typeOf(path)) {
            FileSystemEntityType.file -> {
                val bytes = tree.readFile(path)
                val file = File(worktree, path.components.joinToString(File.separator))
                val config = RepositoryConfig.load(File(gitDirectory, "config"))
                val mode = if (config.fileMode && isExecutable(file)) GitIndexMode.executable else GitIndexMode.regular
                entries[path.value] = GitIndexEntry(
                    path = path.bytes,
                    objectId = writeBlob(bytes),
                    mode = mode,
                    size = bytes.size.toLong(),
                )
            }
            FileSystemEntityType.link -> {
                val target = tree.readSymlink(path)
                val bytes = utf8Encode(target)
                entries[path.value] = GitIndexEntry(
                    path = path.bytes,
                    objectId = writeBlob(bytes),
                    mode = GitIndexMode.symlink,
                    size = bytes.size.toLong(),
                )
            }
            FileSystemEntityType.directory -> {
                val directory = File(worktree, path.components.joinToString(File.separator))
                for (entity in listEntries(directory)) {
                    stagePath(GitWorktreePath("${path.value}/${entity.name}"), entries, cancellation)
                }
            }
            FileSystemEntityType.notFound -> throw GitException(
                GitErrorCode.invalidRepository,
                "worktree path is missing",
            )
            else -> throw GitException(GitErrorCode.unsupportedRepositoryFormat, "worktree path has unsupported type")
        }
    }

    private fun worktreeChange(entry: GitIndexEntry): GitChangeKind? {
        val path = GitWorktreePath(utf8Decode(entry.path))
        val tree = GitWorktree(worktree)
        val type = tree.typeOf(path)
        if (type == FileSystemEntityType.notFound) return GitChangeKind.deleted
        val bytes = when (type) {
            FileSystemEntityType.file -> tree.readFile(path)
            FileSystemEntityType.link -> utf8Encode(tree.readSymlink(path))
            else -> return GitChangeKind.modified
        }
        val mode = if (type == FileSystemEntityType.link) {
            GitIndexMode.symlink
        } else if (entry.mode == GitIndexMode.executable) {
            GitIndexMode.executable
        } else {
            GitIndexMode.regular
        }
        val id = GitObjectId.sha1Of(GitObjectFrame.encode(GitObjectType.blob, bytes))
        if (id != entry.objectId || mode != entry.mode) {
            return GitChangeKind.modified
        }
        return null
    }

    private fun collectUntracked(
        directory: File,
        prefix: List<String>,
        tracked: Map<String, GitIndexEntry>,
        output: MutableList<GitStatusEntry>,
        cancellation: GitCancellationToken?,
    ) {
        for (entity in listEntries(directory)) {
            cancellation?.throwIfCancelled()
            val name = entity.name
            if (prefix.isEmpty() && name == ".git") continue
            val components = prefix + name
            val path = GitWorktreePath(components.joinToString("/"))
            val type = entityType(entity)
            if (type == FileSystemEntityType.directory) {
                collectUntracked(entity, components, tracked, output, cancellation)
            } else if (!tracked.containsKey(path.value)) {
                output.add(GitStatusEntry(path, GitChangeKind.untracked))
            }
        }
    }

    private fun headEntries(): Map<String, GitIndexEntry> {
        val head = readHeadObjectId() ?: return LinkedHashMap()
        return entriesForCommit(head)
    }

    private fun entriesForCommit(commitId: GitObjectId): Map<String, GitIndexEntry> {
        val commit = readCommit(commitId)
        val entries = LinkedHashMap<String, GitIndexEntry>()
        collectTreeEntries(commit.tree, emptyList(), entries)
        return entries
    }

    private fun materializeCheckout(
        current: Map<String, GitIndexEntry>,
        target: Map<String, GitIndexEntry>,
        cancellation: GitCancellationToken? = null,
        progress: ((written: Int, total: Int) -> Unit)? = null,
    ) {
        val tree = GitWorktree(worktree)
        val removedPaths = current.keys.filter { !target.containsKey(it) }.sortedDescending()
        for (path in removedPaths) {
            cancellation?.throwIfCancelled()
            val removed = GitWorktreePath(path)
            tree.deleteEntry(removed)
            tree.removeEmptyParents(removed)
        }
        val changedPaths = target.keys.filter { path ->
            val before = current[path] ?: return@filter true
            val after = target.getValue(path)
            before.objectId != after.objectId || before.mode != after.mode
        }.sorted()
        progress?.invoke(0, changedPaths.size)
        for ((index, path) in changedPaths.withIndex()) {
            cancellation?.throwIfCancelled()
            val entry = target.getValue(path)
            val destination = GitWorktreePath(path)
            val bytes = readBlob(entry.objectId)
            val before = current[path]
            if (before != null && (before.mode == GitIndexMode.symlink) != (entry.mode == GitIndexMode.symlink)) {
                tree.deleteEntry(destination)
            }
            when (entry.mode) {
                GitIndexMode.regular, GitIndexMode.executable -> tree.writeFile(destination, bytes, flush = false)
                GitIndexMode.symlink -> {
                    val targetText = try {
                        utf8Decode(bytes)
                    } catch (error: DartFormatException) {
                        throw GitException(GitErrorCode.symlinkEscape, "symlink payload is not valid UTF-8")
                    }
                    tree.writeSymlink(destination, targetText)
                }
            }
            val written = index + 1
            if (progress != null && (written % 64 == 0 || written == changedPaths.size)) {
                progress(written, changedPaths.size)
            }
        }
    }

    private fun writeHead(value: String) {
        val destination = File(gitDirectory, "HEAD")
        val lock = File("${destination.path}.lock")
        if (!createFileExclusive(lock)) {
            throw GitException(GitErrorCode.refLocked, "HEAD lock already exists")
        }
        try {
            writeStringFlushed(lock, value)
            renameFile(lock, destination)
        } finally {
            if (fileExists(lock)) lock.delete()
        }
    }

    private fun collectTreeEntries(treeId: GitObjectId, prefix: List<String>, output: MutableMap<String, GitIndexEntry>) {
        val tree = readTree(treeId)
        for (entry in tree.entries) {
            val name = utf8Decode(entry.name)
            if (name.equals(".git", ignoreCase = true)) {
                throw GitException(GitErrorCode.pathEscapesWorktree, "tree entry names repository metadata")
            }
            val components = prefix + name
            if (entry.mode == GitTreeMode.directory) {
                collectTreeEntries(entry.objectId, components, output)
            } else {
                val path = GitWorktreePath(components.joinToString("/"))
                output[path.value] = GitIndexEntry(
                    path = path.bytes,
                    objectId = entry.objectId,
                    mode = when (entry.mode) {
                        GitTreeMode.regular -> GitIndexMode.regular
                        GitTreeMode.executable -> GitIndexMode.executable
                        GitTreeMode.symlink -> GitIndexMode.symlink
                        GitTreeMode.directory -> throw IllegalStateException("handled above")
                    },
                )
            }
        }
    }

    private fun writeTree(entries: List<GitIndexEntry>, cancellation: GitCancellationToken?): GitObjectId {
        val root = TreeNode()
        for (entry in entries) {
            cancellation?.throwIfCancelled()
            val path = GitWorktreePath(utf8Decode(entry.path))
            var node = root
            for (component in path.components.subList(0, path.components.size - 1)) {
                if (node.files.containsKey(component)) {
                    throw GitException(GitErrorCode.invalidTree, "file and directory paths collide")
                }
                node = node.directories.getOrPut(component) { TreeNode() }
            }
            val name = path.components.last()
            if (node.directories.containsKey(name) || node.files.containsKey(name)) {
                throw GitException(GitErrorCode.invalidTree, "duplicate tree path")
            }
            node.files[name] = entry
        }
        return writeTreeNode(root, cancellation)
    }

    private fun writeTreeNode(node: TreeNode, cancellation: GitCancellationToken?): GitObjectId {
        cancellation?.throwIfCancelled()
        val entries = ArrayList<GitTreeEntry>()
        for ((fileName, entry) in node.files) {
            val name = utf8Encode(fileName)
            entries.add(
                when (entry.mode) {
                    GitIndexMode.regular -> GitTreeEntry.regular(name, entry.objectId)
                    GitIndexMode.executable -> GitTreeEntry.executable(name, entry.objectId)
                    GitIndexMode.symlink -> GitTreeEntry.symlink(name, entry.objectId)
                },
            )
        }
        for ((directoryName, child) in node.directories) {
            entries.add(GitTreeEntry.directory(utf8Encode(directoryName), writeTreeNode(child, cancellation)))
        }
        return objects.write(GitObjectType.tree, GitTree(entries).encode())
    }

    private fun headTarget(): HeadTarget {
        val value = readString(File(gitDirectory, "HEAD")).dartTrim()
        if (value.startsWith("ref: ")) {
            return HeadTarget(ref = GitRefName.branch(value.substring(5)), direct = null)
        }
        return HeadTarget(ref = null, direct = GitObjectId.parseSha1(value))
    }

    fun dispose() {}

    private class TreeNode {
        val directories = LinkedHashMap<String, TreeNode>()
        val files = LinkedHashMap<String, GitIndexEntry>()
    }

    private class HeadTarget(val ref: GitRefName?, val direct: GitObjectId?)
}

private fun encodeCommit(request: GitCommitRequest, tree: GitObjectId, parent: GitObjectId?): ByteArray {
    val headers = StringBuilder("tree ${tree.hex}\n")
    if (parent != null) headers.append("parent ${parent.hex}\n")
    headers.append("author ${request.author.gitText}\n")
    headers.append("committer ${request.committer.gitText}\n")
    headers.append("\n")
    return utf8Encode(headers.toString()) + utf8Encode(request.message)
}

private val unsafeIdentityCharacters = Regex("""[\x00-\x1f\x7f<>]""")

private fun isSafeIdentityField(value: String): Boolean = !unsafeIdentityCharacters.containsMatchIn(value)

private fun isExecutable(file: File): Boolean = try {
    val permissions = Files.getPosixFilePermissions(file.toPath())
    PosixFilePermission.OWNER_EXECUTE in permissions ||
        PosixFilePermission.GROUP_EXECUTE in permissions ||
        PosixFilePermission.OTHERS_EXECUTE in permissions
} catch (error: UnsupportedOperationException) {
    file.canExecute()
} catch (error: IOException) {
    file.canExecute()
}
