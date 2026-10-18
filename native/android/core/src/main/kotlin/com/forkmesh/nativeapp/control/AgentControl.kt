package com.forkmesh.nativeapp.control

import com.forkmesh.nativeapp.agents.AgentAttachmentRef
import com.forkmesh.nativeapp.agents.AgentLaunchException
import com.forkmesh.nativeapp.agents.AgentLaunchSession
import com.forkmesh.nativeapp.agents.AgentLiveEvent
import com.forkmesh.nativeapp.agents.AgentLiveStream
import com.forkmesh.nativeapp.agents.AgentQueueApi
import com.forkmesh.nativeapp.agents.AgentReadState
import com.forkmesh.nativeapp.agents.AgentsList
import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.agents.RunnerAgentOps
import com.forkmesh.nativeapp.agents.promptOrPhotoNote
import com.forkmesh.nativeapp.code.ConflictResolution
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.jsonText
import java.util.concurrent.Callable
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.ExecutionException
import java.util.concurrent.Executors

enum class AgentTransportBadge(val wire: String, val label: String) {
    DesktopLan("lan", "Desktop · LAN"),
    DesktopRelay("relay", "Desktop · relay"),
    Node("node", "Node"),
    Runner("runner", "Runner"),
    ;

    companion object {
        fun forDesktop(transport: ControlTransport): AgentTransportBadge =
            if (transport == ControlTransport.Lan) DesktopLan else DesktopRelay
    }
}

enum class OutsidePromptPath { Terminal, Peer, None }

