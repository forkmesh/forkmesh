package com.forkmesh.nativeapp.surfaces

import com.forkmesh.nativeapp.inbox.InboxRoutes
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.navigation.ComposeTarget
import com.forkmesh.nativeapp.navigation.InboxFilter
import com.forkmesh.nativeapp.navigation.RepoRef
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.navigation.RouteCodec
import java.net.URI

enum class AgentRunState(val raw: String, val label: String, internal val rank: Int) {
    Queued("queued", "Queued", 3),
    Running("running", "Running", 2),
    Waiting("waiting", "Waiting", 1),
    NeedsApproval("needs_approval", "Needs you", 0),
    Completed("completed", "Done", 5),
    Failed("failed", "Failed", 4),
    Stopped("stopped", "Stopped", 5);

    val isActive: Boolean get() = this == Queued || this == Running || this == Waiting || this == NeedsApproval

    companion object {
        fun of(raw: String?): AgentRunState? = entries.firstOrNull { it.raw == raw }

        fun from(status: String, isActive: Boolean, needsApproval: Boolean = false): AgentRunState {
            if (needsApproval) return NeedsApproval
            return when (status.trim().lowercase().replace('_', '-')) {
                "queued", "pending", "claimed" -> Queued
                "running", "started", "in-progress" -> Running
                "waiting", "waiting-for-input" -> Waiting
                "needs-approval" -> NeedsApproval
                "done", "completed", "success", "succeeded", "merged" -> Completed
                "failed", "error" -> Failed
                "stopped", "cancelled", "canceled" -> Stopped
                else -> if (isActive) Running else Completed
            }
        }
    }
}

data class AgentRunInput(
    val id: String,
    val title: String,
    val repoOwner: String,
    val repoName: String,
    val status: String,
    val isActive: Boolean,
    val provider: String = "",
    val model: String = "",
    val detail: String = "",
    val updatedAtMs: Long = 0,
    val startedAtMs: Long = 0,
    val localId: Int = 0,
    val isDesktop: Boolean = false,
    val canStop: Boolean = false,
    val route: Route? = null,
)

data class AgentApprovalInput(val id: String, val sessionId: Int, val question: String, val askedAtMs: Long)

data class AgentRunCard(
    val id: String,
    val title: String,
    val repo: String,
    val detail: String = "",
    val provider: String = "",
    val model: String = "",
    val state: AgentRunState,
    val updatedAtMs: Long = 0,
    val startedAtMs: Long = 0,
    val approvalId: String = "",
    val canStop: Boolean = false,
    val target: Route? = null,
) {
    val route: Route get() = if (approvalId.isNotEmpty()) Route.Approval(approvalId) else target ?: Route.Agent(id)
}

data class AgentsSurface(val needsYou: Int = 0, val running: Int = 0, val queued: Int = 0, val runs: List<AgentRunCard> = emptyList()) {
    val active: List<AgentRunCard> get() = runs.filter { it.state.isActive }
    val activeCount: Int get() = running + queued + needsYou

    val summaryLine: String
        get() {
            if (needsYou == 0 && running == 0 && queued == 0) return "No agents running"
            val parts = ArrayList<String>()
            if (needsYou > 0) parts += "$needsYou need${if (needsYou == 1) "s" else ""} you"
            if (running > 0) parts += "$running running"
            if (queued > 0) parts += "$queued queued"
            return parts.joinToString(", ")
        }

