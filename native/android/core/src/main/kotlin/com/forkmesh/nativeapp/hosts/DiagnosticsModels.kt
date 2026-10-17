package com.forkmesh.nativeapp.hosts

import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.double
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.list
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str

enum class DesktopHealthLevel(val wire: String, val label: String) {
    Ok("ok", "Healthy"),
    Busy("busy", "Busy"),
    Strained("strained", "Under pressure"),
    Unknown("unknown", "Unknown"),
    ;

    companion object {
        fun of(wire: String): DesktopHealthLevel = entries.firstOrNull { it.wire == wire } ?: Unknown
    }
}

data class DesktopHealth(
    val desktop: String = "",
    val level: DesktopHealthLevel = DesktopHealthLevel.Unknown,
    val cpuPercent: Double = -1.0,
    val cpuCount: Int = 0,
    val load1: Double = -1.0,
    val memoryTotal: Double = -1.0,
    val memoryAvailable: Double = -1.0,
    val swapTotal: Double = -1.0,
    val swapFree: Double = -1.0,
    val appResident: Double = -1.0,
    val pressureValid: Boolean = false,
    val memoryFull: Double = 0.0,
    val memorySome: Double = 0.0,
    val ioFull: Double = 0.0,
    val diskTotal: Double = -1.0,
    val diskFree: Double = -1.0,
    val agentsRunning: Int = 0,
    val agentsQueued: Int = 0,
    val agentsTotal: Int = 0,
    val stalls: Int = 0,
    val openFiles: Int = -1,
    val openFileLimit: Int = -1,
    val threads: Int = -1,
    val onBattery: Boolean = false,
    val batteryPercent: Int = -1,
    val sampledAtMs: Long = 0,
    val pressureLine: String = "",
) {
    val memoryUsedShare: Double
        get() {
            if (memoryTotal <= 0 || memoryAvailable < 0) return 0.0
            return ((memoryTotal - memoryAvailable) / memoryTotal).coerceIn(0.0, 1.0)
        }

    val memoryUsedShareOrNull: Double? get() = if (memoryTotal > 0) maxOf(0.0, memoryTotal - maxOf(0.0, memoryAvailable)) / memoryTotal else null

    val diskUsedShare: Double
        get() {
            if (diskTotal <= 0 || diskFree < 0) return 0.0
            return ((diskTotal - diskFree) / diskTotal).coerceIn(0.0, 1.0)
        }

    val memoryLine: String
        get() = if (memoryTotal <= 0) "—" else "${RemoteFormat.bytes(memoryTotal - maxOf(0.0, memoryAvailable))} of ${RemoteFormat.bytes(memoryTotal)} used"

    val diskLine: String
        get() = if (diskTotal <= 0) "—" else "${RemoteFormat.bytes(maxOf(0.0, diskFree))} free of ${RemoteFormat.bytes(diskTotal)}"

    val batteryShare: Double
        get() = if (batteryPercent < 0) 0.0 else (batteryPercent / 100.0).coerceIn(0.0, 1.0)

    val batteryLine: String
        get() = if (batteryPercent >= 0) "${minOf(100, batteryPercent)}% left" else "On battery"

    val agentsLine: String
        get() {
            val parts = mutableListOf("$agentsRunning running")
            if (agentsQueued > 0) parts.add("$agentsQueued queued")
            return parts.joinToString(" · ")
        }

    val loadLine: String
        get() {
            if (load1 < 0) return "—"
            return if (cpuCount > 0) "${RemoteFormat.fixed(load1, 1)} on $cpuCount CPUs" else RemoteFormat.fixed(load1, 1)
        }

    companion object {
        fun fromJson(json: Map<String, Any?>): DesktopHealth {
            val memory = json.obj("memory")
            val pressure = json.obj("pressure")
            val disk = json.obj("disk")
            val agents = json.obj("agents")
            val process = json.obj("process")
            val power = json.obj("power")
            val stalls = when (val raw = json["stalls"]) {
                is Number -> raw.toInt()
                is List<*> -> raw.size
                else -> json.list("stalls").size
            }
            return DesktopHealth(
                desktop = json.str("desktop"),
                level = DesktopHealthLevel.of(json.str("level")),
                cpuPercent = json.double("cpuPercent", -1.0),
                cpuCount = json.int("cpuCount"),
                load1 = json.double("load1", -1.0),
                memoryTotal = memory.double("total", -1.0),
                memoryAvailable = memory.double("available", -1.0),
                swapTotal = memory.double("swapTotal", -1.0),
                swapFree = memory.double("swapFree", -1.0),
                appResident = memory.double("appResident", -1.0),
                pressureValid = pressure.bool("valid"),
                memoryFull = pressure.double("memoryFull"),
                memorySome = pressure.double("memorySome"),
                ioFull = pressure.double("ioFull"),
                diskTotal = disk.double("total", -1.0),
                diskFree = disk.double("free", -1.0),
                agentsRunning = agents.int("running"),
                agentsQueued = agents.int("queued"),
                agentsTotal = agents.int("total"),
                stalls = stalls,
                openFiles = process.int("openFiles", -1),
                openFileLimit = process.int("openFileLimit", -1),
                threads = process.int("threads", -1),
                onBattery = power.bool("onBattery"),
                batteryPercent = power.int("percent", -1),
                sampledAtMs = json.long("sampledAtMs"),
                pressureLine = json.str("pressureLine"),
            )
        }
    }
}

enum class LogSeverity(val wire: String, val title: String) {
    All("all", "All"),
    Warning("warning", "Warnings"),
    Error("error", "Errors"),
    Info("info", "Info"),
    ;

    companion object {
        fun of(wire: String): LogSeverity = entries.firstOrNull { it.wire == wire } ?: Info
    }
}

