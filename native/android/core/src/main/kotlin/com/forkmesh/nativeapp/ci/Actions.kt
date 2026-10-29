package com.forkmesh.nativeapp.ci

import com.forkmesh.nativeapp.json.firstStr
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import com.forkmesh.nativeapp.model.modelInt
import java.security.SecureRandom
import java.util.Base64

enum class ActionsRunStatus(val raw: String, val label: String) {
    AwaitingApproval("awaiting-approval", "Awaiting approval"),
    Queued("queued", "Queued"),
    Running("running", "Running"),
    Success("success", "Success"),
    Failed("failed", "Failed"),
    Rejected("rejected", "Rejected"),
    Cancelled("cancelled", "Cancelled"),
    Skipped("skipped", "Skipped"),
    Unknown("unknown", "Unknown"),
    ;

    val isActive: Boolean get() = this == Queued || this == Running || this == AwaitingApproval
    val isFinished: Boolean get() = !isActive && this != Unknown

    companion object {
        fun parse(raw: String): ActionsRunStatus = when (val clean = raw.trim().lowercase()) {
            "awaiting-approval", "awaiting_approval" -> AwaitingApproval
            "passed" -> Success
            "failure", "error" -> Failed
            "canceled", "stopped" -> Cancelled
            else -> entries.firstOrNull { it.raw == clean } ?: Unknown
        }
    }
}

data class ActionsStep(val name: String, val status: String) {
    val isDone: Boolean get() = status in setOf("success", "failed", "skipped", "cancelled")
}

private fun refLabelOf(ref: String): String = when {
    ref.startsWith("refs/heads/") -> ref.removePrefix("refs/heads/")
    ref.startsWith("refs/tags/") -> ref.removePrefix("refs/tags/")
    else -> ref
}

data class ActionsRunSummary(
    val id: Int,
    val workflow: String = "",
    val workflowPath: String = "",
    val commit: String = "",
    val ref: String = "",
    val status: ActionsRunStatus = ActionsRunStatus.Unknown,
    val createdAtMs: Long = 0,
    val startedAtMs: Long = 0,
    val finishedAtMs: Long = 0,
    val logTail: String = "",
    val pullNumber: Int = 0,
    val steps: List<ActionsStep> = emptyList(),
) {
    val displayName: String get() = workflow.ifEmpty { "Run #$id" }
    val refLabel: String get() = refLabelOf(ref)
    val shortCommit: String get() = commit.take(8)
    val stepsDone: Int get() = steps.count { it.isDone }

    fun durationLabel(nowMs: Long): String {
        if (startedAtMs <= 0) return ""
        val end = if (finishedAtMs > 0) finishedAtMs else if (status == ActionsRunStatus.Running) nowMs else 0
        if (end < startedAtMs) return ""
        val seconds = (end - startedAtMs + 500) / 1_000
        if (seconds < 60) return "${seconds}s"
        val minutes = seconds / 60
        if (minutes < 60) return "${minutes}m ${seconds % 60}s"
        return "${minutes / 60}h ${minutes % 60}m"
    }

    companion object {
        fun fromJson(json: Map<String, Any?>): ActionsRunSummary? {
            val id = modelInt(json["id"]).toInt()
            if (id <= 0) return null
            return ActionsRunSummary(
                id = id,
                workflow = json.firstStr("workflow", "workflowName"),
                workflowPath = json.str("workflowPath"),
                commit = json.str("commit"),
                ref = json.str("ref"),
                status = ActionsRunStatus.parse(json.str("status")),
                createdAtMs = modelInt(json["createdAt"] ?: json["createdAtMs"]),
                startedAtMs = modelInt(json["startedAt"] ?: json["startedAtMs"]),
                finishedAtMs = modelInt(json["finishedAt"] ?: json["finishedAtMs"]),
                logTail = json.str("logTail"),
                pullNumber = modelInt(json["pullNumber"]).toInt(),
                steps = json.maps("steps").take(64).map { ActionsStep(it.str("name"), it.str("status").lowercase()) },
            )
        }
    }
}

