package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.api.ApiJsonResult
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.model.auth.AuthSession
import com.forkmesh.nativeapp.security.DeviceIdentity
import java.security.SecureRandom

@Suppress("EnumEntryName")
enum class PublishStatus {
    published,

    mirrored,

    unauthorized,

    stale,

    upgradeRequired,

    limited,

    failed,
}

data class PublishOutcome(
    val status: PublishStatus,
    val message: String,
    val publishedAs: String = "",
    val canonicalNodeId: String = "",
) {
    val isSuccess: Boolean get() = status == PublishStatus.published
}

data class PublishHead(
    val commit: String = "",
    val rootCommit: String = "",
    val subject: String = "",
    val authorName: String = "",
    val at: Long? = null,
    val commitCount: Int = 0,
    val branchCount: Int = 0,
)

open class PublishService(
    val api: ApiService,
    private val identity: () -> DeviceIdentity?,
    private val prefs: PrefStore,
    private val clock: () -> Long = System::currentTimeMillis,
) {
    @Volatile
    private var cachedNodeId: String? = null

    @Synchronized
    fun nodeId(): String {
        cachedNodeId?.let { return it }
        var value = prefs.getString(nodeIdKey) ?: ""
        if (!nodeIdPattern.matches(value)) {
            val bytes = ByteArray(16).also(SecureRandom()::nextBytes)
            value = bytes.joinToString("") { (it.toInt() and 0xff).toString(16).padStart(2, '0') }
            prefs.putString(nodeIdKey, value)
        }
        cachedNodeId = value
        return value
    }

    open fun credentials(owner: String, repo: String, signed: Boolean = true): ForkMeshRemoteCredentials {
        val signer = if (signed) identity() else null
        return ForkMeshRemoteCredentials(
            origin = api.gitOrigin,
            owner = owner,
            repo = repo,
            signer = signer,
            pubkeyB64url = signer?.publicKeyB64url ?: "",
        )
    }

    open fun push(
        workspace: GitWorkspace,
        owner: String,
        repo: String,
        branch: String,
        onProgress: ((GitTransferProgress) -> Unit)? = null,
    ): LocalPushResult = workspace.push(credentials = credentials(owner, repo), branch = branch, onProgress = onProgress)

    open fun fetch(
        workspace: GitWorkspace,
        owner: String,
        repo: String,
        onProgress: ((GitTransferProgress) -> Unit)? = null,
    ): LocalFetchResult = workspace.fetch(credentials = credentials(owner, repo), onProgress = onProgress)

    open fun pull(
        workspace: GitWorkspace,
        owner: String,
        repo: String,
        branch: String,
        onProgress: ((GitTransferProgress) -> Unit)? = null,
    ): LocalPullResult = workspace.pull(credentials = credentials(owner, repo), branch = branch, onProgress = onProgress)

    open fun publish(
        session: AuthSession?,
        workspace: GitWorkspace,
        repo: String,
        branch: String,
        isPublic: Boolean,
        description: String = "",
        head: PublishHead = PublishHead(),
        onProgress: ((GitTransferProgress) -> Unit)? = null,
        forkedFrom: String = "",
    ): PublishOutcome {
        val refusal = describeSession(session)
        if (refusal != null || session == null) {
            return PublishOutcome(status = PublishStatus.unauthorized, message = refusal ?: "")
        }
        val owner = session.nodeName
        val push = push(workspace = workspace, owner = owner, repo = repo, branch = branch, onProgress = onProgress)
        val rejected = push.refs.filter { it.isRejected }
        if (rejected.isNotEmpty()) {
            val first = rejected.first()
            return PublishOutcome(
                status = PublishStatus.failed,
                message = if (first.message.isEmpty()) {
                    "ForkMesh refused the push for ${first.branch}."
                } else {
                    "ForkMesh refused ${first.branch}: ${first.message}"
                },
            )
        }
        return publishCatalogRecord(
            session = session,
            repo = repo,
            branch = branch,
            isPublic = isPublic,
            description = description,
            head = head,
            forkedFrom = forkedFrom,
        )
    }

    open fun publishCatalogRecord(
        session: AuthSession,
        repo: String,
        branch: String,
        isPublic: Boolean,
        description: String = "",
        head: PublishHead = PublishHead(),
        forkedFrom: String = "",
    ): PublishOutcome {
        val owner = session.nodeName
        val signer = identity() ?: return PublishOutcome(
            status = PublishStatus.unauthorized,
            message = "This account has no key that may publish. Sign in again on this device.",
        )
        val record = buildCatalogRecord(
            owner = owner,
            name = repo,
            description = description,
            isPublic = isPublic,
            branch = branch,
            head = head,
            maintainer = signer.publicKeyB64url,
            nodeId = nodeId(),
            machineName = session.thisDevice?.label ?: "",
            cloneUrl = "${api.gitOrigin}/$owner/$repo",
            updatedAt = clock(),
            forkedFrom = forkedFrom,
        )
        val signable = LinkedHashMap(record).apply { remove("signature") }
        val answer: ApiJsonResult = try {
            val signature = signer.sign(catalogV2Canonical(signable).toByteArray(Charsets.UTF_8))
            api.publishRepository(
                LinkedHashMap<String, Any?>(record).apply {
                    put("catalogSigVersion", 2)
                    put("catalogSig", signature)
                    put("publishIntent", "user")
                },
            )
        } catch (error: Exception) {
            return PublishOutcome(status = PublishStatus.failed, message = "Could not reach ForkMesh to publish ($error).")
        }
        return interpret(answer, owner = owner, repo = repo)
    }

    private fun interpret(answer: ApiJsonResult, owner: String, repo: String): PublishOutcome {
        val publishedAs = "$owner/$repo"
        if (answer.ok && answer.body["mirror"] == true) {
            return PublishOutcome(
                status = PublishStatus.mirrored,
                canonicalNodeId = (answer.body["canonicalNodeId"] ?: "").toString(),
                publishedAs = publishedAs,
                message = "Another node already publishes $publishedAs, so this device was recorded as a mirror. " +
                    "Publish under a different name to make this phone the source.",
            )
        }
        if (answer.ok) {
            return PublishOutcome(status = PublishStatus.published, publishedAs = publishedAs, message = "Published $publishedAs.")
        }
        return when (answer.statusCode) {
            409 -> PublishOutcome(
                status = PublishStatus.stale,
                message = "ForkMesh already has a newer record for this repository. Fetch and try again.",
            )
            426 -> PublishOutcome(
                status = PublishStatus.upgradeRequired,
                message = "This repository needs owner-sealed encryption, which the mobile app cannot do yet. " +
                    "Publish it as public instead.",
            )
            429 -> PublishOutcome(
                status = PublishStatus.limited,
                message = "ForkMesh is rate-limiting publications for this account, or the account has too many " +
                    "repositories.",
            )
            401, 403 -> PublishOutcome(
                status = PublishStatus.unauthorized,
                message = when (answer.error) {
                    "account_required" -> "This account has no key that may publish. Sign in again on this device."
                    "maintainer_mismatch" -> "ForkMesh does not recognise this device key for $owner."
                    "blocked" -> "This repository name is blocked on ForkMesh."
                    else -> "ForkMesh refused this publication (${answer.error})."
                },
            )
            410 -> PublishOutcome(
                status = PublishStatus.failed,
                message = "This repository was deleted on ForkMesh and cannot be republished from here.",
            )
            else -> PublishOutcome(
                status = PublishStatus.failed,
                message = if (answer.error.isEmpty()) {
                    "ForkMesh could not publish this repository (HTTP ${answer.statusCode})."
                } else {
                    "ForkMesh could not publish this repository (${answer.error})."
                },
            )
        }
    }

    companion object {
        const val nodeIdKey = "node/catalogNodeId"

        const val maxPublishNameLength = 80

        private val nodeIdPattern = Regex("[0-9a-f]{32}")

        fun validatePublishName(name: String): String? {
            LocalRepoStore.validateName(name)?.let { return it }
            if (name.length > maxPublishNameLength) {
                return "Published names are at most $maxPublishNameLength characters."
            }
            return null
        }

        fun describeSession(session: AuthSession?): String? {
            if (session == null || session.nodeName.isEmpty()) {
                return "Sign in to publish a repository to ForkMesh."
            }
            if (!session.canPublish || !session.canOwnerSign) {
                return "This sign-in is not a mobile node, so it cannot publish. Sign out and sign in again on this " +
                    "device to register it as a node."
            }
            return null
        }
    }
}

