package com.forkmesh.nativeapp.git.repository

import com.forkmesh.nativeapp.git.bytes.GitBytesBuilder
import com.forkmesh.nativeapp.git.bytes.encodeLowerHex
import com.forkmesh.nativeapp.git.bytes.readUint32
import com.forkmesh.nativeapp.git.crypto.GitSha1CollisionDetector
import com.forkmesh.nativeapp.git.crypto.GitSha1ObjectOrigin
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.objects.GitAnnotatedTag
import com.forkmesh.nativeapp.git.objects.GitCommit
import com.forkmesh.nativeapp.git.objects.GitObjectId
import com.forkmesh.nativeapp.git.objects.GitObjectType
import com.forkmesh.nativeapp.git.objects.GitTree
import com.forkmesh.nativeapp.git.pack.GitPack
import com.forkmesh.nativeapp.git.pack.GitPackIndexWriteEntry
import com.forkmesh.nativeapp.git.pack.GitPackIndexWriter
import com.forkmesh.nativeapp.git.pack.GitPackObject
import com.forkmesh.nativeapp.git.pack.GitPackReader
import com.forkmesh.nativeapp.git.protocol.GitFetchResponse
import com.forkmesh.nativeapp.git.protocol.GitPktLine
import com.forkmesh.nativeapp.git.protocol.GitProtocolV2
import com.forkmesh.nativeapp.git.protocol.GitProtocolV2Advertisement
import com.forkmesh.nativeapp.git.protocol.GitPushRefStatus
import com.forkmesh.nativeapp.git.protocol.GitPushRefUpdate
import com.forkmesh.nativeapp.git.protocol.GitReceivePack
import com.forkmesh.nativeapp.git.protocol.GitRemoteRef
import com.forkmesh.nativeapp.git.protocol.GitSmartHttp
import com.forkmesh.nativeapp.git.protocol.GitSmartHttpService
import com.forkmesh.nativeapp.git.refs.GitFetchRefspec
import com.forkmesh.nativeapp.git.refs.GitPushRefspec
import com.forkmesh.nativeapp.git.refs.GitRefName
import com.forkmesh.nativeapp.git.refs.GitRefUpdateResult
import com.forkmesh.nativeapp.git.refs.GitRemoteName
import com.forkmesh.nativeapp.git.refs.RefStore
import com.forkmesh.nativeapp.git.security.GitCancellationToken
import com.forkmesh.nativeapp.git.security.GitObjectQuarantine
import com.forkmesh.nativeapp.git.security.GitOperationJournal
import com.forkmesh.nativeapp.git.security.GitResourceLimits
import com.forkmesh.nativeapp.git.transport.GitCertificateChallengeHandler
import com.forkmesh.nativeapp.git.transport.GitHttpHeaderProvider
import com.forkmesh.nativeapp.git.transport.GitHttpRequest
import com.forkmesh.nativeapp.git.transport.GitHttpTransport
import com.forkmesh.nativeapp.git.transport.GitIoHttpTransport
import com.forkmesh.nativeapp.git.transport.GitRemoteUrl
import com.forkmesh.nativeapp.git.worktree.createDirectories
import com.forkmesh.nativeapp.git.worktree.createFileExclusive
import com.forkmesh.nativeapp.git.worktree.fileExists
import com.forkmesh.nativeapp.git.worktree.renameFile
import com.forkmesh.nativeapp.git.worktree.writeStringFlushed
import java.io.File
import java.net.URI

class GitRemoteAccess(
    val transport: GitHttpTransport? = null,
    val headerProvider: GitHttpHeaderProvider? = null,
    val certificateChallenge: GitCertificateChallengeHandler? = null,
    trustedRootCertificatesPem: ByteArray? = null,
) {
    val trustedRootCertificatesPem: ByteArray? = trustedRootCertificatesPem?.copyOf()
}

class GitCloneRequest(
    val url: GitRemoteUrl,
    val worktree: File,
    val remoteName: String = "origin",
    val branch: String? = null,
    val depth: Int? = null,
    val singleBranch: Boolean = false,
    val access: GitRemoteAccess = GitRemoteAccess(),
    val limits: GitResourceLimits? = null,
    val detector: GitSha1CollisionDetector? = null,
    val cancellation: GitCancellationToken? = null,
) {
    init {
        GitRemoteName.validate(remoteName)
        if (branch != null) GitBranchName(branch)
        if (depth != null && (depth < 1 || depth > GitResourceLimits.hardMaxCommitWalk)) {
            throw GitException(
                GitErrorCode.unsupportedRepositoryFormat,
                "shallow clone depth must be in 1..${GitResourceLimits.hardMaxCommitWalk}",
            )
        }
    }
}

