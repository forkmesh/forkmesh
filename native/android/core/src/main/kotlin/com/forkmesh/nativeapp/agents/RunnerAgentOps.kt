package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.agents.ops.AgentDeletePart
import com.forkmesh.nativeapp.agents.ops.AgentDeleteReport
import com.forkmesh.nativeapp.agents.ops.AgentDiff
import com.forkmesh.nativeapp.agents.ops.MergeOutcome
import com.forkmesh.nativeapp.agents.ops.MergeStats
import com.forkmesh.nativeapp.agents.ops.OpsState
import com.forkmesh.nativeapp.agents.ops.reviewSections
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.code.DiffHealthGit
import com.forkmesh.nativeapp.code.DiffReviewModel
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import com.forkmesh.nativeapp.model.RepoComparison
import com.forkmesh.nativeapp.security.DeployApproval
import com.forkmesh.nativeapp.security.DeployApprover
import java.security.SecureRandom
import java.util.Base64

data class RunnerDiffStats(
    val files: Int = -1,
    val added: Int = -1,
    val removed: Int = -1,
    val ahead: Int = -1,
    val behind: Int = -1,
    val conflicted: Boolean = false,
    val known: Boolean = false,
) {
    companion object {
        fun fromJson(json: JsonMap): RunnerDiffStats {
            if (json.isEmpty() || json["known"] == false) return RunnerDiffStats()
            return RunnerDiffStats(
                files = json.int("files", -1),
                added = json.int("added", -1),
                removed = json.int("removed", -1),
                ahead = json.int("ahead", -1),
                behind = json.int("behind", -1),
                conflicted = json.bool("conflicted"),
                known = true,
            )
        }
    }
}

data class RunnerMergeState(
    val state: String = "",
    val base: String = "",
    val mergeOid: String = "",
    val mergedAtMs: Long = 0,
    val branchDeleted: Boolean = false,
    val cleanupPending: List<String> = emptyList(),
    val conflicts: List<String> = emptyList(),
    val message: String = "",
) {
    val isMerged: Boolean get() = state == "merged"
    val isConflict: Boolean get() = state == "conflict"

    companion object {
        fun fromJson(json: JsonMap): RunnerMergeState = RunnerMergeState(
            state = json.str("state").trim().lowercase(),
            base = json.str("base"),
            mergeOid = json.str("mergeOid"),
            mergedAtMs = launchMs(json["mergedAt"]),
            branchDeleted = json.bool("branchDeleted"),
            cleanupPending = json.strings("cleanupPending").filter { it.isNotEmpty() },
            conflicts = json.strings("conflicts").filter { it.isNotEmpty() },
            message = json.str("message"),
        )
    }
}

data class RunnerSessionRef(val owner: String, val repo: String, val sessionId: String, val orgScope: Boolean = false)

data class RunnerAgentDiff(
    val diff: AgentDiff,
    val page: Int = 0,
    val pageSize: Int = RunnerAgentOps.PAGE_SIZE,
    val pageCount: Int = 1,
    val commits: Int = 0,
    val ahead: Int = 0,
    val behind: Int = 0,
    val clean: Boolean? = null,
    val conflicts: List<String> = emptyList(),
) {
    val hasMore: Boolean get() = page + 1 < pageCount

    val loadedFiles: Int get() = minOf(diff.files.size, (page + 1) * pageSize)

    val syncNote: String
        get() = when {
            clean == false -> "Conflicts with ${diff.base.ifEmpty { "the base" }}: ${conflicts.take(5).joinToString(", ")}"
            behind > 0 -> "${diff.base.ifEmpty { "The base" }} has $behind new commit${if (behind == 1) "" else "s"} since this branch was cut."
            else -> ""
        }

    fun appending(next: RunnerAgentDiff): RunnerAgentDiff {
        val joined = listOf(diff.committed, next.diff.committed).filter { it.isNotEmpty() }
            .joinToString("") { if (it.endsWith("\n")) it else it + "\n" }
        return next.copy(diff = next.diff.copy(committed = joined))
    }

    companion object {
        fun fromJson(json: JsonMap): RunnerAgentDiff {
            val base = AgentDiff.from(json)
            val pageCount = maxOf(1, json.int("pageCount", 1))
            val page = json.int("page")
            val clean = json["clean"] as? Boolean
            return RunnerAgentDiff(
                diff = base.copy(truncated = page + 1 < pageCount),
                page = page,
                pageSize = maxOf(1, json.int("pageSize", RunnerAgentOps.PAGE_SIZE)),
                pageCount = pageCount,
                commits = json.int("commits"),
                ahead = json.int("ahead"),
                behind = json.int("behind"),
                clean = clean,
                conflicts = json.strings("conflicts").filter { it.isNotEmpty() },
            )
        }
    }
}