fun buildCatalogRecord(
    owner: String,
    name: String,
    description: String,
    isPublic: Boolean,
    branch: String,
    maintainer: String,
    nodeId: String,
    updatedAt: Long,
    head: PublishHead = PublishHead(),
    machineName: String = "",
    cloneUrl: String = "",
    platform: String = "mobile",
    version: String = "",
    forkedFrom: String = "",
): LinkedHashMap<String, Any?> {
    val updatedMs = updatedAt.toString()
    val commitAt = head.at?.let { (it / 1000).toString() } ?: ""
    val record = linkedMapOf<String, Any?>(
        "owner" to owner,
        "name" to name,
        "visibility" to if (isPublic) "public" else "private",
        "mirrorEncryption" to "",
        "opaqueRepoId" to "",
        "keyEpoch" to 0,
        "encryptedManifestHash" to "",
        "encryptedManifestSig" to "",
        "sizeBytes" to 0,
        "description" to cleanCatalogString(description, 240),
        "logoMetadata" to linkedMapOf<String, Any?>(
            "description" to "",
            "languages" to linkedMapOf<String, Any?>(),
            "topics" to emptyList<Any?>(),
            "fileStructure" to emptyList<Any?>(),
            "frameworks" to emptyList<Any?>(),
            "projectCategory" to "",
        ),
        "cloneUrl" to cleanCatalogString(cloneUrl, 2048),
        "solana" to "",
        "channel" to cleanCatalogString("#$owner-$name", 120),
        "hostedSince" to "",
        "lastSync" to updatedMs,
        "updatedAt" to updatedMs,
        "rootCommit" to cleanCatalogString(head.rootCommit, 64),
        "source" to "local-node",
        "commit" to cleanCatalogString(head.commit, 64),
        "branch" to cleanCatalogString(branch, 120),
        "issueCount" to "",
        "issueMaxNumber" to "",
        "commitCount" to if (head.commitCount > 0) "${head.commitCount}" else "",
        "branchCount" to if (head.branchCount > 0) "${head.branchCount}" else "",
        "pullCount" to "",
        "discussionCount" to "",
        "activityWeeks" to List(52) { 0 },
        "worktreeCount" to "",
        "artifactCount" to "",
        "platform" to cleanCatalogString(platform, 16),
        "version" to cleanCatalogString(version, 32),
        "nodeId" to cleanCatalogString(nodeId, 64),
        "clonesServed" to "",
        "websiteServed" to "",
        "maintainer" to cleanCatalogString(maintainer, 120),
        "signature" to "",
        "stateHash" to "",
        "stateSig" to "",
    )
    normalizeForkedFrom(forkedFrom, owner = owner, name = name)?.let { record["forkedFrom"] = it }
    val machine = cleanCatalogString(machineName, 63)
    if (machine.isNotEmpty()) record["machineName"] = machine
    val subject = cleanCatalogString(head.subject, 120)
    if (subject.isNotEmpty()) record["commitSubject"] = subject
    val author = cleanCatalogString(head.authorName, 64)
    if (author.isNotEmpty()) record["commitAuthorName"] = author
    if (commitAt.isNotEmpty()) record["commitAt"] = commitAt
    return record
}

fun normalizeForkedFrom(value: String, owner: String, name: String): String? {
    val cleaned = cleanCatalogString(value, 130).lowercase()
    val slash = cleaned.indexOf('/')
    if (slash < 0) return null
    val parentOwner = safeCatalogSegment(cleaned.substring(0, slash)) ?: return null
    val parentName = safeCatalogSegment(cleaned.substring(slash + 1)) ?: return null
    if (parentOwner == owner.trim().lowercase() && parentName == name.trim().lowercase()) return null
    return "$parentOwner/$parentName"
}

private val SAFE_SEGMENT = Regex("^[A-Za-z0-9._:-]+$")

private fun safeCatalogSegment(value: String): String? {
    val trimmed = value.trim()
    if (trimmed.isEmpty() || trimmed.length > 80) return null
    return if (SAFE_SEGMENT.matches(trimmed)) trimmed else null
}
