package com.forkmesh.nativeapp.tools

enum class LogCategory(val label: String, val description: String) {
    Stall("STALL", "UI stalls: frames, event-loop blocks, named slow sections"),
    Error("ERROR", "Failures: HTTP errors, uncaught exceptions, failed calls"),
    Net("NET", "HTTP requests to ForkMesh services"),
    Relay("RELAY", "Relay socket: connects, peers, encryption notices"),
    Sync("SYNC", "Node sync drains and applies"),
    Auth("AUTH", "Session, identity and device-unlock events"),
    Push("PUSH", "Push notification registration"),
    Perf("PERF", "Slow awaited work (the UI stayed responsive)"),
    Info("INFO", "Everything else the app printed"),
    ;

    companion object {
        fun classify(line: String): LogCategory {
            val lower = line.lowercase()
            if (lower.startsWith("[stall]")) return Stall
            if (lower.startsWith("[perf]")) return Perf
            if (lower.contains("exception") || lower.contains("failed") || lower.contains("error")) return Error
            if (lower.startsWith("[push]") || lower.contains("apns")) return Push
            if (lower.contains("node sync")) return Sync
            if (lower.contains("auth") ||
                lower.contains("session") ||
                lower.contains("identity") ||
                lower.contains("userpresence")
            ) {
                return Auth
            }
            return Info
        }
    }
}

class AppLogEntry(val timestampMs: Long, val category: LogCategory, val message: String) {
    override fun toString(): String = "${dartIsoLocal(timestampMs)} ${category.label} $message"
}

object DebugPrint {
    @Volatile
    var sink: (String?) -> Unit = { message -> if (message != null) println(message) }
}

fun debugPrint(message: String?) = DebugPrint.sink(message)

object LogcatLines {
    private const val PRIORITIES = "VDIWEFA"

    fun message(line: String, ownTag: String): String? {
        if (line.length < 3 || line[1] != '/' || line[0] !in PRIORITIES) return null
        val separator = line.indexOf(": ", 2)
        if (separator < 0) return null
        val tag = line.substring(2, separator).trim()
        val message = line.substring(separator + 2)
        if (tag.isEmpty() || tag == ownTag || message.isBlank()) return null
        return "$tag: $message"
    }
}

class AppLog(val capacity: Int = 2000, private val now: () -> Long = { System.currentTimeMillis() }) {
    private val lock = Any()
    private val ring = ArrayList<AppLogEntry>()
    private var head = 0
    private val totals = IntArray(LogCategory.entries.size)
    private var lines = 0

    val totalLines: Int get() = synchronized(lock) { lines }

    fun totalFor(category: LogCategory): Int = synchronized(lock) { totals[category.ordinal] }

    fun add(category: LogCategory, message: String) {
        synchronized(lock) {
            val entry = AppLogEntry(now(), category, message)
            if (ring.size < capacity) {
                ring.add(entry)
            } else {
                ring[head] = entry
                head = (head + 1) % capacity
            }
            totals[category.ordinal]++
            lines++
        }
    }

    fun addLine(line: String) {
        val trimmed = line.trimEnd()
        if (trimmed.isEmpty()) return
        add(LogCategory.classify(trimmed), trimmed)
    }

    fun addRelayLine(line: String) {
        add(if (line.lowercase().contains("failed")) LogCategory.Error else LogCategory.Relay, line)
    }

    fun captureProcessLogs(): () -> Unit {
        val previousPrint = DebugPrint.sink
        DebugPrint.sink = { message ->
            if (message != null) addLine(message)
            previousPrint(message)
        }
        val previousHandler = Thread.getDefaultUncaughtExceptionHandler()
        val handler = Thread.UncaughtExceptionHandler { thread, error ->
            add(LogCategory.Error, "Uncaught: $error")
            previousHandler?.uncaughtException(thread, error)
        }
        Thread.setDefaultUncaughtExceptionHandler(handler)
        return {
            DebugPrint.sink = previousPrint
            if (Thread.getDefaultUncaughtExceptionHandler() === handler) {
                Thread.setDefaultUncaughtExceptionHandler(previousHandler)
            }
        }
    }

    fun entries(categories: Set<LogCategory>? = null, sinceMs: Long? = null): List<AppLogEntry> {
        synchronized(lock) {
            val out = ArrayList<AppLogEntry>()
            val n = ring.size
            for (i in 0 until n) {
                val entry = ring[(head + n - 1 - i) % n]
                if (sinceMs != null && entry.timestampMs < sinceMs) break
                if (categories != null && entry.category !in categories) continue
                out.add(entry)
            }
            return out
        }
    }

    fun countsSince(sinceMs: Long): IntArray {
        val counts = IntArray(LogCategory.entries.size)
        for (entry in entries(sinceMs = sinceMs)) counts[entry.category.ordinal]++
        return counts
    }
}