data class ActionsWorkflowSummary(
    val path: String,
    val name: String = "",
    val events: List<String> = emptyList(),
    val manual: Boolean = false,
    val enabled: Boolean = true,
    val runsOn: List<String> = emptyList(),
    val valid: Boolean = true,
) {
    val displayName: String get() = name.ifEmpty { path }
    val canDispatch: Boolean get() = manual && enabled && valid

    companion object {
        fun fromJson(json: Map<String, Any?>): ActionsWorkflowSummary? {
            val path = json.str("path")
            if (path.isEmpty()) return null
            return ActionsWorkflowSummary(
                path = path,
                name = json.str("name"),
                events = json.strings("events"),
                manual = json["manual"] == true,
                enabled = json["enabled"] != false,
                runsOn = json.strings("runsOn"),
                valid = json["valid"] != false,
            )
        }
    }
}

data class ActionsControlResult(
    val requestId: String,
    val action: String,
    val applied: Boolean,
    val reason: String = "",
    val runId: Int = 0,
    val finishedAtMs: Long = 0,
) {
    companion object {
        fun fromJson(json: Map<String, Any?>): ActionsControlResult? {
            val requestId = json.str("requestId")
            if (requestId.isEmpty()) return null
            return ActionsControlResult(
                requestId = requestId,
                action = json.str("action"),
                applied = json.str("status") == "applied",
                reason = json.str("reason"),
                runId = modelInt(json["runId"]).toInt(),
                finishedAtMs = modelInt(json["finishedAt"]),
            )
        }
    }
}

data class ActionsSummary(
    val available: Boolean = true,
    val node: String = "",
    val updatedAtMs: Long = 0,
    val schemaVersion: Int = 1,
    val controlAvailable: Boolean = false,
    val actionsEnabled: Boolean? = null,
    val workflows: List<ActionsWorkflowSummary> = emptyList(),
    val runs: List<ActionsRunSummary> = emptyList(),
    val controls: List<ActionsControlResult> = emptyList(),
) {
    companion object {
        fun fromJson(json: Map<String, Any?>): ActionsSummary {
            val v2 = modelInt(json["schemaVersion"]) == 2L
            val runs = json.maps("runs").take(50).mapNotNull(ActionsRunSummary::fromJson)
                .sortedWith(compareByDescending<ActionsRunSummary> { it.createdAtMs }.thenByDescending { it.id })
            return ActionsSummary(
                available = json["ok"] == true,
                node = json.str("node").take(80),
                updatedAtMs = maxOf(0L, modelInt(json["updatedAt"])),
                schemaVersion = if (v2) 2 else 1,
                controlAvailable = v2 && json["controlAvailable"] == true,
                actionsEnabled = if (v2) json["actionsEnabled"] == true else null,
                workflows = if (v2) json.maps("workflows").take(100).mapNotNull(ActionsWorkflowSummary::fromJson) else emptyList(),
                runs = runs,
                controls = json.maps("controls").take(100).mapNotNull(ActionsControlResult::fromJson),
            )
        }
    }
}

enum class ActionsControlKind(val raw: String, val label: String) {
    Dispatch("dispatch", "Run workflow"),
    Rerun("rerun", "Re-run"),
    Cancel("cancel", "Cancel"),
    Approve("approve", "Approve"),
    Reject("reject", "Reject"),
    SetWorkflowEnabled("set-workflow-enabled", "Workflow switch"),
    SetRepositoryEnabled("set-repository-enabled", "Actions switch"),
    ;

    val needsAuthentication: Boolean get() = this == Approve || this == Dispatch || this == SetRepositoryEnabled
    val isDestructive: Boolean get() = this == Cancel || this == Reject
}

