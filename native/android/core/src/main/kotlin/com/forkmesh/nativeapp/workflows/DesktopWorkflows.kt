package com.forkmesh.nativeapp.workflows

import com.forkmesh.nativeapp.agents.ops.OpsState
import com.forkmesh.nativeapp.control.ControlCalling
import com.forkmesh.nativeapp.control.ControlErrorText
import com.forkmesh.nativeapp.control.ControlFamilyChannel
import com.forkmesh.nativeapp.control.DesktopControlFamilyName
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.list
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.optBool
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import java.util.Locale

data class DesktopWorkflowRow(
    val repo: String,
    val workflowId: String,
    val name: String,
    val description: String,
    val icon: String,
    val enabled: Boolean,
    val specialAgent: Boolean,
    val steps: Int,
    val trigger: String,
    val running: Boolean,
    val runningStep: String,
    val hasSchedule: Boolean,
    val scheduleActive: Boolean,
    val nextRunAt: Long,
    val lastRunAt: Long,
    val lastStatus: String,
    val lastSummary: String,
    val runs: Int,
    val failedRuns: Int,
    val error: String,
) {
    val id: String get() = "$repo|$workflowId"
    val kindLabel: String get() = if (specialAgent) "Special agent" else "Desktop workflow"

    companion object {
        fun of(json: Map<String, Any?>) = DesktopWorkflowRow(
            repo = json.str("repo"),
            workflowId = json.str("id"),
            name = json.str("name", json.str("id")),
            description = json.str("description"),
            icon = json.str("icon"),
            enabled = json.optBool("enabled") ?: true,
            specialAgent = json.bool("specialAgent"),
            steps = json.int("steps"),
            trigger = json.str("trigger"),
            running = json.bool("running") || json.bool("agentWorking"),
            runningStep = json.str("runningStep"),
            hasSchedule = json.bool("hasSchedule"),
            scheduleActive = json.bool("scheduleActive"),
            nextRunAt = json.long("nextRunAt"),
            lastRunAt = json.long("lastRunAt"),
            lastStatus = json.str("lastStatus"),
            lastSummary = json.str("lastSummary"),
            runs = json.int("runs"),
            failedRuns = json.int("failedRuns"),
            error = json.str("error"),
        )
    }
}

data class DesktopWorkflowStepRun(
    val nodeId: String,
    val name: String,
    val type: String,
    val status: String,
    val startedAt: Long,
    val finishedAt: Long,
    val output: String,
    val error: String,
    val logs: List<String>,
) {
    val ms: Long get() = if (finishedAt >= startedAt && startedAt > 0) finishedAt - startedAt else 0

    companion object {
        fun of(json: Map<String, Any?>) = DesktopWorkflowStepRun(
            json.str("nodeId"), json.str("name", json.str("nodeId")), json.str("type"), json.str("status"),
            json.long("startedAtMs"), json.long("finishedAtMs"), json.str("output"), json.str("error"), json.strings("logs"),
        )
    }
}

data class DesktopWorkflowRun(
    val id: String,
    val trigger: String,
    val status: String,
    val startedAt: Long,
    val finishedAt: Long,
    val caption: String,
    val output: String,
    val error: String,
    val steps: List<DesktopWorkflowStepRun>,
) {
    val summary: String
        get() = when {
            error.isNotEmpty() -> error
            caption.isNotEmpty() -> caption
            else -> output.split("\n").firstOrNull().orEmpty()
        }

    companion object {
        fun of(json: Map<String, Any?>) = DesktopWorkflowRun(
            json.str("id"), json.str("trigger"), json.str("status"), json.long("startedAtMs"), json.long("finishedAtMs"),
            json.str("caption"), json.str("output"), json.str("error"), json.maps("steps").map(DesktopWorkflowStepRun::of),
        )
    }
}

