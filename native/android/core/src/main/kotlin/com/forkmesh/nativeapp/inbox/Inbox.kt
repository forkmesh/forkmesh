package com.forkmesh.nativeapp.inbox

import com.forkmesh.nativeapp.account.notificationIconForKind
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.model.NotificationDeepLink
import com.forkmesh.nativeapp.model.RepoDetailTab
import com.forkmesh.nativeapp.navigation.ComposeTarget
import com.forkmesh.nativeapp.navigation.InboxFilter
import com.forkmesh.nativeapp.navigation.PullTab
import com.forkmesh.nativeapp.navigation.RepoRef
import com.forkmesh.nativeapp.navigation.RepoSection
import com.forkmesh.nativeapp.navigation.Route
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.UUID

enum class InboxSource { Ping, Task, Local }

enum class InboxSyncState { Synced, Pending, Failed, LocalOnly }

enum class InboxCategory {
    Approval, Review, Assigned, Mention, Ping;

    fun matches(filter: InboxFilter): Boolean = when (filter) {
        InboxFilter.All, InboxFilter.Pings -> true
        InboxFilter.Reviews -> this == Review
        InboxFilter.Assigned -> this == Assigned
        InboxFilter.Mentions -> this == Mention
        InboxFilter.Approvals -> this == Approval
    }

    companion object {
        fun of(kind: String): InboxCategory = when (kind.trim().lowercase()) {
            "agent_approval", "agent-approval" -> Approval
            "pull_submitted", "review_requested", "pull_review_requested" -> Review
            "issue_assigned", "organization_task_started" -> Assigned
            "mention" -> Mention
            else -> Ping
        }
    }
}

data class InboxItem(
    val id: String,
    val source: InboxSource,
    val category: InboxCategory,
    val kind: String,
    val title: String,
    val body: String = "",
    val repo: String = "",
    val actor: String = "",
    val href: String = "",
    val ts: Long = 0,
    val unread: Boolean = true,
    val sync: InboxSyncState = InboxSyncState.Synced,
    val route: Route? = null,
    val remoteId: String = "",
    val sessionId: String = "",
    val approvalId: String = "",
    val desktopPingId: String = "",
    val desktopNode: String = "",
) {
    val fromDesktop: Boolean get() = source == InboxSource.Ping && kind == "desktop_ping"

    val icon: String
        get() = when (source) {
            InboxSource.Task -> "task_alt"
            InboxSource.Local -> if (kind == LocalPingLog.CHAT_KIND) "chat_bubble_outline" else "error_outline"
            InboxSource.Ping -> if (category == InboxCategory.Approval) "verified_user_outlined" else notificationIconForKind(kind)
        }

    val kindLabel: String
        get() = when {
            source == InboxSource.Task -> "Task"
            source == InboxSource.Local -> if (kind == LocalPingLog.CHAT_KIND) "Chat" else "On this phone"
            category == InboxCategory.Approval -> "Approval"
            category == InboxCategory.Review -> "Review"
            category == InboxCategory.Assigned -> "Assigned"
            category == InboxCategory.Mention -> "Mention"
            else -> ForkNotification(id = "", kind = kind, title = "").kindLabel
        }

    companion object {
        fun fromPing(ping: ForkNotification): InboxItem = InboxItem(
            id = "ping:${ping.id}",
            source = InboxSource.Ping,
            category = InboxCategory.of(ping.kind),
            kind = ping.kind,
            title = ping.title,
            body = ping.body,
            repo = ping.repo,
            actor = ping.actor,
            href = ping.href,
            ts = ping.ts,
            unread = ping.isUnread,
            route = InboxRoutes.route(ping.kind, ping.href, ping.meta, ping.repo),
            remoteId = ping.id,
            sessionId = ping.meta.str("sessionId"),
            approvalId = ping.meta.str("approvalId"),
            desktopPingId = ping.meta.str("pingId"),
            desktopNode = ping.meta.str("node"),
        )
    }
}

