package com.forkmesh.nativeapp.agents.ops

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.code.DiffHealthGit
import com.forkmesh.nativeapp.code.DiffReviewModel
import com.forkmesh.nativeapp.code.DiffReviewSection
import com.forkmesh.nativeapp.code.diff.parseUnifiedDiff
import java.util.concurrent.CopyOnWriteArrayList

open class OpsState {
    private val listeners = CopyOnWriteArrayList<() -> Unit>()

    @Volatile
    var revision = 0
        private set

    fun addListener(listener: () -> Unit) = listeners.add(listener)

    fun removeListener(listener: () -> Unit) = listeners.remove(listener)

    protected fun changed() {
        revision++
        for (listener in listeners) runCatching { listener() }
    }
}

class ApprovalsQueue(private val client: AgentOpsClient, private val launch: (() -> Unit) -> Unit = { it() }) : OpsState() {
    private val lock = Any()

    @Volatile
    var approvals: List<PendingApproval> = emptyList()
        private set

    @Volatile
    var loading = false
        private set

    @Volatile
    var errorText = ""
        private set

    @Volatile
    var unsupported = false
        private set

    @Volatile
    var answering: Set<String> = emptySet()
        private set

    @Volatile
    var lastAnswered: String? = null
        private set

    private var tap: Any? = null
    private var refreshQueued = false

    val count: Int get() = approvals.size

    val isEmpty: Boolean get() = approvals.isEmpty()

    fun approval(id: String): PendingApproval? = approvals.firstOrNull { it.id == id }

    fun approvals(sessionId: Int): List<PendingApproval> = approvals.filter { it.sessionId == sessionId }

    val sessionIdsNeedingYou: Set<Int> get() = approvals.map { it.sessionId }.toSet()

    fun start() {
        synchronized(lock) {
            if (tap != null) return
            tap = client.onChanged { scheduleRefresh() }
        }
    }

    fun stop() {
        val token = synchronized(lock) { tap.also { tap = null } } ?: return
        client.removeTap(token)
    }

    fun scheduleRefresh() {
        synchronized(lock) {
            if (refreshQueued) return
            refreshQueued = true
        }
        launch {
            try {
                refresh()
            } finally {
                synchronized(lock) { refreshQueued = false }
            }
        }
    }

    fun refresh() {
        loading = true
        changed()
        try {
            approvals = client.approvals().sortedByDescending { it.askedAtMs }
            errorText = ""
            unsupported = false
        } catch (error: Exception) {
            if (AgentOpsClient.isUnsupported(error)) {
                unsupported = true
                approvals = emptyList()
                errorText = ""
            } else {
                errorText = AgentOpsClient.describe(error)
            }
        } finally {
            loading = false
            changed()
        }
    }

    fun approve(approvalId: String, lifetime: ApprovalLifetime = ApprovalLifetime.Once): Boolean =
        answer(approvalId, true, lifetime)

    fun deny(approvalId: String): Boolean = answer(approvalId, false, ApprovalLifetime.Once)

    private fun answer(approvalId: String, allow: Boolean, lifetime: ApprovalLifetime): Boolean {
        synchronized(lock) {
            if (!AgentOpsIds.isApprovalId(approvalId) || approvalId in answering) return false
            answering = answering + approvalId
        }
        changed()
        try {
            val confirmation = client.prepareAnswer(approvalId, allow, lifetime)
            client.answer(approvalId, allow, lifetime, confirmation.token)
            approvals = approvals.filter { it.id != approvalId }
            lastAnswered = approvalId
            errorText = ""
            return true
        } catch (error: Exception) {
            errorText = AgentOpsClient.describe(error)
            if (AgentOpsClient.errorCode(error) == "unknown_approval") approvals = approvals.filter { it.id != approvalId }
            return false
        } finally {
            synchronized(lock) { answering = answering - approvalId }
            changed()
        }
    }
}

