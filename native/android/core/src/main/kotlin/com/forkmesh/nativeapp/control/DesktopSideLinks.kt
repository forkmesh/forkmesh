package com.forkmesh.nativeapp.control

import java.util.concurrent.CopyOnWriteArrayList

internal class DesktopKeyedTaps {
    private val taps = CopyOnWriteArrayList<Triple<Any, String, (ControlEvent) -> Unit>>()

    fun add(key: String, tap: (ControlEvent) -> Unit): Any {
        val token = Any()
        taps.add(Triple(token, key, tap))
        return token
    }

    fun remove(token: Any): Boolean = taps.removeAll { it.first === token }

    fun deliver(key: String, event: ControlEvent) {
        if (key.isEmpty()) return
        for ((_, wanted, tap) in taps) if (wanted == key) runCatching { tap(event) }
    }
}

internal class DesktopSideLinks(
    private val opener: RelayControlOpener?,
    private val probeMs: Long,
    private val taps: DesktopKeyedTaps,
    private val onClosed: () -> Unit,
) {
    private val lock = Object()
    private val links = HashMap<String, RelayControlConnection>()
    private val opening = HashSet<String>()

    fun open(key: String): RelayControlConnection? = synchronized(lock) { links[key]?.takeIf { it.isOpen } }

    fun link(target: PairedDesktop): ControlLink {
        val key = target.key
        val kept = synchronized(lock) {
            while (key in opening) lock.wait()
            links[key]
        }
        if (kept != null) {
            if (kept.isOpen && (kept.idleMs < probeMs || kept.confirmAlive())) return kept
            synchronized(lock) {
                if (links[key] === kept) links.remove(key)
                tapTokens.remove(kept)
            }
            kept.close()
        }
        val dial = opener ?: throw LocalControlException("not_connected")
        synchronized(lock) {
            while (key in opening) lock.wait()
            links[key]?.takeIf { it.isOpen }?.let { return it }
            opening.add(key)
        }
        try {
            val connection = dial.open(target)
            keep(key, connection)
            return connection
        } finally {
            synchronized(lock) {
                opening.remove(key)
                lock.notifyAll()
            }
        }
    }

    fun adopt(connection: RelayControlConnection): Boolean {
        val key = connection.desktopKey
        if (key.isEmpty() || !connection.isOpen) return false
        keep(key, connection)
        return true
    }

    fun take(key: String): RelayControlConnection? {
        val connection = synchronized(lock) {
            val kept = links[key]?.takeIf { it.isOpen } ?: return null
            links.remove(key)
            kept
        }
        synchronized(lock) { tapTokens.remove(connection) }?.let(connection::removeEventTap)
        return connection
    }

    private val tapTokens = HashMap<RelayControlConnection, Any>()

    private fun keep(key: String, connection: RelayControlConnection) {
        val previous = synchronized(lock) { links.put(key, connection) }
        if (previous != null && previous !== connection) {
            synchronized(lock) { tapTokens.remove(previous) }
            previous.close()
        }
        val token = connection.addEventTap { event -> taps.deliver(key, event) }
        synchronized(lock) { tapTokens.put(connection, token) }?.let(connection::removeEventTap)
        connection.onClose {
            val mine = synchronized(lock) {
                if (links[key] === connection) {
                    links.remove(key)
                    tapTokens.remove(connection)
                    true
                } else {
                    false
                }
            }
            if (mine) onClosed()
        }
    }

    fun closeAll() {
        val all = synchronized(lock) {
            tapTokens.clear()
            links.values.toList().also { links.clear() }
        }
        for (connection in all) runCatching { connection.close() }
    }
}

class DesktopScopedCalling(private val hub: DesktopControlHub, val desktopKey: String) : ControlCalling {
    override fun request(method: String, params: Map<String, Any?>, timeoutMs: Long): Map<String, Any?> =
        hub.request(method, params, timeoutMs, desktopKey)

    override fun addEventTap(tap: (ControlEvent) -> Unit): Any = hub.addEventTap(desktopKey, tap)

    override fun removeEventTap(token: Any) = hub.removeEventTap(token)
}
