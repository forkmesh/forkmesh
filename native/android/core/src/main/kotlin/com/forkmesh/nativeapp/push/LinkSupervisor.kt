package com.forkmesh.nativeapp.push

class LinkSupervisor(
    private val clock: () -> Long = { System.currentTimeMillis() },
    private val catchUpIntervalMs: Long = CATCH_UP_INTERVAL_MS,
) {
    private val connectedSince = LinkedHashMap<String, Long>()
    private val droppedAt = LinkedHashMap<String, Long>()
    private val caughtUpAt = LinkedHashMap<String, Long>()
    private val signedOut = LinkedHashSet<String>()

    @Synchronized
    fun onConnected(id: String) {
        connectedSince[id] = clock()
        droppedAt.remove(id)
        signedOut.remove(id)
        caughtUpAt[id] = clock()
    }

    @Synchronized
    fun onDropped(id: String) {
        if (connectedSince.remove(id) != null || id !in droppedAt) droppedAt[id] = clock()
    }

    @Synchronized
    fun onSignedOut(id: String) {
        connectedSince.remove(id)
        droppedAt.remove(id)
        signedOut.add(id)
    }

    @Synchronized
    fun forget(id: String) {
        connectedSince.remove(id)
        droppedAt.remove(id)
        caughtUpAt.remove(id)
        signedOut.remove(id)
    }

    @Synchronized
    fun connected(): Set<String> = connectedSince.keys.toSet()

    @Synchronized
    fun isSignedOut(id: String): Boolean = id in signedOut

    @Synchronized
    fun downForMs(id: String): Long = droppedAt[id]?.let { clock() - it } ?: 0

    @Synchronized
    fun catchUpDue(id: String): Boolean {
        if (id in signedOut || id in connectedSince) return false
        val last = caughtUpAt[id] ?: return true
        return clock() - last >= catchUpIntervalMs
    }

    @Synchronized
    fun markCaughtUp(id: String) {
        caughtUpAt[id] = clock()
    }

    companion object {
        const val CATCH_UP_INTERVAL_MS = 10 * 60_000L
    }
}
