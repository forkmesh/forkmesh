package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.AccountEventLoop
import com.forkmesh.nativeapp.api.AccountSettings
import com.forkmesh.nativeapp.api.HttpTransport
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.api.ServerUrl
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.model.auth.AuthSession
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.security.DeviceIdentity
import java.util.concurrent.CopyOnWriteArrayList
import kotlin.math.roundToLong
import kotlin.random.Random

data class NodeSyncToken(val ts: String, val sig: String)

data class NodeSyncSnapshot(
    val mode: NodeSyncMode,
    val connection: NodeSyncConnection,
    val payload: NodeSyncPayload,
    val carried: List<NodeSyncItem>,
    val lastSyncAt: Long?,
    val lastError: String,
    val syncing: Boolean,
    val started: Boolean,
    val ackedTotal: Int,
    val owner: String,
) {
    val pendingTotal: Int get() = payload.items.size

    val isActingAsNode: Boolean get() = mode == NodeSyncMode.ACTIVE

    val counts: Map<NodeSyncKind, Int> get() = NodeSyncKind.entries.associateWith { payload.countOf(it) }
}

class NodeSyncEngine(
    private val settings: AccountSettings,
    private val identity: DeviceIdentity,
    private val session: () -> AuthSession?,
    private val prefs: PrefStore,
    private val transport: HttpTransport = HttpTransport.DEFAULT,
    private val connector: AccountEventLoop.Connector = AccountEventLoop.Connector.WEB_SOCKET,
    private val pollIntervalMs: Long = 60_000,
    private val requestTimeoutMs: Int = 20_000,
    private val random: Random = Random.Default,
    val loop: EventLoop = EventLoop("NodeSync"),
    private val projectId: () -> String = { "" },
) {
    companion object {
        const val CANONICAL_PREFIX = "forkmesh-issues-pull-v1"

        val ACKABLE_KINDS = listOf(NodeSyncKind.ISSUES, NodeSyncKind.PULLS, NodeSyncKind.DISCUSSIONS)

        const val INITIAL_RECONNECT_DELAY_MS = 2_000L
        const val MAX_RECONNECT_DELAY_MS = 30_000L

        const val CARRIED_LIMIT = 100
        const val CARRIED_PREFS_KEY = "nodeSync/carried"

        fun carriedPrefsKeyFor(relayId: String): String = if (relayId.isEmpty()) CARRIED_PREFS_KEY else "$CARRIED_PREFS_KEY/$relayId"

        fun syncCanonical(owner: String, ts: String): String = "$CANONICAL_PREFIX\n$owner\n$ts"

        private fun errorCode(body: String): String = try {
            val decoded = Json.parse(body)
            if (decoded is Map<*, *> && decoded["error"] != null) com.forkmesh.nativeapp.json.jsonText(decoded["error"]) else ""
        } catch (_: Exception) {
            ""
        }
    }

    @Volatile
    var applier: NodeSyncApplier? = null

    @Volatile
    var onNewItems: (() -> Unit)? = null

    @Volatile
    var onUnauthorized: ((String) -> Unit)? = null

    private val listeners = CopyOnWriteArrayList<() -> Unit>()

    fun addListener(listener: () -> Unit) {
        listeners.add(listener)
    }

    fun removeListener(listener: () -> Unit) {
        listeners.remove(listener)
    }

    private var lastMode = NodeSyncMode.DISABLED
    private var connectionState = NodeSyncConnection.OFFLINE
    private var currentPayload = NodeSyncPayload()
    private var carriedItems: List<NodeSyncItem> = emptyList()
    private var lastSync: Long? = null
    private var error = ""
    private var isSyncing = false
    private var isStarted = false
    private var disposed = false
    private var foreground = true
    private var acked = 0
    private var syncGeneration = 0

    private var pollTimer: LoopTimer? = null
    private var reconnectTimer: LoopTimer? = null
    private var reconnectDelayMs = INITIAL_RECONNECT_DELAY_MS
    private var socket: AccountEventLoop.Socket? = null
    private var socketGeneration = 0
    private val earlyFrames = ArrayList<Pair<Int, String>>()

    private val settingsListener: () -> Unit = { loop.post { onSettingsChanged() } }

    val mode: NodeSyncMode get() = computeMode()
    val connection: NodeSyncConnection get() = loop.call { connectionState }
    val payload: NodeSyncPayload get() = loop.call { currentPayload }
    val repos: List<NodeSyncRepoState> get() = payload.repos
    val items: List<NodeSyncItem> get() = payload.items

    val carried: List<NodeSyncItem> get() = loop.call { carriedItems }
    val lastSyncAt: Long? get() = loop.call { lastSync }
    val lastError: String get() = loop.call { error }
    val syncing: Boolean get() = loop.call { isSyncing }
    val started: Boolean get() = loop.call { isStarted }
    val isActingAsNode: Boolean get() = mode == NodeSyncMode.ACTIVE
    val ackedTotal: Int get() = loop.call { acked }
    val pendingTotal: Int get() = payload.items.size
    val counts: Map<NodeSyncKind, Int> get() = payload.let { p -> NodeSyncKind.entries.associateWith { p.countOf(it) } }

    fun snapshot(): NodeSyncSnapshot = loop.call {
        NodeSyncSnapshot(
            mode = computeMode(),
            connection = connectionState,
            payload = currentPayload,
            carried = carriedItems,
            lastSyncAt = lastSync,
            lastError = error,
            syncing = isSyncing,
            started = isStarted,
            ackedTotal = acked,
            owner = owner,
        )
    }

    val owner: String
        get() {
            val current = session()
            if (current == null || !current.isMobileNode) return ""
            return dartTrimText(current.nodeName).lowercase()
        }

    val hasDesktopNode: Boolean get() = session()?.desktopDevices?.isNotEmpty() ?: false

    fun signedToken(owner: String, nowMs: Long? = null): NodeSyncToken {
        val ts = "${nowMs ?: System.currentTimeMillis()}"
        val sig = identity.sign(syncCanonical(owner, ts).toByteArray(Charsets.UTF_8))
        return NodeSyncToken(ts, sig)
    }

    private fun httpUri(path: String, query: Map<String, String>): String =
        ServerUrl.parse(settings.serverUrl).http(path, query)

    fun syncUri(nowMs: Long? = null): String {
        val account = owner
        val token = signedToken(account, nowMs)
        return httpUri("/api/sync", linkedMapOf("owner" to account, "ts" to token.ts, "sig" to token.sig))
    }

    fun eventsUri(nowMs: Long? = null): String {
        val account = owner
        val token = signedToken(account, nowMs)
        return ServerUrl.parse(settings.serverUrl)
            .webSocket("/api/nodes/events", linkedMapOf("owner" to account, "ts" to token.ts, "sig" to token.sig))
    }

    fun start() = loop.call {
        if (disposed || isStarted) return@call
        isStarted = true
        settings.addListener(settingsListener)
        foreground = true
        evaluate(kick = true)
    }

    fun stop() = loop.call {
        isStarted = false
        syncGeneration++
        pollTimer?.cancel()
        pollTimer = null
        reconnectTimer?.cancel()
        reconnectTimer = null
        closeSocket()
        setConnection(NodeSyncConnection.OFFLINE)
    }

    fun refreshSession() = loop.post {
        if (!disposed) evaluate(kick = true)
    }

    private fun onSettingsChanged() {
        if (disposed || computeMode() == lastMode) return
        evaluate(kick = true)
    }

    fun setForeground(resumed: Boolean) = loop.post {
        if (disposed || resumed == foreground) return@post
        foreground = resumed
        if (resumed) {
            reconnectDelayMs = INITIAL_RECONNECT_DELAY_MS
            evaluate(kick = true)
        } else {
            pollTimer?.cancel()
            pollTimer = null
            reconnectTimer?.cancel()
            reconnectTimer = null
            syncGeneration++
            closeSocket()
            setConnection(NodeSyncConnection.OFFLINE)
        }
    }

    private fun evaluate(kick: Boolean = false) {
        val next = computeMode()
        val changed = next != lastMode
        lastMode = next
        if (next != NodeSyncMode.ACTIVE || !isStarted || !foreground) {
            pollTimer?.cancel()
            pollTimer = null
            reconnectTimer?.cancel()
            reconnectTimer = null
            syncGeneration++
            closeSocket()
            if (connectionState != NodeSyncConnection.OFFLINE) connectionState = NodeSyncConnection.OFFLINE
            if (changed) notifyListeners()
            return
        }
        if (pollTimer == null) {
            pollTimer = loop.periodic(pollIntervalMs) { loop.io { syncNow(manual = false) } }
        }
        if (socket == null && reconnectTimer?.isActive != true) openSocket()
        if (changed) notifyListeners()
        if (kick) loop.io { syncNow(manual = false) }
    }

    private fun computeMode(): NodeSyncMode {
        if (!settings.serveAsNode) return NodeSyncMode.DISABLED
        val current = session()
        if (current == null || !current.isMobileNode) return NodeSyncMode.DISABLED
        if (dartTrimText(current.nodeName).isEmpty()) return NodeSyncMode.DISABLED
        if (current.desktopDevices.isNotEmpty()) return NodeSyncMode.STANDBY
        return NodeSyncMode.ACTIVE
    }

    fun dispose() {
        loop.call {
            disposed = true
            settings.removeListener(settingsListener)
            pollTimer?.cancel()
            pollTimer = null
            reconnectTimer?.cancel()
            reconnectTimer = null
            closeSocket()
        }
        loop.awaitIdle(2_000)
        listeners.clear()
        loop.shutdown()
    }

    fun syncNow(manual: Boolean = true) {
        if (loop.isOnLoop) {
            loop.io { syncNow(manual) }
            return
        }
        val begun = loop.call {
            if (disposed || isSyncing) return@call null
            val account = owner
            if (account.isEmpty() || !settings.serveAsNode) return@call null
            if (!manual && computeMode() != NodeSyncMode.ACTIVE) return@call null
            val generation = ++syncGeneration
            isSyncing = true
            notifyListeners()
            generation to account
        } ?: return
        val (generation, account) = begun
        try {
            val uri = syncUri()
            val response = transport.send(
                HttpClient.Request.get(uri)
                    .header("accept", "application/json")
                    .timeouts(requestTimeoutMs, requestTimeoutMs),
            )
            if (loop.call { generation != syncGeneration || disposed }) return
            if (response.status == 401 || response.status == 403) {
                val code = errorCode(response.text())
                loop.call { error = code.ifEmpty { "unauthorized" } }
                onUnauthorized?.invoke(code)
                return
            }
            if (response.status < 200 || response.status >= 300) {
                loop.call { error = "HTTP ${response.status}" }
                return
            }
            val next = NodeSyncPayload.parse(Json.parse(response.text()))
            val fresh = loop.call {
                val count = newItemCount(next)
                currentPayload = next
                lastSync = System.currentTimeMillis()
                error = ""
                count
            }
            carry(next)
            if (loop.call { generation != syncGeneration || disposed }) return
            ackApplied(next, account)
            if (fresh > 0) onNewItems?.invoke()
        } catch (failure: Exception) {
            loop.call { if (generation == syncGeneration && !disposed) error = dartErrorText(failure) }
        } finally {
            loop.call {
                if (!disposed) {
                    isSyncing = false
                    notifyListeners()
                }
            }
        }
    }

    private fun newItemCount(next: NodeSyncPayload): Int {
        val seen = currentPayload.items.map { "${it.repoFullName}:${it.kind}:${it.id}" }.toHashSet()
        return next.items.count { "${it.repoFullName}:${it.kind}:${it.id}" !in seen }
    }

    private fun carry(next: NodeSyncPayload) {
        val drained = next.items.filter {
            it.kind == NodeSyncKind.ABOUT || it.kind == NodeSyncKind.AGENT_PROMPTS || it.kind == NodeSyncKind.BRANCH_OPS
        }
        if (drained.isEmpty()) return
        val stored = loop.call {
            val merged = carriedItems + drained
            carriedItems = if (merged.size > CARRIED_LIMIT) merged.subList(merged.size - CARRIED_LIMIT, merged.size).toList() else merged
            carriedItems
        }
        try {
            prefs.putString(carriedKey, Json.stringify(stored.map { it.toJson() }))
        } catch (_: Exception) {
        }
    }

    private val carriedKey: String get() = carriedPrefsKeyFor(projectId())

    fun loadCarried() {
        loop.call { carriedItems = emptyList() }
        try {
            val key = carriedKey
            if (key != CARRIED_PREFS_KEY) {
                val legacy = prefs.getString(CARRIED_PREFS_KEY) ?: ""
                if (legacy.isNotEmpty()) {
                    if ((prefs.getString(key) ?: "").isEmpty()) prefs.putString(key, legacy)
                    prefs.remove(CARRIED_PREFS_KEY)
                }
            }
            val raw = prefs.getString(key) ?: ""
            if (raw.isEmpty()) {
                loop.call { if (!disposed) notifyListeners() }
                return
            }
            val decoded = Json.parse(raw) as? List<*> ?: return
            val restored = ArrayList<NodeSyncItem>()
            for (row in decoded) {
                if (row !is Map<*, *>) continue
                NodeSyncItem.fromJson(stringKeyed(row))?.let(restored::add)
            }
            loop.call {
                carriedItems = restored
                if (!disposed) notifyListeners()
            }
        } catch (_: Exception) {
        }
    }

    fun clearCarried() {
        loop.call { carriedItems = emptyList() }
        try {
            prefs.remove(carriedKey)
        } catch (_: Exception) {
        }
        loop.call { if (!disposed) notifyListeners() }
    }

    private fun ackApplied(next: NodeSyncPayload, account: String) {
        val apply = applier ?: return
        val accepted = LinkedHashMap<String, LinkedHashMap<NodeSyncKind, MutableList<Long>>>()
        for (item in next.items) {
            if (item.kind !in ACKABLE_KINDS || item.id <= 0) continue
            val ok = try {
                apply.apply(item)
            } catch (_: Exception) {
                false
            }
            if (!ok) continue
            accepted.getOrPut(item.repoFullName) { LinkedHashMap() }.getOrPut(item.kind) { ArrayList() }.add(item.id)
        }
        for ((repoFullName, kinds) in accepted) {
            for ((kind, ids) in kinds) {
                if (ids.isEmpty()) continue
                deleteInbox(account, repoFullName, kind, ids)
            }
        }
    }

    private fun deleteInbox(account: String, repoFullName: String, kind: NodeSyncKind, ids: List<Long>) {
        val slash = repoFullName.indexOf('/')
        if (slash <= 0) return
        val repoOwner = repoFullName.substring(0, slash)
        val repoName = repoFullName.substring(slash + 1)
        val token = signedToken(repoOwner)
        val uri = httpUri(
            "/api/repo/$repoOwner/$repoName/${kind.wireName}",
            linkedMapOf("owner" to account, "ts" to token.ts, "sig" to token.sig, "ids" to ids.joinToString(",")),
        )
        try {
            val response = transport.send(
                HttpClient.Request("DELETE", uri)
                    .header("accept", "application/json")
                    .timeouts(requestTimeoutMs, requestTimeoutMs),
            )
            if (response.status in 200..299) {
                loop.call { acked += ids.size }
                return
            }
            loop.call { error = "ack ${kind.wireName}: HTTP ${response.status}" }
        } catch (failure: Exception) {
            loop.call { error = "ack ${kind.wireName}: ${dartErrorText(failure)}" }
        }
    }

    private fun openSocket() {
        if (disposed || !isStarted || !foreground) return
        if (socket != null || computeMode() != NodeSyncMode.ACTIVE) return
        val attempt = ++socketGeneration
        setConnection(NodeSyncConnection.CONNECTING)
        loop.io {
            val opened = try {
                connector.connect(eventsUri(), eventsFor(attempt))
            } catch (_: Exception) {
                null
            }
            loop.post { onSocket(attempt, opened) }
        }
    }

    private fun eventsFor(attempt: Int): AccountEventLoop.Events = object : AccountEventLoop.Events {
        override fun onText(text: String) = loop.post { onFrameFor(attempt, text) }

        override fun onBinary(data: ByteArray) = loop.post { onFrameFor(attempt, String(data, Charsets.UTF_8)) }

        override fun onClosed() = loop.post { if (attempt == socketGeneration) onSocketClosed() }
    }

    private fun onFrameFor(attempt: Int, text: String) {
        if (attempt != socketGeneration) return
        if (socket == null) {
            earlyFrames.add(attempt to text)
            return
        }
        onFrame(text)
    }

    private fun onSocket(attempt: Int, opened: AccountEventLoop.Socket?) {
        if (opened == null) {
            if (attempt == socketGeneration) scheduleReconnect()
            return
        }
        if (attempt != socketGeneration || disposed || !isStarted || !foreground || computeMode() != NodeSyncMode.ACTIVE) {
            loop.io { runCatching { opened.close() } }
            setConnection(NodeSyncConnection.OFFLINE)
            return
        }
        socket = opened
        reconnectDelayMs = INITIAL_RECONNECT_DELAY_MS
        setConnection(NodeSyncConnection.LIVE)
        loop.io { syncNow(manual = false) }
        val buffered = earlyFrames.filter { it.first == attempt }.map { it.second }
        earlyFrames.clear()
        for (frame in buffered) onFrame(frame)
    }

    private fun onFrame(raw: String) {
        val frame = try {
            Json.parse(raw) as? Map<*, *>
        } catch (_: Exception) {
            return
        }
        if (frame == null || frame["type"] != "event") return
        loop.io { syncNow(manual = false) }
    }

    private fun onSocketClosed() {
        if (socket == null) return
        closeSocket()
        setConnection(NodeSyncConnection.OFFLINE)
        if (disposed || !isStarted || !foreground) return
        if (computeMode() != NodeSyncMode.ACTIVE) return
        scheduleReconnect()
    }

    private fun closeSocket() {
        socketGeneration++
        earlyFrames.clear()
        val current = socket ?: return
        socket = null
        loop.io { runCatching { current.close() } }
    }

    private fun scheduleReconnect() {
        if (disposed || !isStarted || !foreground) return
        if (computeMode() != NodeSyncMode.ACTIVE) return
        if (reconnectTimer?.isActive == true) return
        val delay = jittered(reconnectDelayMs)
        reconnectDelayMs = minOf(reconnectDelayMs * 2, MAX_RECONNECT_DELAY_MS)
        setConnection(NodeSyncConnection.OFFLINE)
        reconnectTimer = loop.timer(delay) {
            reconnectTimer = null
            if (!disposed && isStarted && foreground) openSocket()
        }
    }

    private fun jittered(baseMs: Long): Long {
        val span = (baseMs * 0.25).roundToLong()
        if (span <= 0) return baseMs
        return baseMs - span + random.nextLong(span * 2 + 1)
    }

    private fun setConnection(next: NodeSyncConnection) {
        if (connectionState == next) return
        connectionState = next
        if (!disposed) notifyListeners()
    }

    private fun notifyListeners() {
        for (listener in listeners) {
            try {
                listener()
            } catch (failure: Exception) {
                loop.failures.add(failure)
            }
        }
    }
}