data class ActionsControlRequest(
    val requestId: String,
    val kind: ActionsControlKind,
    val node: String,
    val runId: Int? = null,
    val workflow: String? = null,
    val ref: String? = null,
    val enabled: Boolean? = null,
) {
    val isValid: Boolean
        get() {
            if (!REQUEST_ID.matches(requestId) || node.isEmpty()) return false
            return when (kind) {
                ActionsControlKind.Rerun, ActionsControlKind.Cancel, ActionsControlKind.Approve, ActionsControlKind.Reject ->
                    (runId ?: 0) > 0
                ActionsControlKind.Dispatch -> {
                    val w = workflow ?: return false
                    val r = ref ?: return false
                    WORKFLOW.matches(w) && r.startsWith("refs/heads/") && r.length > "refs/heads/".length && r.length <= 160
                }
                ActionsControlKind.SetWorkflowEnabled -> enabled != null && workflow != null && WORKFLOW.matches(workflow)
                ActionsControlKind.SetRepositoryEnabled -> enabled != null
            }
        }

    val body: Map<String, Any?>
        get() {
            val body = linkedMapOf<String, Any?>("requestId" to requestId, "action" to kind.raw, "node" to node)
            if (runId != null) body["runId"] = runId.toLong()
            if (workflow != null) body["workflow"] = workflow
            if (ref != null) body["ref"] = ref
            if (enabled != null) body["enabled"] = enabled
            return body
        }

    companion object {
        val REQUEST_ID = Regex("^[A-Za-z0-9_-]{16,64}$")
        val WORKFLOW = Regex("^\\.forkmesh/[A-Za-z0-9._-]{1,96}\\.ya?ml$")
        private val random = SecureRandom()

        fun makeRequestId(): String {
            val bytes = ByteArray(18)
            random.nextBytes(bytes)
            return Base64.getUrlEncoder().withoutPadding().encodeToString(bytes)
        }

        fun branchRef(branch: String): String {
            val trimmed = branch.trim()
            return if (trimmed.startsWith("refs/")) trimmed else "refs/heads/$trimmed"
        }
    }
}

data class ActionsPendingControl(val request: ActionsControlRequest, val sentAtMs: Long) {
    val id: String get() = request.requestId
}

data class CiNotice(val text: String, val good: Boolean)

object CiLog {
    const val MAX_BYTES = 16 * 1024
    const val MAX_LINES = 400
    private val ANSI = Regex("\u001B\\[[0-9;?]*[A-Za-z]")

    fun lines(text: String, maxLines: Int = MAX_LINES): List<String> {
        var clipped = text
        val bytes = clipped.toByteArray(Charsets.UTF_8)
        if (bytes.size > MAX_BYTES) clipped = String(bytes.copyOfRange(bytes.size - MAX_BYTES, bytes.size), Charsets.UTF_8)
        val all = clipped.replace("\r\n", "\n").split("\n").map(::stripAnsi)
        val trimmed = all.takeLast(maxLines).toMutableList()
        while (trimmed.isNotEmpty() && trimmed.last().isBlank()) trimmed.removeAt(trimmed.lastIndex)
        return trimmed
    }

    fun stripAnsi(line: String): String = ANSI.replace(line, "")

    fun failureExcerpt(text: String, count: Int = 60): String {
        val all = lines(text)
        val markers = listOf("error", "failed", "fatal", "panic", "exception", "assert")
        val index = all.indexOfLast { line -> markers.any { line.lowercase().contains(it) } }
        if (index >= 0) {
            val start = maxOf(0, index - count / 2)
            val end = minOf(all.size, start + count)
            return all.subList(start, end).joinToString("\n")
        }
        return all.takeLast(count).joinToString("\n")
    }
}

data class CiPrompt(val repo: RepoRef, val pull: Int?, val text: String) {
    val composeTarget: com.forkmesh.nativeapp.navigation.ComposeTarget
        get() = com.forkmesh.nativeapp.navigation.ComposeTarget(
            repo = com.forkmesh.nativeapp.navigation.RepoRef(repo.owner, repo.name),
            pull = pull,
            text = text,
        )
}

