package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.localrepos.LocalRepoRecord
import com.forkmesh.nativeapp.model.AgentSession
import com.forkmesh.nativeapp.model.PublishedPull
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.navigation.RepoRef

data class AgentRepoGroup(
    val repo: Repository,
    val sessions: List<AgentSession>,
    val launched: List<AgentLaunchSession> = emptyList(),
    val pulls: List<PublishedPull> = emptyList(),
    val targets: List<AgentLaunchTarget> = emptyList(),
    val sealedForOthers: Int = 0,
) {
    fun runsInCloud(session: AgentLaunchSession): Boolean = launchRunsInCloud(session, targets)

    val cloud: List<AgentLaunchSession> get() = launched.filter(::runsInCloud).sortedWith(AgentsList.launchedOrder)
    val hasSealed: Boolean get() = sealedForOthers > 0
    val total: Int get() = sessions.size + launched.size
    val cloudRunning: Int get() = launched.count { it.isActive && runsInCloud(it) }
    val hasActive: Boolean get() = sessions.any { it.isActive } || launched.any { it.isActive }
}

sealed class AgentEntry {
    abstract val repo: Repository
    abstract val isActive: Boolean

    data class Desktop(override val repo: Repository, val session: AgentSession) : AgentEntry() {
        override val isActive: Boolean get() = session.isActive
    }

    data class Launched(
        override val repo: Repository,
        val session: AgentLaunchSession,
        val pulls: List<PublishedPull>,
        val cloud: Boolean,
    ) : AgentEntry() {
        override val isActive: Boolean get() = session.isActive
    }
}

enum class AgentTone { Accent, Success, Danger, Muted, Secondary }

object AgentsList {
    val launchedOrder: Comparator<AgentLaunchSession> =
        compareBy<AgentLaunchSession> { !it.isActive }.thenByDescending { it.updatedAtMs }

    val sessionOrder: Comparator<AgentSession> = compareBy<AgentSession> { !it.isActive }.thenByDescending { it.id }

    val groupOrder: Comparator<AgentRepoGroup> =
        compareBy<AgentRepoGroup> { !it.hasActive }.thenBy { it.repo.fullName.lowercase() }

    fun group(
        repo: Repository,
        page: com.forkmesh.nativeapp.model.AgentSessionPage,
        options: AgentLaunchOptions,
        pulls: List<PublishedPull>,
    ): AgentRepoGroup = regroup(repo, null, page, options, pulls)

    fun regroup(
        repo: Repository,
        previous: AgentRepoGroup?,
        page: com.forkmesh.nativeapp.model.AgentSessionPage?,
        options: AgentLaunchOptions?,
        pulls: List<PublishedPull>?,
    ): AgentRepoGroup {
        val kept = previous?.takeIf { sameRepo(it.repo, repo) }
        val launched = if (options == null) {
            kept?.launched ?: emptyList()
        } else {
            keptCloudRuns(kept?.launched ?: emptyList(), options.sessions, options.finishedBefore)
        }
        return AgentRepoGroup(
            repo = repo,
            sessions = (page?.sessions ?: kept?.sessions ?: emptyList()).sortedWith(sessionOrder),
            launched = launched.sortedWith(launchedOrder),
            pulls = pulls ?: kept?.pulls ?: emptyList(),
            targets = options?.targets ?: kept?.targets ?: emptyList(),
            sealedForOthers = page?.sealedForOthers ?: kept?.sealedForOthers ?: 0,
        )
    }

    fun beyondListedWindow(session: AgentLaunchSession, finishedBefore: Long): Boolean =
        finishedBefore > 0 && session.updatedAtMs > 0 && session.updatedAtMs <= finishedBefore && !session.isActive

    fun keptCloudRuns(
        previous: List<AgentLaunchSession>,
        fresh: List<AgentLaunchSession>,
        finishedBefore: Long,
    ): List<AgentLaunchSession> {
        val listed = fresh.map { it.sessionId }.toSet()
        return fresh + previous.filter { it.sessionId !in listed && beyondListedWindow(it, finishedBefore) }
    }

    fun needsPulls(options: AgentLaunchOptions): Boolean = needsPulls(options.sessions)

    fun needsPulls(sessions: List<AgentLaunchSession>): Boolean = sessions.any { it.branchName.isNotEmpty() }

    fun groupFor(groups: List<AgentRepoGroup>, repo: Repository): AgentRepoGroup? =
        groups.firstOrNull { sameRepo(it.repo, repo) }

    private fun sameRepo(a: Repository, b: Repository): Boolean = a.owner == b.owner && a.name == b.name

    fun visible(groups: List<AgentRepoGroup>): List<AgentRepoGroup> =
        groups.filter { it.sessions.isNotEmpty() || it.launched.isNotEmpty() || it.hasSealed }.sortedWith(groupOrder)