data class DesktopLogLine(val offset: Long, val timeMs: Long, val severity: LogSeverity, val text: String) {
    val body: String get() = if (timeMs <= 0 || text.length <= 21) text else text.drop(19).trim()

    companion object {
        fun fromJson(json: Map<String, Any?>) =
            DesktopLogLine(json.long("offset"), json.long("t"), LogSeverity.of(json.str("severity")), json.str("text"))
    }
}

data class DesktopLogPage(val lines: List<DesktopLogLine>, val start: Long, val end: Long, val more: Boolean) {
    companion object {
        fun fromJson(json: Map<String, Any?>) =
            DesktopLogPage(json.maps("lines").map(DesktopLogLine::fromJson), json.long("start"), json.long("end"), json.bool("more"))
    }
}

data class StallRecord(
    val index: Int,
    val whenText: String,
    val summary: String,
    val lines: Int,
    val bytes: Int,
    val text: String,
    val truncated: Boolean,
) {
    companion object {
        fun fromJson(json: Map<String, Any?>) = StallRecord(
            index = json.int("index"),
            whenText = json.str("when"),
            summary = json.str("summary"),
            lines = json.int("lines"),
            bytes = json.int("bytes"),
            text = json.str("text"),
            truncated = json.bool("truncated"),
        )
    }
}

data class StallList(val stalls: List<StallRecord>, val canSend: Boolean) {
    companion object {
        fun fromJson(json: Map<String, Any?>) = StallList(json.maps("stalls").map(StallRecord::fromJson), json.bool("canSend"))
    }
}

data class ProcessAgent(val session: Int, val label: String, val stoppable: Boolean)

data class DesktopProcess(
    val pid: Int,
    val name: String,
    val user: String,
    val rssBytes: Double,
    val percent: Double,
    val command: String,
    val agent: ProcessAgent?,
) {
    companion object {
        fun fromJson(json: Map<String, Any?>): DesktopProcess {
            val agent = json.obj("agent")
            return DesktopProcess(
                pid = json.int("pid"),
                name = json.str("name"),
                user = json.str("user"),
                rssBytes = json.double("rssBytes"),
                percent = json.double("percent"),
                command = json.str("command"),
                agent = if (agent.int("session") > 0) ProcessAgent(agent.int("session"), agent.str("label"), agent.bool("stoppable")) else null,
            )
        }
    }
}

data class ProcessList(val processes: List<DesktopProcess>, val memoryTotal: Double) {
    val agentProcesses: List<DesktopProcess> get() = processes.filter { it.agent != null }

    fun share(process: DesktopProcess): Double =
        if (memoryTotal > 0) minOf(1.0, process.rssBytes / memoryTotal) else process.percent / 100

    companion object {
        fun fromJson(json: Map<String, Any?>) =
            ProcessList(json.maps("processes").map(DesktopProcess::fromJson), json.double("memoryTotal"))
    }
}

sealed class DiagnosticsAction {
    data class StopAgent(val session: Int) : DiagnosticsAction()
    data object SendStalls : DiagnosticsAction()
    data class SendStall(val index: Int) : DiagnosticsAction()

    val params: Map<String, Any?>
        get() = when (this) {
            is StopAgent -> linkedMapOf("action" to "stopAgent", "session" to session.toLong())
            SendStalls -> linkedMapOf("action" to "sendStalls")
            is SendStall -> linkedMapOf("action" to "sendStall", "index" to index.toLong())
        }
}

enum class DesktopBackupResult(val wire: String, val label: String) {
    Ok("ok", "Backed up"),
    Running("running", "Backing up now"),
    Failed("failed", "Last backup failed"),
    None("none", "No backup yet"),
    Off("off", "Hourly backups are off"),
    Unknown("unknown", "Unknown"),
    ;

    companion object {
        fun of(wire: String): DesktopBackupResult = entries.firstOrNull { it.wire == wire } ?: Unknown
    }
}

data class DesktopBackupStatus(
    val enabled: Boolean,
    val keep: Int,
    val running: Boolean,
    val result: DesktopBackupResult,
    val location: String,
    val count: Int,
    val totalBytes: Double,
    val lastBackupAtMs: Long,
    val lastBackupBytes: Double,
    val nextDueAtMs: Long,
    val error: String,
    val failedAtMs: Long,
) {
    val hasBackup: Boolean get() = lastBackupAtMs > 0

    val sizeLine: String
        get() {
            if (count <= 0) return "—"
            val copies = if (count == 1) "1 copy" else "$count copies"
            return "${RemoteFormat.bytes(lastBackupBytes)} · $copies, ${RemoteFormat.bytes(totalBytes)} in all"
        }

    val errorText: String
        get() = when (error) {
            "" -> ""
            "tar_missing" -> "The desktop has no tar tool to write backups with."
            "nothing_to_back_up" -> "There was nothing to back up yet."
            "rename_failed" -> "The backup was written but could not be saved in place."
            "low_disk" -> "The backup was skipped to keep free space on the desktop's disk."
            else -> "The backup could not be written. Open Settings, Data on the desktop for details."
        }

    companion object {
        fun fromJson(json: Map<String, Any?>) = DesktopBackupStatus(
            enabled = json.bool("enabled"),
            keep = json.int("keep"),
            running = json.bool("running"),
            result = DesktopBackupResult.of(json.str("result")),
            location = json.str("location"),
            count = json.int("count"),
            totalBytes = json.double("totalBytes"),
            lastBackupAtMs = json.long("lastBackupAtMs"),
            lastBackupBytes = json.double("lastBackupBytes"),
            nextDueAtMs = json.long("nextDueAtMs"),
            error = json.str("error"),
            failedAtMs = json.long("failedAtMs"),
        )
    }
}
