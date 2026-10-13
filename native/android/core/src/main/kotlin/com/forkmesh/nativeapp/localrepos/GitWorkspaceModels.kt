package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.git.bytes.dartTrim
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.repository.GitIdentity
import java.time.Instant
import java.time.ZoneId
import java.util.Locale

class GitAuthor(val name: String, val email: String) {
    fun toIdentity(nowMillis: Long, zone: ZoneId = ZoneId.systemDefault()): GitIdentity = GitIdentity(
        name = name,
        email = email,
        timestampSeconds = nowMillis / 1000,
        timezoneOffsetMinutes = zone.rules.getOffset(Instant.ofEpochMilli(nowMillis)).totalSeconds / 60,
    )

    override fun equals(other: Any?): Boolean = other is GitAuthor && other.name == name && other.email == email

    override fun hashCode(): Int = 31 * name.hashCode() + email.hashCode()

    override fun toString(): String = "GitAuthor($name <$email>)"

    companion object {
        fun fromAccount(nodeName: String, email: String = ""): GitAuthor {
            val name = identityField(nodeName, fallback = "forkmesh")
            val safeEmail = identityField(email, fallback = "")
            return GitAuthor(name = name, email = if (safeEmail.contains('@')) safeEmail else "$name@mobile.forkmesh")
        }

        private fun identityField(raw: String, fallback: String): String {
            val buffer = StringBuilder()
            val trimmed = raw.dartTrim()
            var index = 0
            while (index < trimmed.length) {
                val rune = trimmed.codePointAt(index)
                index += Character.charCount(rune)
                if (rune < 0x20 || rune == 0x7f) continue
                if (rune == 0x3c || rune == 0x3e) continue
                buffer.appendCodePoint(rune)
            }
            val text = buffer.toString().dartTrim()
            return text.ifEmpty { fallback }
        }
    }
}

@Suppress("EnumEntryName")
enum class LocalFileKind { file, directory, symlink }

data class LocalFileEntry(
    val name: String,
    val path: String,
    val kind: LocalFileKind,
    val size: Long,
    val tracked: Boolean,
) {
    val isDirectory: Boolean get() = kind == LocalFileKind.directory
}

data class LocalFileContent(
    val path: String,
    val bytes: Int,
    val text: String,
    val isBinary: Boolean,
    val isTooLarge: Boolean,
) {
    val isEditable: Boolean get() = !isBinary && !isTooLarge

    companion object {
        const val maxEditableBytes = 2 * 1024 * 1024
    }
}

@Suppress("EnumEntryName")
enum class LocalChangeKind { added, modified, deleted, untracked }

data class LocalChange(val path: String, val kind: LocalChangeKind, val staged: Boolean)

data class LocalStatus(
    val staged: List<LocalChange>,
    val unstaged: List<LocalChange>,
    val untracked: List<LocalChange>,
) {
    val isClean: Boolean get() = staged.isEmpty() && unstaged.isEmpty() && untracked.isEmpty()
    val hasWorktreeChanges: Boolean get() = unstaged.isNotEmpty() || untracked.isNotEmpty()
    val changeCount: Int get() = staged.size + unstaged.size + untracked.size

    companion object {
        val empty = LocalStatus(emptyList(), emptyList(), emptyList())
    }
}

data class LocalBranch(
    val name: String,
    val isCurrent: Boolean,
    val tipId: String?,
)

data class LocalCommit(
    val id: String,
    val parents: List<String>,
    val subject: String,
    val message: String,
    val authorName: String,
    val authorEmail: String,
    val time: Long,
) {
    val shortId: String get() = if (id.length > 7) id.substring(0, 7) else id
}

data class LocalRepoSummary(
    val branch: String,
    val headId: String?,
    val lastCommitSubject: String,
    val lastCommitAt: Long?,
)

data class LocalImportResult(val importedFiles: Int, val skippedEntries: Int, val commitId: String)