data class UnifiedAgentRow(
    val id: String,
    val badge: AgentTransportBadge,
    val title: String,
    val repoOwner: String,
    val repoName: String,
    val node: String,
    val status: String,
    val statusLabel: String,
    val isActive: Boolean,
    val provider: String = "",
    val model: String = "",
    val updatedAtMs: Long = 0,
    val canStop: Boolean = false,
    val canPrompt: Boolean = false,
    val canContinue: Boolean = false,
    val localId: Int = 0,
    val cloudSessionId: String = "",
    val orgScope: Boolean = false,
    val outside: Boolean = false,
    val kindLabel: String = "",
    val cwd: String = "",
    val branchName: String = "",
    val baseBranch: String = "",
    val outsidePath: OutsidePromptPath = OutsidePromptPath.None,
    val outsideUser: String = "",
    val runner: String = "",
    val prNumber: Int = 0,
    val merged: Boolean = false,
    val filesChanged: Int = -1,
    val linesAdded: Int = -1,
    val linesRemoved: Int = -1,
    val behind: Int = -1,
    val conflicted: Boolean = false,
    val canMerge: Boolean = false,
    val canOpenPull: Boolean = false,
    val needsCleanup: Boolean = false,
    val terminal: String = "",
    val finishedAtMs: Long = 0,
    val unread: Boolean? = null,
) {
    val repoLabel: String get() = if (repoOwner.isEmpty()) repoName else "$repoOwner/$repoName"
    val isDesktop: Boolean get() = badge != AgentTransportBadge.Node && badge != AgentTransportBadge.Runner
    val isRunner: Boolean get() = badge == AgentTransportBadge.Runner
    val badgeLabel: String get() = if (isRunner && runner.isNotEmpty()) "Runner \u00B7 $runner" else badge.label
    val hasBranchOps: Boolean get() = !isDesktop && cloudSessionId.isNotEmpty() && branchName.isNotEmpty()
    val canMergeBranch: Boolean get() = hasBranchOps && !isActive && !merged
    val canCreatePull: Boolean get() = hasBranchOps && canOpenPull && prNumber <= 0 && !isActive && !merged
    val isPhoneTerminal: Boolean get() = outside && terminal.isNotEmpty()
    val offersTerminal: Boolean
        get() = isDesktop && !outside && localId > 0 && (provider == "claude-code" || provider == "codex")

    val outsideNote: String
        get() {
            val what = kindLabel.ifEmpty { "This agent" }
            val where = if (node.isEmpty()) "your desktop" else node
            val folder = if (cwd.isEmpty()) "" else ", in $cwd"
            if (outsideUser.isNotEmpty()) {
                return "$what is running in $outsideUser's login on $where. The desktop runs in a different " +
                    "login, so it cannot show this agent's transcript or steer it. Open ForkMesh in " +
                    "$outsideUser's login to watch and steer it there."
            }
            if (isPhoneTerminal) {
                return "$what was started from your phone in a terminal on $where$folder. Open Terminal to watch it " +
                    "live and drive it; what you send here is typed into it."
            }
            if (outsidePath == OutsidePromptPath.Terminal) {
                return "$what was started outside ForkMesh on $where$folder. The desktop cannot show its " +
                    "transcript; what you send here is typed into its terminal."
            }
            if (canPrompt) {
                return "$what was started outside ForkMesh on $where$folder. The desktop cannot show its " +
                    "transcript; you can still steer it here."
            }
            return "$what was started outside ForkMesh on $where$folder. The desktop cannot show its " +
                "transcript; follow and steer it in the terminal that started it."
        }

    val outsideSummary: String
        get() {
            val what = kindLabel.ifEmpty { "This agent" }
            val where = if (node.isEmpty()) "your desktop" else node
            val folder = if (cwd.isEmpty()) "" else " in ${cwd.trimEnd('/').substringAfterLast('/')}"
            return if (outsideUser.isNotEmpty()) {
                "$what is running in $outsideUser's login on $where. ForkMesh cannot read or steer another " +
                    "login's agents; open ForkMesh in that login to follow it."
            } else if (isPhoneTerminal) {
                "$what started from your phone in a terminal on $where$folder. Open the terminal for full control; " +
                    "what you send here is typed into it."
            } else if (outsidePath == OutsidePromptPath.Terminal) {
                "$what started outside ForkMesh on $where$folder. What you send here is typed into its terminal."
            } else if (canPrompt) {
                "$what started outside ForkMesh on $where$folder. You can watch and steer it here."
            } else {
                "$what started outside ForkMesh on $where$folder. You can watch it here; steer it in the " +
                    "terminal that started it."
            }
        }

    val composerHint: String
        get() = when {
            !outside -> "Message this agent"
            outsidePath == OutsidePromptPath.Terminal -> "Types into your terminal"
            outsidePath == OutsidePromptPath.Peer -> "Message (arrives as another session)"
            else -> "Message for ${kindLabel.ifEmpty { "the agent" }}"
        }

    companion object {
        fun outsidePath(agent: LocalOutsideAgent): OutsidePromptPath = when {
            agent.typeable -> OutsidePromptPath.Terminal
            agent.steer && agent.kind == "claude" -> OutsidePromptPath.Peer
            else -> OutsidePromptPath.None
        }

        fun fromDesktop(row: LocalAgentRow, transport: ControlTransport, desktop: String): UnifiedAgentRow {
            val session = row.session
            val updated = maxOf(session.finishedAtMs, session.startedAtMs, session.createdAtMs)
            return UnifiedAgentRow(
                id = "desktop:${session.id}",
                badge = AgentTransportBadge.forDesktop(transport),
                title = row.title,
                repoOwner = row.owner,
                repoName = row.name,
                node = desktop,
                status = session.status,
                statusLabel = session.statusLabel,
                isActive = session.isActive,
                provider = session.provider,
                model = session.model,
                updatedAtMs = updated,
                canStop = row.stoppable,
                canPrompt = true,
                canContinue = row.canContinue,
                localId = session.id,
                branchName = session.branchName,
                baseBranch = session.diffBase,
                conflicted = session.conflicted && !session.merged,
                finishedAtMs = if (session.isActive) 0 else session.finishedAtMs,
                unread = row.unread,
            )
        }

        fun fromOutside(agent: LocalOutsideAgent, transport: ControlTransport, desktop: String): UnifiedAgentRow {
            val path = outsidePath(agent)
            return UnifiedAgentRow(
                id = "outside:${agent.id}",
                badge = AgentTransportBadge.forDesktop(transport),
                title = agent.title.ifEmpty { agent.label },
                repoOwner = agent.owner,
                repoName = agent.name,
                node = desktop,
                status = "running",
                statusLabel = "Running",
                isActive = true,
                provider = agent.provider,
                model = agent.model,
                updatedAtMs = agent.lastActivityMs,
                canStop = agent.stoppable,
                canPrompt = path != OutsidePromptPath.None,
                localId = agent.id,
                outside = true,
                kindLabel = agent.label,
                cwd = agent.cwd,
                outsidePath = path,
                outsideUser = agent.otherUser,
                terminal = agent.terminal,
            )
        }

        fun fromCloud(session: AgentLaunchSession): UnifiedAgentRow {
            val updated = maxOf(session.updatedAtMs, session.completedAtMs, session.createdAtMs)
            val title = session.title.trim().ifEmpty { session.retryPrompt }
            val stats = session.diffStats
            return UnifiedAgentRow(
                id = "node:${session.sessionId}",
                badge = if (session.isOnRunner) AgentTransportBadge.Runner else AgentTransportBadge.Node,
                title = if (title.isEmpty()) "Agent run" else title.take(120),
                repoOwner = session.owner,
                repoName = session.repo,
                node = session.targetNode,
                status = session.status,
                statusLabel = session.statusLabel,
                isActive = session.isActive,
                provider = session.provider,
                model = session.requestedModel,
                updatedAtMs = updated,
                canStop = session.isActive,
                canPrompt = session.isPromptable,
                canContinue = !session.isActive && session.isPromptable,
                localId = session.localAgentId,
                cloudSessionId = session.sessionId,
                orgScope = session.orgScope,
                branchName = session.branchName,
                baseBranch = session.baseBranch,
                runner = session.runner,
                prNumber = session.prNumber,
                merged = session.isMergedBranch,
                filesChanged = if (stats.known) stats.files else -1,
                linesAdded = if (stats.known) stats.added else -1,
                linesRemoved = if (stats.known) stats.removed else -1,
                behind = if (stats.known) stats.behind else -1,
                conflicted = stats.conflicted || session.merge.isConflict,
                canMerge = session.canMerge,
                canOpenPull = session.canOpenPull,
                needsCleanup = session.needsCleanup,
                finishedAtMs = AgentReadState.runFinishedAt(session),
            )
        }
    }
}