    companion object {
        const val MAX_RUNS = 6
        val empty = AgentsSurface()

        fun build(runs: List<AgentRunInput>, approvals: List<AgentApprovalInput>, limit: Int = MAX_RUNS): AgentsSurface {
            val approvalBySession = HashMap<Int, AgentApprovalInput>()
            for (approval in approvals.sortedByDescending { it.askedAtMs }) {
                if (approval.sessionId > 0) approvalBySession.putIfAbsent(approval.sessionId, approval)
            }
            val seen = HashSet<String>()
            val cards = ArrayList<AgentRunCard>()
            for (run in runs) {
                if (run.id.isEmpty() || !seen.add(run.id)) continue
                val approval = if (run.isDesktop && run.localId > 0) approvalBySession[run.localId] else null
                val state = AgentRunState.from(run.status, run.isActive, approval != null)
                val repo = if (run.repoOwner.isEmpty()) run.repoName else "${run.repoOwner}/${run.repoName}"
                cards += AgentRunCard(
                    id = SurfaceText.clip(run.id, 80),
                    title = SurfaceText.clip(run.title.ifEmpty { "Agent run" }, 80),
                    repo = SurfaceText.clip(repo, 120),
                    detail = approval?.let { SurfaceText.clip(it.question, 160) } ?: SurfaceText.clip(run.detail, 160),
                    provider = SurfaceText.clip(run.provider, 40),
                    model = SurfaceText.clip(run.model, 60),
                    state = state,
                    updatedAtMs = maxOf(run.updatedAtMs, approval?.askedAtMs ?: 0),
                    startedAtMs = run.startedAtMs,
                    approvalId = approval?.let { SurfaceText.clip(it.id, 120) } ?: "",
                    canStop = run.canStop && state.isActive,
                    target = run.route,
                )
            }
            val orphans = approvals.count { a -> cards.none { it.approvalId == a.id } }
            val needsYou = cards.count { it.state == AgentRunState.NeedsApproval } + orphans
            val running = cards.count { it.state == AgentRunState.Running || it.state == AgentRunState.Waiting }
            val queued = cards.count { it.state == AgentRunState.Queued }
            val ordered = cards.sortedWith(
                compareBy<AgentRunCard>({ !it.state.isActive }, { it.state.rank }, { -it.updatedAtMs }, { it.id }),
            )
            return AgentsSurface(needsYou, running, queued, ordered.take(maxOf(0, limit)))
        }
    }
}

data class InboxSurfaceItem(val id: String, val title: String, val subtitle: String, val kind: String, val atMs: Long, val route: Route?) {
    val destination: Route get() = route ?: Route.Inbox(InboxFilter.All)
}

data class InboxSurface(val unread: Int = 0, val items: List<InboxSurfaceItem> = emptyList()) {
    companion object {
        const val MAX_ITEMS = 3
        val empty = InboxSurface()

        fun build(unread: Int, notifications: List<ForkNotification>, limit: Int = MAX_ITEMS): InboxSurface {
            val items = notifications.filter { it.readAt <= 0 }.sortedByDescending { it.ts }.take(maxOf(0, limit)).map { note ->
                InboxSurfaceItem(
                    id = SurfaceText.clip(note.id, 80),
                    title = SurfaceText.clip(note.title.ifEmpty { note.kindLabel }, 80),
                    subtitle = SurfaceText.clip(note.repo.ifEmpty { note.kindLabel }, 80),
                    kind = SurfaceText.clip(note.kind, 40),
                    atMs = note.ts,
                    route = InboxRoutes.route(note.kind, note.href, note.meta, note.repo),
                )
            }
            return InboxSurface(maxOf(0, unread), items)
        }
    }
}

enum class SurfaceSection(val raw: String) { Agents("agents"), Inbox("inbox") }

