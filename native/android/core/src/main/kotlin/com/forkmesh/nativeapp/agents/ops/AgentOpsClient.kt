package com.forkmesh.nativeapp.agents.ops

import com.forkmesh.nativeapp.control.ControlCalling
import com.forkmesh.nativeapp.control.ControlErrorText
import com.forkmesh.nativeapp.control.ControlFamilyChannel
import com.forkmesh.nativeapp.control.DesktopControlFamilyName
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.list
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.str

class AgentOpsException(val code: String) : RuntimeException(code)

class AgentOpsClient(channel: ControlCalling, private val clock: () -> Long = System::currentTimeMillis) {
    val family = ControlFamilyChannel(DesktopControlFamilyName.AgentOps, channel)

    val nowMs: Long get() = clock()

    fun approvals(): List<PendingApproval> = family.call("approvals").maps("approvals").mapNotNull(PendingApproval::from)

    fun prepareAnswer(approvalId: String, allow: Boolean, lifetime: ApprovalLifetime = ApprovalLifetime.Once): AgentOpsConfirmation {
        val params = linkedMapOf<String, Any?>("id" to approvalId)
        if (allow) params["lifetime"] = lifetime.wire
        return prepare(if (allow) AgentOpsAction.Approve else AgentOpsAction.Deny, params)
    }

    fun answer(approvalId: String, allow: Boolean, lifetime: ApprovalLifetime = ApprovalLifetime.Once, token: String) {
        val params = linkedMapOf<String, Any?>("id" to approvalId, "decision" to if (allow) "allow" else "deny", "token" to token)
        if (allow) params["lifetime"] = lifetime.wire
        family.call("answer", params)
    }

    fun prepareMerge(sessionId: Int): AgentOpsConfirmation = prepare(AgentOpsAction.Merge, linkedMapOf("id" to sessionId.toLong()))

    fun merge(sessionId: Int, token: String, typed: String): MergeOutcome =
        MergeOutcome.from(family.call("merge", linkedMapOf("id" to sessionId.toLong(), "token" to token, "typed" to typed), 180_000))

    fun mergeStats(sessionId: Int? = null): MergeLeaderboard {
        val params = linkedMapOf<String, Any?>()
        if (sessionId != null && sessionId > 0) params["id"] = sessionId.toLong()
        return MergeLeaderboard.from(family.call("mergestats", params, 60_000))
    }

    fun diff(sessionId: Int): AgentDiff =
        AgentDiff.from(family.call("diff", linkedMapOf("id" to sessionId.toLong()), 60_000))

    fun prepareDelete(sessionId: Int): AgentOpsConfirmation = prepare(AgentOpsAction.Delete, linkedMapOf("id" to sessionId.toLong()))

    fun delete(sessionId: Int, token: String): AgentDeleteReport? =
        AgentDeleteReport.from(family.call("delete", linkedMapOf("id" to sessionId.toLong(), "token" to token), 90_000))

    fun prepareBulk(action: BulkAgentAction, ids: List<Int>): AgentOpsConfirmation =
        prepare(action.opsAction, linkedMapOf("ids" to canonical(ids).map(Int::toLong)))

    fun bulk(action: BulkAgentAction, ids: List<Int>, token: String): BulkOutcome {
        val reply = family.call(
            "bulk",
            linkedMapOf("action" to action.wire, "ids" to canonical(ids).map(Int::toLong), "token" to token),
            60_000,
        )
        return BulkOutcome(
            action,
            reply.list("done").map { jsonLong(it).toInt() },
            reply.list("skipped").map { jsonLong(it).toInt() },
        )
    }

    fun recycleBin(): RecycleBin = RecycleBin.from(family.call("bin", linkedMapOf(), 30_000))

    fun restoreRecycled(ids: List<Int>): Pair<RecycleBin, RecycleBinChange> {
        val reply = family.call("bin.restore", linkedMapOf("ids" to canonical(ids).map(Int::toLong)), 60_000)
        return RecycleBin.from(reply) to RecycleBinChange.Restored(
            reply.list("restored").map { jsonLong(it).toInt() },
            reply.list("failed").map { jsonLong(it).toInt() },
        )
    }

    fun preparePurge(ids: List<Int>): AgentOpsConfirmation =
        prepare(AgentOpsAction.Purge, linkedMapOf("ids" to canonical(ids).map(Int::toLong)))