class GitCloneResult(
    val repository: GitRepository,
    val checkedOutBranch: String?,
    val fetch: GitFetchResult,
)

class GitFetchRequest(
    val remote: String = "origin",
    val url: GitRemoteUrl? = null,
    val refspec: GitFetchRefspec? = null,
    val deepen: GitDeepen? = null,
    val access: GitRemoteAccess = GitRemoteAccess(),
    val cancellation: GitCancellationToken? = null,
) {
    init {
        GitRemoteName.validate(remote)
        deepen?.validate()
    }
}

sealed class GitDeepen {
    data class Relative(val count: Int) : GitDeepen()

    data class Since(val epochSeconds: Long) : GitDeepen()

    object Unshallow : GitDeepen() {
        override fun toString(): String = "Unshallow"
    }

    internal fun validate() {
        when (this) {
            is Relative -> if (count < 1 || count > GitResourceLimits.hardMaxCommitWalk) {
                throw GitException(
                    GitErrorCode.unsupportedRepositoryFormat,
                    "deepen depth must be in 1..${GitResourceLimits.hardMaxCommitWalk}",
                )
            }
            is Since -> if (epochSeconds < 0) {
                throw GitException(GitErrorCode.unsupportedRepositoryFormat, "deepen-since must not be before 1970")
            }
            Unshallow -> Unit
        }
    }

    val depth: Int?
        get() = when (this) {
            is Relative -> count
            is Since -> null
            Unshallow -> infiniteDepth
        }

    val isRelative: Boolean get() = this is Relative

    val since: Long? get() = (this as? Since)?.epochSeconds

    companion object {
        const val infiniteDepth = 0x7fffffff
    }
}

@Suppress("EnumEntryName")
enum class GitAncestry { ancestor, notAncestor, unknown }

class GitRefUpdateRecord(val ref: String, val oldId: GitObjectId?, val newId: GitObjectId) {
    val isNew: Boolean get() = oldId == null
    val isChanged: Boolean get() = oldId != newId
}

class GitFetchResult(
    val updates: List<GitRefUpdateRecord>,
    val packBytes: Int,
    val objects: Int,
    val remoteHead: String?,
    val negotiationRounds: Int,
)

class GitPushRequest(
    val remote: String = "origin",
    refspecs: List<GitPushRefspec> = emptyList(),
    val access: GitRemoteAccess = GitRemoteAccess(),
    val cancellation: GitCancellationToken? = null,
) {
    val refspecs: List<GitPushRefspec> = refspecs.toList()

    init {
        GitRemoteName.validate(remote)
        if (refspecs.size > 1000) {
            throw GitException(GitErrorCode.resourceLimitExceeded, "too many push refspecs")
        }
    }
}

@Suppress("EnumEntryName")
enum class GitPushRefOutcome { updated, upToDate, rejected }

class GitPushRefResult(
    val localRef: String,
    val remoteRef: String,
    val oldId: GitObjectId?,
    val newId: GitObjectId,
    val outcome: GitPushRefOutcome,
    val code: GitErrorCode? = null,
    val message: String? = null,
)

class GitPushResult(val refs: List<GitPushRefResult>, val packBytes: Int, val objects: Int)

