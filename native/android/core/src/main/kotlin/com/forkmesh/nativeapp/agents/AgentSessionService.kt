package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.localrepos.GitAuthor
import com.forkmesh.nativeapp.localrepos.GitWorkspace
import com.forkmesh.nativeapp.model.dartDateTimeTryParseMs
import java.time.Instant
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

enum class AgentTaskPriority { High, Normal }

enum class AgentTaskState { Queued, Running, Done, Failed, Cancelled }

data class AgentTask(
    val id: String,
    val repoName: String,
    val prompt: String,
    val priority: AgentTaskPriority,
    val createdAtMs: Long,
    val state: AgentTaskState = AgentTaskState.Queued,
    val images: List<PromptAttachment> = emptyList(),
)

data class AgentBranchRecord(
    val sessionId: String,
    val repoName: String,
    val branch: String,
    val baseBranch: String,
    val startedAtMs: Long,
    val finishedAtMs: Long? = null,
) {
    val isFinished: Boolean get() = finishedAtMs != null

    fun toJson(): Map<String, Any?> {
        val out = linkedMapOf<String, Any?>(
            "sessionId" to sessionId,
            "repoName" to repoName,
            "branch" to branch,
            "baseBranch" to baseBranch,
            "startedAt" to Instant.ofEpochMilli(startedAtMs).toString(),
        )
        if (finishedAtMs != null) out["finishedAt"] = Instant.ofEpochMilli(finishedAtMs).toString()
        return out
    }

    companion object {
        fun fromJson(value: Any?): AgentBranchRecord? {
            val map = value as? Map<*, *> ?: return null
            val started = dartDateTimeTryParseMs(jsonText(map["startedAt"])) ?: return null
            val branch = jsonText(map["branch"])
            if (branch.isEmpty()) return null
            return AgentBranchRecord(
                sessionId = jsonText(map["sessionId"]),
                repoName = jsonText(map["repoName"]),
                branch = branch,
                baseBranch = jsonText(map["baseBranch"] ?: "main"),
                startedAtMs = started,
                finishedAtMs = map["finishedAt"]?.let { dartDateTimeTryParseMs(jsonText(it)) },
            )
        }
    }
}

class AgentRunHandle internal constructor(val task: AgentTask, val branch: String, val baseBranch: String) {
    private val list = CopyOnWriteArrayList<AgentEvent>()
    private val latch = CountDownLatch(1)

    val events: List<AgentEvent> get() = list.toList()
    val isComplete: Boolean get() = latch.count == 0L

    fun awaitDone(timeoutMs: Long = Long.MAX_VALUE): Boolean = latch.await(timeoutMs, TimeUnit.MILLISECONDS)

    internal fun add(event: AgentEvent) = list.add(event)

    internal fun complete() = latch.countDown()
}

enum class AgentStartRefusal { NotConfigured, DirtyWorktree, AlreadyRunning }

class AgentStartException(val refusal: AgentStartRefusal, override val message: String) : RuntimeException(message) {
    override fun toString(): String = message
}

