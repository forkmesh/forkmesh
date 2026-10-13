package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.inbox.PingSnapshots
import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.model.NotificationPage
import java.util.concurrent.CopyOnWriteArrayList

class NotificationInbox(
    private val api: ApiService,
    private val nodeName: () -> String,
    private val snapshots: PingSnapshots? = null,
    private val project: String = "",
    private val clock: () -> Long = { System.currentTimeMillis() },
) {
    private val lock = Any()
    private var items: List<ForkNotification> = emptyList()
    private var unread = 0
    private var total = 0
    private var busy = false
    private var lastError = ""
    private var activeNode = ""
    private var fetched = false
    private val listeners = CopyOnWriteArrayList<() -> Unit>()

    private val node: String get() = nodeName().trim().lowercase()

    init {
        val node = this.node
        if (node.isNotEmpty() && snapshots != null) synchronized(lock) { switchTo(node) }
    }

    private fun switchTo(node: String) {
        activeNode = node
        items = emptyList()
        unread = 0
        total = 0
        lastError = ""
        busy = false
        fetched = false
        snapshots?.load(project, node)?.let {
            items = it.notifications
            unread = it.unread
            total = it.total
        }
    }

    private fun persist(node: String) {
        val store = snapshots ?: return
        val page = synchronized(lock) { if (node != activeNode) null else NotificationPage(items, unread, total) } ?: return
        store.replace(project, node, page)
    }

    private fun hasCurrentNode(): Boolean = node == activeNode

    val notifications: List<ForkNotification>
        get() = synchronized(lock) { if (hasCurrentNode()) items else emptyList() }

    val unreadCount: Int get() = synchronized(lock) { if (hasCurrentNode()) unread else 0 }

    val inboxTotal: Int get() = synchronized(lock) { if (hasCurrentNode()) total else 0 }

    val loading: Boolean get() = synchronized(lock) { hasCurrentNode() && busy }

    val error: String get() = synchronized(lock) { if (hasCurrentNode()) lastError else "" }

    val hasLoaded: Boolean
        get() = synchronized(lock) {
            hasCurrentNode() && (busy || items.isNotEmpty() || lastError.isNotEmpty())
        }

    fun addListener(listener: () -> Unit) {
        listeners.add(listener)
    }

    fun removeListener(listener: () -> Unit) {
        listeners.remove(listener)
    }

    private fun notifyListeners() {
        for (listener in listeners) listener()
    }

    fun needsRefresh(): Boolean = node != synchronized(lock) { activeNode } || !hasLoaded || !synchronized(lock) { fetched }

    fun find(id: String): ForkNotification? = if (id.isEmpty()) null else notifications.firstOrNull { it.id == id }

    fun insert(row: ForkNotification): Boolean {
        val node = this.node
        if (row.id.isEmpty() || node.isEmpty()) return false
        snapshots?.insert(project, node, row)
        var added = false
        synchronized(lock) {
            if (node != activeNode) switchTo(node)
            if (items.none { it.id == row.id }) {
                items = listOf(row) + items
                if (row.isUnread) unread += 1
                total += 1
                added = true
            }
        }
        notifyListeners()
        return added
    }

    fun accept(node: String, page: NotificationPage) {
        val clean = node.trim().lowercase()
        if (clean.isEmpty() || clean != this.node) return
        synchronized(lock) {
            if (clean != activeNode) switchTo(clean)
            items = page.notifications
            unread = page.unread
            total = page.total
            lastError = ""
            fetched = true
        }
        notifyListeners()
    }

    fun refresh(silent: Boolean = false) {
        val node = this.node
        var changed = false
        synchronized(lock) {
            if (node != activeNode) {
                switchTo(node)
                changed = true
            }
        }
        if (changed) notifyListeners()
        if (node.isEmpty()) return
        var quietRead: Boolean
        synchronized(lock) {
            quietRead = silent && (items.isNotEmpty() || lastError.isNotEmpty())
            if (!quietRead) {
                busy = true
                lastError = ""
            }
        }
        if (!quietRead) notifyListeners()
        try {
            val page = api.notifications(node).let { snapshots?.save(project, node, it) ?: it }
            synchronized(lock) {
                if (node != activeNode) return
                fetched = true
                items = page.notifications
                unread = page.unread
                total = page.total
                lastError = ""
            }
        } catch (error: Exception) {
            synchronized(lock) {
                if (node == activeNode) {
                    lastError = error.toString()
                    fetched = true
                }
            }
        } finally {
            var current: Boolean
            synchronized(lock) {
                current = node == activeNode
                if (current) busy = false
            }
            if (current) notifyListeners()
        }
    }

    fun markAllRead() {
        val node = this.node
        synchronized(lock) {
            if (node.isEmpty() || node != activeNode || items.isEmpty()) return
        }
        api.markNotificationsRead(node, all = true)
        synchronized(lock) {
            val now = clock()
            items = items.map { it.copyWith(readAt = now) }
            unread = 0
        }
        snapshots?.markRead(project, node, null, clock())
        notifyListeners()
    }

    fun deleteAll() {
        val node = this.node
        synchronized(lock) {
            if (node.isEmpty() || node != activeNode || items.isEmpty()) return
        }
        api.deleteAllNotifications(node)
        synchronized(lock) {
            if (node != activeNode) return
            items = emptyList()
            unread = 0
            total = 0
            lastError = ""
        }
        snapshots?.clear(project, node)
        notifyListeners()
    }

    fun delete(notification: ForkNotification) {
        val id = notification.id
        val node = this.node
        val (index, removed) = synchronized(lock) {
            if (id.isEmpty() || node.isEmpty() || node != activeNode) return
            val at = items.indexOfFirst { it.id == id }
            if (at < 0) return
            val row = items[at]
            items = items.filterIndexed { i, _ -> i != at }
            if (row.isUnread && unread > 0) unread -= 1
            if (total > 0) total -= 1
            at to row
        }
        snapshots?.remove(project, node, id)
        notifyListeners()
        try {
            api.deleteNotification(node, id)
        } catch (error: Exception) {
            var restored = false
            synchronized(lock) {
                if (node == activeNode && items.none { it.id == id }) {
                    items = items.toMutableList().apply { add(index.coerceIn(0, size), removed) }
                    if (removed.isUnread) unread += 1
                    total += 1
                    restored = true
                }
            }
            if (restored) {
                persist(node)
                notifyListeners()
            }
            throw error
        }
    }

    fun markRead(notification: ForkNotification) {
        if (!notification.isUnread || notification.id.isEmpty()) return
        val node = this.node
        synchronized(lock) {
            if (node.isEmpty() || node != activeNode) return
        }
        api.markNotificationsRead(node, ids = listOf(notification.id))
        synchronized(lock) {
            val now = clock()
            items = items.map { if (it.id == notification.id) it.copyWith(readAt = now) else it }
            unread = items.count { it.isUnread }
        }
        snapshots?.markRead(project, node, listOf(notification.id), clock())
        notifyListeners()
    }
}