internal class RemoteSession private constructor(
    val url: GitRemoteUrl,
    val transport: GitHttpTransport,
    val ownsTransport: Boolean,
    val limits: GitResourceLimits,
    val cancellation: GitCancellationToken,
    val sink: GitProgressSink,
    val deadline: Long,
) {
    val host: String get() = url.uri.host

    fun close() {
        if (ownsTransport) transport.close()
    }

    fun checkpoint() {
        cancellation.throwIfCancelled()
        if (System.currentTimeMillis() > deadline) {
            throw GitException(GitErrorCode.networkUnavailable, "operation deadline exceeded")
        }
    }

    private fun serviceUri(suffix: String, query: String? = null): URI {
        val base = url.uri
        var path = base.rawPath ?: ""
        if (path.endsWith("/")) path = path.substring(0, path.length - 1)
        val text = StringBuilder()
            .append(base.scheme).append("://").append(base.rawAuthority)
            .append(path).append('/').append(suffix)
        val effectiveQuery = query ?: base.rawQuery
        if (effectiveQuery != null) text.append('?').append(effectiveQuery)
        return URI(text.toString())
    }

    fun advertisement(service: GitSmartHttpService): ByteArray {
        checkpoint()
        sink.add(GitProgress(GitProgressPhase.connecting, remoteHost = host))
        val headers = LinkedHashMap<String, String>()
        if (service == GitSmartHttpService.uploadPack) headers["git-protocol"] = "version=2"
        headers["accept"] = "*/*"
        val response = transport.send(
            GitHttpRequest(
                method = "GET",
                url = serviceUri("info/refs", query = "service=${service.name}"),
                headers = headers,
                maxResponseBytes = limits.maxAdvertisementBytes,
                expectedContentType = service.advertisementContentType,
                deadline = deadline,
            ),
            cancellation = cancellation,
            limits = limits,
        )
        return response.body
    }

    fun post(
        service: GitSmartHttpService,
        body: ByteArray,
        maxResponseBytes: Int,
        protocolV2: Boolean = false,
    ): ByteArray {
        checkpoint()
        val headers = LinkedHashMap<String, String>()
        headers["content-type"] = service.requestContentType
        headers["accept"] = service.resultContentType
        if (protocolV2) headers["git-protocol"] = "version=2"
        val response = transport.send(
            GitHttpRequest(
                method = "POST",
                url = serviceUri(service.name),
                headers = headers,
                body = body,
                maxResponseBytes = maxResponseBytes,
                expectedContentType = service.resultContentType,
                deadline = deadline,
            ),
            cancellation = cancellation,
            limits = limits,
        )
        return response.body
    }

    companion object {
        fun open(
            url: GitRemoteUrl,
            access: GitRemoteAccess,
            limits: GitResourceLimits,
            cancellation: GitCancellationToken,
            sink: GitProgressSink,
        ): RemoteSession {
            val provided = access.transport
            return RemoteSession(
                url = url,
                transport = provided ?: GitIoHttpTransport(
                    headerProvider = access.headerProvider,
                    certificateChallenge = access.certificateChallenge,
                    trustedRootCertificatesPem = access.trustedRootCertificatesPem,
                ),
                ownsTransport = provided == null,
                limits = limits,
                cancellation = cancellation,
                sink = sink,
                deadline = System.currentTimeMillis() + limits.operationDeadline.inWholeMilliseconds,
            )
        }
    }
}

private fun encodePackets(packets: Iterable<GitPktLine>): ByteArray {
    val output = GitBytesBuilder()
    for (packet in packets) {
        output.add(packet.encode())
    }
    return output.takeBytes()
}

private class FetchPlan(
    val remoteRefs: List<GitRemoteRef>,
    val wants: List<GitObjectId>,
    val refWrites: Map<String, GitObjectId>,
    val remoteHead: String?,
)

private class Negotiated(
    val packBytes: ByteArray,
    val rounds: Int,
    val shallow: List<GitObjectId>,
    val unshallow: List<GitObjectId>,
)

internal class FetchShape(
    val depth: Int? = null,
    val singleBranch: Boolean = false,
    val branch: String? = null,
    val deepen: GitDeepen? = null,
) {
    companion object {
        val full = FetchShape()
    }
}

internal class RemoteOperations(private val repository: GitRepository) {
    fun fetch(
        url: GitRemoteUrl,
        remoteName: String,
        refspec: GitFetchRefspec,
        access: GitRemoteAccess,
        cancellation: GitCancellationToken,
        sink: GitProgressSink,
        writeFetchHead: Boolean = true,
        shape: FetchShape = FetchShape.full,
    ): GitFetchResult {
        val session = RemoteSession.open(
            url = url,
            access = access,
            limits = repository.limits,
            cancellation = cancellation,
            sink = sink,
        )
        val journal = GitOperationJournal.begin(repository.gitDirectory, "fetch")
        try {
            val advertisement = GitSmartHttp.parseUploadPackAdvertisement(
                session.advertisement(GitSmartHttpService.uploadPack),
                maxPackets = repository.limits.maxPktLines,
            )
            requireV2Fetch(advertisement)
            val shallow = repository.shallowCommits()
            if (shape.depth != null || shape.deepen != null || shallow.isNotEmpty()) requireShallowFetch(advertisement)
            val capabilities = ArrayList<String>()
            capabilities.add("agent=${GitProtocolV2.agent}")
            if (advertisement.supports("object-format")) capabilities.add("object-format=sha1")
            val plan = planFetch(
                session,
                capabilities,
                refspec,
                shape,
                includeTags = shallow.isEmpty(),
                wantPresent = shape.deepen != null,
            )
            var packBytes = 0
            var objects = 0
            var rounds = 0
            if (plan.wants.isNotEmpty()) {
                val negotiated = negotiateAndReceive(
                    session,
                    plan.wants,
                    capabilities,
                    shallow,
                    deepen = shape.depth ?: shape.deepen?.depth,
                    deepenRelative = shape.deepen?.isRelative ?: false,
                    deepenSince = shape.deepen?.since,
                )
                rounds = negotiated.rounds
                packBytes = negotiated.packBytes.size
                val declared = if (packBytes >= 12) maxOf(0, packObjectCount(negotiated.packBytes)) else 0
                val received = packBytes.toLong()
                val sink = session.sink
                val host = session.host
                sink.add(GitProgress(GitProgressPhase.indexingPack, bytes = received, total = declared, remoteHost = host))
                val boundary = LinkedHashSet<GitObjectId>(shallow)
                boundary.addAll(negotiated.shallow)
                boundary.removeAll(negotiated.unshallow.toSet())
                val roots = ArrayList<GitObjectId>(plan.wants)
                for (id in negotiated.unshallow) {
                    if (!repository.hasObject(id)) continue
                    repository.readCommitOrTagTarget(id)?.let { roots.addAll(it.parents) }
                }
                objects = installUntrustedPack(negotiated.packBytes, roots, boundary, cancellation) { resolved, total ->
                    sink.add(
                        GitProgress(
                            GitProgressPhase.indexingPack,
                            bytes = received,
                            objects = resolved,
                            total = total,
                            remoteHost = host,
                        ),
                    )
                }
                if (boundary != shallow) GitShallowFile.write(repository.gitDirectory, boundary)
            }
            session.checkpoint()
            session.sink.add(
                GitProgress(GitProgressPhase.updatingRefs, refs = plan.refWrites.size, remoteHost = session.host),
            )
            val updates = writeFetchedRefs(plan.refWrites)
            if (writeFetchHead) {
                writeFetchHead(plan, url)
            }
            return GitFetchResult(
                updates = updates,
                packBytes = packBytes,
                objects = objects,
                remoteHead = plan.remoteHead,
                negotiationRounds = rounds,
            )
        } finally {
            journal.complete()
            session.close()
        }
    }