object CiPrompts {
    fun fixFailedRun(run: ActionsRunSummary, repo: RepoRef): CiPrompt {
        val lines = mutableListOf(
            "The ${run.displayName} workflow failed on ${run.refLabel.ifEmpty { "this branch" }} (run #${run.id}, commit ${run.shortCommit}).",
            "Find the cause and fix it.",
        )
        val failedSteps = run.steps.filter { it.status == "failed" }.map { it.name }
        if (failedSteps.isNotEmpty()) lines.add("Failed steps: ${failedSteps.joinToString(", ")}.")
        val excerpt = CiLog.failureExcerpt(run.logTail)
        if (excerpt.isNotEmpty()) lines.add("Log excerpt:\n```\n$excerpt\n```")
        return CiPrompt(repo, if (run.pullNumber > 0) run.pullNumber else null, lines.joinToString("\n\n"))
    }
}

class PullChecksSummary(runs: List<ActionsRunSummary>, pull: Int) {
    val runs: List<ActionsRunSummary>

    init {
        val latest = LinkedHashMap<String, ActionsRunSummary>()
        for (run in runs) {
            if (run.pullNumber != pull) continue
            val key = run.workflowPath.ifEmpty { run.workflow }
            val existing = latest[key]
            if (existing != null && existing.createdAtMs >= run.createdAtMs) continue
            latest[key] = run
        }
        this.runs = latest.values.sortedBy { it.displayName }
    }

    val passed: Int get() = runs.count { it.status == ActionsRunStatus.Success || it.status == ActionsRunStatus.Skipped }
    val failed: Int
        get() = runs.count {
            it.status == ActionsRunStatus.Failed || it.status == ActionsRunStatus.Rejected || it.status == ActionsRunStatus.Cancelled
        }
    val pending: Int get() = runs.count { it.status.isActive }
    val awaitingApproval: List<ActionsRunSummary> get() = runs.filter { it.status == ActionsRunStatus.AwaitingApproval }

    val state: String
        get() = when {
            runs.isEmpty() -> "none"
            failed > 0 -> "failed"
            pending > 0 -> "pending"
            else -> "passed"
        }

    val label: String
        get() = when (state) {
            "none" -> "No checks"
            "failed" -> "$failed of ${runs.size} checks failed"
            "pending" -> "$pending of ${runs.size} checks running"
            else -> "All ${runs.size} checks passed"
        }
}