class MergeCleanupModel(private val client: AgentOpsClient, val sessionId: Int) : OpsState() {
    @Volatile
    var confirmation: AgentOpsConfirmation? = null
        private set

    @Volatile
    var preparing = false
        private set

    @Volatile
    var running = false
        private set

    @Volatile
    var errorText = ""
        private set

    @Volatile
    var mergedBranch: String? = null
        private set

    @Volatile
    var mergeStats: MergeStats? = null
        private set

    @Volatile
    var conflicted = false
        private set

    val outcome: MergeOutcome?
        get() = mergedBranch?.let { MergeOutcome(it, mergeStats) }

    val canRun: Boolean
        get() = confirmation != null && !running && mergedBranch == null

    fun prepare() {
        preparing = true
        changed()
        try {
            confirmation = client.prepareMerge(sessionId)
            errorText = ""
        } catch (error: Exception) {
            confirmation = null
            errorText = AgentOpsClient.describe(error)
        } finally {
            preparing = false
            changed()
        }
    }

    val busy: Boolean
        get() = preparing || running

    fun mergeNow(): Boolean {
        if (busy || mergedBranch != null) return false
        prepare()
        return run()
    }

    fun run(): Boolean {
        val current = confirmation ?: return false
        if (!canRun) return false
        if (current.isExpired(client.nowMs)) {
            confirmation = null
            errorText = AgentOpsClient.describe("confirmation_expired")
            changed()
            return false
        }
        running = true
        changed()
        try {
            val outcome = client.merge(sessionId, current.token, current.typeToConfirm)
            mergeStats = outcome.stats
            mergedBranch = outcome.branch.ifEmpty { current.typeToConfirm }
            errorText = ""
            confirmation = null
            conflicted = false
            return true
        } catch (error: Exception) {
            confirmation = null
            conflicted = AgentOpsClient.errorCode(error) == "conflicts"
            errorText = AgentOpsClient.describe(error)
            return false
        } finally {
            running = false
            changed()
        }
    }
}

class AgentDiffModel(private val client: AgentOpsClient, val sessionId: Int, private val store: PrefStore? = null) : OpsState() {
    @Volatile
    var diff: AgentDiff? = null
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

    fun load() {
        loading = true
        changed()
        try {
            val loaded = client.diff(sessionId)
            if (loaded != diff || review == null) {
                review = DiffReviewModel(
                    reviewSections(loaded),
                    reviewScope(loaded, sessionId),
                    git = if (loaded.merged) DiffHealthGit.Merged else DiffHealthGit.Unknown,
                    store = store,
                )
            }
            diff = loaded
            errorText = ""
        } catch (error: Exception) {
            errorText = if (AgentOpsClient.isUnsupported(error)) {
                "Update ForkMesh on the desktop to see an agent's changes here."
            } else {
                AgentOpsClient.describe(error)
            }
        } finally {
            loading = false
            changed()
        }
    }
}

class AgentDeleteModel(private val client: AgentOpsClient, val sessionId: Int) : OpsState() {
    @Volatile
    var confirmation: AgentOpsConfirmation? = null
        private set

    @Volatile
    var preparing = false
        private set

    @Volatile
    var running = false
        private set

    @Volatile
    var errorText = ""
        private set

    @Volatile
    var deleted = false
        private set

    @Volatile
    var report: AgentDeleteReport? = null
        private set

    fun prepare() {
        preparing = true
        changed()
        try {
            confirmation = client.prepareDelete(sessionId)
            errorText = ""
        } catch (error: Exception) {
            confirmation = null
            errorText = if (AgentOpsClient.isUnsupported(error)) {
                "Update ForkMesh on the desktop to delete agents from here."
            } else {
                AgentOpsClient.describe(error)
            }
        } finally {
            preparing = false
            changed()
        }
    }