    private fun requireV2Fetch(advertisement: GitProtocolV2Advertisement) {
        if (!advertisement.supports("ls-refs") || !advertisement.supports("fetch")) {
            throw GitException(
                GitErrorCode.protocolUnsupported,
                "remote does not advertise protocol v2 ls-refs and fetch",
            )
        }
        val objectFormat = advertisement.valueOf("object-format")
        if (advertisement.supports("object-format") &&
            (objectFormat == null || !objectFormat.split(" ").contains("sha1"))
        ) {
            throw GitException(GitErrorCode.unsupportedRepositoryFormat, "remote object format is unsupported")
        }
    }

    private fun requireShallowFetch(advertisement: GitProtocolV2Advertisement) {
        val features = (advertisement.valueOf("fetch") ?: "").split(" ")
        if (!features.contains("shallow")) {
            throw GitException(GitErrorCode.protocolUnsupported, "remote does not support shallow fetch")
        }
    }

    private fun planFetch(
        session: RemoteSession,
        capabilities: List<String>,
        refspec: GitFetchRefspec,
        shape: FetchShape = FetchShape.full,
        includeTags: Boolean = true,
        wantPresent: Boolean = false,
    ): FetchPlan {
        session.sink.add(GitProgress(GitProgressPhase.listingRefs, remoteHost = session.host))
        val prefixes = LinkedHashSet<String>()
        prefixes.add("HEAD")
        prefixes.add(if (refspec.isWildcard) refspec.source.substring(0, refspec.source.length - 1) else refspec.source)
        prefixes.add("refs/tags/")
        val body = session.post(
            GitSmartHttpService.uploadPack,
            encodePackets(GitProtocolV2.lsRefsRequest(refPrefixes = prefixes, capabilities = capabilities)),
            maxResponseBytes = repository.limits.maxAdvertisementBytes,
            protocolV2 = true,
        )
        val remoteRefs = GitProtocolV2.parseLsRefs(
            GitPktLine.decodeAll(body, maximumPackets = repository.limits.maxPktLines),
            maxRefCount = repository.limits.maxRefCount,
        )
        var remoteHead: String? = null
        val refWrites = LinkedHashMap<String, GitObjectId>()
        val wants = LinkedHashSet<GitObjectId>()
        val onlyRef: String? = if (!shape.singleBranch) {
            null
        } else if (shape.branch != null) {
            "refs/heads/${shape.branch}"
        } else {
            remoteRefs.firstOrNull { it.name == "HEAD" }?.symrefTarget
        }
        for (remoteRef in remoteRefs) {
            if (remoteRef.name == "HEAD") {
                remoteHead = remoteRef.symrefTarget
                continue
            }
            if (shape.singleBranch) {
                val only = if (onlyRef != null && remoteRef.name == onlyRef) refspec.destinationFor(remoteRef.name) else null
                if (only != null) {
                    GitRefName.parse(only)
                    refWrites[only] = remoteRef.objectId
                    wants.add(remoteRef.objectId)
                }
                continue
            }
            val destination = refspec.destinationFor(remoteRef.name)
            if (destination != null) {
                GitRefName.parse(destination)
                refWrites[destination] = remoteRef.objectId
                wants.add(remoteRef.objectId)
                continue
            }
            if (includeTags && remoteRef.name.startsWith("refs/tags/")) {
                val local = GitRefName.parse(remoteRef.name)
                val existing = RefStore(repository.gitDirectory).readDirect(local)
                if (existing != null) continue
                refWrites[local.value] = remoteRef.objectId
                wants.add(remoteRef.objectId)
            }
        }
        val missing = ArrayList<GitObjectId>()
        for (want in wants) {
            session.cancellation.throwIfCancelled()
            if (wantPresent || !repository.hasObject(want)) missing.add(want)
        }
        return FetchPlan(remoteRefs = remoteRefs, wants = missing, refWrites = refWrites, remoteHead = remoteHead)
    }