data class DesktopTerminalInfo(
    val terminal: String,
    val provider: String = "",
    val label: String = "",
    val cli: String = "",
    val repositoryOwner: String = "",
    val repositoryName: String = "",
    val cwd: String = "",
    val startedAtMs: Long = 0,
    val device: String = "",
    val running: Boolean = true,
    val cols: Int = 0,
    val rows: Int = 0,
    val host: String = "",
) {
    val repoLabel: String get() = if (repositoryOwner.isEmpty()) repositoryName else "$repositoryOwner/$repositoryName"

    companion object {
        fun fromJson(json: Map<String, Any?>): DesktopTerminalInfo = DesktopTerminalInfo(
            terminal = jsonText(json["terminal"]),
            provider = jsonText(json["provider"]),
            label = jsonText(json["label"]),
            cli = jsonText(json["cli"]),
            repositoryOwner = jsonText(json["repositoryOwner"]),
            repositoryName = jsonText(json["repositoryName"]),
            cwd = jsonText(json["cwd"]),
            startedAtMs = (json["startedAtMs"] as? Number)?.toLong() ?: 0,
            device = jsonText(json["device"]),
            running = json["running"] != false,
            cols = (json["cols"] as? Number)?.toInt() ?: 0,
            rows = (json["rows"] as? Number)?.toInt() ?: 0,
            host = jsonText(json["host"]),
        )

        fun list(json: Map<String, Any?>): List<DesktopTerminalInfo> =
            (json["terminals"] as? List<*>).orEmpty()
                .mapNotNull { item -> (item as? Map<*, *>)?.entries?.associate { (key, value) -> key.toString() to value } }
                .map(::fromJson)
                .filter { it.terminal.isNotEmpty() }
    }
}

fun agentRouteRowId(id: String): String {
    val clean = id.trim()
    if (clean.startsWith("desktop:") || clean.startsWith("node:") || clean.startsWith("outside:")) return clean
    if (clean.toIntOrNull() != null) return "desktop:$clean"
    return "node:$clean"
}

