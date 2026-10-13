package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.model.PublishedPull
import com.forkmesh.nativeapp.model.dartDateTimeTryParseMs
import com.forkmesh.nativeapp.model.dartDoubleTryParse

internal fun launchMs(value: Any?): Long = when (value) {
    is Number -> {
        val n = value.toLong()
        if (n in 1 until 100_000_000_000L) n * 1000 else n
    }
    is String -> dartDateTimeTryParseMs(value) ?: dartDoubleTryParse(value.trim())?.let { launchMs(it) } ?: 0L
    else -> 0L
}

private fun launchStr(value: Any?): String = if (value == null) "" else jsonText(value)

private fun launchStrings(value: Any?): List<String> =
    (value as? List<*>)?.map(::launchStr)?.filter { it.isNotEmpty() } ?: emptyList()

private fun launchNumber(value: Any?): Int = when (value) {
    is Number -> value.toInt()
    else -> launchStr(value).trim().toIntOrNull() ?: 0
}

data class AgentLaunchTarget(
    val node: String,
    val providers: List<String> = emptyList(),
    val online: Boolean = false,
    val lastSeenAtMs: Long = 0,
    val source: String = "",
    val runtimeMode: String = "",
    val runner: String = "",
    val needsLogin: List<String> = emptyList(),
    val queueNode: String = "",
    val runnerPin: String = "",
) {
    val isSource: Boolean get() = source.lowercase() == "true"

    val isMirror: Boolean
        get() {
            val runtime = runtimeMode.lowercase()
            if (runtime.isNotEmpty()) return runtime == "headless" || runtime.contains("mirror") || isRunner
            return !isSource
        }

    val isRunner: Boolean get() = runtimeMode.lowercase().startsWith("runner")

    val signInNode: String get() = runner.trim().takeIf { isRunner && it.isNotEmpty() } ?: node

    fun runs(provider: String): Boolean = provider.isEmpty() || provider == "auto" || provider in providers

    val statusLabel: String get() = if (online) "online" else "offline"

    companion object {
        fun fromJson(json: JsonMap): AgentLaunchTarget = AgentLaunchTarget(
            node = launchStr(json["node"]),
            providers = launchStrings(json["providers"]),
            online = json["online"] == true,
            lastSeenAtMs = launchMs(json["lastSeenAt"]),
            source = launchStr(json["source"]),
            runtimeMode = launchStr(json["runtimeMode"]),
            runner = launchStr(json["runner"]),
            needsLogin = (json["needsLogin"] as? Map<*, *>)?.keys?.map { it.toString() } ?: emptyList(),
            queueNode = launchStr(json["queueNode"]),
            runnerPin = launchStr(json["runnerPin"]),
        )
    }
}

data class AgentLaunchMessage(
    val role: String = "",
    val author: String = "",
    val text: String = "",
    val atMs: Long = 0,
    val state: String = "",
    val attachments: List<AgentAttachmentMeta> = emptyList(),
) {
    val isUser: Boolean get() = role.lowercase() == "user"

    val isStopRequest: Boolean get() = state.lowercase() == "stopping"

    val isTask: Boolean get() = isUser && !isStopRequest

    companion object {
        fun fromJson(json: JsonMap): AgentLaunchMessage = AgentLaunchMessage(
            role = launchStr(json["role"]),
            author = launchStr(json["author"]),
            text = launchStr(json["text"]),
            atMs = launchMs(json["at"]),
            state = launchStr(json["state"]),
            attachments = AgentAttachmentMeta.list(json["attachments"], limit = PromptAttachmentLimits.MAX_COUNT),
        )
    }
}

