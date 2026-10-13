package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.api.ApiJsonResult
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.DartUri
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import com.forkmesh.nativeapp.model.RepoBranch

enum class BranchOpKind(val wire: String) {
    Merge("merge"),
    Update("update"),
    Delete("delete"),
}

data class BranchOpResume(val requestId: String, val node: String, val pinBase: String, val pinOid: String = "") {
    companion object {
        fun from(json: Map<String, Any?>): BranchOpResume? {
            val requestId = json.str("requestId").trim()
            val node = json.str("node").trim()
            if (requestId.isEmpty() || node.isEmpty()) return null
            return BranchOpResume(requestId, node, json.str("pinBase").trim(), json.str("pinOid").trim())
        }
    }
}

data class BranchOpRequest(
    val op: BranchOpKind,
    val branch: String,
    val expectedOid: String,
    val defaultBranch: String,
    val base: String = "",
    val baseOid: String = "",
    val deleteBranch: Boolean = false,
) {
    val target: String get() = base.ifEmpty { defaultBranch }

    fun body(resume: BranchOpResume? = null): Map<String, Any?> {
        val body = linkedMapOf<String, Any?>(
            "op" to op.wire,
            "branch" to branch,
            "expectedOid" to expectedOid,
            "defaultBranch" to defaultBranch,
        )
        when (op) {
            BranchOpKind.Merge -> {
                body["base"] = target
                body["baseOid"] = baseOid
                body["deleteBranch"] = deleteBranch
            }
            BranchOpKind.Update -> if (base.isNotEmpty()) body["base"] = base
            BranchOpKind.Delete -> Unit
        }
        if (resume != null) {
            body["requestId"] = resume.requestId
            body["node"] = resume.node
            body["pinBase"] = resume.pinBase
            if (resume.pinOid.isNotEmpty()) body["pinOid"] = resume.pinOid
        }
        return body
    }

    companion object {
        fun merge(branch: RepoBranch, default: RepoBranch, deleteBranch: Boolean = false): BranchOpRequest =
            BranchOpRequest(BranchOpKind.Merge, branch.name, branch.sha.lowercase(), default.name, default.name, default.sha.lowercase(), deleteBranch)

        fun update(branch: RepoBranch, defaultBranch: String): BranchOpRequest =
            BranchOpRequest(BranchOpKind.Update, branch.name, branch.sha.lowercase(), defaultBranch)

        fun delete(branch: RepoBranch, defaultBranch: String): BranchOpRequest =
            BranchOpRequest(BranchOpKind.Delete, branch.name, branch.sha.lowercase(), defaultBranch)
    }
}

enum class BranchOpState { Processing, Done, Queued, Failed }

data class BranchOpAnswer(
    val httpStatus: Int,
    val state: BranchOpState,
    val status: String = "",
    val error: String = "",
    val message: String = "",
    val branch: String = "",
    val base: String = "",
    val mergeOid: String = "",
    val branchDeleted: Boolean = false,
    val cleanupPending: List<String> = emptyList(),
    val conflicts: List<String> = emptyList(),
    val resume: BranchOpResume? = null,
    val alreadyWaiting: Boolean = false,
) {
    val isConflict: Boolean get() = error == "merge_conflict"
    val retryable: Boolean get() = httpStatus == 503 || error in BranchOpWording.RETRYABLE
    val landedOnBase: Boolean get() = state == BranchOpState.Done && status == "merged"

    companion object {
        fun parse(httpStatus: Int, json: Map<String, Any?>): BranchOpAnswer {
            val status = json.str("status").trim().lowercase()
            val error = json.str("error").trim()
            val executed = json.bool("executed")
            val ok = httpStatus in 200..299 && json["ok"] != false
            val state = when {
                ok && (httpStatus == 202 || status == "processing") -> BranchOpState.Processing
                ok && executed -> BranchOpState.Done
                ok && json.containsKey("queued") -> BranchOpState.Queued
                ok -> BranchOpState.Done
                else -> BranchOpState.Failed
            }
            return BranchOpAnswer(
                httpStatus = httpStatus,
                state = state,
                status = status,
                error = error,
                message = json.str("message").trim(),
                branch = json.str("branch"),
                base = json.str("base"),
                mergeOid = json.str("mergeOid").trim().lowercase(),
                branchDeleted = json.bool("branchDeleted"),
                cleanupPending = json.strings("cleanupPending").filter { it.isNotEmpty() },
                conflicts = json.strings("conflicts").filter { it.isNotEmpty() },
                resume = BranchOpResume.from(json),
                alreadyWaiting = state == BranchOpState.Queued && json["queued"] == false,
            )
        }

        fun unreachable(error: Throwable): BranchOpAnswer =
            BranchOpAnswer(0, BranchOpState.Failed, error = "unreachable", message = "Could not reach ForkMesh: ${error.message ?: error}")
    }
}

