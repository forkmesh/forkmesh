package com.forkmesh.nativeapp.insights

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.str
import java.io.File

enum class ExitKind(val wire: String, val title: String, val icon: String) {
    Crash("crash", "Crash", "warning_amber"),
    NativeCrash("nativeCrash", "Native crash", "memory"),
    Anr("anr", "Not responding", "hourglass_empty"),
    LowMemory("lowMemory", "Stopped for memory", "sd_storage_outlined"),
    ExcessiveResource("excessiveResource", "Too much CPU or wakeups", "bolt"),
    InitFailure("initFailure", "Failed to start", "timer_outlined"),
    ;

    companion object {
        const val REASON_LOW_MEMORY = 3
        const val REASON_CRASH = 4
        const val REASON_CRASH_NATIVE = 5
        const val REASON_ANR = 6
        const val REASON_INITIALIZATION_FAILURE = 7
        const val REASON_EXCESSIVE_RESOURCE_USAGE = 9

        fun fromReason(reason: Int): ExitKind? = when (reason) {
            REASON_CRASH -> Crash
            REASON_CRASH_NATIVE -> NativeCrash
            REASON_ANR -> Anr
            REASON_LOW_MEMORY -> LowMemory
            REASON_EXCESSIVE_RESOURCE_USAGE -> ExcessiveResource
            REASON_INITIALIZATION_FAILURE -> InitFailure
            else -> null
        }

        fun of(wire: String): ExitKind? = entries.firstOrNull { it.wire == wire }
    }
}

data class ExitReport(
    val id: String,
    val kind: ExitKind,
    val receivedAt: Long,
    val appVersion: String = "",
    val osVersion: String = "",
    val summary: String,
    val detail: String = "",
) {
    val stallLogLine: String
        get() {
            var line = "[exitinfo] ${kind.wire} $summary"
            if (appVersion.isNotEmpty()) line += " app=$appVersion"
            if (osVersion.isNotEmpty()) line += " os=$osVersion"
            return line
        }

    fun agentPrompt(): String {
        val lines = mutableListOf(
            "The ForkMesh Android app reported a ${kind.title.lowercase()} through ApplicationExitInfo. Find the cause in native/android and fix it.",
            "",
            "Summary: $summary",
        )
        if (appVersion.isNotEmpty()) lines.add("App version: $appVersion")
        if (osVersion.isNotEmpty()) lines.add("OS: $osVersion")
        if (detail.isNotEmpty()) {
            lines.add("")
            lines.add("Trace:")
            lines.add(detail.take(8 * 1024))
        }
        return lines.joinToString("\n")
    }

    fun toJson(): Map<String, Any?> = linkedMapOf(
        "id" to id,
        "kind" to kind.wire,
        "receivedAt" to receivedAt,
        "appVersion" to appVersion,
        "osVersion" to osVersion,
        "summary" to summary,
        "detail" to detail,
    )

    companion object {
        const val SUMMARY_LIMIT = 300
        const val DETAIL_LIMIT = 16 * 1024

        fun create(
            id: String,
            kind: ExitKind,
            receivedAt: Long,
            appVersion: String = "",
            osVersion: String = "",
            summary: String,
            detail: String = "",
        ) = ExitReport(
            id = id,
            kind = kind,
            receivedAt = receivedAt,
            appVersion = appVersion.take(40),
            osVersion = osVersion.take(60),
            summary = summary.take(SUMMARY_LIMIT),
            detail = detail.take(DETAIL_LIMIT),
        )

        fun fromExit(
            reason: Int,
            timestamp: Long,
            pid: Int,
            description: String,
            trace: String,
            appVersion: String,
            osVersion: String,
        ): ExitReport? {
            val kind = ExitKind.fromReason(reason) ?: return null
            val firstLine = trace.lineSequence().map { it.trim() }.firstOrNull { it.isNotEmpty() }.orEmpty()
            val summary = description.trim().ifEmpty { firstLine }.ifEmpty { kind.title }
            return create("$timestamp-$pid", kind, timestamp, appVersion, osVersion, summary, trace)
        }

        fun fromJson(json: Map<String, Any?>): ExitReport? {
            val kind = ExitKind.of(json.str("kind")) ?: return null
            val id = json.str("id")
            if (id.isEmpty()) return null
            return create(id, kind, json.long("receivedAt"), json.str("appVersion"), json.str("osVersion"), json.str("summary"), json.str("detail"))
        }
    }
}

class ExitReportStore(private val directory: () -> File) {
    private val lock = Any()

    private fun file(): File {
        val folder = directory()
        folder.mkdirs()
        return File(folder, FILE_NAME)
    }

    fun load(): List<ExitReport> = synchronized(lock) { read() }

    private fun read(): List<ExitReport> = runCatching {
        val target = file()
        if (!target.exists()) return emptyList()
        (Json.parse(target.readText()) as? List<*>).orEmpty().mapNotNull { ExitReport.fromJson(it.asJsonMap()) }
    }.getOrDefault(emptyList())

    private fun write(reports: List<ExitReport>) {
        runCatching {
            val target = file()
            val temp = File(target.parentFile, "$FILE_NAME.tmp")
            temp.writeText(Json.stringify(reports.map { it.toJson() }))
            if (!temp.renameTo(target)) {
                target.writeText(temp.readText())
                temp.delete()
            }
        }
    }

    fun append(incoming: List<ExitReport>): List<ExitReport> = synchronized(lock) {
        val reports = read().toMutableList()
        val known = reports.map { it.id }.toHashSet()
        for (report in incoming) if (known.add(report.id)) reports.add(report)
        val kept = reports.sortedByDescending { it.receivedAt }.take(MAX_REPORTS)
        write(kept)
        kept
    }

    fun newestTimestamp(): Long = load().maxOfOrNull { it.receivedAt } ?: 0

    fun remove(id: String) = synchronized(lock) { write(read().filter { it.id != id }) }

    fun clear() = synchronized(lock) { write(emptyList()) }

    companion object {
        const val FILE_NAME = "exit-reports.json"
        const val MAX_REPORTS = 20

        fun counts(reports: List<ExitReport>): List<Pair<ExitKind, Int>> =
            ExitKind.entries.mapNotNull { kind -> reports.count { it.kind == kind }.takeIf { it > 0 }?.let { kind to it } }
    }
}