data class AgentLaunchSession(
    val sessionId: String,
    val owner: String = "",
    val repo: String = "",
    val provider: String = "",
    val status: String = "",
    val displayStatus: String = "",
    val targetNode: String = "",
    val title: String = "",
    val createdBy: String = "",
    val createdAtMs: Long = 0,
    val updatedAtMs: Long = 0,
    val completedAtMs: Long = 0,
    val requestedModel: String = "",
    val createPr: Boolean = false,
    val issueNumber: Int = 0,
    val history: List<AgentLaunchMessage> = emptyList(),
    val diagnostic: String = "",
    val localAgentId: Int = 0,
    val taskKey: String = "",
    val branchName: String = "",
    val baseBranch: String = "",
    val prNumber: Int = 0,
    val merged: Boolean = false,
    val mergedAtMs: Long = 0,
    val lastError: String = "",
    val orgScope: Boolean = false,
    val liveText: String = "",
    val liveAtMs: Long = 0,
    val runner: String = "",
    val diffStats: RunnerDiffStats = RunnerDiffStats(),
    val merge: RunnerMergeState = RunnerMergeState(),
    val canMerge: Boolean = false,
    val canOpenPull: Boolean = false,
    val stopRequestedAtMs: Long = 0,
) {
    val isOnRunner: Boolean get() = runner.isNotEmpty()

    val isMergedBranch: Boolean get() = merged || merge.isMerged

    val needsCleanup: Boolean
        get() = isMergedBranch && branchName.isNotEmpty() && (!merge.branchDeleted || merge.cleanupPending.isNotEmpty())

    val normalizedStatus: String get() = status.lowercase().replace('_', '-')

    val isActive: Boolean get() = normalizedStatus in setOf("security-pending", "queued", "running")

    val isComplete: Boolean get() = normalizedStatus == "completed"

    val isFailed: Boolean get() = normalizedStatus == "failed" || normalizedStatus == "rejected"

    val isStopped: Boolean
        get() = normalizedStatus in setOf("stopped", "cancelled", "canceled", "interrupted") ||
            (stopRequestedAtMs > 0 && isFailed)

    val isPromptable: Boolean
        get() = localAgentId > 0 && normalizedStatus in setOf("queued", "running", "completed", "failed")

    val firstPrompt: String get() = history.firstOrNull { it.isTask }?.text?.trim() ?: ""

    val retryPrompt: String get() = firstPrompt.ifEmpty { title.trim() }

    val isRetryable: Boolean get() = (isFailed || isStopped) && retryPrompt.isNotEmpty()

    val canResume: Boolean get() = isPromptable

    val statusLabel: String get() = if (isStopped) "stopped" else displayStatus.ifEmpty { status.replace('_', ' ') }

    val displayTitle: String
        get() {
            if (title.trim().isNotEmpty()) return title.trim()
            val text = history.firstOrNull { it.isTask }?.text?.trim() ?: ""
            if (text.isNotEmpty()) return text.split('\n').first()
            return "Agent session"
        }

    val diagnosticNote: String
        get() {
            val note = diagnostic.trim()
            return if (note == lastError.trim()) "" else note
        }

    fun attachmentRefs(message: AgentLaunchMessage, owner: String, repo: String): List<AgentAttachmentRef> =
        message.attachments.map {
            AgentAttachmentRef(it, AttachmentOrigin.Relay(owner, repo, sessionId, orgScope))
        }

    companion object {
        fun fromJson(json: JsonMap, orgScope: Boolean = false): AgentLaunchSession {
            val info = json.obj("agentInfo")
            fun fact(key: String): String = launchStr(json[key]).ifEmpty { launchStr(info[key]) }
            val issue = launchNumber(json["issueNumber"])
            val pr = launchNumber(json["prNumber"])
            val baseBranch = fact("baseBranch")
            val diagnostic = json["diagnostic"]
            val live = json.obj("live")
            return AgentLaunchSession(
                sessionId = launchStr(json["id"]).ifEmpty { launchStr(json["sessionId"]) },
                owner = launchStr(json["org"]),
                repo = launchStr(json["repo"]),
                provider = launchStr(json["provider"]),
                status = launchStr(json["status"]),
                displayStatus = launchStr(json["displayStatus"]),
                targetNode = launchStr(json["targetNode"]),
                title = launchStr(json["title"]),
                createdBy = launchStr(json["createdBy"]),
                createdAtMs = launchMs(json["createdAt"]),
                updatedAtMs = launchMs(json["updatedAt"]),
                completedAtMs = launchMs(json["completedAt"]),
                requestedModel = launchStr(json["requestedModel"]),
                createPr = json["createPr"] == true || info["createPr"] == true,
                issueNumber = if (issue > 0) issue else launchNumber(info["issueNumber"]),
                localAgentId = launchNumber(json["localAgentId"]),
                taskKey = launchStr(json["taskKey"]),
                branchName = launchStr(json["branch"]).ifEmpty { fact("branchName") },
                baseBranch = baseBranch.ifEmpty { fact("baseRef") },
                prNumber = if (pr > 0) pr else launchNumber(info["prNumber"]),
                merged = json["merged"] == true || info["merged"] == true || RunnerMergeState.fromJson(json.obj("merge")).isMerged,
                mergedAtMs = launchMs(info["mergedAt"]),
                lastError = fact("lastError"),
                orgScope = orgScope,
                history = json.maps("history").map(AgentLaunchMessage::fromJson),
                diagnostic = if (diagnostic is Map<*, *>) launchStr(diagnostic["message"]) else "",
                liveText = launchStr(live["text"]),
                liveAtMs = launchMs(live["at"]),
                runner = launchStr(json["runner"]),
                diffStats = RunnerDiffStats.fromJson(json.obj("diffStats")),
                merge = RunnerMergeState.fromJson(json.obj("merge")),
                canMerge = json["canMerge"] == true,
                canOpenPull = json["canOpenPull"] == true,
                stopRequestedAtMs = launchMs(json["stopRequestedAt"]),
            )
        }
    }
}

