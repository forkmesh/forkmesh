package com.forkmesh.nativeapp.ci

import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.model.modelInt

enum class PullChecksReason(val raw: String) {
    None(""),
    Author("author"),
    Workflows("workflows"),
    ;

    companion object {
        fun parse(raw: String): PullChecksReason = entries.firstOrNull { it.raw == raw.trim().lowercase() } ?: None
    }
}

data class PullChecksApproval(
    val required: Boolean = false,
    val reason: PullChecksReason = PullChecksReason.None,
    val author: String = "",
    val authorTrust: String = "",
    val approvedBy: String = "",
) {
    val reasonLabel: String
        get() = when (reason) {
            PullChecksReason.Author ->
                if (author.isEmpty()) "The author is not trusted to run code here." else "@$author is not trusted to run code here."
            PullChecksReason.Workflows -> "This pull request changes files under .forkmesh/, so a maintainer has to approve every run."
            PullChecksReason.None -> ""
        }

    val trustLabel: String
        get() = when (authorTrust) {
            "writer" -> "@$author can write to this repository, so their checks run on their own."
            "allowlist" -> "Checks for @$author's pull requests run on their own."
            else -> ""
        }

    companion object {
        fun fromJson(json: Map<String, Any?>): PullChecksApproval = PullChecksApproval(
            required = json["required"] == true,
            reason = PullChecksReason.parse(json.str("reason")),
            author = json.str("author").trim().removePrefix("@").take(64),
            authorTrust = json.str("authorTrust").trim().lowercase(),
            approvedBy = json.str("approvedBy").trim().take(64),
        )
    }
}

enum class PullChecksAction(val raw: String) {
    Approve("approve"),
    Trust("trust"),
    Untrust("untrust"),
    ;

    val needsPresence: Boolean get() = this == Approve || this == Trust

    fun label(author: String): String = when (this) {
        Approve -> "Approve and run"
        Trust -> "Always run for @$author"
        Untrust -> "Stop running automatically"
    }
}

data class PullChecksRequest(val action: PullChecksAction, val head: String = "", val account: String = "") {
    val isValid: Boolean
        get() = when (action) {
            PullChecksAction.Approve -> OID.matches(head)
            PullChecksAction.Trust, PullChecksAction.Untrust -> ACCOUNT.matches(account)
        }

    val body: Map<String, Any?>
        get() = when (action) {
            PullChecksAction.Approve -> linkedMapOf("action" to action.raw, "head" to head)
            PullChecksAction.Trust, PullChecksAction.Untrust -> linkedMapOf("action" to action.raw, "account" to account)
        }