fun mergeAgentRows(desktop: List<UnifiedAgentRow>, cloud: List<UnifiedAgentRow>, desktopName: String): List<UnifiedAgentRow> {
    val desktopIds = desktop.map { it.localId }.toSet()
    val name = desktopName.lowercase()
    val others = cloud.filter { row ->
        !(row.localId > 0 && name.isNotEmpty() && row.node.lowercase() == name && row.localId in desktopIds)
    }
    return (desktop + others).sortedWith(
        compareBy<UnifiedAgentRow>({ !it.isActive }, { -it.updatedAtMs }, { it.id }),
    )
}

fun cloudTranscript(session: AgentLaunchSession): String {
    val turns = session.history.mapNotNull { turn ->
        val body = turn.text.trimEnd()
        when {
            body.isEmpty() -> null
            turn.isUser -> body.lines().joinToString("\n") { "> $it" }
            else -> body
        }
    }.toMutableList()
    if (session.isActive && session.liveText.isNotBlank()) turns += session.liveText.trimEnd()
    if (session.lastError.isNotBlank() && session.isFailed) turns += "!! ${session.lastError.trim()}"
    return turns.joinToString("\n\n")
}

fun cloudTranscriptDelta(row: UnifiedAgentRow, session: AgentLaunchSession): AgentTranscriptDelta {
    val text = cloudTranscript(session)
    val attachments = session.history.flatMap { session.attachmentRefs(it, row.repoOwner, row.repoName) }
    return AgentTranscriptDelta(text, text.length, reset = true, active = session.isActive, status = session.status, attachments = attachments)
}

internal val OUTSIDE_TRANSCRIPT_MISSING = setOf("unknown_session", "no_transcript", "unsupported")

data class DesktopListCover(val oldestCreatedAtMs: Long = 0) {
    companion object {
        const val LIST_CAP = 200

        fun of(snapshot: LocalAgentsSnapshot): DesktopListCover {
            val created = snapshot.sessions.map { it.session.createdAtMs }.filter { it > 0 }
            return DesktopListCover(if (snapshot.sessions.size >= LIST_CAP) created.minOrNull() ?: 0 else 0)
        }
    }

    fun covers(createdAtMs: Long): Boolean = oldestCreatedAtMs <= 0 || createdAtMs >= oldestCreatedAtMs
}

fun desktopAgents(rows: List<UnifiedAgentRow>, shown: List<Pair<String, Int>>, idle: Int = 20): List<UnifiedAgentRow> {
    val known = shown.map { (repo, id) -> "${repo.lowercase()}#$id" }.toSet()
    val listed = rows.filter { row ->
        row.isDesktop && (row.outside || "${row.repoName.lowercase()}#${row.localId}" !in known)
    }
    val recent = listed.filter { !it.isActive }.sortedByDescending { it.updatedAtMs }.take(maxOf(0, idle))
    return listed.filter { it.isActive } + recent
}

fun liveDesktopRow(rows: List<UnifiedAgentRow>, repoName: String, sessionId: Int): UnifiedAgentRow? =
    rows.firstOrNull { it.isDesktop && !it.outside && it.localId == sessionId && it.repoName.equals(repoName, ignoreCase = true) }

fun AgentControlling.desktopAttachments(repoName: String, sessionId: Int): List<AgentAttachmentRef>? {
    val row = liveDesktopRow(rows, repoName, sessionId) ?: return null
    return transcript(row, 0).attachments
}

data class AgentTranscriptDelta(
    val text: String,
    val next: Int,
    val reset: Boolean,
    val active: Boolean,
    val status: String,
    val attachments: List<AgentAttachmentRef>? = null,
    val from: Int = 0,
) {
    companion object {
        fun fromDesktop(json: JsonMap, since: Int, sessionId: Int = 0): AgentTranscriptDelta {
            val from = (json["from"] as? Number)?.toInt() ?: 0
            val understood = json["next"] != null
            return AgentTranscriptDelta(
                text = jsonText(json["transcript"]),
                next = if (understood) (json["next"] as? Number)?.toInt() ?: 0 else 0,
                reset = !understood || json["reset"] == true || (since > 0 && from < since && from == 0),
                active = json["active"] == true,
                status = jsonText(json["status"]),
                attachments = if (json["attachments"] == null) null else LocalAgentTranscript.fromJson(json, sessionId).attachments,
                from = from,
            )
        }
    }
}