data class GitTransferProgress(
    val phase: String,
    val bytes: Long = 0,
    val objects: Int = 0,
    val refs: Int = 0,
    val total: Int = 0,
) {
    val label: String
        get() = when (phase) {
            "connecting" -> "Connecting to ForkMesh"
            "listingRefs" -> "Reading branches"
            "negotiating" -> "Working out what to transfer"
            "receivingPack" -> if (bytes > 0) "Downloading ${humanBytes(bytes)}" else "Downloading"
            "indexingPack" -> when {
                total > 0 && objects > 0 && objects < total -> "Indexing $objects of ${count(total, "object")}"
                total > 0 -> "Indexing ${count(total, "object")}"
                objects > 0 -> "Indexing ${count(objects, "object")}"
                else -> "Indexing objects"
            }
            "updatingRefs" -> "Updating branches"
            "checkingOut" -> when {
                total > 0 && objects > 0 && objects < total -> "Checking out $objects of ${count(total, "file")}"
                total > 0 -> "Checking out ${count(total, "file")}"
                else -> "Checking out files"
            }
            "preparingPack" -> "Packing $objects objects"
            "sendingPack" -> if (bytes > 0) "Uploading ${humanBytes(bytes)}" else "Uploading"
            "awaitingReport" -> "Waiting for ForkMesh"
            "completed" -> "Finishing up"
            else -> "Working"
        }

    private companion object {
        fun count(value: Int, noun: String): String = if (value == 1) "1 $noun" else "$value ${noun}s"

        fun humanBytes(bytes: Long): String {
            if (bytes < 1024) return "$bytes B"
            if (bytes < 1024 * 1024) return "${String.format(Locale.ROOT, "%.0f", bytes / 1024.0)} KB"
            return "${String.format(Locale.ROOT, "%.1f", bytes / (1024.0 * 1024.0))} MB"
        }
    }
}

data class LocalPushRef(
    val remoteRef: String,
    val newId: String,
    val outcome: String,
    val message: String = "",
) {
    val isRejected: Boolean get() = outcome == "rejected"
    val branch: String
        get() = if (remoteRef.startsWith("refs/heads/")) remoteRef.substring("refs/heads/".length) else remoteRef
}

data class LocalPushResult(val refs: List<LocalPushRef>, val packBytes: Int, val objects: Int) {
    val isRejected: Boolean get() = refs.any { it.isRejected }
    val isUpToDate: Boolean get() = refs.isNotEmpty() && refs.all { it.outcome == "upToDate" }
}

data class LocalFetchResult(
    val updatedRefs: List<String>,
    val packBytes: Int,
    val objects: Int,
    val remoteHead: String?,
    val servedBy: String? = null,
) {
    val isUpToDate: Boolean get() = updatedRefs.isEmpty()
}

data class LocalCloneResult(
    val branch: String?,
    val packBytes: Int,
    val objects: Int,
)

data class LocalSyncState(
    val branch: String,
    val localTip: String?,
    val remoteTip: String?,
    val ahead: Int?,
    val behind: Int?,
) {
    val hasRemote: Boolean get() = !remoteTip.isNullOrEmpty()
    val isSynced: Boolean get() = hasRemote && localTip == remoteTip

    val label: String
        get() {
            if (!hasRemote) return "Not pushed yet"
            if (isSynced) return "In sync with ForkMesh"
            val parts = buildList {
                if (ahead == null || ahead > 0) add("${ahead ?: "500+"} to push")
                if (behind == null || behind > 0) add("${behind ?: "500+"} to fetch")
            }
            return if (parts.isEmpty()) "In sync with ForkMesh" else parts.joinToString(" · ")
        }

    companion object {
        fun unknown(branch: String) = LocalSyncState(branch, null, null, null, null)
    }
}

class GitWorkspaceException(
    override val message: String,
    val code: GitErrorCode? = null,
    val detail: String = "",
) : RuntimeException(message) {
    override fun toString(): String = message
}