data class DesktopWorkflowDetail(
    val repo: String,
    val workflowId: String,
    val name: String,
    val description: String,
    val enabled: Boolean,
    val error: String,
    val nodes: List<Step>,
    val runs: List<DesktopWorkflowRun>,
    val active: DesktopWorkflowRun?,
) {
    data class Step(val id: String, val type: String, val name: String)

    val latest: DesktopWorkflowRun? get() = active ?: runs.firstOrNull()

    val shownRuns: List<DesktopWorkflowRun> get() = listOfNotNull(active) + runs.filter { it.id != active?.id }

    companion object {
        fun of(json: Map<String, Any?>): DesktopWorkflowDetail {
            val activeJson = json.obj("active")
            return DesktopWorkflowDetail(
                json.str("repo"), json.str("id"), json.str("name", json.str("id")), json.str("description"),
                json.optBool("enabled") ?: true, json.str("error"),
                json.maps("nodes").map { Step(it.str("id"), it.str("type"), it.str("name", it.str("id"))) },
                json.maps("runs").map(DesktopWorkflowRun::of),
                if (activeJson.isEmpty()) null else DesktopWorkflowRun.of(activeJson),
            )
        }
    }
}

data class DesktopWorkflowConfirmation(
    val token: String,
    val action: String,
    val repo: String,
    val workflowId: String,
    val summary: String,
    val assignment: String,
)

object DesktopWorkflowsText {
    const val SECTION_TITLE = "On your desktop"
    const val ACCOUNT_TITLE = "On ForkMesh"
    const val OFFLINE = "Connect to your desktop to see and run its workflows and special agents."
    const val EMPTY = "The desktop has no workflows or special agents in its repositories yet."

    fun describe(error: Throwable): String {
        if (error is LocalControlException) {
            describe(error.code)?.let { return ControlErrorText.withReason(it, error) }
            if (error.reason.isNotBlank()) return error.reason.trim()
        }
        return ControlErrorText.describe(error)
    }

    fun describe(code: String): String? = when (code) {
        "unsupported", "unknown_method" -> "Update ForkMesh on the desktop to run its workflows from this phone."
        "unknown_repo" -> "The desktop no longer has that repository."
        "unknown_workflow" -> "That workflow is no longer on the desktop."
        "not_started" -> "The desktop did not start it."
        "not_running" -> "It is not running."
        "bad_request" -> "The desktop could not read that request."
        "no_checkout" -> "The desktop has no forkmesh/forkmesh checkout to run the cleanup from."
        "no_python" -> "python3 is not installed on the desktop."
        "cleanup_failed" -> "The cleanup step failed on the desktop."
        "busy" -> "Another disk cleanup step is still running on the desktop."
        else -> null
    }

    fun subtitle(row: DesktopWorkflowRow, relative: (Long) -> String): String {
        val parts = mutableListOf(row.kindLabel, row.repo)
        when {
            row.running -> parts.add(if (row.runningStep.isEmpty()) "Running" else "Running ${row.runningStep}")
            row.lastRunAt > 0 -> parts.add(WorkflowsText.status(row.lastStatus) + " " + relative(row.lastRunAt))
            row.trigger.isNotEmpty() -> parts.add(row.trigger)
        }
        if (!row.enabled) parts.add("Off")
        return parts.filter { it.isNotEmpty() }.joinToString(" · ")
    }

    fun facts(row: DesktopWorkflowRow, at: (Long) -> String): String {
        val parts = mutableListOf(row.kindLabel, row.repo)
        if (row.trigger.isNotEmpty()) parts.add(row.trigger)
        parts.add(if (row.steps == 1) "1 step" else "${row.steps} steps")
        if (row.runs > 0) parts.add(if (row.failedRuns > 0) "${row.runs} runs, ${row.failedRuns} failed" else "${row.runs} runs")
        if (row.nextRunAt > 0 && row.enabled) parts.add("Next ${at(row.nextRunAt)}")
        return parts.filter { it.isNotEmpty() }.joinToString(" · ")
    }
}

class DesktopWorkflowsStore(channel: ControlCalling, private val launch: (() -> Unit) -> Unit = { it() }) : OpsState() {
    val family = ControlFamilyChannel(DesktopControlFamilyName.Workflows, channel)
    private val lock = Any()
    private var tap: Any? = family.addEventTap { suffix, _ ->
        if (suffix == "changed") {
            bells++
            launch { refresh() }
        }
    }
    private var refreshing = false
    private var again = false

    @Volatile
    var bells = 0
        private set