object InboxRoutes {
    fun route(kind: String, href: String, meta: Map<String, Any?>, repo: String): Route? {
        if (InboxCategory.of(kind) == InboxCategory.Approval) {
            val approval = meta.str("approvalId")
            val session = meta.str("sessionId")
            if (approval.isNotEmpty() && session.isNotEmpty()) return Route.Approval("$session/$approval")
            if (session.isNotEmpty()) return Route.Agent(session)
        }
        if (kind == "desktop_ping" && meta.str("sessionId").isNotEmpty()) return Route.Agent(meta.str("sessionId"))
        val number = meta.int("number")
        NotificationDeepLink.parse(href)?.let { return route(it) }
        val ref = RepoRef.parse(repo) ?: return null
        return when (kind) {
            "pull_submitted" -> if (number > 0) Route.Pull(ref, number, PullTab.Conversation) else Route.Repo(ref, RepoSection.Pulls)
            "issue_assigned", "issue_opened", "mention" -> if (number > 0) Route.Issue(ref, number) else Route.Repo(ref, RepoSection.Issues)
            "release_published" -> Route.Repo(ref, RepoSection.Releases)
            else -> Route.Repo(ref, null)
        }
    }

    fun webLink(href: String, origin: String): String? {
        val trimmed = href.trim()
        if (trimmed.isEmpty()) return null
        val target = try {
            java.net.URI(origin.trimEnd('/') + "/").resolve(trimmed)
        } catch (_: Exception) {
            return null
        }
        val scheme = target.scheme?.lowercase()
        if (scheme != "https" && scheme != "http") return null
        if (target.host.isNullOrEmpty() || !target.rawUserInfo.isNullOrEmpty()) return null
        val path = target.rawPath.orEmpty()
        if (path == "/api" || path.startsWith("/api/")) return null
        return target.toString()
    }

    fun route(link: NotificationDeepLink): Route {
        val ref = RepoRef(link.repo.owner, link.repo.name)
        return when (link.initialTab) {
            RepoDetailTab.Issues -> if (link.number > 0) Route.Issue(ref, link.number) else Route.Repo(ref, RepoSection.Issues)
            RepoDetailTab.Pulls ->
                if (link.number > 0) Route.Pull(ref, link.number, PullTab.Conversation) else Route.Repo(ref, RepoSection.Pulls)
            RepoDetailTab.Discussions ->
                if (link.number > 0) Route.Discussion(ref, link.number) else Route.Repo(ref, RepoSection.Discussions)
            RepoDetailTab.Commits ->
                if (link.reference.isNotEmpty()) Route.Commit(ref, link.reference) else Route.Commits(ref, link.branch)
            RepoDetailTab.Code -> if (link.reference.isNotEmpty()) Route.Commit(ref, link.reference) else Route.Repo(ref, null)
            RepoDetailTab.Mirrors -> Route.Repo(ref, RepoSection.Mirrors)
            RepoDetailTab.Releases -> Route.Repo(ref, RepoSection.Releases)
            RepoDetailTab.About -> Route.Repo(ref, RepoSection.About)
            RepoDetailTab.Actions -> Route.Repo(ref, RepoSection.Actions)
            RepoDetailTab.Agents ->
                if (link.session > 0) Route.LaunchedAgent(ref, link.session.toString()) else Route.Repo(ref, RepoSection.Agents)
            RepoDetailTab.Worktrees -> Route.Repo(ref, RepoSection.Worktrees)
        }
    }

    fun approvalParts(id: String): Pair<String, String>? {
        val parts = id.split('/')
        if (parts.size != 2 || parts[0].isEmpty() || parts[1].isEmpty()) return null
        return parts[0] to parts[1]
    }
}

object InboxFeed {
    const val MAX_ITEMS = 300

    fun taskItem(task: Map<String, Any?>, actor: String): InboxItem? {
        val id = task.str("id")
        if (id.isEmpty() || task.str("assigneeKind") != "user" || !task.str("assignee").equals(actor, ignoreCase = true) ||
            task.str("status") == "done"
        ) {
            return null
        }
        val updated = maxOf(task.long("updatedAt"), task.long("createdAt"))
        return InboxItem(
            id = "task:$id@$updated",
            source = InboxSource.Task,
            category = InboxCategory.Assigned,
            kind = "organization_task",
            title = task.str("title", "Organization task"),
            body = task.str("details").trim().take(280),
            repo = task.str("repository"),
            actor = task.str("createdBy"),
            ts = updated,
            route = Route.Task(id),
            remoteId = id,
        )
    }

