package com.forkmesh.nativeapp.collab

import com.forkmesh.nativeapp.api.DartException
import com.forkmesh.nativeapp.model.RemoteFork
import com.forkmesh.nativeapp.model.RepoComparison

data class PullCompareResult(val comparison: RepoComparison?, val error: String, val title: String)

object PullCompose {
    const val TITLE = "New pull request"
    const val THIS_REPOSITORY = "This repository"
    const val COMPARE_LABEL = "Compare"
    const val COMPARING = "Comparing…"
    const val PICK_BRANCHES = "Pick the branch with your changes and the branch to merge it into."
    const val SAME_BRANCH = "Choose two different branches."
    const val NO_CHANGES = "There is nothing to compare: the head branch has no changes on top of the base."
    const val COMPARE_FAILED =
        "The mirror could not compare these branches yet. A branch pushed a moment ago can take a minute to appear."
    const val PASTE_PATCH = "Paste a patch instead"
    const val USE_BRANCHES = "Compare branches instead"
    const val TITLE_REQUIRED = "Add a title."
    const val REMOTE_NOTE = "Sent straight to the other instance and reviewed there. Both copies must share history."
    const val DESCRIPTION_HINT = "Leave a comment. Markdown is supported."

    fun targetLabel(fork: RemoteFork?): String = fork?.displayName ?: THIS_REPOSITORY

    fun confirmLabel(fork: RemoteFork?): String = if (fork == null) "Create pull request" else "Send pull request"

    fun ableToMerge(comparison: RepoComparison): String {
        val count = comparison.commitCount
        return if (count > 0) "$count ${if (count == 1) "commit" else "commits"} to merge." else "Ready to send."
    }

    fun sentLocal(): String = PENDING_NOTE

    fun sentRemote(fork: RemoteFork, number: Int): String =
        if (number > 0) "Sent to ${fork.displayName} as pull request #$number." else "Sent to ${fork.displayName}. It is reviewed there."

    fun remoteError(error: Throwable, fork: RemoteFork): String {
        val code = (error as? DartException)?.message ?: dartErrorText(error)
        return when (code) {
            "remote_unreachable" ->
                "Nothing was delivered: ${fork.host} could not be reached, or it runs a ForkMesh version that does not take pull requests from other instances yet."
            "bad_signature" -> "${fork.host} could not verify the signature, so nothing was opened there."
            "not_found" -> "${fork.owner}/${fork.name} was not found on ${fork.host}."
            "rate_limited", "inbox_full", "quota_exceeded" -> "${fork.host} is not taking more pull requests right now ($code). Try again later."
            else -> "Could not send the pull request to ${fork.displayName} ($code)."
        }
    }

    fun defaultTitle(head: String): String {
        val leaf = head.split("/").lastOrNull { it.isNotEmpty() } ?: head
        val words = leaf.replace("-", " ").replace("_", " ").trim()
        if (words.isEmpty()) return ""
        return words.substring(0, 1).uppercase() + words.substring(1)
    }

    fun ready(title: String, patch: String): Boolean = title.trim().isNotEmpty() && patch.trim().isNotEmpty()

    fun canSubmit(title: String, patch: String, pasting: Boolean, target: RemoteFork?): Boolean =
        ready(title, patch) && (!pasting || target == null)

    fun headChoices(branches: List<String>, base: String): List<String> = branches.filter { it != base }

    fun compareProblem(base: String, head: String): String? = when {
        base.trim().isEmpty() || head.trim().isEmpty() -> PICK_BRANCHES
        base.trim() == head.trim() -> SAME_BRANCH
        else -> null
    }

    fun compared(result: RepoComparison, head: String, title: String): PullCompareResult =
        if (result.patch.trim().isEmpty()) {
            PullCompareResult(null, NO_CHANGES, title)
        } else {
            PullCompareResult(result, "", if (title.trim().isEmpty()) defaultTitle(head.trim()) else title)
        }

    fun description(body: String): String = body.replace(Regex("[\\r\\n]+$"), "")

    fun submitProblem(title: String, patch: String, pasting: Boolean, target: RemoteFork?, host: String): String? = when {
        !ready(title, patch) -> if (title.trim().isEmpty()) TITLE_REQUIRED else PICK_BRANCHES
        target != null && (pasting || host.isEmpty()) -> PICK_BRANCHES
        else -> null
    }
}