    companion object {
        val OID = Regex("^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
        val ACCOUNT = Regex("^[A-Za-z](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?$")

        fun approve(head: String) = PullChecksRequest(PullChecksAction.Approve, head = head.trim().lowercase())
        fun trust(account: String) = PullChecksRequest(PullChecksAction.Trust, account = account.trim().removePrefix("@"))
        fun untrust(account: String) = PullChecksRequest(PullChecksAction.Untrust, account = account.trim().removePrefix("@"))
    }
}

data class RunnerPullChecks(
    val number: Int,
    val configured: Boolean = false,
    val head: String = "",
    val runs: List<ActionsRunSummary> = emptyList(),
    val approval: PullChecksApproval = PullChecksApproval(),
    val canApprove: Boolean = false,
    val viewer: String = "",
    val previousHeads: Int = 0,
) {
    val shortHead: String get() = head.take(7)
    val isEmpty: Boolean get() = runs.isEmpty() && !approval.required
    val visible: Boolean get() = configured || !isEmpty

    val actions: List<PullChecksAction>
        get() {
            if (!canApprove) return emptyList()
            val out = ArrayList<PullChecksAction>()
            if (approval.required && PullChecksRequest.OID.matches(head)) out.add(PullChecksAction.Approve)
            if (approval.author.isNotEmpty() && approval.authorTrust.isEmpty()) out.add(PullChecksAction.Trust)
            if (approval.author.isNotEmpty() && approval.authorTrust == "allowlist") out.add(PullChecksAction.Untrust)
            return out
        }

    val summary: String
        get() = when {
            approval.required -> "Waiting for a maintainer to approve running this pull request's code."
            runs.isEmpty() && configured -> "No checks have run for this commit yet."
            runs.isEmpty() -> "This repository has no pull request workflows."
            else -> PullChecksSummary(runs.map { it.copy(pullNumber = number) }, number).label
        }

    companion object {
        fun fromJson(json: Map<String, Any?>): RunnerPullChecks = RunnerPullChecks(
            number = modelInt(json["number"]).toInt(),
            configured = json["configured"] == true,
            head = json.str("head").trim().lowercase(),
            runs = json.maps("runs").take(64).mapNotNull(ActionsRunSummary::fromJson),
            approval = PullChecksApproval.fromJson(json.obj("approval")),
            canApprove = json["canApprove"] == true,
            viewer = json.str("viewer"),
            previousHeads = maxOf(0, modelInt(json["previousHeads"]).toInt()),
        )
    }
}

data class RunnerRunLog(val runId: Int, val log: String, val updatedAtMs: Long) {
    companion object {
        fun fromJson(json: Map<String, Any?>): RunnerRunLog = RunnerRunLog(
            runId = modelInt(json["runId"]).toInt(),
            log = json.str("log"),
            updatedAtMs = maxOf(0L, modelInt(json["updatedAt"])),
        )
    }
}

class PullChecksModel(private val client: CiClient, val repo: RepoRef, val number: Int) {
    @Volatile var checks: RunnerPullChecks? = null
        private set
    @Volatile var unavailable: Boolean = false
        private set
    @Volatile var loading: Boolean = false
        private set
    @Volatile var error: String? = null
        private set
    @Volatile var notice: CiNotice? = null
        private set
    @Volatile var busy: PullChecksAction? = null
        private set

    val visible: Boolean get() = !unavailable && (checks?.visible == true || error != null || notice != null)
    val actions: List<PullChecksAction> get() = if (busy != null) emptyList() else checks?.actions.orEmpty()
    val hasActiveRuns: Boolean get() = checks?.runs.orEmpty().any { it.status.isActive && it.status != ActionsRunStatus.AwaitingApproval }

    fun load() {
        loading = true
        try {
            checks = client.pullChecks(repo, number)
            unavailable = false
            error = null
        } catch (failure: CiError) {
            if (failure.code == "not_available") {
                unavailable = true
                checks = null
                error = null
            } else {
                error = failure.message
            }
        } catch (failure: Exception) {
            error = failure.message ?: "$failure"
        } finally {
            loading = false
        }
    }

    fun request(action: PullChecksAction, head: String? = null): PullChecksRequest {
        val current = checks
        val author = current?.approval?.author.orEmpty()
        return when (action) {
            PullChecksAction.Approve -> PullChecksRequest.approve(head ?: current?.head.orEmpty())
            PullChecksAction.Trust -> PullChecksRequest.trust(author)
            PullChecksAction.Untrust -> PullChecksRequest.untrust(author)
        }
    }

    fun perform(action: PullChecksAction, head: String? = null): Boolean {
        val request = request(action, head)
        val author = request.account
        if (!request.isValid) {
            notice = CiNotice(
                if (action == PullChecksAction.Approve) "There is no commit waiting for approval." else "This pull request has no verified author.",
                false,
            )
            return false
        }
        busy = action
        notice = null
        val ok = try {
            val answer = client.pullChecksAction(repo, number, request)
            notice = CiNotice(doneMessage(action, request, answer, author), true)
            true
        } catch (failure: Exception) {
            notice = CiNotice(failure.message ?: "$failure", false)
            false
        } finally {
            busy = null
        }
        load()
        return ok
    }

    companion object {
        fun doneMessage(action: PullChecksAction, request: PullChecksRequest, answer: Map<String, Any?>, author: String): String {
            val approved = modelInt(answer["approved"]).toInt()
            val queued = if (approved == 1) "1 check is queued" else "$approved checks are queued"
            return when (action) {
                PullChecksAction.Approve -> "Approved ${request.head.take(7)}. $queued."
                PullChecksAction.Trust ->
                    "Checks for @$author now run on their own." + if (approved > 0) " $queued." else ""
                PullChecksAction.Untrust -> "Checks for @$author need approval again."
            }
        }
    }
}