data class BranchOpResult(val request: BranchOpRequest, val answer: BranchOpAnswer, val resume: BranchOpResume?, val stillWorking: Boolean = false) {
    val outcome: String get() = BranchOpWording.outcome(request, answer, stillWorking)
    val failed: Boolean get() = !stillWorking && answer.state == BranchOpState.Failed
    val canResume: Boolean get() = stillWorking || (failed && answer.retryable && resume != null)
    val offersActions: Boolean get() = !stillWorking && answer.landedOnBase && request.target == request.defaultBranch
}

object BranchOpWording {
    val RETRYABLE = setOf("merge_status_pending", "merge_node_unavailable")
    const val OWNER_ONLY = "Only the repository owner can manage branches."
    const val STILL_WORKING = "The core is still working on it. Check again to ask for the same request's result."

    fun label(op: BranchOpKind, defaultBranch: String, deleteBranch: Boolean = false): String = when (op) {
        BranchOpKind.Merge -> if (deleteBranch) "Merge & delete" else "Merge into $defaultBranch"
        BranchOpKind.Update -> "Update from $defaultBranch"
        BranchOpKind.Delete -> "Delete"
    }

    fun progress(request: BranchOpRequest): String = when (request.op) {
        BranchOpKind.Merge -> "Merging ${request.branch} into ${request.target}…"
        BranchOpKind.Update -> "Updating ${request.branch} from ${request.target}…"
        BranchOpKind.Delete -> "Deleting ${request.branch}…"
    }

    fun confirmTitle(request: BranchOpRequest): String = when (request.op) {
        BranchOpKind.Merge -> if (request.deleteBranch) "Merge & delete ${request.branch}?" else "Merge ${request.branch}?"
        BranchOpKind.Update -> "Update ${request.branch}?"
        BranchOpKind.Delete -> "Delete ${request.branch}?"
    }

    fun confirmBody(request: BranchOpRequest): String {
        val tip = request.expectedOid.take(8)
        return when (request.op) {
            BranchOpKind.Merge -> "Merges ${request.branch} ($tip) into ${request.target}" +
                (if (request.deleteBranch) " and deletes the branch afterwards." else ".") +
                " Push workflows on ${request.target} start when the merge lands."
            BranchOpKind.Update -> "Merges ${request.target} into ${request.branch} ($tip) so it has the latest changes."
            BranchOpKind.Delete -> "Deletes ${request.branch} at $tip. Commits that are on no other branch can only be recovered from a desktop that still has them."
        }
    }

    fun queued(request: BranchOpRequest, alreadyWaiting: Boolean): String {
        val what = when (request.op) {
            BranchOpKind.Merge -> "Merging ${request.branch} into ${request.target}"
            BranchOpKind.Update -> "Updating ${request.branch} from ${request.target}"
            BranchOpKind.Delete -> "Deleting ${request.branch}"
        }
        return if (alreadyWaiting) {
            "$what is already waiting; it runs when the owner's desktop is online."
        } else {
            "$what is queued; it runs when the owner's desktop is online."
        }
    }

    fun done(request: BranchOpRequest, answer: BranchOpAnswer): String {
        val branch = answer.branch.ifEmpty { request.branch }
        val base = answer.base.ifEmpty { request.target }
        val head = answer.message.ifEmpty {
            when (answer.status) {
                "merged" -> if (answer.branchDeleted) "Merged $branch into $base and deleted it." else "Merged $branch into $base."
                "updated" -> "Updated $branch from $base."
                "deleted" -> "Deleted $branch."
                "already" -> if (request.op == BranchOpKind.Delete) "$branch is already gone." else "$branch is already up to date with $base."
                else -> "Done."
            }
        }
        if (answer.cleanupPending.isEmpty() || answer.message.isNotEmpty()) return head
        return "$head ${answer.cleanupPending.joinToString(", ")} still to catch up."
    }

