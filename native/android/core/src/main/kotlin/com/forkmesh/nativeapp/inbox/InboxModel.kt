package com.forkmesh.nativeapp.inbox

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.model.NotificationPage
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.navigation.ComposeTarget
import com.forkmesh.nativeapp.navigation.InboxFilter
import com.forkmesh.nativeapp.navigation.RepoRef
import java.net.URLEncoder
import java.nio.charset.StandardCharsets

fun interface InboxClient {
    fun call(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?>
}

enum class PingPauseChoice(val label: String, val durationMs: Long) {
    OneHour("For 1 hour", 3_600_000L),
    OneDay("For 1 day", 86_400_000L),
    OneWeek("Until next week", 7 * 86_400_000L),
}

class InboxModel(
    private val client: InboxClient,
    private val node: () -> String,
    store: PrefStore,
    var filter: InboxFilter = InboxFilter.All,
    private val nowMs: () -> Long = System::currentTimeMillis,
    localPings: LocalPingLog? = null,
    private val snapshots: PingSnapshots? = null,
    private val project: String = "",
) {
    companion object {
        const val PING_PAGE_SIZE = 100
        const val MAX_PAUSE_MS: Long = 30L * 86_400_000
        private val taskAccessCodes = setOf("org_member_required", "forbidden", "http_403", "http_404")
    }

    val readState = InboxReadState(store)
    val dismissedTasks = InboxReadState(store, InboxReadState.DISMISSED_TASKS_KEY)
    val syncQueue = PingSyncQueue(store)
    val localPings: LocalPingLog = localPings ?: LocalPingLog(store, nowMs)

    var items: List<InboxItem> = emptyList()
        private set
    var errors: List<String> = emptyList()
        private set
    var pauseUntilMs: Long = 0
        private set
    var loaded: Boolean = false
        private set
    var unreadOnly: Boolean = false
    var onChanged: (() -> Unit)? = null

    private var pingItems: List<InboxItem> = emptyList()
    private var taskItems: List<InboxItem> = emptyList()

    init {
        val me = account
        if (me.isNotEmpty()) {
            snapshots?.load(project, me)?.let {
                pingItems = pingItemsOf(it.notifications)
                rebuild()
            }
        }
    }

    val visible: List<InboxItem> get() = InboxFeed.filter(items, filter, unreadOnly)
    val unreadCounts: Map<InboxFilter, Int> get() = InboxFeed.unreadCounts(items)
    val unreadTotal: Int get() = unreadCounts[InboxFilter.All] ?: 0
    val isPaused: Boolean get() = pauseUntilMs > nowMs()
    val pendingSyncCount: Int get() = syncQueue.operations.size
    val canDeleteAllPings: Boolean get() = items.isNotEmpty()

    val pauseLabel: String
        get() {
            if (!isPaused) return ""
            val minutes = (pauseUntilMs - nowMs() + 59_999) / 60_000
            fun phrase(count: Long, unit: String) = "Pings paused for $count more $unit${if (count == 1L) "" else "s"}"
            if (minutes < 60) return phrase(minutes, "minute")
            val hours = (minutes + 59) / 60
            if (hours < 48) return phrase(hours, "hour")
            return phrase((hours + 23) / 24, "day")
        }

    private val account: String get() = node().trim().lowercase()

    private fun rebuild() {
        items = InboxFeed.merge(pingItems, taskItems, localPings.items)
    }

    fun refreshLocal() = rebuild()

    fun load() {
        val me = account
        if (me.isEmpty()) {
            items = localPings.items
            errors = listOf("Sign in to see your inbox.")
            return
        }
        val problems = ArrayList<String>()
        syncQueue.drain { send(it, me) }
        try {
            val encoded = URLEncoder.encode(me, StandardCharsets.UTF_8).replace("+", "%20")
            val data = client.call("GET", "/api/notifications?node=$encoded&limit=$PING_PAGE_SIZE", null)
            val page = NotificationPage.fromJson(data).let { snapshots?.save(project, me, it) ?: it }
            pauseUntilMs = data.long("pauseUntil")
            pingItems = pingItemsOf(page.notifications)
        } catch (_: Exception) {
            problems += "Pings could not be loaded."
        }
        try {
            val data = client.call("GET", "/api/tasks", null)
            val actor = data.str("actor").ifEmpty { me }.lowercase()
            taskItems = data.maps("tasks").mapNotNull { InboxFeed.taskItem(it, actor) }
                .filterNot { dismissedTasks.isRead(it.id) }.map { it.copy(unread = !readState.isRead(it.id)) }
        } catch (error: OrgApiException) {
            if (error.code in taskAccessCodes) taskItems = emptyList() else problems += "Assigned tasks could not be loaded."
        } catch (_: Exception) {
            problems += "Assigned tasks could not be loaded."
        }
        errors = problems
        loaded = true
        rebuild()
    }

    private fun pingItemsOf(rows: List<ForkNotification>): List<InboxItem> = rows.mapNotNull { ping ->
        if (syncQueue.isDeleted(ping.id)) return@mapNotNull null
        var item = InboxItem.fromPing(ping)
        if (syncQueue.isMarkedRead(ping.id)) item = item.copy(unread = false)
        syncQueue.state(ping.id)?.let { item = item.copy(sync = it) }
        item
    }

    fun insert(ping: ForkNotification): InboxItem? {
        if (ping.id.isEmpty()) return null
        val me = account
        if (me.isNotEmpty()) snapshots?.insert(project, me, ping)
        pingItems.firstOrNull { it.remoteId == ping.id }?.let { return it }
        val item = pingItemsOf(listOf(ping)).firstOrNull() ?: return null
        pingItems = listOf(item) + pingItems
        rebuild()
        return item
    }

    private fun send(op: PingSyncOperation, node: String): Boolean = try {
        when (op.kind) {
            PingSyncOperation.Kind.MarkRead -> client.call("POST", "/api/notifications", mapOf("node" to node, "ids" to listOf(op.remoteId)))
            PingSyncOperation.Kind.Delete -> client.call("DELETE", "/api/notifications", mapOf("node" to node, "id" to op.remoteId))
        }
        true
    } catch (_: Exception) {
        false
    }

    private fun update(id: String, change: (InboxItem) -> InboxItem) {
        pingItems = pingItems.map { if (it.id == id) change(it) else it }
        taskItems = taskItems.map { if (it.id == id) change(it) else it }
        rebuild()
    }

    fun markRead(item: InboxItem) {
        if (!item.unread) return
        when (item.source) {
            InboxSource.Local -> {
                localPings.markRead(item.remoteId)
                rebuild()
            }
            InboxSource.Task -> {
                readState.markRead(listOf(item.id))
                update(item.id) { it.copy(unread = false) }
            }
            InboxSource.Ping -> {
                update(item.id) { it.copy(unread = false) }
                snapshots?.markRead(project, account, listOf(item.remoteId), nowMs())
                if (!send(PingSyncOperation(PingSyncOperation.Kind.MarkRead, item.remoteId, queuedAtMs = nowMs()), account)) {
                    syncQueue.enqueue(PingSyncOperation.Kind.MarkRead, item.remoteId, nowMs())
                    update(item.id) { it.copy(sync = InboxSyncState.Pending) }
                }
                onChanged?.invoke()
            }
        }
    }

    fun markAllRead() {
        readState.markRead(taskItems.map { it.id })
        localPings.markAllRead()
        val unreadPings = pingItems.filter { it.unread }
        pingItems = pingItems.map { it.copy(unread = false) }
        taskItems = taskItems.map { it.copy(unread = false) }
        rebuild()
        if (unreadPings.isEmpty()) return
        snapshots?.markRead(project, account, unreadPings.map { it.remoteId }, nowMs())
        try {
            client.call("POST", "/api/notifications", mapOf("node" to account, "all" to true))
        } catch (_: Exception) {
            unreadPings.forEach { syncQueue.enqueue(PingSyncOperation.Kind.MarkRead, it.remoteId, nowMs()) }
        }
        onChanged?.invoke()
    }

    fun delete(item: InboxItem) {
        when (item.source) {
            InboxSource.Local -> {
                localPings.remove(item.remoteId)
                rebuild()
            }
            InboxSource.Task -> {
                readState.markRead(listOf(item.id))
                dismissedTasks.markRead(listOf(item.id))
                taskItems = taskItems.filter { it.id != item.id }
                rebuild()
            }
            InboxSource.Ping -> {
                pingItems = pingItems.filter { it.id != item.id }
                snapshots?.remove(project, account, item.remoteId)
                rebuild()
                if (!send(PingSyncOperation(PingSyncOperation.Kind.Delete, item.remoteId, queuedAtMs = nowMs()), account)) {
                    syncQueue.enqueue(PingSyncOperation.Kind.Delete, item.remoteId, nowMs())
                }
                onChanged?.invoke()
            }
        }
    }

    fun deleteAllPings(): String? {
        val me = account
        if (me.isNotEmpty()) {
            try {
                client.call("DELETE", "/api/notifications", mapOf("node" to me, "all" to true))
            } catch (_: Exception) {
                return "Pings could not be deleted."
            }
            pingItems = emptyList()
            syncQueue.removeAll()
            snapshots?.clear(project, me)
        }
        readState.markRead(taskItems.map { it.id })
        dismissedTasks.markRead(taskItems.map { it.id })
        taskItems = emptyList()
        localPings.removeAll()
        rebuild()
        onChanged?.invoke()
        return null
    }

    fun item(remoteId: String): InboxItem? = items.firstOrNull { it.remoteId == remoteId }

    fun pause(choice: PingPauseChoice?): String? {
        val now = nowMs()
        val until = choice?.let { now + minOf(it.durationMs, MAX_PAUSE_MS) } ?: 0L
        return try {
            val data = client.call("POST", "/api/notifications", mapOf("node" to account, "pauseUntil" to until))
            pauseUntilMs = if (data.containsKey("pauseUntil")) data.long("pauseUntil") else until
            null
        } catch (_: Exception) {
            "Pings could not be ${if (choice == null) "resumed" else "paused"}."
        }
    }

    fun prompt(item: InboxItem): ComposeTarget {
        val text = if (item.source == InboxSource.Ping) {
            pingAgentPrompt(
                ForkNotification(
                    id = item.remoteId, kind = item.kind, title = item.title, body = item.body, repo = item.repo,
                    href = item.href, actor = item.actor, ts = item.ts,
                ),
            )
        } else {
            listOf(item.title, item.body).filter { it.isNotEmpty() }.joinToString("\n\n")
        }
        return ComposeTarget(repo = RepoRef.parse(item.repo), text = text)
    }
}