fun launchRunsInCloud(session: AgentLaunchSession, targets: List<AgentLaunchTarget>): Boolean {
    if (session.isOnRunner) return true
    val node = session.targetNode.trim().lowercase()
    if (node.isEmpty()) return false
    for (target in targets) {
        if (target.node.trim().lowercase() == node) return target.isMirror
    }
    return false
}

data class AgentLaunchOptions(
    val canQueue: Boolean = false,
    val targets: List<AgentLaunchTarget> = emptyList(),
    val providers: List<String> = emptyList(),
    val models: Map<String, List<String>> = emptyMap(),
    val sessions: List<AgentLaunchSession> = emptyList(),
    val orgScope: Boolean = false,
    val accessReason: String = "",
    val finishedBefore: Long = 0,
) {
    val hasTarget: Boolean get() = targets.isNotEmpty()

    val preferredTarget: AgentLaunchTarget?
        get() {
            if (targets.isEmpty()) return null
            return targets.firstOrNull { it.online && it.isMirror }
                ?: targets.firstOrNull { it.online }
                ?: targets.firstOrNull { it.isMirror }
                ?: targets.first()
        }

    fun targetNamed(node: String): AgentLaunchTarget? {
        if (node.isEmpty()) return null
        return targets.firstOrNull { it.node == node }
            ?: targets.firstOrNull { it.isRunner && it.runner == node }
            ?: targets.firstOrNull { it.isRunner && it.runnerPin.isEmpty() && it.queueNode == node }
    }

    companion object {
        fun fromJson(json: JsonMap, orgScope: Boolean = false): AgentLaunchOptions {
            val models = LinkedHashMap<String, List<String>>()
            val rawModels = json["models"]
            if (rawModels is Map<*, *>) {
                for ((key, value) in rawModels) models[launchStr(key)] = launchStrings(value)
            }
            return AgentLaunchOptions(
                orgScope = orgScope,
                accessReason = launchStr(json["accessReason"]),
                canQueue = json["canQueueAgent"] == true || json["engineeringAccess"] == true,
                targets = json.maps("targets").map(AgentLaunchTarget::fromJson),
                providers = launchStrings(json["providers"]),
                models = models,
                sessions = json.maps("sessions").map { AgentLaunchSession.fromJson(it, orgScope) },
                finishedBefore = launchMs(json["finishedBefore"]),
            )
        }
    }
}

data class AgentAccountSessions(
    val sessions: List<AgentLaunchSession> = emptyList(),
    val finishedBefore: Long = 0,
) {
    companion object {
        fun fromJson(json: JsonMap): AgentAccountSessions = AgentAccountSessions(
            sessions = json.maps("sessions").map { AgentLaunchSession.fromJson(it) },
            finishedBefore = launchMs(json["finishedBefore"]),
        )
    }
}

enum class AgentStageState { Pending, Active, Done, Failed }

data class AgentTaskStage(
    val key: String,
    val label: String,
    val detail: String,
    val state: AgentStageState,
)

class AgentTaskChain(val session: AgentLaunchSession, val pullStatus: String = "", pullNumber: Int = 0) {
    val prNumber: Int = if (session.prNumber > 0) session.prNumber else pullNumber

    val isMerged: Boolean get() = session.merged || pullStatus.trim().lowercase() == "merged"

    val isPullClosed: Boolean get() = pullStatus.trim().lowercase() == "closed"

    val stages: List<AgentTaskStage> by lazy { build() }