    @Volatile
    var rows: List<DesktopWorkflowRow> = emptyList()
        private set

    @Volatile
    var desktop = ""
        private set

    @Volatile
    var loaded = false
        private set

    @Volatile
    var loading = false
        private set

    @Volatile
    var error = ""
        private set

    @Volatile
    var unsupported = false
        private set

    fun refresh() {
        synchronized(lock) {
            if (refreshing) {
                again = true
                return
            }
            refreshing = true
        }
        loading = true
        changed()
        try {
            do {
                synchronized(lock) { again = false }
                try {
                    val reply = family.call("list")
                    rows = reply.maps("workflows").map(DesktopWorkflowRow::of)
                    desktop = reply.str("desktop")
                    error = ""
                    unsupported = false
                } catch (failure: Exception) {
                    unsupported = failure is LocalControlException && (failure.code == "unsupported" || failure.code == "unknown_method")
                    error = DesktopWorkflowsText.describe(failure)
                }
                loaded = true
            } while (synchronized(lock) { again })
        } finally {
            synchronized(lock) { refreshing = false }
            loading = false
            changed()
        }
    }

    fun row(id: String): DesktopWorkflowRow? = rows.firstOrNull { it.id == id }

    fun detail(row: DesktopWorkflowRow): DesktopWorkflowDetail =
        DesktopWorkflowDetail.of(family.call("get", linkedMapOf("repo" to row.repo, "id" to row.workflowId)))

    fun prepare(action: String, row: DesktopWorkflowRow, assignment: String = ""): DesktopWorkflowConfirmation {
        val reply = family.call("prepare", linkedMapOf("action" to action, "repo" to row.repo, "id" to row.workflowId))
        val token = reply.str("token")
        if (token.isEmpty()) throw LocalControlException("bad_answer")
        return DesktopWorkflowConfirmation(token, action, row.repo, row.workflowId, reply.str("summary"), assignment)
    }

    fun perform(confirmation: DesktopWorkflowConfirmation): String {
        val params = linkedMapOf<String, Any?>(
            "action" to confirmation.action, "repo" to confirmation.repo, "id" to confirmation.workflowId, "token" to confirmation.token,
        )
        val assignment = confirmation.assignment.trim()
        if (assignment.isNotEmpty()) params["assignment"] = assignment
        val reply = family.call("run", params, 45_000)
        refresh()
        return reply.str("message")
    }

    fun stop() {
        val token = synchronized(lock) { tap.also { tap = null } } ?: return
        family.removeEventTap(token)
    }
}

object WorkflowDesktopSteps {
    val steps: Map<String, String> = mapOf(
        "desktop.diskUsage" to "usage",
        "desktop.cleanAgents" to "agents",
        "desktop.cleanTests" to "tests",
        "desktop.cleanVersions" to "versions",
        "desktop.cleanCaches" to "caches",
        "desktop.diskReport" to "report",
    )
    const val STEP_TIMEOUT_MS = 20L * 60 * 1000

    fun step(type: String): String? = steps[type]

    fun hasDesktopSteps(draft: WorkflowDraft): Boolean = draft.nodes.any { it.type in steps }

    fun stepNames(draft: WorkflowDraft): List<String> = draft.ordered.filter { it.type in steps && !it.disabled }.map { it.name }

    fun confirmMessage(draft: WorkflowDraft): String {
        val names = stepNames(draft)
        return (if (names.isEmpty()) "" else names.joinToString(", ") + ". ") + WorkflowsText.DESKTOP_RUN_NOTE
    }

    fun bytes(value: Long): String {
        var size = maxOf(0L, value).toDouble()
        for (unit in listOf("B", "KB", "MB", "GB")) {
            if (size < 1024) return if (unit == "B") "${size.toLong()} B" else String.format(Locale.ROOT, "%.1f %s", size, unit)
            size /= 1024
        }
        return String.format(Locale.ROOT, "%.1f TB", size)
    }
}