    fun covered(
        groups: List<AgentRepoGroup>,
        rows: List<com.forkmesh.nativeapp.control.UnifiedAgentRow>,
        cover: com.forkmesh.nativeapp.control.DesktopListCover?,
    ): List<AgentRepoGroup> {
        if (cover == null) return groups
        val live = rows.filter { it.isDesktop && !it.outside && !it.elsewhere }.map { "${it.repoName.lowercase()}#${it.localId}" }.toSet()
        return visible(
            groups.map { g ->
                g.copy(sessions = g.sessions.filter { "${g.repo.name.lowercase()}#${it.id}" in live || !cover.covers(it.createdAtMs) })
            },
        )
    }

    fun flatten(groups: List<AgentRepoGroup>): List<AgentEntry> {
        val all = ArrayList<AgentEntry>()
        for (group in groups) {
            for (session in group.sessions) all.add(AgentEntry.Desktop(group.repo, session))
            for (session in group.launched) all.add(AgentEntry.Launched(group.repo, session, group.pulls, group.runsInCloud(session)))
        }
        return all.filter { it.isActive } + all.filter { !it.isActive }
    }

    fun sessionCount(groups: List<AgentRepoGroup>): Int = groups.sumOf { it.total }

    fun running(groups: List<AgentRepoGroup>): Int =
        groups.sumOf { g -> g.sessions.count { it.isActive } + g.launched.count { it.isActive } }

    fun desktopRunning(groups: List<AgentRepoGroup>): Int = groups.sumOf { g -> g.sessions.count { it.isActive } }

    fun cloudRunning(groups: List<AgentRepoGroup>): Int = groups.sumOf { it.cloudRunning }

    fun sealed(groups: List<AgentRepoGroup>): Int = groups.sumOf { it.sealedForOthers }

    fun cloudRuns(groups: List<AgentRepoGroup>): List<Pair<Repository, AgentLaunchSession>> =
        groups.flatMap { g -> g.cloud.map { g.repo to it } }.sortedWith { a, b -> launchedOrder.compare(a.second, b.second) }

    fun desktopRunningRows(groups: List<AgentRepoGroup>): List<Pair<Repository, AgentSession>> =
        groups.flatMap { g -> g.sessions.filter { it.isActive }.map { g.repo to it } }

    fun pullsFor(groups: List<AgentRepoGroup>, repo: Repository): List<PublishedPull> = groupFor(groups, repo)?.pulls ?: emptyList()

    fun statusTone(session: AgentSession): AgentTone = when (session.statusLabel) {
        "Merged", "Done" -> AgentTone.Success
        "Failed", "Stopped" -> AgentTone.Danger
        "Running", "Queued", "Waiting" -> AgentTone.Accent
        else -> AgentTone.Muted
    }

    fun badgeTone(label: String): AgentTone {
        val normalized = label.lowercase()
        return when {
            normalized.contains("running") || normalized.contains("queued") || normalized.contains("waiting") -> AgentTone.Accent
            normalized.contains("done") || normalized.contains("completed") -> AgentTone.Success
            normalized.contains("failed") || normalized.contains("stopped") -> AgentTone.Danger
            else -> AgentTone.Secondary
        }
    }

    fun desktopDetail(repo: Repository, session: AgentSession, nowMs: Long = System.currentTimeMillis()): String = listOf(
        repo.fullName,
        if (session.issueNumber > 0) "Issue #${session.issueNumber}" else "",
        session.providerLabel,
        session.model,
        session.branchName,
        session.rateLimitCountdownLabel(nowMs),
        if (session.numTurns > 0) "${session.numTurns} turns" else "",
        session.durationLabel,
        session.costLabel,
        session.prLabel,
        session.diffLabel,
        if (session.conflicted) "Conflict" else "",
        session.tokenLabel,
        if (session.isFailed) session.statusFailureReason(80) else "",
    ).filter { it.isNotEmpty() }.joinToString(" · ")

    fun launchedDetail(repo: Repository, session: AgentLaunchSession, pulls: List<PublishedPull>, cloud: Boolean): String {
        val last = session.history.lastOrNull()
        val chain = agentTaskChain(session, pulls)
        return listOf(
            repo.fullName,
            when {
                session.isOnRunner -> "Runner · ${session.runner}"
                cloud && session.targetNode.isNotEmpty() -> "cloud · ${session.targetNode}"
                else -> session.targetNode
            },
            session.provider,
            session.branchName,
            chain.summary.ifEmpty { if (session.createPr) "opens PR" else "" },
            (last?.text?.trim() ?: "").split('\n').first(),
        ).filter { it.isNotEmpty() }.joinToString(" · ")
    }

    fun sealedLabel(count: Int): String =
        if (count == 1) "1 session is encrypted for another device" else "$count sessions are encrypted for another device"

