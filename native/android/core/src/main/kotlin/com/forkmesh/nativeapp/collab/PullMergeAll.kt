package com.forkmesh.nativeapp.collab

import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.str

data class PullMergeAllConfirmation(val token: String, val summary: String, val expiresInMs: Int) {
    companion object {
        fun fromJson(j: Map<String, Any?>) =
            PullMergeAllConfirmation(j.str("token"), j.str("summary"), j.int("expiresInMs", 60_000))
    }
}

data class PullMergeAllResult(val message: String, val sessionId: Int, val pulls: Int, val skipped: List<String>) {
    companion object {
        fun fromJson(j: Map<String, Any?>) = PullMergeAllResult(
            j.str("message"),
            j.int("sessionId", 0),
            j.int("pulls", 0),
            (j["skipped"] as? List<*>).orEmpty().mapNotNull { it as? String }.take(PullMergeAllRules.MAX_SKIPPED),
        )
    }
}

object PullMergeAllRules {
    const val ACTION = "merge-all-ai"
    const val TITLE = "Merge all with AI"
    const val MAX_SKIPPED = 50
    const val UNPAIRED = "Pair this phone with your desktop to merge every pull request with AI."

    fun params(owner: String, name: String): Map<String, Any?> =
        linkedMapOf("owner" to owner, "name" to name, "action" to ACTION)

    fun errorMessage(code: String): String = when (code) {
        "unsupported", "unknown_method" -> "This desktop cannot merge every pull request with AI yet. Update ForkMesh on the desktop."
        "not_owner" -> "Only the desktop's owner can merge every pull request with AI."
        "not_repo_owner" -> "Only the repository's owner can merge every pull request at once."
        "unknown_repo" -> "The desktop does not have that repository."
        "read_only" -> "This repository is read-only on the desktop."
        "busy" -> "An agent is already merging this repository's pull requests."
        "no_open_pulls" -> "No open pull requests to merge."
        "nothing_mergeable" -> "None of the open pull requests can be merged by an agent on the desktop."
        "launch_failed" -> "The desktop could not start the merge agent."
        "bad_request" -> "The desktop could not read that request."
        "missing", "expired", "mismatch", "confirmation_missing", "confirmation_expired", "confirmation_mismatch" ->
            "The confirmation expired. Try again."
        "" -> "The desktop refused."
        else -> code.replace('_', ' ')
    }
}

class PullMergeAllRemote(private val call: DesktopFamilyCall) {
    fun prepare(owner: String, name: String): PullMergeAllConfirmation {
        val confirmation = PullMergeAllConfirmation.fromJson(call("prepare", PullMergeAllRules.params(owner, name), 20_000))
        if (confirmation.token.isEmpty()) throw LocalControlException("confirmation_missing")
        return confirmation
    }

    fun run(owner: String, name: String, token: String): PullMergeAllResult {
        val params = LinkedHashMap(PullMergeAllRules.params(owner, name))
        params["token"] = token
        return PullMergeAllResult.fromJson(call("run", params, 90_000))
    }

    companion object {
        fun message(error: Throwable): String =
            if (error is LocalControlException) PullMergeAllRules.errorMessage(error.code) else (error.message ?: "$error")
    }
}