data class SurfaceSnapshot(
    val generatedAtMs: Long,
    val signedIn: Boolean,
    val agents: AgentsSurface = AgentsSurface.empty,
    val inbox: InboxSurface = InboxSurface.empty,
    val version: Int = CURRENT_VERSION,
) {
    companion object {
        const val CURRENT_VERSION = 1
        const val MAX_BYTES = 64 * 1024

        fun signedOut(ms: Long) = SurfaceSnapshot(ms, false)

        fun decode(text: String?): SurfaceSnapshot? {
            if (text.isNullOrEmpty() || text.length > MAX_BYTES) return null
            val m = try {
                Json.parse(text).asJsonMap()
            } catch (_: Exception) {
                return null
            }
            if (m.int("version") != CURRENT_VERSION) return null
            val a = m.obj("agents")
            val i = m.obj("inbox")
            return SurfaceSnapshot(
                generatedAtMs = m.long("generatedAt"),
                signedIn = m.bool("signedIn"),
                agents = AgentsSurface(
                    a.int("needsYou"), a.int("running"), a.int("queued"),
                    a.maps("runs").mapNotNull { r ->
                        AgentRunCard(
                            id = r.str("id"), title = r.str("title"), repo = r.str("repo"), detail = r.str("detail"),
                            provider = r.str("provider"), model = r.str("model"),
                            state = AgentRunState.of(r.str("state")) ?: return@mapNotNull null,
                            updatedAtMs = r.long("updatedAt"), startedAtMs = r.long("startedAt"), approvalId = r.str("approvalId"),
                            canStop = r.bool("canStop"), target = r["route"]?.asJsonMap()?.let(RouteCodec::fromMap),
                        )
                    },
                ),
                inbox = InboxSurface(
                    i.int("unread"),
                    i.maps("items").map { r ->
                        InboxSurfaceItem(
                            r.str("id"), r.str("title"), r.str("subtitle"), r.str("kind"), r.long("at"),
                            r["route"]?.asJsonMap()?.let(RouteCodec::fromMap),
                        )
                    },
                ),
            )
        }
    }

    fun encode(): String = Json.stringify(
        linkedMapOf(
            "version" to version.toLong(),
            "generatedAt" to generatedAtMs,
            "signedIn" to signedIn,
            "agents" to linkedMapOf(
                "needsYou" to agents.needsYou.toLong(), "running" to agents.running.toLong(), "queued" to agents.queued.toLong(),
                "runs" to agents.runs.map { r ->
                    linkedMapOf<String, Any?>(
                        "id" to r.id, "title" to r.title, "repo" to r.repo, "detail" to r.detail, "provider" to r.provider,
                        "model" to r.model, "state" to r.state.raw, "updatedAt" to r.updatedAtMs, "startedAt" to r.startedAtMs,
                        "approvalId" to r.approvalId, "canStop" to r.canStop,
                    ).also { map -> r.target?.let { map["route"] = RouteCodec.toMap(it) } }
                },
            ),
            "inbox" to linkedMapOf(
                "unread" to inbox.unread.toLong(),
                "items" to inbox.items.map { r ->
                    linkedMapOf<String, Any?>("id" to r.id, "title" to r.title, "subtitle" to r.subtitle, "kind" to r.kind, "at" to r.atMs)
                        .also { map -> r.route?.let { map["route"] = RouteCodec.toMap(it) } }
                },
            ),
        ),
    )

    fun changedSections(old: SurfaceSnapshot?): Set<SurfaceSection> {
        if (old == null || old.version != version || old.signedIn != signedIn) return SurfaceSection.entries.toSet()
        val changed = HashSet<SurfaceSection>()
        if (old.agents != agents) changed += SurfaceSection.Agents
        if (old.inbox != inbox) changed += SurfaceSection.Inbox
        return changed
    }
}

object SurfaceText {
    fun clip(text: String, limit: Int): String {
        val flat = text.replace('\n', ' ').replace('\r', ' ').trim()
        if (flat.length <= limit) return flat
        return flat.take(maxOf(0, limit - 1)) + "…"
    }
}

data class SharedContent(val text: String, val urls: List<String>, val repo: String = "") {
    val prompt: String
        get() {
            val parts = ArrayList<String>()
            val trimmed = text.trim()
            if (trimmed.isNotEmpty()) parts += trimmed
            val links = urls.filter { it !in trimmed }
            if (links.isNotEmpty()) parts += links.joinToString("\n")
            return parts.joinToString("\n\n")
        }

    val composeTarget: ComposeTarget get() = ComposeTarget(repo = RepoRef.parse(repo), text = prompt.ifEmpty { null })

    companion object {
        const val MAX_TEXT_LENGTH = 20_000
        const val MAX_URLS = 10
        const val MAX_URL_LENGTH = 2048
        const val MAX_IMAGES = 10
        private val urlPattern = Regex("https?://[^\\s<>\"']+")

        fun cleanUrls(urls: List<String>): List<String> {
            val out = LinkedHashSet<String>()
            for (raw in urls) {
                val text = raw.trim()
                if (text.isEmpty() || text.length > MAX_URL_LENGTH) continue
                val uri = try {
                    URI(text)
                } catch (_: Exception) {
                    continue
                }
                val scheme = uri.scheme?.lowercase()
                if ((scheme != "https" && scheme != "http") || uri.host.isNullOrEmpty()) continue
                out += text
                if (out.size == MAX_URLS) break
            }
            return out.toList()
        }

        fun from(text: String?, subject: String? = null, repo: String = ""): SharedContent? {
            val body = listOfNotNull(subject?.trim()?.takeIf { it.isNotEmpty() && text?.contains(it) != true }, text?.trim())
                .filter { it.isNotEmpty() }.joinToString("\n\n").take(MAX_TEXT_LENGTH)
            val urls = cleanUrls(urlPattern.findAll(body).map { it.value.trimEnd('.', ',', ')', ']') }.toList())
            if (body.isEmpty() && urls.isEmpty()) return null
            val onlyLink = urls.size == 1 && body == urls[0]
            return SharedContent(if (onlyLink) "" else body, urls, RepoRef.parse(repo)?.slug ?: "")
        }
    }
}