    fun merge(pings: List<InboxItem>, tasks: List<InboxItem>, local: List<InboxItem>): List<InboxItem> {
        val seen = HashSet<String>()
        val out = (pings + tasks + local).filter { seen.add(it.id) }
        return out.sortedWith { a, b ->
            val aPinned = a.category == InboxCategory.Approval && a.unread
            val bPinned = b.category == InboxCategory.Approval && b.unread
            when {
                aPinned != bPinned -> if (aPinned) -1 else 1
                a.ts != b.ts -> b.ts.compareTo(a.ts)
                else -> a.id.compareTo(b.id)
            }
        }.take(MAX_ITEMS)
    }

    fun filter(items: List<InboxItem>, filter: InboxFilter, unreadOnly: Boolean = false): List<InboxItem> = items.filter { item ->
        if (unreadOnly && !item.unread) return@filter false
        if (filter == InboxFilter.Pings) item.source != InboxSource.Task else item.category.matches(filter)
    }

    fun unreadCounts(items: List<InboxItem>): Map<InboxFilter, Int> =
        InboxFilter.entries.associateWith { filter(items, it, unreadOnly = true).size }
}

fun pingAgentPrompt(ping: ForkNotification, zone: ZoneId = ZoneId.systemDefault()): String {
    val head = ArrayList<String>()
    val kind = ping.kindLabel.trim()
    if (kind.isNotEmpty()) head += kind
    if (ping.ts > 0) head += DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm").withZone(zone).format(Instant.ofEpochMilli(ping.ts))
    val lines = arrayListOf(if (head.isEmpty()) "ForkMesh ping" else "ForkMesh ping (${head.joinToString(" · ")})")
    ping.title.trim().takeIf { it.isNotEmpty() }?.let { lines += it }
    ping.body.trim().takeIf { it.isNotEmpty() }?.let { lines += listOf("", it) }
    val context = ArrayList<String>()
    ping.repo.trim().takeIf { it.isNotEmpty() }?.let { context += "Repository: $it" }
    ping.actor.trim().takeIf { it.isNotEmpty() }?.let { context += "From: $it" }
    ping.href.trim().takeIf { it.isNotEmpty() }?.let { context += "Link: $it" }
    if (context.isNotEmpty()) lines += listOf("") + context
    return lines.joinToString("\n")
}

class InboxReadState(private val store: PrefStore, private val key: String = KEY) {
    companion object {
        const val KEY = "inbox/read.v1"
        const val DISMISSED_TASKS_KEY = "inbox/dismissed-tasks.v1"
        const val MAX_ENTRIES = 400
    }

    private val order = ArrayList(readList())
    private val set = HashSet(order)

    private fun readList(): List<String> = try {
        (Json.parse(store.getString(key) ?: "[]") as? List<*>).orEmpty().map { it.toString() }.takeLast(MAX_ENTRIES)
    } catch (_: Exception) {
        emptyList()
    }

    fun isRead(id: String): Boolean = id in set

    fun markRead(ids: List<String>) {
        var changed = false
        for (id in ids) {
            if (id.isNotEmpty() && set.add(id)) {
                order += id
                changed = true
            }
        }
        if (!changed) return
        if (order.size > MAX_ENTRIES) {
            val drop = order.size - MAX_ENTRIES
            order.take(drop).forEach(set::remove)
            repeat(drop) { order.removeAt(0) }
        }
        store.putString(key, Json.stringify(order))
    }
}

data class PingSyncOperation(val kind: Kind, val remoteId: String, val attempts: Int = 0, val queuedAtMs: Long) {
    enum class Kind(val raw: String) { MarkRead("markRead"), Delete("delete") }
}

class PingSyncQueue(private val store: PrefStore) {
    companion object {
        const val KEY = "inbox/ping-sync.v1"
        const val MAX_OPERATIONS = 200
        const val MAX_ATTEMPTS = 5
    }

