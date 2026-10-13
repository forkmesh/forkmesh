package com.forkmesh.nativeapp.collab

import com.forkmesh.nativeapp.api.DartException
import com.forkmesh.nativeapp.model.PublishedPull
import com.forkmesh.nativeapp.model.PullEvent
import com.forkmesh.nativeapp.model.PullRequest

enum class CollabTone { Success, Danger, Accent, AccentEdge, Secondary, Tertiary }

object PullCard {
    fun tone(pull: PublishedPull): CollabTone = when (pull.status) {
        "merged" -> CollabTone.AccentEdge
        "closed" -> CollabTone.Danger
        else -> CollabTone.Success
    }

    fun subtitle(pull: PublishedPull): String =
        "#${pull.number}" + if (pull.head.isNotEmpty() || pull.base.isNotEmpty()) " · ${pull.head} → ${pull.base}" else ""

    fun badge(pull: PublishedPull): String = if (pull.signed) "signed" else pull.status
}

object PullDetail {
    const val NO_DESCRIPTION = "No description provided."
    const val NO_PATCH = "No patch content was published for this pull request yet."
    const val REVIEW_ACTIONS_COPY =
        "Submit comments, approvals, or requested changes through the signed Worker pull inbox."
    const val TIMELINE_EMPTY = "No review or comment events have been published yet."

    fun canRequestMerge(pull: PublishedPull): Boolean = pull.status.trim().lowercase() == "open"

    fun shipControlsCopy(pull: PublishedPull): String = if (canRequestMerge(pull)) {
        "Queue signed PR requests. The desktop node will verify mergeability, show conflicts if any, and run Git only after the repo owner approves locally."
    } else {
        "This pull request is ${pull.status}; merge/review-changing requests may be rejected by the desktop node if the local state does not support them."
    }

    fun body(pull: PublishedPull): String = pull.body.ifEmpty { NO_DESCRIPTION }

    fun branchesChip(pull: PublishedPull): String? =
        if (pull.head.isNotEmpty() || pull.base.isNotEmpty()) "${pull.head} → ${pull.base}" else null

    fun commandTarget(pull: PublishedPull): String = "pull:${pull.number}"

    fun commandPayload(pull: PublishedPull): Map<String, Any?> = linkedMapOf<String, Any?>("pullNumber" to pull.number).apply {
        if (pull.base.isNotEmpty()) put("base", pull.base)
        if (pull.head.isNotEmpty()) put("head", pull.head)
    }
}

data class PullShipCommand(
    val command: String,
    val label: String,
    val icon: String,
    val successMessage: String,
    val filled: Boolean = false,
    val needsOpen: Boolean = false,
) {
    val chipLabel: String get() = when (command) {
        "pull.merge" -> "Merge on desktop"
        "pull.merge_delete" -> "Merge + delete branch"
        "pull.fix_agent" -> "Fix with agent"
        "pull.close" -> "Close"
        "pull.reopen" -> "Reopen"
        "pull.delete" -> "Delete"
        else -> label
    }
}

object PullShipCommands {
    fun offered(pull: PublishedPull): List<PullShipCommand> {
        val status = pull.status.trim().lowercase()
        val open = status == "open"
        return all(pull).filter {
            when (it.command) {
                "pull.merge", "pull.fix_agent", "pull.close" -> open
                "pull.merge_delete" -> open && pull.head.isNotBlank()
                "pull.reopen" -> status == "closed"
                else -> true
            }
        }
    }

    fun all(pull: PublishedPull): List<PullShipCommand> = listOf(
        PullShipCommand(
            "pull.merge",
            "Request merge on desktop",
            "call_merge_outlined",
            "Merge queued. Approve it in the ForkMesh desktop app to merge pull request #${pull.number}.",
            filled = true,
            needsOpen = true,
        ),
        PullShipCommand(
            "pull.merge_delete",
            "Request merge + delete branch",
            "delete_sweep_outlined",
            "Merge + delete request queued. Approve it in the ForkMesh desktop app.",
            needsOpen = true,
        ),
        PullShipCommand(
            "pull.fix_agent",
            "Request fix with agent",
            "smart_toy_outlined",
            "Fix-with-agent request queued. Approve it in the ForkMesh desktop app.",
        ),
        PullShipCommand(
            "pull.close",
            "Request close on desktop",
            "archive_outlined",
            "Close request queued. Approve it in the ForkMesh desktop app.",
        ),
        PullShipCommand(
            "pull.reopen",
            "Request reopen on desktop",
            "lock_open_outlined",
            "Reopen request queued. Approve it in the ForkMesh desktop app.",
        ),
        PullShipCommand(
            "pull.delete",
            "Request delete on desktop",
            "delete_outline",
            "Delete request queued. Approve it in the ForkMesh desktop app.",
        ),
    )
}

object DesktopCommand {
    const val FAILURE_MESSAGE = "Desktop request was not queued."

    fun canonical(owner: String, name: String, command: String, target: String, ts: String): String =
        com.forkmesh.nativeapp.control.DesktopCommandContract.canonical(owner, name, command, target, ts)