    private fun negotiateAndReceive(
        session: RemoteSession,
        wants: List<GitObjectId>,
        capabilities: List<String>,
        shallow: Set<GitObjectId> = emptySet(),
        deepen: Int? = null,
        deepenRelative: Boolean = false,
        deepenSince: Long? = null,
    ): Negotiated {
        val shallowLines = shallow.sortedBy { it.hex }
        val limits = repository.limits
        val walk = HaveWalk(repository, limits.maxCommitWalk)
        walk.seed()
        val common = LinkedHashSet<GitObjectId>()
        var round = 0
        while (true) {
            session.checkpoint()
            round += 1
            val batch = walk.nextBatch(havesPerRound, common)
            val exhausted = batch.isEmpty() || walk.sent >= maxHaves || round >= limits.maxNegotiationRounds
            val haves = LinkedHashSet<GitObjectId>()
            haves.addAll(common)
            haves.addAll(batch)
            session.sink.add(GitProgress(GitProgressPhase.negotiating, objects = haves.size, remoteHost = session.host))
            val body = session.post(
                GitSmartHttpService.uploadPack,
                encodePackets(
                    GitProtocolV2.fetchRequest(
                        wants = wants,
                        haves = haves,
                        done = exhausted,
                        capabilities = capabilities,
                        shallow = shallowLines,
                        deepen = deepen,
                        deepenRelative = deepenRelative,
                        deepenSince = deepenSince,
                    ),
                ),
                maxResponseBytes = limits.maxCompressedPackBytes,
                protocolV2 = true,
            )
            session.sink.add(
                GitProgress(GitProgressPhase.receivingPack, bytes = body.size.toLong(), remoteHost = session.host),
            )
            val response = GitFetchResponse.parse(
                GitPktLine.decodeAll(body, maximumPackets = limits.maxPktLines),
                maxProgressBytes = limits.maxSidebandBytes,
            )
            common.addAll(response.acknowledged)
            val pack = response.packBytes
            if (pack != null) return Negotiated(pack, round, response.shallow, response.unshallow)
            if (exhausted || response.ready) {
                throw GitException(GitErrorCode.protocolUnsupported, "remote ended negotiation without a packfile")
            }
        }
    }

    private fun installUntrustedPack(
        packBytes: ByteArray,
        wants: List<GitObjectId>,
        shallow: Set<GitObjectId>,
        cancellation: GitCancellationToken,
        progress: ((resolved: Int, total: Int) -> Unit)? = null,
    ): Int {
        val limits = repository.limits
        if (packBytes.size > limits.maxCompressedPackBytes) {
            throw GitException(GitErrorCode.resourceLimitExceeded, "pack exceeds the compressed byte limit")
        }
        val quarantine = GitObjectQuarantine.begin(repository.gitDirectory, cancellation = cancellation, limits = limits)
        try {
            val staged = quarantine.stage("fetch.pack", packBytes)
            val pack = GitPackReader.parse(
                packBytes,
                limits,
                origin = GitSha1ObjectOrigin.untrustedTransport,
                cancellation = cancellation,
                detector = repository.detector,
                progress = progress,
            )
            verifyConnectivity(pack, wants, shallow, cancellation)
            if (pack.objects.isEmpty()) {
                quarantine.reject()
                return 0
            }
            val checksum = pack.checksum
            val name = "pack-${encodeLowerHex(checksum)}"
            val indexBytes = GitPackIndexWriter.write(
                pack.objects.map { GitPackIndexWriteEntry(it.objectId, offset = it.offset, crc32 = it.crc32) },
                checksum,
            )
            val stagedIndex = quarantine.stage("fetch.idx", indexBytes)
            val packDirectory = File(File(repository.gitDirectory, "objects"), "pack")
            createDirectories(packDirectory)
            val packFile = File(packDirectory, "$name.pack")
            val indexFile = File(packDirectory, "$name.idx")
            cancellation.throwIfCancelled()
            if (fileExists(packFile) && fileExists(indexFile)) {
                quarantine.reject()
            } else {
                renameFile(staged, packFile)
                cancellation.throwIfCancelled()
                renameFile(stagedIndex, indexFile)
                quarantine.reject()
            }
            repository.packedObjects.invalidate()
            return pack.objects.size
        } catch (error: Throwable) {
            quarantine.reject()
            throw error
        }
    }

