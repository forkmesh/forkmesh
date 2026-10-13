package com.forkmesh.nativeapp.tools

import kotlin.math.max

enum class PerformanceEventKind(val wireName: String) {
    Operation("operation"),

    Frame("frame"),

    EventLoop("eventLoop"),

    Sync("sync"),
}

class PerformanceEvent(
    val kind: PerformanceEventKind,
    val name: String,
    val durationMicros: Long,
    val timestampMs: Long,
    val details: Map<String, Any?> = emptyMap(),
    val stackTrace: String = "",
) {
    val durationMs: Long get() = durationMicros / 1000

    val isStall: Boolean get() = kind != PerformanceEventKind.Operation
}

fun interface HeartbeatScheduler {
    fun schedule(delayMs: Long, action: () -> Unit): () -> Unit
}

class PerformanceMonitorService(
    val enabled: Boolean = true,
    val thresholdMs: Long = 500,
    val stallThresholdMs: Long = 200,
    val heartbeatIntervalMs: Long = 100,
    val maxEvents: Int = 100,
    private val logSink: ((String) -> Unit)? = null,
    val stallLog: StallLogFile? = null,
    private val now: () -> Long = { System.currentTimeMillis() },
    private val elapsedMicrosForTest: (() -> Long)? = null,
    private val clockMicros: () -> Long = { System.nanoTime() / 1000 },
    @Volatile var scheduler: HeartbeatScheduler? = null,
) : ChangeNotifier() {
    private val lock = Any()
    private val recorded = ArrayList<PerformanceEvent>()

    private var heartbeatCancel: (() -> Unit)? = null
    private var heartbeatToken = 0L
    private var lastBeatUs = 0L
    private var lastAttributedStallUs = -1L

    private val inFlight = LinkedHashMap<Long, String>()
    private var nextOperationId = 0L
    private val recentlyFinished = ArrayDeque<Pair<String, Long>>()

    private var stalls = 0

    @Volatile
    var onHeartbeatLag: ((Long) -> Unit)? = null

    val stallCount: Int get() = synchronized(lock) { stalls }

    val events: List<PerformanceEvent> get() = synchronized(lock) { recorded.toList() }

    val hasEvents: Boolean get() = synchronized(lock) { recorded.isNotEmpty() }

    val latestEvent: PerformanceEvent? get() = synchronized(lock) { recorded.firstOrNull() }

    val stallLogPath: String? get() = stallLog?.path

    val heartbeatRunning: Boolean get() = synchronized(lock) { heartbeatCancel != null }

    fun <T> track(name: String, details: Map<String, Any?> = emptyMap(), action: () -> T): T {
        if (!enabled) return action()
        val callStack = Throwable()
        val started = System.nanoTime()
        val id = beginOperation(name)
        var error: Throwable? = null
        try {
            return action()
        } catch (thrown: Throwable) {
            error = thrown
            throw thrown
        } finally {
            finishOperation(id, name, (System.nanoTime() - started) / 1000, details, error, error ?: callStack)
        }
    }

    fun beginOperation(name: String): Long = synchronized(lock) {
        val id = nextOperationId++
        inFlight[id] = name
        id
    }

    fun finishOperation(
        id: Long?,
        name: String,
        elapsedMicros: Long,
        details: Map<String, Any?> = emptyMap(),
        error: Any? = null,
        stack: Throwable? = null,
    ) {
        if (!enabled) return
        synchronized(lock) {
            if (id != null) inFlight.remove(id)
            recentlyFinished.addLast(name to clockMicros())
            if (recentlyFinished.size > RECENT_LIMIT) recentlyFinished.removeFirst()
        }
        val elapsed = elapsedMicrosForTest?.invoke() ?: elapsedMicros
        if (elapsed >= thresholdMs * 1000) {
            val merged = LinkedHashMap(details)
            if (error != null) merged["error"] = "$error"
            recordSlowEvent(PerformanceEventKind.Operation, name, elapsed, merged, formatStack(stack))
        }
    }

    fun recordExchange(method: String, url: String, status: Int, elapsedMicros: Long, error: Throwable? = null) {
        if (!enabled) return
        var host = ""
        var path = ""
        var query = ""
        try {
            val uri = java.net.URI(url)
            host = uri.host ?: ""
            path = uri.rawPath ?: ""
            query = uri.rawQuery ?: ""
        } catch (_: Exception) {
            path = url.substringAfter("://").substringAfter('/', "").let { "/" + it.substringBefore('?') }
        }
        val details = LinkedHashMap<String, Any?>()
        details["host"] = host
        details["path"] = path
        if (method == "GET" && query.isNotEmpty()) details["query"] = query
        val failure: Any? = error ?: if (status >= 400) "HTTP $status" else null
        finishOperation(null, "api.$method $path", elapsedMicros, details, failure, error)
    }

    fun <T> timeSync(name: String, details: Map<String, Any?> = emptyMap(), action: () -> T): T {
        if (!enabled) return action()
        val startUs = clockMicros()
        try {
            return action()
        } finally {
            val endUs = clockMicros()
            val elapsed = endUs - startUs
            if (elapsed >= stallThresholdMs * 1000) {
                synchronized(lock) { lastAttributedStallUs = endUs }
                recordSlowEvent(PerformanceEventKind.Sync, name, elapsed, details, formatStack(Throwable()))
            }
        }
    }

    fun startHeartbeat() {
        if (!enabled) return
        val scheduler = scheduler ?: return
        synchronized(lock) {
            if (heartbeatCancel != null) return
            lastBeatUs = clockMicros()
            val token = ++heartbeatToken
            heartbeatCancel = scheduler.schedule(heartbeatIntervalMs) { beat(token) }
        }
    }

    fun stopHeartbeat() {
        val cancel = synchronized(lock) {
            heartbeatToken++
            heartbeatCancel.also { heartbeatCancel = null }
        }
        cancel?.invoke()
    }

    private class Beat(val lagUs: Long, val attributed: Boolean, val details: Map<String, Any?>)

    private fun beat(token: Long) {
        val scheduler = scheduler
        val nowUs = clockMicros()
        val beat = synchronized(lock) {
            if (token != heartbeatToken || heartbeatCancel == null || scheduler == null) return
            val windowStartUs = lastBeatUs
            val lag = nowUs - windowStartUs - heartbeatIntervalMs * 1000
            lastBeatUs = nowUs
            heartbeatCancel = scheduler.schedule(heartbeatIntervalMs) { beat(token) }
            val details = LinkedHashMap<String, Any?>()
            if (inFlight.isNotEmpty()) details["inFlight"] = inFlight.values.toSet().joinToString(", ")
            val recent = recentlyFinished.filter { it.second >= windowStartUs }.map { it.first }
            if (recent.isNotEmpty()) details["finishedDuring"] = recent.toSet().joinToString(", ")
            Beat(lag, lastAttributedStallUs >= windowStartUs, details)
        }
        onHeartbeatLag?.invoke(max(0L, beat.lagUs))
        if (beat.lagUs < stallThresholdMs * 1000 || beat.attributed) return
        recordSlowEvent(PerformanceEventKind.EventLoop, "ui-isolate blocked", beat.lagUs, beat.details)
    }

    fun recordFrameDelay(buildMicros: Long, rasterMicros: Long, vsyncMicros: Long = 0, totalMicros: Long? = null) {
        if (!enabled) return
        val total = totalMicros ?: (buildMicros + rasterMicros)
        if (total < stallThresholdMs * 1000) return
        val details = LinkedHashMap<String, Any?>()
        details["buildMs"] = buildMicros / 1000
        details["rasterMs"] = rasterMicros / 1000
        if (vsyncMicros / 1000 > 0) details["vsyncMs"] = vsyncMicros / 1000
        synchronized(lock) {
            if (inFlight.isNotEmpty()) details["inFlight"] = inFlight.values.toSet().joinToString(", ")
        }
        recordSlowEvent(PerformanceEventKind.Frame, "frame", total, details)
    }

    fun recordSlowEvent(
        kind: PerformanceEventKind,
        name: String,
        durationMicros: Long,
        details: Map<String, Any?> = emptyMap(),
        stackTrace: String = "",
    ) {
        val floorMs = if (kind == PerformanceEventKind.Operation) thresholdMs else stallThresholdMs
        if (!enabled || durationMicros < floorMs * 1000) return
        val event = PerformanceEvent(kind, name, durationMicros, now(), java.util.Collections.unmodifiableMap(LinkedHashMap(details)), stackTrace)
        synchronized(lock) {
            if (event.isStall) stalls++
            recorded.add(0, event)
            while (recorded.size > maxEvents) recorded.removeAt(recorded.lastIndex)
        }
        log(event)
        notifyListeners()
    }

    fun clear() {
        val had = synchronized(lock) {
            val any = recorded.isNotEmpty()
            recorded.clear()
            any
        }
        if (had) notifyListeners()
    }

    private fun log(event: PerformanceEvent) {
        val details = event.details.entries.joinToString(" ") { "${it.key}=${quote(it.value)}" }
        val detailSuffix = if (details.isEmpty()) "" else " $details"
        val line = "${if (event.isStall) "[stall]" else "[perf]"} ${event.kind.wireName} ${event.name} ${event.durationMs}ms$detailSuffix"
        val sink = logSink
        if (sink != null) sink(line) else debugPrint(line)
        val file = stallLog ?: return
        val stack = if (event.stackTrace.isEmpty()) "" else "\n" + event.stackTrace.split('\n').joinToString("\n") { "    $it" }
        file.append("${dartIsoLocal(event.timestampMs)} $line$stack")
    }

    fun dispose() {
        stopHeartbeat()
    }

    companion object {
        private const val RECENT_LIMIT = 16

        fun formatStack(throwable: Throwable?): String =
            throwable?.stackTrace?.take(8)?.joinToString("\n") { it.toString() } ?: ""

        private fun quote(value: Any?): String {
            val text = "$value"
            return if (text.contains(' ')) "\"$text\"" else text
        }
    }
}