    fun failure(request: BranchOpRequest, answer: BranchOpAnswer): String {
        val base = answer.base.ifEmpty { request.target }
        return when {
            answer.httpStatus == 403 || answer.error == "forbidden" -> OWNER_ONLY
            answer.httpStatus == 401 || answer.error == "invalid_session" -> "Sign in again to manage branches."
            answer.isConflict -> answer.message.ifEmpty {
                if (request.op == BranchOpKind.Update) {
                    "${request.branch} conflicts with $base. Nothing was changed."
                } else {
                    "${request.branch} does not merge cleanly into $base. Nothing was changed."
                }
            }
            answer.message.isNotEmpty() -> answer.message
            else -> when (answer.error) {
                "stale_base", "stale_head" -> "A branch moved since the list was loaded. Refresh and try again."
                "unsupported_branch" -> "The cores cannot change this branch."
                "invalid_request" -> "The request could not be resumed. Try again."
                "default_branch" -> "The default branch cannot be changed here."
                "invalid_commit" -> "The branch tips are not known. Refresh the branch list and try again."
                "invalid_branch" -> "That branch name is not valid."
                "queue_full" -> "Too many branch operations are already waiting for the owner's desktop."
                "merge_status_pending" -> "The result is not known yet. Check again."
                "merge_node_unavailable" -> "The core running this request is not answering yet. Check again."
                "" -> "ForkMesh refused the request (HTTP ${answer.httpStatus})."
                else -> answer.error.replace('_', ' ')
            }
        }
    }

    fun outcome(request: BranchOpRequest, answer: BranchOpAnswer, stillWorking: Boolean = false): String = when {
        stillWorking -> STILL_WORKING
        answer.state == BranchOpState.Queued -> queued(request, answer.alreadyWaiting)
        answer.state == BranchOpState.Done -> done(request, answer)
        answer.state == BranchOpState.Processing -> answer.message.ifEmpty { progress(request) }
        else -> failure(request, answer)
    }
}

class BranchOpsClient(
    private val post: (String, Map<String, Any?>) -> ApiJsonResult,
    private val sleep: (Long) -> Unit = { Thread.sleep(it) },
    private val maxPolls: Int = MAX_POLLS,
    private val pollMs: Long = POLL_MS,
) {
    companion object {
        const val MAX_POLLS = 90
        const val POLL_MS = 2000L

        fun path(owner: String, repo: String): String =
            "/api/repo/${DartUri.encodeComponent(owner.trim())}/${DartUri.encodeComponent(repo.trim())}/branches/ops"

        fun from(api: ApiService): BranchOpsClient = BranchOpsClient({ path, body ->
            api.jsonExchange("POST", path, body = body, timeoutMs = 30_000, extraHeaders = mapOf("Origin" to api.gitOrigin))
        })
    }

    fun run(
        owner: String,
        repo: String,
        request: BranchOpRequest,
        resume: BranchOpResume? = null,
        progress: (BranchOpAnswer) -> Unit = {},
    ): BranchOpResult {
        var pinned = resume
        var last = BranchOpAnswer(0, BranchOpState.Processing)
        val polls = maxOf(1, maxPolls)
        for (attempt in 0 until polls) {
            val answer = try {
                val reply = post(path(owner, repo), request.body(pinned))
                BranchOpAnswer.parse(reply.statusCode, reply.body)
            } catch (error: Exception) {
                BranchOpAnswer.unreachable(error)
            }
            last = answer
            val waiting = when {
                answer.state == BranchOpState.Processing -> {
                    pinned = answer.resume ?: pinned
                    progress(answer)
                    true
                }
                answer.state == BranchOpState.Failed && answer.retryable && pinned != null -> true
                else -> false
            }
            if (!waiting) return BranchOpResult(request, answer, pinned)
            if (attempt < polls - 1) sleep(pollMs)
        }
        return BranchOpResult(request, last, pinned, stillWorking = true)
    }
}