    private fun verifyConnectivity(
        pack: GitPack,
        wants: List<GitObjectId>,
        shallow: Set<GitObjectId>,
        cancellation: GitCancellationToken,
    ) {
        val byId = HashMap<GitObjectId, GitPackObject>()
        for (item in pack.objects) byId[item.objectId] = item
        val verified = HashSet<GitObjectId>()
        val stack = ArrayList<GitObjectId>(wants)
        var visited = 0
        while (stack.isNotEmpty()) {
            cancellation.throwIfCancelled()
            val id = stack.removeAt(stack.size - 1)
            if (!verified.add(id)) continue
            visited += 1
            if (visited > repository.limits.maxCommitWalk * 8) {
                throw GitException(GitErrorCode.resourceLimitExceeded, "connectivity check exceeded the configured limit")
            }
            val packObject = byId[id]
            if (packObject == null) {
                if (!repository.hasObject(id)) {
                    throw GitException(GitErrorCode.packCorrupt, "remote pack does not contain a required object")
                }
                continue
            }
            when (packObject.type) {
                GitObjectType.blob -> {}
                GitObjectType.tree -> for (entry in GitTree.parse(packObject.body).entries) {
                    stack.add(entry.objectId)
                }
                GitObjectType.commit -> {
                    val commit = GitCommit.parse(packObject.body)
                    stack.add(commit.tree)
                    stack.addAll(walkParents(id, commit, shallow))
                }
                GitObjectType.tag -> stack.add(GitAnnotatedTag.parse(packObject.body).target)
            }
        }
    }

    private fun writeFetchedRefs(refWrites: Map<String, GitObjectId>): List<GitRefUpdateRecord> {
        val store = RefStore(repository.gitDirectory)
        val updates = ArrayList<GitRefUpdateRecord>()
        for (name in refWrites.keys.sorted()) {
            val target = refWrites.getValue(name)
            val ref = GitRefName.parse(name)
            if (!repository.hasObject(target)) {
                throw GitException(GitErrorCode.packCorrupt, "fetched ref target is not present after install")
            }
            val current = store.readDirect(ref)
            if (current == target) {
                updates.add(GitRefUpdateRecord(ref = name, oldId = current, newId = target))
                continue
            }
            val result = store.update(ref, expectedOld = current, next = target)
            if (result != GitRefUpdateResult.updated) {
                throw GitException(GitErrorCode.refLocked, "remote-tracking ref changed during fetch")
            }
            updates.add(GitRefUpdateRecord(ref = name, oldId = current, newId = target))
        }
        return updates
    }

    private fun writeFetchHead(plan: FetchPlan, url: GitRemoteUrl) {
        val buffer = StringBuilder()
        var first = true
        for (remoteRef in plan.remoteRefs) {
            if (remoteRef.name == "HEAD") continue
            val isBranch = remoteRef.name.startsWith("refs/heads/")
            val isTag = remoteRef.name.startsWith("refs/tags/")
            if (!isBranch && !isTag) continue
            val short = remoteRef.name.substring(if (isBranch) 11 else 10)
            val marker = if (first && isBranch) "" else "not-for-merge"
            if (first && isBranch) first = false
            buffer.append(remoteRef.objectId.hex).append('\t').append(marker).append('\t')
                .append(if (isBranch) "branch" else "tag").append(" '").append(short).append("' of ")
                .append(url.uri.toString()).append('\n')
        }
        val destination = File(repository.gitDirectory, "FETCH_HEAD")
        val lock = File("${destination.path}.lock")
        if (!createFileExclusive(lock)) {
            throw GitException(GitErrorCode.refLocked, "FETCH_HEAD lock already exists")
        }
        try {
            writeStringFlushed(lock, buffer.toString())
            renameFile(lock, destination)
        } finally {
            if (fileExists(lock)) lock.delete()
        }
    }