    fun run(): Boolean {
        val current = confirmation ?: return false
        if (running || deleted) return false
        if (current.isExpired(client.nowMs)) {
            confirmation = null
            errorText = AgentOpsClient.describe("confirmation_expired")
            changed()
            return false
        }
        running = true
        changed()
        try {
            val answer = client.delete(sessionId, current.token)
            report = answer
            confirmation = null
            if (answer != null && answer.agentKept) {
                errorText = answer.summary("on the desktop")
                return false
            }
            deleted = true
            errorText = ""
            return true
        } catch (error: Exception) {
            confirmation = null
            errorText = AgentOpsClient.describe(error)
            return false
        } finally {
            running = false
            changed()
        }
    }
}

class BulkActionsModel(private val client: AgentOpsClient) : OpsState() {
    @Volatile
    var selection: Set<Int> = emptySet()
        private set

    @Volatile
    var selecting = false
        set(value) {
            field = value
            changed()
        }

    @Volatile
    var confirmation: AgentOpsConfirmation? = null
        private set

    @Volatile
    var pendingAction: BulkAgentAction? = null
        private set

    @Volatile
    var running = false
        private set

    @Volatile
    var errorText = ""
        private set

    @Volatile
    var lastOutcome: BulkOutcome? = null
        private set

    private var pendingIds: List<Int> = emptyList()

    fun toggle(id: Int) {
        selection = if (id in selection) selection - id else selection + id
        changed()
    }

    fun clear() {
        selection = emptySet()
        confirmation = null
        pendingAction = null
        changed()
    }

    fun eligible(action: BulkAgentAction, candidates: List<BulkCandidate>): List<Int> {
        val pool = if (selection.isEmpty()) candidates else candidates.filter { it.id in selection }
        return AgentOpsClient.canonical(pool.filter { it.allows(action) }.map { it.id })
    }

    fun prepare(action: BulkAgentAction, candidates: List<BulkCandidate>) {
        val ids = eligible(action, candidates)
        if (ids.isEmpty()) {
            errorText = if (action == BulkAgentAction.Stop) "Nothing to stop." else "Nothing to continue."
            changed()
            return
        }
        try {
            confirmation = client.prepareBulk(action, ids)
            pendingAction = action
            pendingIds = ids
            errorText = ""
        } catch (error: Exception) {
            errorText = AgentOpsClient.describe(error)
        }
        changed()
    }

    fun run(): BulkOutcome? {
        val current = confirmation ?: return null
        val action = pendingAction ?: return null
        if (running) return null
        if (current.isExpired(client.nowMs)) {
            confirmation = null
            errorText = AgentOpsClient.describe("confirmation_expired")
            changed()
            return null
        }
        running = true
        changed()
        try {
            val outcome = client.bulk(action, pendingIds, current.token)
            lastOutcome = outcome
            errorText = ""
            confirmation = null
            pendingAction = null
            selection = emptySet()
            selecting = false
            return outcome
        } catch (error: Exception) {
            confirmation = null
            errorText = AgentOpsClient.describe(error)
            return null
        } finally {
            running = false
            changed()
        }
    }

    fun cancel() {
        confirmation = null
        pendingAction = null
        changed()
    }
}

class AgentUsageModel(private val client: AgentOpsClient) : OpsState() {
    @Volatile
    var summary: UsageSummary? = null
        private set

    @Volatile
    var loading = false
        private set

    @Volatile
    var errorText = ""
        private set

    @Volatile
    var unsupported = false
        private set

    val nowMs: Long get() = client.nowMs

    fun refresh() {
        loading = true
        changed()
        try {
            summary = client.usage()
            errorText = ""
            unsupported = false
        } catch (error: Exception) {
            unsupported = AgentOpsClient.isUnsupported(error)
            errorText = if (unsupported) "" else AgentOpsClient.describe(error)
        } finally {
            loading = false
            changed()
        }
    }

    val reminders: List<UsageReminder> get() = summary?.reminders(client.nowMs) ?: emptyList()

    @Volatile
    var busy = false
        private set