class RepoActionsModel(
    private val client: CiClient,
    val repo: RepoRef,
    private val desktop: DesktopActions? = null,
    private val now: () -> Long = System::currentTimeMillis,
    private val makeRequestId: () -> String = ActionsControlRequest::makeRequestId,
    private val background: (Runnable) -> Unit = { REFRESHER.execute(it) },
) {
    private val lock = Any()
    private var generation = 0

    @Volatile var summary: ActionsSummary? = null
        private set
    @Volatile var loading: Boolean = false
        private set
    @Volatile var error: String? = null
        private set
    @Volatile var pending: List<ActionsPendingControl> = emptyList()
        private set
    @Volatile var notice: CiNotice? = null
        private set
    @Volatile var workflowFilter: String? = null
    @Volatile var fromDesktop: Boolean = false
        private set
    @Volatile var desktopProblem: String? = null
        private set
    @Volatile var desktopLogs: Map<Int, String> = emptyMap()
        private set
    private var desktopTap: Any? = null
    @Volatile private var followed: Int? = null
    private val logNext = java.util.concurrent.ConcurrentHashMap<Int, Long>()

    @Volatile var commitFilter: String? = null

    val runs: List<ActionsRunSummary>
        get() {
            val all = summary?.runs.orEmpty().filter { matchesCommit(it.commit, commitFilter) }
            val filter = workflowFilter ?: return all
            return all.filter { it.workflowPath == filter || it.workflow == filter }
        }

    val workflows: List<ActionsWorkflowSummary> get() = summary?.workflows.orEmpty()
    val canControl: Boolean get() = summary?.controlAvailable == true && !summary?.node.isNullOrEmpty()
    val dispatchableWorkflows: List<ActionsWorkflowSummary> get() = if (canControl) workflows.filter { it.canDispatch } else emptyList()
    val hasActiveRuns: Boolean get() = runs.any { it.status.isActive }
    val isUnavailable: Boolean get() = summary?.let { !it.available } ?: false
    val needsFollowUp: Boolean get() = pending.isNotEmpty()

    val emptyMessage: String
        get() = when {
            isUnavailable && desktopProblem != null -> "No node that hosts this repository is answering right now. $desktopProblem"
            isUnavailable -> "No node that hosts this repository is answering right now. Pull to refresh later."
            summary?.actionsEnabled == false -> "Actions are turned off for this repository."
            else -> "No workflow runs yet."
        }

    fun run(id: Int): ActionsRunSummary? {
        val run = summary?.runs?.firstOrNull { it.id == id } ?: return null
        if (!fromDesktop) return run
        return desktopLogs[id]?.let { run.copy(logTail = it) } ?: run
    }

    fun checks(pull: Int): PullChecksSummary = PullChecksSummary(summary?.runs.orEmpty(), pull)

    fun load() {
        val mine = synchronized(lock) { ++generation }
        loading = true
        try {
            var local: ActionsSummary? = null
            if (desktop != null) {
                watchDesktop(desktop)
                try {
                    local = desktop.summary(repo)
                    if (mine != generation) return
                    desktopProblem = null
                } catch (failure: Exception) {
                    if (mine != generation) return
                    desktopProblem = DesktopActions.explain(failure)
                }
            }
            if (local != null && local.runs.isNotEmpty()) {
                apply(local, true)
                return
            }
            try {
                val loaded = client.actionsRuns(repo)
                if (mine != generation) return
                if (!loaded.available && local != null) apply(local, true) else apply(loaded, false)
            } catch (failure: Exception) {
                if (mine != generation) return
                if (local != null) apply(local, true) else error = failure.message ?: "$failure"
            }
        } finally {
            if (mine == generation) loading = false
        }
    }

    private fun apply(loaded: ActionsSummary, desktop: Boolean) {
        summary = loaded
        fromDesktop = desktop
        error = null
        settle()
    }

    @Synchronized
    private fun watchDesktop(desktop: DesktopActions) {
        if (desktopTap != null) return
        desktopTap = desktop.onChange {
            background(Runnable {
                load()
                refreshFollowed()
                for (listener in listeners) listener()
            })
        }
    }

    private val listeners = java.util.concurrent.CopyOnWriteArrayList<() -> Unit>()

    fun addChangeListener(listener: () -> Unit): () -> Unit {
        listeners.add(listener)
        return { listeners.remove(listener) }
    }

    @Synchronized
    fun stop() {
        desktopTap?.let { desktop?.removeTap(it) }
        desktopTap = null
        followed = null
    }

    fun follow(runId: Int) {
        followed = runId
        loadLog(runId)
    }

    fun unfollow(runId: Int) {
        if (followed == runId) followed = null
    }

    fun refreshFollowed() {
        followed?.let(::loadLog)
    }

    fun loadLog(runId: Int) {
        val desktop = desktop ?: return
        if (!fromDesktop) return
        val since = logNext[runId]
        val slice = try {
            desktop.log(repo, runId, since)
        } catch (failure: Exception) {
            return
        }
        val base = if (since != null && !slice.reset && slice.from == since) desktopLogs[runId].orEmpty() else ""
        desktopLogs = desktopLogs + (runId to (base + slice.text).takeLast(DesktopActions.MAX_LOG_CHARS))
        logNext[runId] = slice.next
    }

    fun actions(run: ActionsRunSummary): List<ActionsControlKind> {
        if (!canControl) return emptyList()
        if (pending.any { it.request.runId == run.id }) return emptyList()
        return when (run.status) {
            ActionsRunStatus.AwaitingApproval -> listOf(ActionsControlKind.Approve, ActionsControlKind.Reject)
            ActionsRunStatus.Queued, ActionsRunStatus.Running -> listOf(ActionsControlKind.Cancel)
            ActionsRunStatus.Success, ActionsRunStatus.Failed, ActionsRunStatus.Rejected,
            ActionsRunStatus.Cancelled, ActionsRunStatus.Skipped -> listOf(ActionsControlKind.Rerun)
            ActionsRunStatus.Unknown -> emptyList()
        }
    }

    fun isPending(runId: Int): Boolean = pending.any { it.request.runId == runId }

    fun send(
        kind: ActionsControlKind,
        runId: Int? = null,
        workflow: String? = null,
        branch: String? = null,
        enabled: Boolean? = null,
    ): Boolean {
        if (fromDesktop && desktop != null) return sendToDesktop(desktop, kind, runId, workflow, branch)
        val node = summary?.node
        if (node == null || !canControl) {
            notice = CiNotice("${kind.label}: this repository's node does not accept remote control.", false)
            return false
        }
        val request = ActionsControlRequest(
            requestId = makeRequestId(),
            kind = kind,
            node = node,
            runId = runId,
            workflow = workflow,
            ref = branch?.let(ActionsControlRequest::branchRef),
            enabled = enabled,
        )
        if (!request.isValid) {
            notice = CiNotice("${kind.label}: that request is not valid.", false)
            return false
        }
        pending = pending + ActionsPendingControl(request, now())
        notice = null
        return try {
            client.actionsControl(repo, request)
            notice = CiNotice("${kind.label} sent to $node.", true)
            true
        } catch (failure: Exception) {
            pending = pending.filterNot { it.id == request.requestId }
            notice = CiNotice("${kind.label}: ${failure.message ?: failure}", false)
            false
        }
    }

    private fun sendToDesktop(desktop: DesktopActions, kind: ActionsControlKind, runId: Int?, workflow: String?, branch: String?): Boolean {
        if (DesktopActions.action(kind) == null) {
            notice = CiNotice("${kind.label}: change this on the desktop.", false)
            return false
        }
        val request = ActionsControlRequest(
            requestId = makeRequestId(),
            kind = kind,
            node = desktop.desktopName,
            runId = runId,
            workflow = workflow,
            ref = branch?.let(ActionsControlRequest::branchRef),
        )
        pending = pending + ActionsPendingControl(request, now())
        notice = null
        return try {
            val message = desktop.control(repo, kind, runId, workflow, branch)
            pending = pending.filterNot { it.id == request.requestId }
            notice = CiNotice(message.ifEmpty { "${kind.label} sent to ${desktop.desktopName}." }, true)
            load()
            true
        } catch (failure: Exception) {
            pending = pending.filterNot { it.id == request.requestId }
            notice = CiNotice("${kind.label}: ${DesktopActions.explain(failure)}", false)
            false
        }
    }

    fun dispatch(workflow: ActionsWorkflowSummary, branch: String): Boolean =
        send(ActionsControlKind.Dispatch, workflow = workflow.path, branch = branch)

    fun settle() {
        val controls = summary?.controls.orEmpty()
        val at = now()
        var outcome: CiNotice? = null
        pending = pending.filterNot { entry ->
            val result = controls.firstOrNull { it.requestId == entry.request.requestId }
            if (result != null) {
                val label = entry.request.kind.label
                outcome = if (result.applied) {
                    CiNotice("$label applied.", true)
                } else {
                    val reason = if (result.reason.isEmpty()) "" else ": ${result.reason.replace('_', ' ')}"
                    CiNotice("$label was refused$reason.", false)
                }
                true
            } else {
                at - entry.sentAtMs > PENDING_TIMEOUT_MS
            }
        }
        outcome?.let { notice = it }
    }

    companion object {
        const val PENDING_TIMEOUT_MS = 120_000L
        const val FOLLOW_UP_DELAY_MS = 5_000L

        fun matchesCommit(commit: String, filter: String?): Boolean {
            val wanted = filter?.trim()?.lowercase().orEmpty()
            if (wanted.isEmpty()) return true
            val have = commit.trim().lowercase()
            if (have.length < 7 || wanted.length < 7) return false
            return have.startsWith(wanted) || wanted.startsWith(have)
        }

        private val REFRESHER = java.util.concurrent.Executors.newSingleThreadExecutor { task ->
            Thread(task, "actions-desktop").apply { isDaemon = true }
        }
    }
}