    private fun build(): List<AgentTaskStage> {
        val s = session
        val claimed = s.localAgentId > 0 || s.branchName.isNotEmpty() || s.normalizedStatus in setOf("running", "completed")
        val node = s.targetNode.ifEmpty { "a mirror" }
        val hasBranch = s.branchName.isNotEmpty()
        val hasPull = prNumber > 0
        val failed = s.isFailed

        fun after(reached: Boolean, previous: Boolean): AgentStageState = when {
            reached -> AgentStageState.Done
            failed -> if (previous) AgentStageState.Failed else AgentStageState.Pending
            else -> if (previous) AgentStageState.Active else AgentStageState.Pending
        }

        val mirror = if (claimed) {
            if (s.isActive && !hasBranch) AgentStageState.Active else AgentStageState.Done
        } else {
            after(false, true)
        }
        val branch = if (hasBranch) {
            if (s.isActive && !hasPull) AgentStageState.Active else AgentStageState.Done
        } else {
            after(false, claimed)
        }
        val pull = when {
            hasPull -> AgentStageState.Done
            failed && hasBranch && s.createPr -> AgentStageState.Failed
            else -> AgentStageState.Pending
        }
        val merge = when {
            isMerged -> AgentStageState.Done
            isPullClosed -> AgentStageState.Failed
            hasPull -> AgentStageState.Active
            else -> AgentStageState.Pending
        }
        val mirrorDetail = when {
            mirror == AgentStageState.Done -> "Ran on $node"
            mirror == AgentStageState.Active && claimed -> "Running on $node"
            mirror == AgentStageState.Active -> "Waiting for $node to pick it up"
            mirror == AgentStageState.Failed -> "Never started on $node"
            else -> node
        }
        val branchDetail = when {
            hasBranch -> if (s.baseBranch.isEmpty()) s.branchName else "${s.branchName} from ${s.baseBranch}"
            branch == AgentStageState.Failed -> "No branch was pushed"
            else -> "Named when the mirror starts work"
        }
        val pullDetail = when {
            hasPull -> "#$prNumber"
            !s.createPr -> "Not requested"
            pull == AgentStageState.Failed -> "Not opened"
            s.isComplete -> "None was opened — the branch did not change"
            else -> "Opens when the agent finishes"
        }
        val mergeDetail = when {
            isMerged -> "Merged"
            isPullClosed -> "Closed without merging"
            hasPull -> "Open — review and merge #$prNumber"
            else -> "After the pull request"
        }
        return listOf(
            AgentTaskStage("prompt", "Prompt", if (s.createdBy.isEmpty()) "Sent" else "Sent by @${s.createdBy}", AgentStageState.Done),
            AgentTaskStage("mirror", "Mirror", mirrorDetail, mirror),
            AgentTaskStage("branch", "Branch", branchDetail, branch),
            AgentTaskStage("pull", "Pull request", pullDetail, pull),
            AgentTaskStage("merge", "Merge", mergeDetail, merge),
        )
    }

    val summary: String
        get() {
            if (isMerged) return "Merged"
            if (prNumber > 0) return if (isPullClosed) "PR #$prNumber closed" else "PR #$prNumber open"
            if (session.branchName.isNotEmpty()) return "On ${session.branchName}"
            return ""
        }
}

fun agentTaskChain(session: AgentLaunchSession, pulls: List<PublishedPull>): AgentTaskChain {
    val pull = pulls.firstOrNull { item ->
        if (session.prNumber > 0) item.number == session.prNumber
        else session.branchName.isNotEmpty() && item.head == session.branchName
    }
    return AgentTaskChain(session, pullStatus = pull?.status ?: "", pullNumber = pull?.number ?: 0)
}

fun agentRetryDetail(session: AgentLaunchSession): String {
    val node = session.targetNode.ifEmpty { "the mirror" }
    return if (session.canResume) {
        "Sends the same task back to $node, which picks this session up on the branch it already cut."
    } else {
        "No node ever started this one, so there is nothing to resume. The same task is queued again on $node."
    }
}

data class AgentQueueResult(
    val queueState: String = "",
    val targetOnline: Boolean = false,
    val message: String = "",
    val session: AgentLaunchSession? = null,
) {
    val waitingForNode: Boolean get() = queueState == "waiting_for_desktop" || !targetOnline

    val label: String
        get() = when {
            message.isNotEmpty() -> message
            waitingForNode -> "Queued. It starts as soon as the node connects."
            else -> "Queued. The node is picking it up now."
        }

    companion object {
        fun fromJson(json: JsonMap): AgentQueueResult {
            val session = json["session"]
            @Suppress("UNCHECKED_CAST")
            return AgentQueueResult(
                queueState = launchStr(json["queueState"]),
                targetOnline = json["targetOnline"] == true,
                message = launchStr(json["message"]),
                session = if (session is Map<*, *>) AgentLaunchSession.fromJson(session as JsonMap) else null,
            )
        }
    }
}

data class AgentRetryResult(val result: AgentQueueResult, val resumed: Boolean) {
    val session: AgentLaunchSession? get() = result.session

    val label: String
        get() = when {
            result.message.isNotEmpty() -> result.message
            resumed -> "Sent back to the mirror. It picks the session up on its branch."
            else -> result.label
        }
}

data class AgentStopResult(
    val stopped: Boolean = false,
    val outcome: String = "",
    val message: String = "",
) {
    val label: String
        get() = message.ifEmpty { if (stopped) "Stopped." else "Asked the node to stop the run." }

    companion object {
        fun fromJson(json: JsonMap): AgentStopResult =
            AgentStopResult(json["stopped"] == true, launchStr(json["outcome"]), launchStr(json["message"]))
    }
}

class AgentLaunchException(override val message: String, val code: String = "", val status: Int = 0) :
    RuntimeException(message) {
    override fun toString(): String = message
}