class TranscriptFollower(val limit: Int = 200_000) {
    var text: String = ""
        private set
    var next: Int = 0
        private set
    var attachments: List<AgentAttachmentRef> = emptyList()
        private set

    fun apply(delta: AgentTranscriptDelta) {
        text = if (delta.reset) delta.text else text + delta.text
        next = delta.next
        delta.attachments?.let { attachments = it }
        if (text.length > limit) text = text.substring(text.length - limit)
    }
}

interface AgentControlling {
    val rows: List<UnifiedAgentRow>

    fun refresh()

    fun stop(row: UnifiedAgentRow)

    fun prompt(row: UnifiedAgentRow, text: String, images: List<PromptAttachment> = emptyList())
    val promptNotice: String

    fun continueRun(row: UnifiedAgentRow)

    fun transcript(row: UnifiedAgentRow, since: Int): AgentTranscriptDelta

    fun startOnDesktop(
        owner: String,
        name: String,
        task: String,
        provider: String = "",
        images: List<PromptAttachment> = emptyList(),
    ): Int
}

class AgentControlCenter(val hub: DesktopControlHub, private val cloud: AgentQueueApi?) : AgentControlling {
    companion object {
        const val TYPED_NOTICE = "Typed into your terminal."
    }

    @Volatile
    override var promptNotice: String = ""
    private val lock = Any()
    private val listeners = CopyOnWriteArrayList<() -> Unit>()
    private val worker = Executors.newSingleThreadExecutor { Thread(it, "AgentControl").apply { isDaemon = true } }
    private val cloudReader = Executors.newSingleThreadExecutor { Thread(it, "AgentControlCloud").apply { isDaemon = true } }
    private var refreshing = false
    private var wantDesktop = false
    private var wantCloud = false
    private val tap: Any = hub.addEventTap { event -> if (event.event == "agents.changed") refreshDesktopAsync() }
    @Volatile
    private var cloudSessions: List<AgentLaunchSession> = emptyList()

    @Volatile
    override var rows: List<UnifiedAgentRow> = emptyList()
        private set

    @Volatile
    var desktopSnapshot: LocalAgentsSnapshot? = null
        private set

    val queue: LocalAgentQueue? get() = if (desktopNote.isEmpty()) desktopSnapshot?.queue else null

    val supportsQueue: Boolean get() = queue != null

    val desktopCover: DesktopListCover?
        get() {
            val snapshot = desktopSnapshot ?: return null
            if (desktopNote.isNotEmpty() || hub.candidates.size != 1) return null
            return DesktopListCover.of(snapshot)
        }

    fun queuePosition(row: UnifiedAgentRow): Int {
        if (!row.isDesktop || row.outside || row.localId <= 0) return -1
        return queue?.position(row.localId) ?: -1
    }

    @Volatile
    var loading = false
        private set

    @Volatile
    var desktopNote = ""
        private set

    @Volatile
    var cloudNote = ""
        private set

    fun addListener(listener: () -> Unit) = listeners.add(listener)

    fun removeListener(listener: () -> Unit) = listeners.remove(listener)

    private fun notifyListeners() {
        for (listener in listeners) runCatching { listener() }
    }

    fun cloudChanged() = refreshAsync()

    fun refreshAsync() {
        worker.execute { refresh() }
    }

    fun refreshDesktopAsync() {
        worker.execute { reload(cloud = false) }
    }

    override fun refresh() = reload(cloud = true)

    fun stream(
        row: UnifiedAgentRow,
        since: Int,
        onEnded: (String) -> Unit = {},
        onDelta: (AgentTranscriptDelta) -> Unit,
    ): DesktopTranscriptStream? {
        if (!row.isDesktop || row.localId == 0) return null
        return DesktopTranscriptStream(hub, row.localId, since, onEnded = onEnded, onDelta = onDelta)
    }

