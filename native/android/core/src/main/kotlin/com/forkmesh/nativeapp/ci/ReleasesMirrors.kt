package com.forkmesh.nativeapp.ci

import com.forkmesh.nativeapp.api.mirrorsPayloadSyncStatus
import com.forkmesh.nativeapp.json.firstStr
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.model.RepoRelease
import com.forkmesh.nativeapp.model.RepoReleaseAsset
import com.forkmesh.nativeapp.model.modelInt

data class RepoArtifactRow(val tag: String, val asset: RepoReleaseAsset) {
    val id: String get() = "$tag:${asset.sha256}:${asset.name}"
}

class ReleaseAnnouncementDraft(var title: String, var body: String) {
    val validationError: String?
        get() = when {
            title.isBlank() -> "Give the announcement a title."
            title.length > 240 -> "Keep the title under 240 characters."
            body.length > 4000 -> "Keep the text under 4000 characters."
            else -> null
        }

    companion object {
        fun forRelease(release: RepoRelease, repo: RepoRef): ReleaseAnnouncementDraft {
            val lines = mutableListOf("${repo.slug} ${release.tag} is out.")
            if (release.assets.isNotEmpty()) {
                val platforms = release.assets.map { it.platformLabel }.toSortedSet()
                lines.add("Downloads: ${platforms.joinToString(", ")}.")
            }
            return ReleaseAnnouncementDraft("${repo.name} ${release.tag}", lines.joinToString("\n"))
        }
    }
}

class RepoReleasesModel(private val client: CiClient, val repo: RepoRef) {
    @Volatile var releases: List<RepoRelease> = emptyList()
        private set
    @Volatile var loading: Boolean = false
        private set
    @Volatile var error: String? = null
        private set
    @Volatile var busy: Boolean = false
        private set

    fun release(tag: String): RepoRelease? = releases.firstOrNull { it.tag == tag }

    val artifacts: List<RepoArtifactRow>
        get() = releases.flatMap { release -> release.assets.map { RepoArtifactRow(release.tag, it) } }

    val artifactsTotalBytes: Long get() = artifacts.sumOf { maxOf(0L, it.asset.size) }

    fun load(refresh: Boolean = false) {
        loading = true
        if (refresh) client.clearRepoCache(repo.owner, repo.name)
        try {
            releases = client.releases(repo.owner, repo.name)
            error = null
        } catch (failure: Exception) {
            error = failure.message ?: "$failure"
        } finally {
            loading = false
        }
    }

    fun announce(
        draft: ReleaseAnnouncementDraft,
        tag: String,
        account: String,
        sign: ((ByteArray) -> String)?,
        nowMillis: Long,
    ): String {
        draft.validationError?.let { return it }
        if (sign == null || account.isEmpty()) return "Sign in on this device to announce a release."
        if (busy) return ""
        busy = true
        return try {
            client.announceRelease(repo, tag, draft.title, draft.body, account.lowercase(), sign, nowMillis)
            "Announced on the Fediverse."
        } catch (failure: Exception) {
            failure.message ?: "$failure"
        } finally {
            busy = false
        }
    }
}

data class RepoMirrorNode(
    val id: String,
    val node: String,
    val machineName: String,
    val status: String,
    val lastSeenMs: Long,
    val syncAgeMs: Long,
    val behind: Int,
    val cloneAvailable: Boolean,
    val branch: String,
    val commit: String,
    val version: String,
    val platform: String,
    val region: String,
    val latencyMs: Int,
    val directVerified: Boolean,
    val directReason: String,
    val artifactCount: Int,
    val actionsState: String,
) {
    val online: Boolean get() = status == "online" || status == "live"

    val light: String
        get() = when {
            !online -> "offline"
            behind > 0 -> "behind"
            directVerified || cloneAvailable -> "current"
            else -> "unverified"
        }

    val subtitle: String
        get() {
            val parts = mutableListOf<String>()
            if (branch.isNotEmpty()) parts.add(if (commit.isEmpty()) branch else "$branch @ ${commit.take(8)}")
            if (behind > 0) parts.add("$behind behind")
            if (latencyMs > 0) parts.add("$latencyMs ms")
            if (region.isNotEmpty()) parts.add(region)
            if (version.isNotEmpty()) parts.add("v$version")
            return parts.joinToString(" · ")
        }

    companion object {
        fun fromJson(json: Map<String, Any?>): RepoMirrorNode {
            val node = json.firstStr("node", "owner")
            return RepoMirrorNode(
                id = json.str("id").ifEmpty { node },
                node = node,
                machineName = json.str("machineName"),
                status = json.str("status"),
                lastSeenMs = modelInt(json["lastSeen"]),
                syncAgeMs = modelInt(json["syncAgeMs"]),
                behind = modelInt(json["behind"]).toInt(),
                cloneAvailable = json["cloneAvailable"] == true,
                branch = json.str("branch"),
                commit = json.str("commit"),
                version = json.str("version"),
                platform = json.str("platform"),
                region = json.str("region"),
                latencyMs = modelInt(json["latencyMs"]).toInt(),
                directVerified = json["directVerified"] == true,
                directReason = json.str("directReason"),
                artifactCount = modelInt(json["artifactCount"]).toInt(),
                actionsState = json.str("actionsState"),
            )
        }
    }
}

class RepoMirrorsModel(private val client: CiClient, val repo: RepoRef) {
    @Volatile var nodes: List<RepoMirrorNode> = emptyList()
        private set
    @Volatile var sync: Map<String, Any?>? = null
        private set
    @Volatile var loading: Boolean = false
        private set
    @Volatile var error: String? = null
        private set
    @Volatile var onlineOnly: Boolean = false

    val shown: List<RepoMirrorNode> get() = if (onlineOnly) nodes.filter { it.online } else nodes
    val syncLabel: String get() = sync.str("label")
    val syncSummary: String get() = sync.str("summary")
    val syncSeverity: String get() = sync.str("severity")
    val totalArtifacts: Int get() = nodes.maxOfOrNull { it.artifactCount } ?: 0

    fun load() {
        loading = true
        try {
            val payload = client.mirrorsPayload(repo)
            nodes = payload.maps("mirrors").take(200).map(RepoMirrorNode::fromJson)
                .sortedWith(compareBy<RepoMirrorNode> { !it.online }.thenBy { it.node })
            sync = mirrorsPayloadSyncStatus(payload)
            error = null
        } catch (failure: Exception) {
            error = failure.message ?: "$failure"
        } finally {
            loading = false
        }
    }
}