    var operations: List<PingSyncOperation> = load()
        private set

    private fun load(): List<PingSyncOperation> = try {
        (Json.parse(store.getString(KEY) ?: "[]") as? List<*>).orEmpty().mapNotNull {
            val m = it.asJsonMap()
            val kind = PingSyncOperation.Kind.entries.firstOrNull { k -> k.raw == m.str("kind") } ?: return@mapNotNull null
            PingSyncOperation(kind, m.str("remoteId"), m.int("attempts"), m.long("queuedAtMs"))
        }.takeLast(MAX_OPERATIONS)
    } catch (_: Exception) {
        emptyList()
    }

    private fun save() {
        store.putString(
            KEY,
            Json.stringify(operations.map { mapOf("kind" to it.kind.raw, "remoteId" to it.remoteId, "attempts" to it.attempts.toLong(), "queuedAtMs" to it.queuedAtMs) }),
        )
    }

    fun enqueue(kind: PingSyncOperation.Kind, remoteId: String, nowMs: Long) {
        if (remoteId.isEmpty()) return
        var list = operations
        if (kind == PingSyncOperation.Kind.Delete) list = list.filter { it.remoteId != remoteId }
        if (list.any { it.remoteId == remoteId && it.kind == kind }) return
        operations = (list + PingSyncOperation(kind, remoteId, queuedAtMs = nowMs)).takeLast(MAX_OPERATIONS)
        save()
    }

    fun state(remoteId: String): InboxSyncState? {
        val op = operations.firstOrNull { it.remoteId == remoteId } ?: return null
        return if (op.attempts >= MAX_ATTEMPTS) InboxSyncState.Failed else InboxSyncState.Pending
    }

    fun isDeleted(remoteId: String): Boolean = operations.any { it.remoteId == remoteId && it.kind == PingSyncOperation.Kind.Delete }

    fun isMarkedRead(remoteId: String): Boolean = operations.any { it.remoteId == remoteId && it.kind == PingSyncOperation.Kind.MarkRead }

    fun drain(send: (PingSyncOperation) -> Boolean): Int {
        var sent = 0
        val remaining = ArrayList<PingSyncOperation>()
        for (op in operations) {
            when {
                op.attempts >= MAX_ATTEMPTS -> remaining += op
                runCatching { send(op) }.getOrDefault(false) -> sent++
                else -> remaining += op.copy(attempts = op.attempts + 1)
            }
        }
        operations = remaining
        save()
        return sent
    }

    fun discardFailed() {
        operations = operations.filter { it.attempts < MAX_ATTEMPTS }
        save()
    }

    fun removeAll() {
        if (operations.isEmpty()) return
        operations = emptyList()
        save()
    }
}

data class LocalPing(
    val id: String,
    val surface: String,
    val title: String,
    val message: String,
    val tsMs: Long,
    val read: Boolean,
    val kind: String = "",
    val conversation: String = "",
)

