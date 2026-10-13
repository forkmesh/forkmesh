package com.forkmesh.nativeapp.inbox

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.model.NotificationPage

class PingSnapshots(private val store: PrefStore, private val nowMs: () -> Long = System::currentTimeMillis) {
    companion object {
        const val KEY = "inbox/pings.v1"
        const val MAX_ROWS = 100
        const val INSERTED_TTL_MS = 5 * 60_000L

        fun key(project: String, node: String): String = "$KEY/${project.trim().ifEmpty { "_" }}/${node.trim().lowercase()}"

        fun merge(page: NotificationPage, extra: List<ForkNotification>): NotificationPage {
            val ids = page.notifications.mapTo(HashSet()) { it.id }
            val added = extra.filter { it.id.isNotEmpty() && ids.add(it.id) }.sortedByDescending { it.ts }
            if (added.isEmpty()) return capped(page)
            return capped(NotificationPage(added + page.notifications, page.unread + added.count { it.isUnread }, page.total + added.size))
        }

        private fun capped(page: NotificationPage): NotificationPage =
            if (page.notifications.size <= MAX_ROWS) page else page.copy(notifications = page.notifications.take(MAX_ROWS))
    }

    private class Inserted(val row: ForkNotification, val atMs: Long)

    private val lock = Any()
    private val inserted = HashMap<String, List<Inserted>>()

    private fun read(key: String): NotificationPage? = try {
        store.getString(key)?.let { NotificationPage.fromJson(Json.parse(it).asJsonMap()) }
    } catch (_: Exception) {
        null
    }

    private fun write(key: String, page: NotificationPage) {
        store.putString(key, Json.stringify(capped(page).toJson()))
    }

    private fun live(key: String): List<Inserted> {
        val now = nowMs()
        val kept = inserted[key].orEmpty().filter { now - it.atMs in 0 until INSERTED_TTL_MS }
        if (kept.isEmpty()) inserted.remove(key) else inserted[key] = kept
        return kept
    }

    fun load(project: String, node: String): NotificationPage? {
        if (node.isBlank()) return null
        val key = key(project, node)
        synchronized(lock) {
            val saved = read(key)
            val extra = live(key).map { it.row }
            if (saved == null && extra.isEmpty()) return null
            return merge(saved ?: NotificationPage(emptyList(), 0), extra)
        }
    }

    fun find(project: String, node: String, id: String): ForkNotification? =
        if (id.isEmpty()) null else load(project, node)?.notifications?.firstOrNull { it.id == id }

    fun save(project: String, node: String, page: NotificationPage): NotificationPage {
        if (node.isBlank()) return page
        val key = key(project, node)
        synchronized(lock) {
            val ids = page.notifications.mapTo(HashSet()) { it.id }
            val kept = live(key).filter { it.row.id !in ids }
            if (kept.isEmpty()) inserted.remove(key) else inserted[key] = kept
            val merged = merge(page, kept.map { it.row })
            write(key, merged)
            return merged
        }
    }

    fun insert(project: String, node: String, row: ForkNotification): Boolean {
        if (node.isBlank() || row.id.isEmpty()) return false
        val key = key(project, node)
        synchronized(lock) {
            inserted[key] = live(key).filter { it.row.id != row.id } + Inserted(row, nowMs())
            val saved = read(key) ?: NotificationPage(emptyList(), 0)
            if (saved.notifications.any { it.id == row.id }) return false
            write(key, merge(saved, listOf(row)))
            return true
        }
    }

    fun replace(project: String, node: String, page: NotificationPage) {
        if (node.isBlank()) return
        synchronized(lock) { write(key(project, node), page) }
    }

    fun markRead(project: String, node: String, ids: List<String>?, atMs: Long) {
        if (node.isBlank()) return
        val key = key(project, node)
        synchronized(lock) {
            val wanted = ids?.toHashSet()
            fun mark(row: ForkNotification) = if (row.isUnread && (wanted == null || row.id in wanted)) row.copyWith(readAt = atMs) else row
            inserted[key]?.let { list -> inserted[key] = list.map { Inserted(mark(it.row), it.atMs) } }
            val saved = read(key) ?: return
            val rows = saved.notifications.map(::mark)
            val cleared = saved.notifications.count { it.isUnread } - rows.count { it.isUnread }
            write(key, NotificationPage(rows, (saved.unread - cleared).coerceAtLeast(0), saved.total))
        }
    }

    fun remove(project: String, node: String, id: String) {
        if (node.isBlank() || id.isEmpty()) return
        val key = key(project, node)
        synchronized(lock) {
            inserted[key]?.let { list -> inserted[key] = list.filter { it.row.id != id } }
            val saved = read(key) ?: return
            val row = saved.notifications.firstOrNull { it.id == id } ?: return
            write(
                key,
                NotificationPage(
                    saved.notifications.filter { it.id != id },
                    if (row.isUnread) (saved.unread - 1).coerceAtLeast(0) else saved.unread,
                    (saved.total - 1).coerceAtLeast(0),
                ),
            )
        }
    }

    fun clear(project: String, node: String) {
        if (node.isBlank()) return
        val key = key(project, node)
        synchronized(lock) {
            inserted.remove(key)
            store.remove(key)
        }
    }
}