    fun selectAccount(provider: String, account: String): Boolean = apply { client.selectAccount(provider, account) }

    fun renameAccount(provider: String, account: String, label: String): Boolean {
        if (AgentOpsClient.accountLabel(label).isEmpty()) return false
        return apply { client.renameAccount(provider, account, label) }
    }

    fun deleteAccount(provider: String, account: String): Boolean = apply { client.deleteAccount(provider, account) }

    fun addAccount(provider: String, label: String): String? {
        if (AgentOpsClient.accountLabel(label).isEmpty()) return null
        var created = ""
        val ok = apply { client.addAccount(provider, label).let { (next, id) -> created = id; next } }
        return if (ok && created.isNotEmpty()) created else null
    }

    private fun apply(call: () -> UsageSummary): Boolean {
        busy = true
        changed()
        try {
            summary = call()
            errorText = ""
            unsupported = false
            return true
        } catch (error: Exception) {
            errorText = AgentOpsClient.describe(error)
            return false
        } finally {
            busy = false
            changed()
        }
    }
}

class MergeLeaderboardModel(private val client: AgentOpsClient, val sessionId: Int? = null) : OpsState() {
    @Volatile
    var board = MergeLeaderboard()
        private set

    @Volatile
    var loading = false
        private set

    @Volatile
    var loaded = false
        private set

    @Volatile
    var errorText = ""
        private set

    fun refresh() {
        loading = true
        changed()
        try {
            board = client.mergeStats(sessionId)
            errorText = ""
        } catch (error: Exception) {
            errorText = if (AgentOpsClient.isUnsupported(error)) {
                "Update ForkMesh on the desktop to see the merge leaderboard here."
            } else {
                AgentOpsClient.describe(error)
            }
        } finally {
            loading = false
            loaded = true
            changed()
        }
    }
}

class WorktreesModel(private val client: AgentOpsClient, val owner: String, val name: String) : OpsState() {
    @Volatile
    var list = WorktreeList()
        private set

    @Volatile
    var loading = false
        private set

    @Volatile
    var loaded = false
        private set

    @Volatile
    var errorText = ""
        private set

    val agentWorktrees: List<AgentWorktree> get() = list.worktrees.filter { !it.isMain }

    fun refresh() {
        loading = true
        changed()
        try {
            list = client.worktrees(owner, name)
            errorText = ""
        } catch (error: Exception) {
            errorText = AgentOpsClient.describe(error)
        } finally {
            loading = false
            loaded = true
            changed()
        }
    }
}

class CapabilityGrantsModel(private val client: AgentOpsClient) : OpsState() {
    @Volatile
    var grants: List<CapabilityGrant> = emptyList()
        private set

    @Volatile
    var loading = false
        private set

    @Volatile
    var errorText = ""
        private set

    @Volatile
    var revoking: Set<String> = emptySet()
        private set

    fun refresh() {
        loading = true
        changed()
        try {
            grants = client.grants()
            errorText = ""
        } catch (error: Exception) {
            errorText = AgentOpsClient.describe(error)
        } finally {
            loading = false
            changed()
        }
    }

    fun prepareRevoke(grant: CapabilityGrant): AgentOpsConfirmation? = try {
        client.prepareRevoke(grant.id)
    } catch (error: Exception) {
        errorText = AgentOpsClient.describe(error)
        changed()
        null
    }

    fun revoke(grant: CapabilityGrant, confirmation: AgentOpsConfirmation): Boolean {
        revoking = revoking + grant.id
        changed()
        try {
            client.revoke(grant.id, confirmation.token)
            grants = grants.filter { it.id != grant.id }
            errorText = ""
            return true
        } catch (error: Exception) {
            errorText = AgentOpsClient.describe(error)
            return false
        } finally {
            revoking = revoking - grant.id
            changed()
        }
    }
}

class RecycleBinModel(private val client: AgentOpsClient) : OpsState() {
    @Volatile
    var bin = RecycleBin()
        private set