    fun watchChoice(groups: List<AgentRepoGroup>): Pair<String, List<Pair<Repository, AgentSession>>> {
        val all = groups.flatMap { g -> g.sessions.map { g.repo to it } }
        var chosen = all.filter { it.second.isActive }
        if (chosen.isEmpty()) {
            chosen = all.sortedByDescending { maxOf(it.second.startedAtMs, it.second.createdAtMs) }.take(3)
        }
        val title = if (chosen.any { it.second.isActive }) "Watch agents" else "Replay agents"
        return title to chosen.take(12)
    }
}

enum class AgentRunMode { Node, Device }

data class AgentStartTarget(val repo: Repository? = null, val record: LocalRepoRecord? = null) {
    init {
        require(repo != null || record != null)
    }

    private val publishedParts: Pair<String, String>
        get() {
            val published = record?.publishedAs?.trim() ?: ""
            val slash = published.indexOf('/')
            if (slash <= 0) return "" to ""
            return published.substring(0, slash) to published.substring(slash + 1)
        }

    val owner: String get() = repo?.owner ?: publishedParts.first
    val publicOwner: String get() = repo?.displayOwner ?: publishedParts.first
    val name: String get() = repo?.name ?: publishedParts.second
    val isOnMesh: Boolean get() = owner.isNotEmpty() && name.isNotEmpty()
    val isMirrored: Boolean get() = record != null

    val ref: RepoRef? get() = if (isOnMesh) RepoRef(owner, name) else null

    val label: String get() = if (isOnMesh) repo?.fullName ?: "$owner/$name" else record?.name ?: ""

    val key: String get() = label.lowercase()

    val detail: String
        get() = when {
            isMirrored && isOnMesh -> "Mirrored on this device"
            isMirrored -> "On this device · not published"
            else -> {
                val mirrors = repo?.mirrors ?: 0
                if (mirrors > 0) {
                    "On the mesh · $mirrors ${if (mirrors == 1) "mirror" else "mirrors"} · not on this device yet"
                } else {
                    "On the mesh · not on this device yet"
                }
            }
        }

    val canRunOnDevice: Boolean get() = isMirrored || isOnMesh

    companion object {
        fun fold(repos: List<Repository>, records: List<LocalRepoRecord>): List<AgentStartTarget> {
            val byKey = LinkedHashMap<String, AgentStartTarget>()
            val orphans = ArrayList<AgentStartTarget>()
            val publicKeys = HashMap<String, String>()
            for (repo in repos) {
                val key = repo.fullName.lowercase()
                byKey[key] = AgentStartTarget(repo = repo)
                publicKeys["${repo.owner}/${repo.name}".lowercase()] = key
            }
            for (record in records) {
                val route = record.publishedAs?.trim()?.lowercase() ?: ""
                val published = publicKeys[route] ?: route
                val existing = if (published.isEmpty()) null else byKey[published]
                when {
                    existing != null -> byKey[published] = AgentStartTarget(existing.repo, record)
                    published.isNotEmpty() -> byKey[published] = AgentStartTarget(record = record)
                    else -> orphans.add(AgentStartTarget(record = record))
                }
            }
            return byKey.values.filter { it.isMirrored } + orphans + byKey.values.filter { !it.isMirrored }
        }
    }
}

data class AgentStartPlan(
    val target: AgentStartTarget,
    val options: AgentLaunchOptions? = null,
    val error: String = "",
    val loading: Boolean = false,
) {
    val node: AgentLaunchTarget? get() = options?.preferredTarget
    val canRunOnNode: Boolean get() = (options?.canQueue ?: false) && node != null
    val defaultMode: AgentRunMode get() = if (canRunOnNode) AgentRunMode.Node else AgentRunMode.Device

    fun runsOnText(mode: AgentRunMode, chosen: AgentLaunchTarget?, deviceAvailable: Boolean): String {
        val kind = when {
            chosen == null -> ""
            chosen.isMirror -> "mirror "
            else -> "node "
        }
        return when {
            !canRunOnNode && !deviceAvailable ->
                "No node on this account can run agents for this repository, and this build cannot mirror it to the device. " +
                    "Sign in on a desktop or server node to run agents."
            mode == AgentRunMode.Node && chosen != null ->
                if (chosen.online) "Sent to $kind${chosen.node}, which is online now."
                else "Queued for $kind${chosen.node}. It starts when that node connects."
            mode == AgentRunMode.Node -> "No node on this account can run it yet."
            target.isMirrored -> "Runs here, on the mirror already on this device."
            target.isOnMesh -> "Clones ${target.label} from its mirrors to this device first, then runs here on your own API key."
            else -> "This repository is only on this device, so the run happens here."
        }
    }

    companion object {
        fun titleFor(prompt: String): String = if (prompt.trim().isEmpty()) "Photo prompt" else prompt.split('\n').first().trim()

        fun providerFor(model: AgentModelChoice): String = if (model.provider == "codex") "codex" else "auto"
    }
}