class AgentSessionService(
    private val prefs: PrefStore,
    scopePrefix: String = "",
    private val staleAfterMs: Long = 15L * 24 * 60 * 60 * 1000,
    private val clock: () -> Long = { System.currentTimeMillis() },
    private val launcher: (Runnable) -> Unit = { Thread(it, "AgentSession").apply { isDaemon = true }.start() },
) {
    companion object {
        const val RECORDS_KEY = "agent/branch-records-v1"

        private val FILLER = setOf(
            "a", "an", "and", "the", "to", "of", "in", "on", "for", "please", "can", "you", "my", "is", "it", "that",
            "this", "with", "add", "make",
        )

        fun agentBranchName(sessionId: String, prompt: String): String {
            val words = prompt.lowercase()
                .replace(Regex("[^a-z0-9\\s-]"), " ")
                .split(Regex("\\s+"))
                .filter { it.isNotEmpty() && it !in FILLER }
                .take(4)
                .joinToString("-")
            val suffix = if (sessionId.length > 6) sessionId.substring(sessionId.length - 6) else sessionId
            val slug = words.ifEmpty { "task" }
            return "agent/$suffix-${if (slug.length > 32) slug.substring(0, 32) else slug}"
        }
    }

    private val recordsPrefsKey = scopePrefix + RECORDS_KEY
    private val lock = Any()
    private val queueList = ArrayList<AgentTask>()
    private val recordList = ArrayList<AgentBranchRecord>()
    private val listeners = CopyOnWriteArrayList<() -> Unit>()

    @Volatile
    private var active: CodingAgent? = null

    @Volatile
    private var activeRepo: String? = null

    @Volatile
    private var activeRunHandle: AgentRunHandle? = null

    @Volatile
    private var lastRunHandle: AgentRunHandle? = null

    private var nextId = 0L

    val queue: List<AgentTask> get() = synchronized(lock) { queueList.toList() }
    val records: List<AgentBranchRecord> get() = synchronized(lock) { recordList.toList() }
    val isRunning: Boolean get() = active != null
    val runningRepo: String? get() = activeRepo

    val activeRun: AgentRunHandle?
        get() = activeRunHandle ?: lastRunHandle?.takeIf { !it.isComplete }

    val lastRun: AgentRunHandle? get() = activeRunHandle ?: lastRunHandle

    fun addListener(listener: () -> Unit) = listeners.add(listener)

    fun removeListener(listener: () -> Unit) = listeners.remove(listener)

    private fun notifyListeners() {
        for (listener in listeners) runCatching { listener() }
    }

    fun load() {
        val decoded = try {
            (Json.parse(prefs.getString(recordsPrefsKey) ?: "") as? List<*>)?.mapNotNull(AgentBranchRecord::fromJson)
        } catch (_: Exception) {
            null
        } ?: emptyList()
        synchronized(lock) {
            recordList.clear()
            recordList.addAll(decoded)
        }
        notifyListeners()
    }

    private fun persist() {
        val encoded = synchronized(lock) { recordList.map { it.toJson() } }
        prefs.putString(recordsPrefsKey, Json.stringify(encoded))
    }

    fun submit(
        repoName: String,
        prompt: String,
        priority: AgentTaskPriority = AgentTaskPriority.Normal,
        id: String? = null,
        images: List<PromptAttachment> = emptyList(),
    ): AgentTask? {
        var trimmed = prompt.trim()
        if (trimmed.isEmpty() && images.isEmpty()) return null
        val agent = active
        if (agent != null && activeRepo == repoName) {
            agent.steer(trimmed, images)
            return null
        }
        if (trimmed.isEmpty()) trimmed = "See the attached photos."
        val now = clock()
        val task = AgentTask(
            id = id ?: "task-${now * 1000 + (synchronized(lock) { nextId++ } % 1000)}",
            repoName = repoName,
            prompt = trimmed,
            priority = priority,
            createdAtMs = now,
            images = images,
        )
        synchronized(lock) { queueList.add(task) }
        notifyListeners()
        return task
    }

    val nextTask: AgentTask?
        get() = synchronized(lock) {
            queueList.filter { it.state == AgentTaskState.Queued }
                .sortedWith(compareBy<AgentTask>({ it.priority.ordinal }, { it.createdAtMs }))
                .firstOrNull()
        }

    private fun setTaskState(id: String, state: AgentTaskState) {
        synchronized(lock) {
            val index = queueList.indexOfFirst { it.id == id }
            if (index < 0) return
            queueList[index] = queueList[index].copy(state = state)
        }
        notifyListeners()
    }

    fun removeTask(id: String) {
        synchronized(lock) { queueList.removeAll { it.id == id } }
        notifyListeners()
    }

    fun cancelActive() {
        active?.cancel()
        notifyListeners()
    }

    fun start(
        task: AgentTask,
        workspace: GitWorkspace,
        buildAgent: (AgentToolset) -> CodingAgent,
        author: GitAuthor,
        readOnly: Boolean = false,
    ): AgentRunHandle {
        synchronized(lock) {
            if (active != null || activeRunHandle != null) {
                throw AgentStartException(
                    AgentStartRefusal.AlreadyRunning,
                    "An agent session is already running. Wait for it, or steer it with a follow-up message.",
                )
            }
        }
        sweepStaleRecords(workspace, task.repoName)
        val status = workspace.status()
        if (!status.isClean) {
            val count = status.changeCount
            throw AgentStartException(
                AgentStartRefusal.DirtyWorktree,
                "This repository has $count uncommitted ${if (count == 1) "change" else "changes"}. Commit or discard " +
                    "them first — an agent session needs a clean starting point so its own edits stay separable from yours.",
            )
        }
        val baseBranch = workspace.branches().filter { it.isCurrent }.joinToString("") { it.name }
        val branchName = agentBranchName(task.id, task.prompt)
        val record = AgentBranchRecord(task.id, task.repoName, branchName, baseBranch.ifEmpty { "main" }, clock())
        synchronized(lock) { recordList.add(record) }
        persist()
        setTaskState(task.id, AgentTaskState.Running)
        workspace.createBranch(branchName, checkout = true)

        val handle = AgentRunHandle(task, record.branch, record.baseBranch)
        activeRunHandle = handle
        lastRunHandle = handle
        launcher(Runnable { execute(handle, record, task, workspace, buildAgent, author, readOnly) })
        return handle
    }

    private fun execute(
        handle: AgentRunHandle,
        record: AgentBranchRecord,
        task: AgentTask,
        workspace: GitWorkspace,
        buildAgent: (AgentToolset) -> CodingAgent,
        author: GitAuthor,
        readOnly: Boolean,
    ) {
        fun emit(event: AgentEvent) {
            handle.add(event)
            notifyListeners()
        }
        emit(AgentNotice("Working on branch ${record.branch}."))
        val toolset = AgentToolset(workspace, readOnly).also { it.author = author }
        val agent = buildAgent(toolset)
        active = agent
        activeRepo = task.repoName
        notifyListeners()
        var failed = false
        try {
            agent.run(task.prompt, task.images) { event ->
                if (event is AgentDone) {
                    failed = event.reason == AgentStopReason.Error || event.reason == AgentStopReason.Refused
                }
                emit(event)
            }
        } catch (error: Throwable) {
            failed = true
            emit(AgentNotice("The run stopped unexpectedly: ${error.message ?: error}", isError = true))
        } finally {
            active = null
            activeRepo = null
            runCatching { agent.dispose() }
            try {
                finishSession(record, workspace)?.let { emit(AgentNotice(it)) }
            } catch (error: Throwable) {
                emit(AgentNotice("Could not restore the base branch: ${error.message ?: error}", isError = true))
            }
            setTaskState(task.id, if (failed) AgentTaskState.Failed else AgentTaskState.Done)
            activeRunHandle = null
            handle.complete()
            notifyListeners()
        }
    }

    private fun finishSession(record: AgentBranchRecord, workspace: GitWorkspace): String? {
        replaceRecord(record, record.copy(finishedAtMs = clock()))
        persist()
        return restoreBase(record, workspace)
    }

    private fun restoreBase(record: AgentBranchRecord, workspace: GitWorkspace): String? = try {
        if (!workspace.status().isClean) {
            "Left you on ${record.branch}: it has uncommitted changes. Review them, then switch back to ${record.baseBranch} yourself."
        } else {
            val branches = workspace.branches()
            val current = branches.filter { it.isCurrent }.joinToString("") { it.name }
            if (current != record.branch || branches.none { it.name == record.baseBranch }) {
                null
            } else {
                workspace.checkout(record.baseBranch)
                "Back on ${record.baseBranch}. The work is on ${record.branch}."
            }
        }
    } catch (_: Exception) {
        null
    }

    private fun replaceRecord(from: AgentBranchRecord, to: AgentBranchRecord) = synchronized(lock) {
        val index = recordList.indexOfFirst { it.sessionId == from.sessionId }
        if (index >= 0) recordList[index] = to else recordList.add(to)
    }

    fun sweepStaleRecords(workspace: GitWorkspace? = null, repoName: String? = null, nowMs: Long = clock()): List<AgentBranchRecord> {
        val stale = synchronized(lock) {
            recordList.filter { !it.isFinished && nowMs - it.startedAtMs >= staleAfterMs }
        }
        if (stale.isEmpty()) return emptyList()
        for (record in stale) {
            if (workspace != null && record.repoName == repoName) restoreBase(record, workspace)
            synchronized(lock) { recordList.removeAll { it.sessionId == record.sessionId } }
        }
        persist()
        notifyListeners()
        return stale
    }

    fun dispose() {
        active?.cancel()
    }
}