data class RunnerMergeResult(
    val status: String,
    val branch: String = "",
    val base: String = "",
    val mergeOid: String = "",
    val branchDeleted: Boolean = false,
    val cleanupPending: List<String> = emptyList(),
    val stats: MergeStats? = null,
    val message: String = "",
) {
    val outcome: MergeOutcome get() = MergeOutcome(branch, stats)

    val summary: String
        get() {
            if (status == "cleaned") {
                return message.ifEmpty { if (cleanupPending.isEmpty()) "Removed $branch." else "Removed $branch; ${cleanupPending.joinToString(", ")} still to catch up." }
            }
            if (status == "updated") return message.ifEmpty { "Updated $branch from $base." }
            if (status == "already") return message.ifEmpty { "$branch is already up to date with $base." }
            val head = message.ifEmpty {
                if (branchDeleted) "Merged $branch into $base and cleaned up." else "Merged $branch into $base."
            }
            val numbers = stats?.summary.orEmpty()
            return if (numbers.isEmpty()) head else "$head $numbers."
        }

    companion object {
        fun fromJson(json: JsonMap): RunnerMergeResult = RunnerMergeResult(
            status = json.str("status"),
            branch = json.str("branch"),
            base = json.str("base"),
            mergeOid = json.str("mergeOid"),
            branchDeleted = json.bool("branchDeleted"),
            cleanupPending = json.strings("cleanupPending").filter { it.isNotEmpty() },
            stats = MergeStats.from(json),
            message = json.str("message"),
        )
    }
}

fun interface RunnerTaskCompleter {
    fun complete(taskId: String, note: String)
}

object RunnerTaskLink {
    private val KEY = Regex("^task:([0-9a-fA-F]{32})$")
    private val TAG = Regex("^\\[task:([0-9a-fA-F]{32})]")
    const val COMPLETED = "Finished the linked task and sent it to review."

    fun taskId(taskKey: String, title: String = "", firstPrompt: String = ""): String {
        KEY.find(taskKey.trim())?.let { return it.groupValues[1].lowercase() }
        for (text in listOf(title, firstPrompt)) {
            TAG.find(text.trimStart())?.let { return it.groupValues[1].lowercase() }
        }
        return ""
    }

    fun taskId(session: AgentLaunchSession): String = taskId(session.taskKey, session.title, session.firstPrompt)

    fun note(branch: String, base: String, mergeOid: String, message: String): String {
        val commit = mergeOid.trim().take(7)
        val head = "Merged $branch into $base" + if (commit.isEmpty()) "" else " ($commit)"
        val tail = message.trim()
        return if (tail.isEmpty()) "$head." else "$head. $tail"
    }

    fun failed(reason: String): String = "Merged, but the linked task was not updated: $reason"
}

data class RunnerPullLink(val number: Int, val branch: String = "", val message: String = "")

data class RunnerPullOpened(val number: Int, val message: String, val linked: Boolean = true)

data class RunnerPullDraft(val title: String, val base: String, val head: String, val description: String) {
    companion object {
        const val SUMMARY_CHARS = 4000
        const val TITLE_CHARS = 160

        fun description(runner: String, targetNode: String, branch: String, lastReply: String = ""): String {
            val where = when {
                runner.trim().isNotEmpty() -> "the ${runner.trim()} runner"
                targetNode.trim().isNotEmpty() -> targetNode.trim()
                else -> "a ForkMesh node"
            }
            val opening = "Opened from the agent run on $where ($branch)."
            val summary = lastReply.trim().take(SUMMARY_CHARS).trim()
            return if (summary.isEmpty()) opening else "$opening\n\n$summary"
        }

        fun lastReply(session: AgentLaunchSession): String =
            session.history.lastOrNull { it.role.lowercase() == "assistant" && it.text.isNotBlank() }?.text.orEmpty()

        fun from(session: AgentLaunchSession, defaultBranch: String): RunnerPullDraft {
            val branch = session.branchName.trim()
            val title = session.title.trim().take(TITLE_CHARS).trim().ifEmpty { branch }
            val base = session.baseBranch.trim().ifEmpty { defaultBranch.trim() }.ifEmpty { "main" }
            return RunnerPullDraft(title, base, branch, description(session.runner, session.targetNode, branch, lastReply(session)))
        }
    }
}