    fun push(
        url: GitRemoteUrl,
        remoteName: String,
        refspecs: List<GitPushRefspec>,
        access: GitRemoteAccess,
        cancellation: GitCancellationToken,
        sink: GitProgressSink,
    ): GitPushResult {
        if (refspecs.isEmpty()) {
            throw GitException(GitErrorCode.invalidRef, "push requires at least one branch")
        }
        val session = RemoteSession.open(
            url = url,
            access = access,
            limits = repository.limits,
            cancellation = cancellation,
            sink = sink,
        )
        val journal = GitOperationJournal.begin(repository.gitDirectory, "push")
        try {
            val advertisement = GitSmartHttp.parseReceivePackAdvertisement(
                session.advertisement(GitSmartHttpService.receivePack),
                maxPackets = repository.limits.maxPktLines,
                maxRefCount = repository.limits.maxRefCount,
            )
            val capabilities = GitReceivePack.selectCapabilities(advertisement)
            val store = RefStore(repository.gitDirectory)
            val results = ArrayList<GitPushRefResult>()
            val commands = ArrayList<GitPushRefUpdate>()
            val newTips = ArrayList<GitObjectId>()
            val seenRemote = HashSet<String>()
            for (refspec in refspecs) {
                session.checkpoint()
                if (!seenRemote.add(refspec.remote.value)) {
                    throw GitException(GitErrorCode.invalidRef, "push refspecs target the same remote branch twice")
                }
                val tip = store.readDirect(refspec.local)
                    ?: throw GitException(GitErrorCode.invalidRef, "local branch ${refspec.localBranch} does not exist")
                val remoteOld = advertisement.refs[refspec.remote.value]
                if (remoteOld == tip) {
                    results.add(
                        GitPushRefResult(
                            localRef = refspec.local.value,
                            remoteRef = refspec.remote.value,
                            oldId = remoteOld,
                            newId = tip,
                            outcome = GitPushRefOutcome.upToDate,
                        ),
                    )
                    continue
                }
                if (remoteOld != null) {
                    requireFastForward(remoteOld, tip, refspec, cancellation)
                }
                commands.add(
                    GitPushRefUpdate(
                        refName = refspec.remote.value,
                        expectedOld = remoteOld ?: GitReceivePack.zeroId,
                        next = tip,
                    ),
                )
                newTips.add(tip)
                results.add(
                    GitPushRefResult(
                        localRef = refspec.local.value,
                        remoteRef = refspec.remote.value,
                        oldId = remoteOld,
                        newId = tip,
                        outcome = GitPushRefOutcome.updated,
                    ),
                )
            }
            if (commands.isEmpty()) {
                return GitPushResult(refs = results, packBytes = 0, objects = 0)
            }
            session.sink.add(GitProgress(GitProgressPhase.preparingPack, remoteHost = session.host))
            val exclude = pushExclusionSet(advertisement.refs.values, newTips, cancellation)
            val pack = repository.writePack(newTips, exclude = exclude, cancellation = cancellation)
            session.checkpoint()
            val objectCount = packObjectCount(pack.packBytes)
            session.sink.add(
                GitProgress(
                    GitProgressPhase.sendingPack,
                    bytes = pack.packBytes.size.toLong(),
                    objects = objectCount,
                    remoteHost = session.host,
                ),
            )
            val body = GitBytesBuilder()
            body.add(encodePackets(GitReceivePack.commandPackets(commands, capabilities = capabilities)))
            body.add(pack.packBytes)
            val responseBody = session.post(
                GitSmartHttpService.receivePack,
                body.takeBytes(),
                maxResponseBytes = repository.limits.maxAdvertisementBytes,
            )
            session.sink.add(GitProgress(GitProgressPhase.awaitingReport, remoteHost = session.host))
            val status = GitReceivePack.parseResponse(
                responseBody,
                sideband = capabilities.contains("side-band-64k"),
                maxPackets = repository.limits.maxPktLines,
                maxProgressBytes = repository.limits.maxSidebandBytes,
            )
            if (!status.unpackOk) {
                throw GitException(
                    GitErrorCode.remoteRejected,
                    "remote failed to unpack the push: ${status.unpackMessage ?: ""}",
                )
            }
            val byRef = LinkedHashMap<String, GitPushRefStatus>()
            for (ref in status.refs) byRef[ref.refName] = ref
            val finalResults = ArrayList<GitPushRefResult>()
            var firstRejection: GitException? = null
            for (result in results) {
                if (result.outcome == GitPushRefOutcome.upToDate) {
                    finalResults.add(result)
                    continue
                }
                val reported = byRef.remove(result.remoteRef)
                    ?: throw GitException(GitErrorCode.protocolUnsupported, "remote report omitted a pushed ref")
                if (reported.accepted) {
                    updateRemoteTracking(remoteName, result)
                    finalResults.add(result)
                    continue
                }
                val code = reported.code ?: GitErrorCode.remoteRejected
                if (firstRejection == null) {
                    firstRejection = GitException(code, "remote rejected ${result.remoteRef}: ${reported.message ?: ""}")
                }
                finalResults.add(
                    GitPushRefResult(
                        localRef = result.localRef,
                        remoteRef = result.remoteRef,
                        oldId = result.oldId,
                        newId = result.newId,
                        outcome = GitPushRefOutcome.rejected,
                        code = code,
                        message = reported.message,
                    ),
                )
            }
            if (byRef.isNotEmpty()) {
                throw GitException(GitErrorCode.protocolUnsupported, "remote report names a ref that was not pushed")
            }
            if (firstRejection != null) throw firstRejection
            return GitPushResult(refs = finalResults, packBytes = pack.packBytes.size, objects = objectCount)
        } finally {
            journal.complete()
            session.close()
        }
    }