    private fun reload(cloud: Boolean) {
        synchronized(lock) {
            wantDesktop = true
            if (cloud) wantCloud = true
            if (refreshing) return
            refreshing = true
        }
        loading = true
        notifyListeners()
        try {
            while (true) {
                val withCloud = synchronized(lock) {
                    if (!wantDesktop && !wantCloud) return@synchronized null
                    val next = wantCloud
                    wantDesktop = false
                    wantCloud = false
                    next
                } ?: break
                val cloudRows = if (withCloud) cloudReader.submit(Callable { loadCloud() }) else null
                val desk = loadDesktop()
                if (cloudRows != null) {
                    rows = mergeAgentRows(desk, cloudSessions.map(UnifiedAgentRow::fromCloud), hub.currentLink?.desktopName ?: "")
                    notifyListeners()
                }
                val nodes = cloudRows?.let { pending ->
                    try {
                        pending.get()
                    } catch (_: ExecutionException) {
                        cloudSessions.map(UnifiedAgentRow::fromCloud)
                    }
                } ?: cloudSessions.map(UnifiedAgentRow::fromCloud)
                rows = mergeAgentRows(desk, nodes, hub.currentLink?.desktopName ?: "")
                notifyListeners()
            }
        } finally {
            synchronized(lock) { refreshing = false }
            loading = false
            notifyListeners()
        }
    }

    private fun loadDesktop(): List<UnifiedAgentRow> {
        if (hub.desktop == null) hub.refreshAccountDesktops()
        if (hub.desktop == null) {
            desktopNote = ""
            desktopSnapshot = null
            return emptyList()
        }
        return try {
            val link = hub.link()
            val snapshot = LocalAgentsSnapshot.fromJson(link.request("agents.list", emptyMap()))
            desktopSnapshot = snapshot
            desktopNote = ""
            snapshot.sessions.map { UnifiedAgentRow.fromDesktop(it, link.transport, link.desktopName) } +
                snapshot.outside.map { UnifiedAgentRow.fromOutside(it, link.transport, link.desktopName) }
        } catch (error: Throwable) {
            desktopNote = ControlErrorText.describe(error)
            emptyList()
        }
    }

    private fun loadCloud(): List<UnifiedAgentRow> {
        val api = cloud ?: return emptyList()
        try {
            val answer = api.accountSessions()
            cloudSessions = AgentsList.keptCloudRuns(cloudSessions, answer.sessions, answer.finishedBefore)
            cloudNote = ""
        } catch (refusal: AgentLaunchException) {
            cloudNote = refusal.message
        } catch (error: Throwable) {
            cloudNote = error.message ?: error.toString()
        }
        return cloudSessions.map(UnifiedAgentRow::fromCloud)
    }

    override fun stop(row: UnifiedAgentRow) {
        if (row.outside && !row.canStop) throw LocalControlException("outside_stop_unsupported")
        if (row.isDesktop) {
            hub.request("agents.stop", mapOf("id" to row.localId))
        } else {
            val api = cloud ?: throw LocalControlException("not_connected")
            api.stop(row.repoOwner, row.repoName, row.cloudSessionId, row.orgScope)
        }
        refresh()
    }

    fun setQueueLimit(limit: Int): LocalAgentQueue {
        if (limit < LocalAgentQueue.MIN_LIMIT || limit > LocalAgentQueue.MAX_LIMIT) throw LocalControlException("bad_limit")
        val answer = LocalAgentQueue.fromJson(hub.request("agents.queue.limit", mapOf("limit" to limit)))
        applyQueue(answer)
        reload(cloud = false)
        return answer
    }

    fun moveQueued(row: UnifiedAgentRow, to: Int): LocalAgentQueue {
        if (!row.isDesktop || row.outside || row.localId <= 0) throw LocalControlException("not_queued")
        val answer = LocalAgentQueue.fromJson(
            hub.request("agents.queue.move", mapOf("id" to row.localId, "to" to maxOf(0, to))),
        )
        applyQueue(answer)
        reload(cloud = false)
        return answer
    }

    fun markRead(sessionId: Int): Boolean {
        if (sessionId <= 0 || hub.desktop == null) return false
        return runCatching { hub.request("agents.markRead", mapOf("id" to sessionId)) }.isSuccess
    }