    fun ownerAccount(
        sessionNodeName: String?,
        sessionPubkey: String?,
        identityPublicKey: String?,
        repoOwner: String,
    ): String {
        val account = sessionNodeName?.trim()?.lowercase() ?: ""
        if (sessionNodeName == null || identityPublicKey == null || account.isEmpty()) {
            throw DartException("Sign in with the repo owner account to request desktop control.")
        }
        if (account != repoOwner.trim().lowercase()) {
            throw DartException("Only the repo owner can request desktop control for this repo.")
        }
        if ((sessionPubkey ?: "").trim() != identityPublicKey) {
            throw DartException("Pair this mobile device with the owner key before requesting desktop control.")
        }
        return account
    }
}

object PullTimeline {
    fun tone(event: PullEvent): CollabTone = when (event.displayTitle) {
        "Approved" -> CollabTone.Success
        "Requested changes" -> CollabTone.Danger
        else -> CollabTone.Tertiary
    }

    fun icon(event: PullEvent): String = when (event.displayTitle) {
        "Approved" -> "check_circle_outline"
        "Requested changes" -> "cancel_outlined"
        else -> "rate_review_outlined"
    }
}

object PullReview {
    const val REVIEW_HINT = "Add a review note"
    const val NOTE_REQUIRED = "A note is required to request changes."

    fun header(number: Int, title: String): String = "#$number · $title"

    fun subtitle(head: String, base: String, status: String): String = "$head → $base · $status"

    fun subtitle(pr: PullRequest): String = subtitle(pr.headBranch, pr.baseBranch, pr.status)

    fun subtitle(pull: PublishedPull): String = subtitle(pull.head, pull.base, pull.status)

    fun commentTitle(number: Int): String = "Comment on PR #$number"

    fun composerTitle(state: String, number: Int): String = when (state) {
        "approve" -> "Approve PR #$number"
        "request-changes" -> "Request changes on PR #$number"
        else -> "Review comment on PR #$number"
    }

    fun composerHelper(state: String): String = when (state) {
        "approve" ->
            "Approval is signed and sent to the pull inbox. The repo owner still applies canonical changes from their desktop node."
        "request-changes" ->
            "Request-changes reviews are signed and sent to the pull inbox. Add a clear note so the author knows what to fix."
        else -> "Review comments are signed and sent to the pull inbox, pending the repo owner applying them."
    }

    fun validationError(state: String, body: String): String? =
        if (state == "request-changes" && body.trim().isEmpty()) NOTE_REQUIRED else null
}

enum class PatchChangeKind { Added, Modified, Deleted, Renamed }

data class PatchFile(val path: String, val kind: PatchChangeKind, val adds: Int = 0, val dels: Int = 0)

fun dartLines(text: String): List<String> {
    if (text.isEmpty()) return emptyList()
    val lines = text.split("\r\n", "\n", "\r")
    return if (text.endsWith('\n') || text.endsWith('\r')) lines.dropLast(1) else lines
}

private val DIFF_HEADER = Regex("^diff --git a/(.*?) b/(.*)$")

fun parsePatchFiles(diff: String): List<PatchFile> {
    if (diff.trim().isEmpty()) return emptyList()
    val files = ArrayList<PatchFile>()
    var path: String? = null
    var kind = PatchChangeKind.Modified
    var adds = 0
    var dels = 0
    fun flush() {
        val current = path
        if (current.isNullOrEmpty()) return
        files.add(PatchFile(current, kind, adds, dels))
    }
    for (line in dartLines(diff)) {
        when {
            line.startsWith("diff --git ") -> {
                flush()
                val match = DIFF_HEADER.find(line)
                path = (match?.groupValues?.get(2) ?: line.substring("diff --git ".length)).trim()
                kind = PatchChangeKind.Modified
                adds = 0
                dels = 0
            }
            line.startsWith("new file mode") || line.startsWith("--- /dev/null") -> kind = PatchChangeKind.Added
            line.startsWith("deleted file mode") || line.startsWith("+++ /dev/null") -> kind = PatchChangeKind.Deleted
            line.startsWith("rename from ") || line.startsWith("similarity index") -> kind = PatchChangeKind.Renamed
            line.startsWith("rename to ") -> path = line.substring("rename to ".length).trim()
            line.startsWith("+++ b/") -> if (path == null) path = line.substring("+++ b/".length).trim()
            line.startsWith("+++ ") || line.startsWith("--- ") -> continue
            line.startsWith("+") -> adds++
            line.startsWith("-") -> dels++
        }
    }
    flush()
    return files
}

fun changedFilesTitle(count: Int): String = "$count changed ${if (count == 1) "file" else "files"}"

enum class DiffLineKind { Added, Removed, Hunk, Context }

fun diffLineKind(line: String): DiffLineKind = when {
    line.startsWith("+") && !line.startsWith("+++") -> DiffLineKind.Added
    line.startsWith("-") && !line.startsWith("---") -> DiffLineKind.Removed
    line.startsWith("@@") -> DiffLineKind.Hunk
    else -> DiffLineKind.Context
}