    fun purgeRecycled(ids: List<Int>, token: String): Pair<RecycleBin, RecycleBinChange> {
        val reply = family.call("bin.purge", linkedMapOf("ids" to canonical(ids).map(Int::toLong), "token" to token), 60_000)
        return RecycleBin.from(reply) to RecycleBinChange.Purged(
            reply.list("purged").map { jsonLong(it).toInt() },
            reply.list("skipped").map { jsonLong(it).toInt() },
        )
    }

    fun setRecycleRetention(hours: Int): RecycleBin =
        RecycleBin.from(family.call("bin.retention", linkedMapOf("hours" to hours.toLong()), 30_000))

    fun worktrees(owner: String, name: String): WorktreeList =
        WorktreeList.from(family.call("worktrees", linkedMapOf("owner" to owner, "name" to name), 45_000))

    fun usage(): UsageSummary = UsageSummary.from(family.call("usage"))

    fun grants(): List<CapabilityGrant> = family.call("grants").maps("grants").map(CapabilityGrant::from)

    fun prepareRevoke(grantId: String): AgentOpsConfirmation = prepare(AgentOpsAction.Revoke, linkedMapOf("grantId" to grantId))

    fun revoke(grantId: String, token: String) {
        family.call("revoke", linkedMapOf("grantId" to grantId, "token" to token))
    }

    fun onChanged(handler: (Map<String, Any?>) -> Unit): Any =
        family.addEventTap { suffix, data -> if (suffix == "changed") handler(data) }

    fun removeTap(token: Any) = family.removeEventTap(token)

    private fun prepare(action: AgentOpsAction, params: Map<String, Any?>): AgentOpsConfirmation {
        val body = LinkedHashMap(params)
        body["action"] = action.wire
        val issued = nowMs
        val reply = family.call("prepare", body)
        return AgentOpsConfirmation.from(reply, issued) ?: throw AgentOpsException("confirmation_required")
    }

    companion object {
        fun canonical(ids: List<Int>): List<Int> = ids.filter { it > 0 }.toSortedSet().toList()

        fun errorCode(error: Throwable): String? = when (error) {
            is AgentOpsException -> error.code
            is LocalControlException -> error.code
            else -> null
        }

        fun isUnsupported(error: Throwable): Boolean {
            val code = errorCode(error) ?: return false
            return code == "unsupported" || code == "unknown_method"
        }

        fun describe(error: Throwable): String {
            val code = errorCode(error) ?: return error.message ?: error.toString()
            if (code == "merge_failed" && error is LocalControlException && error.reason.isNotBlank()) {
                return ControlErrorText.withReason("The merge did not finish.", error)
            }
            return describe(code)
        }

        fun describe(code: String): String = when (code) {
            "not_owner" -> "Only the account that owns this desktop can manage its agents."
            "unknown_approval" -> "That request was already answered, or the agent stopped waiting."
            "not_delivered" -> "The desktop could not deliver the answer; the request stays denied."
            "unknown_session" -> "The desktop no longer has that agent session."
            "no_checkout" -> "The desktop has no checkout of that repository."
            "agent_running" -> "The agent is still working on that branch. Stop it first."
            "conflicts" -> "The branch conflicts with its base. Resolve the conflicts on the desktop or hand them to an agent."
            "conflict_check_unavailable" -> "The desktop's Git is too old to check for conflicts first. Merge on the desktop."
            "no_base_branch" -> "The desktop could not find the branch to merge into."
            "merge_failed" -> "The merge did not finish. The desktop's repository page says why."
            "repo_unavailable" -> "The desktop could not open that repository."
            "name_mismatch" -> "The typed name does not match the branch."
            "branch_gone" -> "The agent's branch and worktree are gone from the desktop."
            "outside_other_user" -> "That agent runs under another user on the desktop, so it can't be ended from here."
            "unknown_grant" -> "That saved permission no longer exists."
            "not_saved" -> "The desktop revoked it but could not save the change."
            "unavailable" -> "The desktop's agent store is not open yet. Try again in a moment."
            "bad_request", "unknown_action" -> "The desktop did not understand that request."
            else -> ControlErrorText.describe(code)
        }
    }
}