    fun markReadAsync(sessionId: Int) {
        if (sessionId <= 0) return
        worker.execute { markRead(sessionId) }
    }

    fun moveQueued(row: UnifiedAgentRow, move: QueueMove): LocalAgentQueue? {
        val to = queue?.target(row.localId, move) ?: return null
        return moveQueued(row, to)
    }

    private fun applyQueue(answer: LocalAgentQueue) {
        val snapshot = desktopSnapshot ?: return
        desktopSnapshot = snapshot.copy(queue = answer)
        notifyListeners()
    }

    override fun prompt(row: UnifiedAgentRow, text: String, images: List<PromptAttachment>) {
        promptNotice = ""
        if (row.outside) {
            if (!row.canPrompt) throw LocalControlException("outside_cli_unsupported")
            if (images.isNotEmpty()) throw LocalControlException("outside_images_unsupported")
            val message = text.trim()
            if (message.isEmpty()) return
            val answer = hub.request("agents.prompt", mapOf("id" to row.localId, "text" to message), 30_000)
            when (answer["status"]) {
                "typed" -> promptNotice = TYPED_NOTICE
                "held_for_approval" -> promptNotice = ControlErrorText.describe("held_for_approval")
            }
            refresh()
            return
        }
        val trimmed = promptOrPhotoNote(text, images)
        if (trimmed.isEmpty()) return
        if (row.isDesktop) {
            val link = hub.link()
            val params = DesktopPromptImages.params(mapOf("id" to row.localId, "text" to trimmed), images, link)
            link.request("agents.prompt", params, 30_000)
        } else {
            val api = cloud ?: throw LocalControlException("not_connected")
            api.steer(row.repoOwner, row.repoName, row.cloudSessionId, trimmed, row.orgScope, images)
        }
        refresh()
    }

    fun fixConflicts(row: UnifiedAgentRow, files: List<String> = emptyList()) {
        if (row.outside) throw LocalControlException("unsupported")
        prompt(row, ConflictResolution.sessionPrompt(row.baseBranch, files))
    }

    fun fixDesktopConflicts(localId: Int, base: String) {
        hub.request("agents.prompt", mapOf("id" to localId, "text" to ConflictResolution.sessionPrompt(base)), 30_000)
        refresh()
    }

    override fun continueRun(row: UnifiedAgentRow) {
        if (row.outside) throw LocalControlException("unsupported")
        if (!row.isDesktop) {
            val api = cloud ?: throw LocalControlException("not_connected")
            api.steer(row.repoOwner, row.repoName, row.cloudSessionId, RunnerAgentOps.CONTINUE_PROMPT, row.orgScope)
            refresh()
            return
        }
        hub.request("agents.continue", mapOf("id" to row.localId))
        refresh()
    }

    fun continueOnDesktop(row: UnifiedAgentRow): RunnerDesktopMove {
        val api = cloud
        if (api == null || row.isDesktop || row.outside || row.cloudSessionId.isEmpty()) throw LocalControlException("unsupported")
        if (hub.desktop == null) hub.refreshAccountDesktops()
        if (hub.desktop == null) throw LocalControlException("not_connected")
        val repo = row.repoLabel
        var session = api.session(row.repoOwner, row.repoName, row.cloudSessionId, row.orgScope)
        try {
            val checked = hub.request(RunnerDesktopHandoff.CAPABILITY, RunnerDesktopHandoff.params(session, row, check = true), 30_000)
            if (checked["already"] == true) {
                val kept = dropRunnerCopy(row, api)
                refresh()
                return move(checked, kept)
            }
        } catch (error: Exception) {
            throw RunnerDesktopMoveException(RunnerDesktopHandoff.failureText(error, repo))
        }
        var stopped = ""
        if (session.isActive) {
            try {
                api.stop(row.repoOwner, row.repoName, row.cloudSessionId, row.orgScope)
            } catch (error: Exception) {
                throw RunnerDesktopMoveException(
                    "Could not stop the run on ${RunnerDesktopHandoff.runnerName(session)}, so it was not moved: ${error.message ?: error}",
                )
            }
            stopped = RunnerDesktopHandoff.runnerName(session)
            runCatching { api.session(row.repoOwner, row.repoName, row.cloudSessionId, row.orgScope) }.getOrNull()?.let { session = it }
        }
        try {
            val answer = hub.request(RunnerDesktopHandoff.CAPABILITY, RunnerDesktopHandoff.params(session, row, check = false), 90_000)
            val kept = dropRunnerCopy(row, api)
            refresh()
            return move(answer, kept)
        } catch (error: Exception) {
            refresh()
            throw RunnerDesktopMoveException(RunnerDesktopHandoff.failureText(error, repo, stopped))
        }
    }