    private fun packObjectCount(packBytes: ByteArray): Int = packBytes.readUint32(8).toInt()

    private fun requireFastForward(
        remoteOld: GitObjectId,
        tip: GitObjectId,
        refspec: GitPushRefspec,
        cancellation: GitCancellationToken,
    ) {
        if (!repository.hasObject(remoteOld)) {
            throw GitException(
                GitErrorCode.nonFastForward,
                "remote ${refspec.remote.value} points at a commit that is not known locally; fetch first",
            )
        }
        if (!repository.isAncestor(remoteOld, tip, cancellation)) {
            throw GitException(
                GitErrorCode.nonFastForward,
                "pushing ${refspec.local.value} to ${refspec.remote.value} would not fast-forward",
            )
        }
    }

    private fun pushExclusionSet(
        remoteTips: Iterable<GitObjectId>,
        newTips: List<GitObjectId>,
        cancellation: GitCancellationToken,
    ): Set<GitObjectId> {
        val knownTips = ArrayList<GitObjectId>()
        for (tip in remoteTips) {
            cancellation.throwIfCancelled()
            if (repository.hasObject(tip)) knownTips.add(tip)
        }
        val remoteCommits = repository.reachableCommits(knownTips, cancellation)
        val exclude = LinkedHashSet<GitObjectId>(remoteCommits)
        val boundaries = LinkedHashSet<GitObjectId>()
        val shallow = repository.shallowCommits()
        val visited = HashSet<GitObjectId>()
        val stack = ArrayList<GitObjectId>(newTips)
        while (stack.isNotEmpty()) {
            cancellation.throwIfCancelled()
            val id = stack.removeAt(stack.size - 1)
            if (!visited.add(id)) continue
            if (remoteCommits.contains(id)) {
                boundaries.add(id)
                continue
            }
            if (visited.size > repository.limits.maxCommitWalk) {
                throw GitException(GitErrorCode.resourceLimitExceeded, "push commit walk exceeded the configured limit")
            }
            val commit = repository.readCommitOrTagTarget(id)
            if (commit != null) stack.addAll(walkParents(id, commit, shallow))
        }
        for (boundary in boundaries) {
            val commit = repository.readCommitOrTagTarget(boundary) ?: continue
            repository.collectTreeClosure(commit.tree, exclude, cancellation)
        }
        return exclude
    }

    private fun updateRemoteTracking(remoteName: String, result: GitPushRefResult) {
        val tracking = GitRefName.parse("refs/remotes/$remoteName/${result.remoteRef.substring(11)}")
        val store = RefStore(repository.gitDirectory)
        val current = store.readDirect(tracking)
        if (current == result.newId) return
        store.update(tracking, expectedOld = current, next = result.newId)
    }

    private companion object {
        const val havesPerRound = 32
        const val maxHaves = 512
    }
}

internal class HaveWalk(private val repository: GitRepository, private val maxCommits: Int) {
    private val queue = ArrayDeque<GitObjectId>()
    private val pendingExpansion = ArrayList<GitObjectId>()
    private val seen = HashSet<GitObjectId>()

    var sent = 0
        private set

    private var shallow: Set<GitObjectId> = emptySet()

    fun seed() {
        shallow = repository.shallowCommits()
        val store = RefStore(repository.gitDirectory)
        val tips = LinkedHashSet<GitObjectId>()
        for (prefix in listOf("refs/heads/", "refs/remotes/")) {
            tips.addAll(store.listDirect(prefix, repository.limits).values)
        }
        repository.readHeadObjectId()?.let { tips.add(it) }
        queue.addAll(tips)
    }

    fun nextBatch(size: Int, common: Set<GitObjectId>): List<GitObjectId> {
        for (id in pendingExpansion) {
            if (common.contains(id)) continue
            val commit = repository.readCommitOrTagTarget(id)
            if (commit != null) queue.addAll(walkParents(id, commit, shallow))
        }
        pendingExpansion.clear()
        val batch = ArrayList<GitObjectId>()
        while (batch.size < size && queue.isNotEmpty()) {
            val id = queue.removeFirst()
            if (!seen.add(id) || common.contains(id)) continue
            if (seen.size > maxCommits) break
            if (!repository.hasObject(id)) continue
            batch.add(id)
            pendingExpansion.add(id)
        }
        sent += batch.size
        return batch
    }
}