class LocalPingLog(
    private val store: PrefStore,
    private val nowMs: () -> Long = System::currentTimeMillis,
    private val makeId: () -> String = { UUID.randomUUID().toString() },
) {
    companion object {
        const val KEY = "inbox/local-pings.v1"
        const val MAX_ENTRIES = 50
        const val MAX_PER_HOUR = 8
        const val MESSAGE_LIMIT = 700
        const val CHAT_KIND = "chat"

        private val patterns = listOf(
            Regex("(?i)(bearer\\s+)[A-Za-z0-9._~+/=-]{8,}") to "$1<redacted>",
            Regex("(?i)((?:token|secret|password|key|sig)[\"']?\\s*[:=]\\s*[\"']?)[^\\s\"'&,}]{4,}") to "$1<redacted>",
            Regex("/(?:Users|home|var/mobile|data/user|data/data|storage/emulated)/[^\\s\"']+") to "<path>",
        )

        fun redact(text: String): String {
            var out = text
            for ((regex, replacement) in patterns) out = regex.replace(out, replacement)
            return out.take(MESSAGE_LIMIT)
        }
    }

    var pings: List<LocalPing> = load()
        private set

    private fun load(): List<LocalPing> = try {
        (Json.parse(store.getString(KEY) ?: "[]") as? List<*>).orEmpty().map {
            val m = it.asJsonMap()
            LocalPing(
                m.str("id"), m.str("surface"), m.str("title"), m.str("message"), m.long("tsMs"), m.bool("read"),
                m.str("kind"), m.str("conversation"),
            )
        }.filter { it.id.isNotEmpty() }.take(MAX_ENTRIES)
    } catch (_: Exception) {
        emptyList()
    }

    private fun save() {
        store.putString(
            KEY,
            Json.stringify(
                pings.map {
                    mapOf(
                        "id" to it.id, "surface" to it.surface, "title" to it.title, "message" to it.message, "tsMs" to it.tsMs,
                        "read" to it.read, "kind" to it.kind, "conversation" to it.conversation,
                    )
                },
            ),
        )
    }

    fun record(surface: String, title: String, message: String): LocalPing? {
        val now = nowMs()
        val clean = redact(message)
        val cleanTitle = title.trim().take(160)
        if (cleanTitle.isEmpty()) return null
        val index = pings.indexOfFirst { it.surface == surface && it.title == cleanTitle && it.message == clean }
        if (index >= 0) {
            val moved = pings[index].copy(tsMs = now, read = false)
            pings = listOf(moved) + pings.filterIndexed { i, _ -> i != index }
            save()
            return moved
        }
        if (pings.count { now - it.tsMs < 3_600_000 } >= MAX_PER_HOUR) return null
        val ping = LocalPing(makeId(), surface.take(60), cleanTitle, clean, now, false)
        pings = (listOf(ping) + pings).take(MAX_ENTRIES)
        save()
        return ping
    }

    @Synchronized
    fun recordChat(messageId: String, sender: String, title: String, message: String, conversation: String, tsMs: Long, read: Boolean): LocalPing? {
        val id = "chat:$messageId"
        val cleanTitle = title.trim().take(160)
        if (messageId.isEmpty() || cleanTitle.isEmpty() || pings.any { it.id == id }) return null
        val ping = LocalPing(id, sender.take(60), cleanTitle, message.take(MESSAGE_LIMIT), tsMs, read, CHAT_KIND, conversation)
        val at = pings.indexOfFirst { it.tsMs < tsMs }.let { if (it < 0) pings.size else it }
        pings = (pings.take(at) + ping + pings.drop(at)).take(MAX_ENTRIES)
        save()
        return ping
    }

    @Synchronized
    fun removeChats(where: (String) -> Boolean): Boolean {
        val kept = pings.filterNot { it.kind == CHAT_KIND && where(it.conversation) }
        if (kept.size == pings.size) return false
        pings = kept
        save()
        return true
    }

    @Synchronized
    fun markConversationRead(conversation: String): Boolean {
        if (pings.none { !it.read && it.conversation == conversation }) return false
        pings = pings.map { if (it.conversation == conversation) it.copy(read = true) else it }
        save()
        return true
    }

    fun markRead(id: String) {
        if (pings.none { it.id == id && !it.read }) return
        pings = pings.map { if (it.id == id) it.copy(read = true) else it }
        save()
    }

    fun markAllRead() {
        if (pings.none { !it.read }) return
        pings = pings.map { it.copy(read = true) }
        save()
    }

    fun remove(id: String) {
        pings = pings.filter { it.id != id }
        save()
    }

    fun removeAll() {
        if (pings.isEmpty()) return
        pings = emptyList()
        save()
    }

    val items: List<InboxItem>
        get() = pings.map {
            InboxItem(
                id = "local:${it.id}", source = InboxSource.Local, category = InboxCategory.Ping,
                kind = it.kind.ifEmpty { "local_failure" }, title = it.title, body = it.message, actor = it.surface, ts = it.tsMs,
                unread = !it.read, sync = InboxSyncState.LocalOnly, route = it.conversation.takeIf { c -> c.isNotEmpty() }?.let(Route::Channel),
                remoteId = it.id,
            )
        }
}
