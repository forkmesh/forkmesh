package com.forkmesh.nativeapp.collab

import com.forkmesh.nativeapp.api.DartException
import com.forkmesh.nativeapp.code.agoLabel
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

    fun author(pull: PublishedPull): String {
        val name = pull.authorName.trim()
        if (name.isNotEmpty()) return name
        val key = pull.author.trim()
        return if (key.length > 10) key.take(8) + "…" else key
    }

    fun age(pull: PublishedPull, nowMs: Long = System.currentTimeMillis()): String =
        if (pull.createdMs > 0) agoLabel(PullTimeline.epochMs(pull.createdMs), nowMs) else ""
}

object PullDetail {
    const val NO_DESCRIPTION = "No description provided."
    const val NO_PATCH = "No patch content was published for this pull request yet."
    const val REVIEW_ACTIONS_COPY =
        "Submit comments, approvals, or requested changes through the signed Worker pull inbox."
    const val TIMELINE_EMPTY = "No review or comment events have been published yet."
    const val COMMITS_TITLE = "Commits"

    fun commitsLine(count: Int): String = "$count ${if (count == 1) "commit" else "commits"}"

    const val CONVERSATION_TITLE = "Conversation"
    const val REVIEW_TITLE = "Review"
    const val COMMENT_TITLE = "Comment"
    const val REVIEW_COPY = "Approve, request changes or comment. Reviews are signed and sent to the pull inbox."
    const val DONE_REVIEW_COPY =
        "This pull request is no longer open, so it cannot be approved or merged. Comments still reach the pull inbox."

    fun openedLine(pull: PublishedPull, nowMs: Long = System.currentTimeMillis()): String {
        val age = PullCard.age(pull, nowMs)
        val who = PullCard.author(pull).ifEmpty { "Someone" }
        return "$who opened this pull request" + if (age.isEmpty()) "" else " · $age"
    }

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
                "pull.merge", "pull.merge_delete" -> false
                "pull.fix_agent", "pull.close" -> open
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
    ): String {
        val account = sessionNodeName?.trim()?.lowercase() ?: ""
        if (sessionNodeName == null || identityPublicKey == null || account.isEmpty()) {
            throw DartException("Sign in with the repo owner account to request desktop control.")
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
        "Requested changes", "Closed" -> CollabTone.Danger
        "Merged" -> CollabTone.AccentEdge
        else -> CollabTone.Tertiary
    }

    fun icon(event: PullEvent): String = when (event.displayTitle) {
        "Approved" -> "check_circle_outline"
        "Requested changes" -> "cancel_outlined"
        "Merged" -> "call_merge"
        "Closed" -> "archive_outlined"
        "Reopened" -> "lock_open_outlined"
        else -> "rate_review_outlined"
    }

    fun epochMs(ts: Long): Long = if (ts > 0 && ts < 100_000_000_000L) ts * 1000 else ts

    fun mergedEvent(pull: PublishedPull): PullEvent? {
        if (pull.status.trim().lowercase() != "merged") return null
        val by = pull.mergedBy.trim()
        val name = if (by.isNotEmpty() && by == pull.author) pull.authorName else ""
        val into = if (pull.base.isEmpty()) "" else " into ${pull.base}"
        val head = if (pull.head.isEmpty()) "" else " ${pull.head}"
        return PullEvent(type = "merged", body = "Merged$head$into.", author = by, authorName = name, state = pull.base, ts = pull.mergedMs)
    }

    fun events(pull: PublishedPull): List<PullEvent> {
        val merged = mergedEvent(pull) ?: return pull.events
        val rows = pull.events.filter { it.type != "merged" }.toMutableList()
        val at = epochMs(merged.ts)
        val index = if (at > 0) rows.indexOfFirst { epochMs(it.ts) > at } else -1
        if (index >= 0) rows.add(index, merged) else rows.add(merged)
        return rows
    }

    fun author(event: PullEvent): String {
        if (event.authorName.isNotEmpty()) return event.authorName
        val key = event.author.trim()
        return if (key.length > 10) key.take(8) + "…" else key
    }
}

enum class PullState(val raw: String, val label: String, val icon: String, val tone: CollabTone) {
    Open("open", "Open", "call_split", CollabTone.Success),
    Merged("merged", "Merged", "call_merge", CollabTone.AccentEdge),
    Closed("closed", "Closed", "archive_outlined", CollabTone.Danger);

    companion object {
        const val MERGED_TITLE = "Pull request successfully merged and closed"
        const val CLOSED_TITLE = "Closed with unmerged commits"
        const val CLOSED_COPY = "This pull request was closed without merging. Reviews and merges are off until it is reopened."

        fun of(pull: PublishedPull): PullState = when (pull.status.trim().lowercase()) {
            "merged", "closed-merged" -> Merged
            "open", "" -> Open
            else -> Closed
        }

        fun summary(pull: PublishedPull, commits: Int, nowMs: Long = System.currentTimeMillis()): String {
            val who = PullCard.author(pull).ifEmpty { "Someone" }
            val count = if (commits > 0) "$commits ${if (commits == 1) "commit" else "commits"}" else "changes"
            val branches = if (pull.base.isEmpty()) "" else " into ${pull.base}" + if (pull.head.isEmpty()) "" else " from ${pull.head}"
            return when (of(pull)) {
                Open -> "$who wants to merge $count$branches"
                Merged -> {
                    val whenText = if (pull.mergedMs > 0) " " + agoLabel(PullTimeline.epochMs(pull.mergedMs), nowMs) else ""
                    "$who's $count merged$branches$whenText"
                }
                Closed -> "$who proposed $count$branches · closed without merging"
            }
        }

        fun mergedCopy(pull: PublishedPull): String {
            val into = pull.base.ifEmpty { "the base branch" }
            val what = if (pull.head.isEmpty()) "These changes are" else "${pull.head} is"
            return "$what in $into. Nothing more can be merged from this pull request; comments are still welcome."
        }

        fun reviewable(pull: PublishedPull): Boolean = of(pull) == Open
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