    @Volatile
    var loading = false
        private set

    @Volatile
    var loaded = false
        private set

    @Volatile
    var busy = false
        private set

    @Volatile
    var unsupported = false
        private set

    @Volatile
    var errorText = ""
        private set

    @Volatile
    var note = ""
        private set

    @Volatile
    var selection: Set<Int> = emptySet()
        private set

    @Volatile
    var confirmation: AgentOpsConfirmation? = null
        private set

    @Volatile
    private var pendingIds: List<Int> = emptyList()

    val selectedIds: List<Int> get() = bin.ids.filter { it in selection }

    fun refresh() {
        loading = true
        changed()
        try {
            apply(client.recycleBin())
            errorText = ""
            unsupported = false
        } catch (error: Exception) {
            unsupported = AgentOpsClient.isUnsupported(error)
            errorText = if (unsupported) {
                "Update ForkMesh on the desktop to manage its recycle bin from here."
            } else {
                AgentOpsClient.describe(error)
            }
        } finally {
            loading = false
            loaded = true
            changed()
        }
    }

    fun toggle(id: Int) {
        selection = if (id in selection) selection - id else selection + id
        changed()
    }

    fun selectAll() {
        selection = bin.ids.toSet()
        changed()
    }

    fun clearSelection() {
        selection = emptySet()
        changed()
    }

    fun restore(ids: List<Int>): Boolean {
        if (ids.isEmpty() || busy) return false
        busy = true
        changed()
        try {
            val (next, change) = client.restoreRecycled(ids)
            apply(next)
            note = change.message
            errorText = ""
            return (change as? RecycleBinChange.Restored)?.done?.isNotEmpty() == true
        } catch (error: Exception) {
            errorText = AgentOpsClient.describe(error)
            return false
        } finally {
            busy = false
            changed()
        }
    }

    fun preparePurge(ids: List<Int>) {
        if (ids.isEmpty() || busy) return
        try {
            confirmation = client.preparePurge(ids)
            pendingIds = ids
            errorText = ""
        } catch (error: Exception) {
            confirmation = null
            errorText = AgentOpsClient.describe(error)
        }
        changed()
    }

    fun purge(): Boolean {
        val pending = confirmation ?: return false
        if (busy) return false
        confirmation = null
        if (pending.isExpired(client.nowMs)) {
            errorText = AgentOpsClient.describe("confirmation_expired")
            changed()
            return false
        }
        busy = true
        changed()
        try {
            val (next, change) = client.purgeRecycled(pendingIds, pending.token)
            apply(next)
            note = change.message
            errorText = ""
            return true
        } catch (error: Exception) {
            errorText = AgentOpsClient.describe(error)
            return false
        } finally {
            busy = false
            changed()
        }
    }

    fun cancelPurge() {
        confirmation = null
        pendingIds = emptyList()
        changed()
    }

    fun setRetention(hours: Int) {
        val clamped = hours.coerceIn(bin.minHours, maxOf(bin.minHours, bin.maxHours))
        if (clamped == bin.retentionHours || busy) return
        busy = true
        changed()
        try {
            apply(client.setRecycleRetention(clamped))
            errorText = ""
        } catch (error: Exception) {
            errorText = AgentOpsClient.describe(error)
        } finally {
            busy = false
            changed()
        }
    }

    private fun apply(next: RecycleBin) {
        bin = next
        selection = selection.intersect(next.ids.toSet())
    }
}

fun reviewSections(diff: AgentDiff): List<DiffReviewSection> = listOf(
    DiffReviewSection("Committed on the branch", parseUnifiedDiff(diff.committed)),
    DiffReviewSection("Uncommitted in the worktree", parseUnifiedDiff(diff.uncommitted)),
)

fun reviewScope(diff: AgentDiff, sessionId: Int): String = if (diff.branch.isEmpty()) "agent/$sessionId" else "agent/${diff.branch}"