    private fun dropRunnerCopy(row: UnifiedAgentRow, api: AgentQueueApi): Boolean {
        try {
            api.cancel(row.repoOwner, row.repoName, row.cloudSessionId, row.orgScope)
        } catch (refusal: AgentLaunchException) {
            if (refusal.status != 404) return true
        } catch (error: Exception) {
            return true
        }
        cloudSessions = cloudSessions.filter { it.sessionId != row.cloudSessionId }
        return false
    }

    private fun move(answer: Map<String, Any?>, runnerKept: Boolean): RunnerDesktopMove {
        val name = hub.currentLink?.desktopName?.takeIf { it.isNotEmpty() }
            ?: hub.desktop?.name?.takeIf { it.isNotEmpty() }
            ?: "your desktop"
        return RunnerDesktopMove(
            agentId = (answer["id"] as? Number)?.toInt() ?: 0,
            desktopName = name,
            branch = answer["branch"] as? String ?: "",
            fresh = answer["fresh"] == true,
            already = answer["already"] == true,
            runnerKept = runnerKept,
        )
    }

    override fun transcript(row: UnifiedAgentRow, since: Int): AgentTranscriptDelta {
        if (row.outside) {
            val params = linkedMapOf<String, Any?>("id" to row.localId)
            if (since > 0) params["since"] = since
            return try {
                AgentTranscriptDelta.fromDesktop(hub.request("agents.transcript", params), since)
            } catch (refusal: LocalControlException) {
                if (refusal.code !in OUTSIDE_TRANSCRIPT_MISSING) throw refusal
                if (since > 0 && refusal.code == "unknown_session") {
                    AgentTranscriptDelta("", since, reset = false, active = false, status = "done")
                } else {
                    val note = row.outsideNote
                    AgentTranscriptDelta(note, note.length, reset = true, active = false, status = row.status)
                }
            }
        }
        if (row.isDesktop) {
            val params = linkedMapOf<String, Any?>("id" to row.localId)
            if (since > 0) params["since"] = since
            return AgentTranscriptDelta.fromDesktop(hub.request("agents.transcript", params), since, row.localId)
        }
        return cloudTranscriptDelta(row, cloudSession(row))
    }

    fun cloudSession(row: UnifiedAgentRow): AgentLaunchSession {
        val api = cloud ?: throw LocalControlException("not_connected")
        return api.session(row.repoOwner, row.repoName, row.cloudSessionId, row.orgScope)
    }

    fun cloudLive(row: UnifiedAgentRow, onEvent: (AgentLiveEvent) -> Unit): AgentLiveStream? {
        val api = cloud ?: return null
        if (row.isDesktop || row.outside || row.cloudSessionId.isEmpty()) return null
        return api.live(row.repoOwner, row.repoName, row.cloudSessionId, row.orgScope, onEvent = onEvent)
    }

    override fun startOnDesktop(
        owner: String,
        name: String,
        task: String,
        provider: String,
        images: List<PromptAttachment>,
    ): Int {
        val params = linkedMapOf<String, Any?>("owner" to owner, "name" to name, "task" to promptOrPhotoNote(task, images))
        if (provider.isNotEmpty()) params["provider"] = provider
        val link = hub.link()
        val result = link.request("agents.start", DesktopPromptImages.params(params, images, link), 30_000)
        refresh()
        return (result["id"] as? Number)?.toInt() ?: 0
    }

    fun dispose() {
        hub.removeEventTap(tap)
        worker.shutdown()
        cloudReader.shutdown()
    }
}