interface RunnerPullInbox {
    fun defaultBranch(owner: String, repo: String): String

    fun compare(owner: String, repo: String, base: String, head: String): RepoComparison

    fun submit(owner: String, repo: String, draft: RunnerPullDraft, comparison: RepoComparison): Int
}

class RunnerOpsException(
    override val message: String,
    val code: String = "",
    val status: Int = 0,
    val conflicts: List<String> = emptyList(),
) : RuntimeException(message) {
    override fun toString(): String = message
}

object RunnerBranchName {
    fun slug(title: String, words: Int = 4, limit: Int = 30): String {
        val parts = title.lowercase().split(Regex("[^a-z0-9]+")).filter { it.isNotEmpty() }.take(words)
        var slug = ""
        for (part in parts) {
            val next = if (slug.isEmpty()) part else "$slug-$part"
            if (next.length > limit) {
                if (slug.isEmpty()) slug = part.take(limit)
                break
            }
            slug = next
        }
        return slug.trim('-')
    }
}

open class RunnerAgentOps(
    private val api: AgentQueueApi,
    private val sleep: (Long) -> Unit = { Thread.sleep(it) },
    private val requestId: () -> String = ::newRequestId,
    private val maxPolls: Int = 20,
    private val pollMs: Long = 1000,
    private val runningAttempts: Int = 10,
    private val runningWaitMs: Long = 2000,
    private val approver: DeployApprover? = null,
) {
    companion object {
        const val PAGE_SIZE = 25
        const val CONTINUE_PROMPT = "Continue where you left off."
        private val STALE = setOf("stale_base", "stale_head")
        private val RUN_DELETE_KEEPS_GOING = setOf("no_branch", "forbidden")
        private val OID = Regex("^[0-9a-f]{40}(?:[0-9a-f]{24})?$")
        const val MAX_PULL_BYTES = 4 * 1024 * 1024

        fun noChanges(branch: String, base: String): String =
            "$branch has no changes against $base, so there is nothing to open a pull request for."

        fun notOnForkMesh(branch: String): String = "$branch is not on ForkMesh yet, so no pull request was opened."

        fun tooLarge(branch: String): String = "$branch is too large to open as a pull request from a phone. Open it from a desktop."

        fun linkFailed(number: Int, reason: String): String =
            "Opened pull request #$number, but the run could not be linked to it: $reason"
        private val random = SecureRandom()

        fun newRequestId(): String {
            val bytes = ByteArray(18)
            random.nextBytes(bytes)
            return Base64.getUrlEncoder().withoutPadding().encodeToString(bytes)
        }

        fun updateConflict(branch: String, base: String, conflicts: List<String>): String {
            val name = branch.ifEmpty { "The branch" }
            val files = if (conflicts.isEmpty()) "" else ": ${conflicts.take(5).joinToString(", ")}"
            return "$name conflicts with ${base.ifEmpty { "its base" }}$files. Nothing was changed; fix the conflicts with the agent, then update again."
        }

        fun describe(code: String, status: Int, serverMessage: String, branch: String = "", conflicts: List<String> = emptyList()): String {
            val name = branch.ifEmpty { "The branch" }
            return when (code) {
                "merge_conflict" -> {
                    val files = if (conflicts.isEmpty()) "" else ": ${conflicts.take(5).joinToString(", ")}"
                    "$name does not merge cleanly$files. Nothing was changed; continue the agent to resolve it, then merge again."
                }
                "stale_base", "stale_head" -> "The branches moved while merging. Refresh and try again."
                "already_merged" -> "Already merged. Clean up removes the branch."
                "not_merged" -> "This branch is not merged yet, so it was kept."
                "session_running" -> "Stop the agent or let it finish before merging."
                "no_branch" -> "The agent has not pushed a branch yet."
                "branch_not_found" -> "The branch is not on the cores yet."
                "diff_too_large" -> "This branch changes more than a phone-sized diff can carry."
                "forbidden" -> "Only an owner who can merge this repository can merge agent branches."
                "merge_node_unavailable", "mirror_unavailable" -> "No core can do this right now. Try again in a minute."
                "merge_status_pending" -> "The core is still working on it. Pull to refresh in a moment."
                "pull_exists" -> serverMessage.ifEmpty { "$name already has a pull request." }
                "invalid_pull" -> "The pull request number is invalid."
                else -> serverMessage.ifEmpty {
                    when {
                        status == 503 -> "No core can do this right now. Try again in a minute."
                        code.isNotEmpty() -> code.replace('_', ' ')
                        else -> "The relay refused the request (HTTP $status)."
                    }
                }
            }
        }
    }

    open fun diff(ref: RunnerSessionRef, page: Int = 0, pageSize: Int = PAGE_SIZE): RunnerAgentDiff {
        try {
            return RunnerAgentDiff.fromJson(api.runnerDiff(ref.owner, ref.repo, ref.sessionId, ref.orgScope, page, pageSize))
        } catch (refusal: AgentLaunchException) {
            throw RunnerOpsException(describe(refusal.code, refusal.status, refusal.message), refusal.code, refusal.status)
        }
    }

    open fun merge(ref: RunnerSessionRef, cleanup: Boolean = true, branch: String = ""): RunnerMergeResult =
        withStaleRetry { run(ref, mergeBody(ref, cleanup), branch) }

    private fun mergeBody(ref: RunnerSessionRef, cleanup: Boolean): Map<String, Any?> {
        val id = requestId().ifEmpty { newRequestId() }
        val body = linkedMapOf<String, Any?>("action" to "merge", "cleanup" to cleanup, "requestId" to id)
        approver?.approve(
            DeployApproval.BRANCH,
            api.routeOwner(ref.owner, ref.repo, ref.orgScope),
            ref.repo.trim(),
            ref.sessionId,
            "",
            "",
            id,
        )?.let { body["deployApproval"] = it }
        return body
    }

    open fun update(ref: RunnerSessionRef, branch: String = "", base: String = ""): RunnerMergeResult {
        try {
            return withStaleRetry { run(ref, mapOf("action" to "update", "requestId" to requestId()), branch) }
        } catch (refusal: RunnerOpsException) {
            if (refusal.code != "merge_conflict") throw refusal
            throw RunnerOpsException(updateConflict(branch, base, refusal.conflicts), refusal.code, refusal.status, refusal.conflicts)
        }
    }

    open fun completeLinkedTask(ref: RunnerSessionRef, result: RunnerMergeResult, tasks: RunnerTaskCompleter): String {
        val session = try {
            api.session(ref.owner, ref.repo, ref.sessionId, ref.orgScope)
        } catch (error: Exception) {
            return RunnerTaskLink.failed(error.message ?: error.toString())
        }
        val taskId = RunnerTaskLink.taskId(session)
        if (taskId.isEmpty()) return ""
        val note = RunnerTaskLink.note(
            result.branch.ifEmpty { session.branchName },
            result.base.ifEmpty { session.baseBranch },
            result.mergeOid,
            result.message,
        )
        return try {
            tasks.complete(taskId, note)
            RunnerTaskLink.COMPLETED
        } catch (error: Exception) {
            RunnerTaskLink.failed(error.message ?: error.toString())
        }
    }

    open fun cleanup(ref: RunnerSessionRef, branch: String = ""): RunnerMergeResult =
        withStaleRetry { run(ref, mapOf("action" to "cleanup", "requestId" to requestId()), branch) }

    open fun discard(ref: RunnerSessionRef, branch: String = ""): RunnerMergeResult =
        withStaleRetry { run(ref, mapOf("action" to "cleanup", "discard" to true, "requestId" to requestId()), branch) }

    open fun delete(
        ref: RunnerSessionRef,
        branch: String,
        active: Boolean = false,
        pull: Int = 0,
        merged: Boolean = false,
    ): AgentDeleteReport {
        if (active) {
            try {
                api.stop(ref.owner, ref.repo, ref.sessionId, ref.orgScope)
            } catch (_: Exception) {
            }
        }
        val branchPart = if (branch.isNotEmpty()) discardBeforeDelete(ref, branch) else null
        api.cancel(ref.owner, ref.repo, ref.sessionId, ref.orgScope)
        val parts = mutableListOf(AgentDeletePart("agent", status = "deleted"))
        if (branchPart != null) parts += branchPart
        if (pull > 0) parts += AgentDeletePart("pull", pull.toString(), "kept", if (merged) "merged" else "relay")
        return AgentDeleteReport(parts, known = listOf("branch", "pull"))
    }

    private fun discardBeforeDelete(ref: RunnerSessionRef, branch: String): AgentDeletePart {
        for (attempt in 1..maxOf(1, runningAttempts)) {
            try {
                val result = discard(ref, branch)
                val gone = result.message.endsWith("already gone.")
                return AgentDeletePart("branch", branch, if (gone) "absent" else "deleted")
            } catch (refusal: RunnerOpsException) {
                when (refusal.code) {
                    "no_branch" -> return AgentDeletePart("branch", branch, "absent")
                    in RUN_DELETE_KEEPS_GOING -> return AgentDeletePart("branch", branch, "kept", "forbidden")
                    "session_running" -> if (attempt < runningAttempts) sleep(runningWaitMs)
                    else -> throw refusal
                }
            }
        }
        return AgentDeletePart("branch", branch, "kept", "running")
    }

    open fun linkPull(ref: RunnerSessionRef, number: Int, branch: String = ""): RunnerPullLink {
        val (status, json) = try {
            api.sessionAction(ref.owner, ref.repo, ref.sessionId, ref.orgScope, mapOf("action" to "link-pull", "number" to number))
        } catch (refusal: AgentLaunchException) {
            throw RunnerOpsException(refusal.message, refusal.code, refusal.status)
        }
        if (status in 200..299 && json["ok"] != false) {
            val linked = json.int("prNumber", number)
            return RunnerPullLink(linked, json.str("branch").ifEmpty { branch }, json.str("message").ifEmpty { "Opened pull request #$linked." })
        }
        val code = json.str("error")
        throw RunnerOpsException(describe(code, status, json.str("message"), branch), code, status)
    }

    open fun openPull(ref: RunnerSessionRef, inbox: RunnerPullInbox): RunnerPullOpened {
        val session = try {
            api.session(ref.owner, ref.repo, ref.sessionId, ref.orgScope)
        } catch (refusal: AgentLaunchException) {
            throw RunnerOpsException(describe(refusal.code, refusal.status, refusal.message), refusal.code, refusal.status)
        }
        if (session.prNumber > 0) {
            return RunnerPullOpened(session.prNumber, "This run already has pull request #${session.prNumber}.")
        }
        val branch = session.branchName.trim()
        if (branch.isEmpty()) throw RunnerOpsException(describe("no_branch", 409, ""), "no_branch", 409)
        val defaultBranch = if (session.baseBranch.isBlank()) inbox.defaultBranch(ref.owner, ref.repo) else ""
        val draft = RunnerPullDraft.from(session, defaultBranch)
        val comparison = try {
            inbox.compare(ref.owner, ref.repo, draft.base, draft.head)
        } catch (_: Exception) {
            throw RunnerOpsException(notOnForkMesh(branch), "comparison_unavailable")
        }
        if (!OID.matches(comparison.baseOid.lowercase()) || !OID.matches(comparison.headOid.lowercase())) {
            throw RunnerOpsException(notOnForkMesh(branch), "comparison_unavailable")
        }
        if (comparison.patch.isBlank()) throw RunnerOpsException(noChanges(branch, draft.base), "no_changes")
        val bytes = comparison.patch.toByteArray(Charsets.UTF_8).size + comparison.commits.toByteArray(Charsets.UTF_8).size
        if (bytes > MAX_PULL_BYTES) throw RunnerOpsException(tooLarge(branch), "pull_too_large")
        val number = try {
            inbox.submit(ref.owner, ref.repo, draft, comparison)
        } catch (error: Exception) {
            throw RunnerOpsException("The pull request could not be opened: ${error.message ?: error}", "pull_failed")
        }
        if (number <= 0) {
            throw RunnerOpsException("The pull request was sent but has no number yet. Refresh in a moment.", "pull_pending")
        }
        return try {
            val link = linkPull(ref, number, branch)
            RunnerPullOpened(number, link.message)
        } catch (refusal: Exception) {
            RunnerPullOpened(number, linkFailed(number, refusal.message ?: refusal.toString()), linked = false)
        }
    }

    open fun continueRun(ref: RunnerSessionRef): AgentQueueResult =
        api.steer(ref.owner, ref.repo, ref.sessionId, CONTINUE_PROMPT, ref.orgScope)

    private fun withStaleRetry(block: () -> RunnerMergeResult): RunnerMergeResult {
        try {
            return block()
        } catch (stale: RunnerOpsException) {
            if (stale.code !in STALE) throw stale
            return block()
        }
    }

    private fun run(ref: RunnerSessionRef, body: Map<String, Any?>, branch: String): RunnerMergeResult {
        for (attempt in 0 until maxPolls) {
            val (status, json) = try {
                api.sessionAction(ref.owner, ref.repo, ref.sessionId, ref.orgScope, body)
            } catch (refusal: AgentLaunchException) {
                throw RunnerOpsException(refusal.message, refusal.code, refusal.status)
            }
            val state = json.str("status")
            if (status == 202 || (status in 200..299 && state == "processing")) {
                if (attempt < maxPolls - 1) sleep(pollMs)
                continue
            }
            if (status in 200..299) return RunnerMergeResult.fromJson(json)
            val code = json.str("error")
            val conflicts = json.strings("conflicts").filter { it.isNotEmpty() }
            throw RunnerOpsException(describe(code, status, json.str("message"), branch, conflicts), code, status, conflicts)
        }
        throw RunnerOpsException(describe("merge_status_pending", 202, ""), "merge_status_pending", 202)
    }
}