class WorkflowDesktopRunner(
    private val channel: ControlCalling?,
    private val api: WorkflowsApi,
    private val now: () -> Long = System::currentTimeMillis,
) {
    fun run(draft: WorkflowDraft, live: (Map<String, String>) -> Unit = {}): WorkflowRun {
        val started = now()
        val channel = channel ?: return WorkflowRun.of(
            linkedMapOf("status" to "error", "error" to WorkflowsText.DESKTOP_MISSING, "trigger" to "desktop"), phone = false,
            id = "desktop-$started", startedAt = started, finishedAt = started,
        )
        val family = ControlFamilyChannel(DesktopControlFamilyName.Diagnostics, channel)
        val states = LinkedHashMap<String, String>()
        fun mark(id: String, state: String) {
            states[id] = state
            live(LinkedHashMap(states))
        }
        val report = LinkedHashMap<String, Any?>()
        var failed = ""
        var freed = 0L
        var free: Long? = null
        var total: Long? = null
        fun passed(): Map<String, Any?> = linkedMapOf(
            "status" to "success", "ms" to 0L,
            "outputs" to linkedMapOf("main" to linkedMapOf("count" to 1L, "items" to listOf(linkedMapOf<String, Any?>()))),
        )
        for (node in draft.ordered) {
            val stepStarted = now()
            if (node.isTrigger) {
                report[node.id] = passed()
                mark(node.id, "success")
                continue
            }
            val fed = draft.inputs(node.id).any { (report[it.from] as? Map<*, *>)?.get("status") == "success" }
            if (failed.isNotEmpty() || !fed) {
                report[node.id] = linkedMapOf("status" to "skipped")
                mark(node.id, "skipped")
                continue
            }
            val step = WorkflowDesktopSteps.step(node.type)
            if (node.disabled || step == null) {
                report[node.id] = passed()
                mark(node.id, "success")
                continue
            }
            mark(node.id, "running")
            try {
                val prepared = family.call("prepare", linkedMapOf("action" to "freeDiskSpace", "step" to step), 30_000)
                val result = family.call(
                    "run", linkedMapOf("action" to "freeDiskSpace", "step" to step, "token" to prepared.str("token")),
                    WorkflowDesktopSteps.STEP_TIMEOUT_MS,
                )
                val stepFreed = result.long("freedBytes")
                val stepFree = result.long("freeBytes")
                freed += stepFreed
                val stepTotal = result.long("totalBytes")
                if (stepTotal > 0) {
                    free = stepFree
                    total = stepTotal
                }
                val item = linkedMapOf<String, Any?>(
                    "step" to step, "freedBytes" to stepFreed, "freed" to WorkflowDesktopSteps.bytes(stepFreed),
                    "items" to result.long("items"), "freeBytes" to stepFree, "free" to WorkflowDesktopSteps.bytes(stepFree),
                )
                report[node.id] = linkedMapOf(
                    "status" to "success", "ms" to now() - stepStarted,
                    "outputs" to linkedMapOf("main" to linkedMapOf("count" to 1L, "items" to listOf(item))),
                    "logs" to result.list("lines").map { it?.toString().orEmpty() },
                )
                mark(node.id, "success")
            } catch (thrown: Exception) {
                val text = DesktopWorkflowsText.describe(thrown)
                failed = "${node.name}: $text"
                report[node.id] = linkedMapOf("status" to "error", "ms" to now() - stepStarted, "error" to text)
                mark(node.id, "error")
            }
        }
        val finished = now()
        val output = linkedMapOf<String, Any?>("freedBytes" to freed)
        if (free != null && total != null) {
            output["freeBytes"] = free
            output["totalBytes"] = total
        }
        val local = WorkflowRun.of(
            linkedMapOf("status" to (if (failed.isEmpty()) "success" else "error"), "error" to failed, "nodes" to report, "trigger" to "desktop"),
            phone = false, id = "desktop-$started", startedAt = started, finishedAt = finished,
        )
        if (draft.id.isEmpty()) return local
        return try {
            api.record(
                draft.id,
                linkedMapOf("startedAt" to started, "finishedAt" to finished, "nodes" to report, "output" to output),
            )
        } catch (thrown: Exception) {
            local.copy(error = listOf(failed, "The run finished but ForkMesh could not save it.").filter { it.isNotEmpty() }.joinToString(" "))
        }
    }
}