class RunnerDiffModel(private val ops: RunnerAgentOps, val ref: RunnerSessionRef, private val store: PrefStore? = null) : OpsState() {
    @Volatile
    var diff: RunnerAgentDiff? = null
        private set

    @Volatile
    var review: DiffReviewModel? = null
        private set

    @Volatile
    var loading = false
        private set

    @Volatile
    var errorText = ""
        private set

    fun load() = fetch(0)

    fun loadMore() {
        val current = diff ?: return load()
        if (current.hasMore) fetch(current.page + 1)
    }

    private fun fetch(page: Int) {
        if (loading) return
        loading = true
        changed()
        try {
            val loaded = ops.diff(ref, page)
            val shown = if (page > 0) diff?.appending(loaded) ?: loaded else loaded
            review = DiffReviewModel(
                reviewSections(shown.diff),
                shown.diff.branch.ifEmpty { "agent/${ref.sessionId}" },
                git = if (shown.diff.merged) DiffHealthGit.Merged else DiffHealthGit.Unknown,
                store = store,
            )
            diff = shown
            errorText = ""
        } catch (error: Exception) {
            errorText = error.message ?: error.toString()
        } finally {
            loading = false
            changed()
        }
    }
}

class RunnerMergeModel(
    private val ops: RunnerAgentOps,
    val ref: RunnerSessionRef,
    val branch: String,
    val cleanupOnly: Boolean = false,
    val updateOnly: Boolean = false,
    val base: String = "",
    private val tasks: RunnerTaskCompleter? = null,
) : OpsState() {
    @Volatile
    var taskNote = ""
        private set

    @Volatile
    var running = false
        private set

    @Volatile
    var errorText = ""
        private set

    @Volatile
    var conflicts: List<String> = emptyList()
        private set

    @Volatile
    var conflicted = false
        private set

    @Volatile
    var result: RunnerMergeResult? = null
        private set

    val canRun: Boolean get() = !running && result == null

    fun run(): Boolean {
        if (!canRun) return false
        running = true
        errorText = ""
        changed()
        try {
            val done = when {
                cleanupOnly -> ops.cleanup(ref, branch)
                updateOnly -> ops.update(ref, branch, base)
                else -> ops.merge(ref, cleanup = true, branch = branch)
            }
            conflicts = emptyList()
            conflicted = false
            if (!cleanupOnly && !updateOnly && done.status == "merged" && tasks != null) {
                taskNote = try {
                    ops.completeLinkedTask(ref, done, tasks)
                } catch (error: Exception) {
                    RunnerTaskLink.failed(error.message ?: error.toString())
                }
            }
            result = done
            return true
        } catch (refusal: RunnerOpsException) {
            errorText = refusal.message
            conflicts = refusal.conflicts
            conflicted = refusal.code == "merge_conflict"
            return false
        } catch (error: Exception) {
            errorText = error.message ?: error.toString()
            return false
        } finally {
            running = false
            changed()
        }
    }
}
