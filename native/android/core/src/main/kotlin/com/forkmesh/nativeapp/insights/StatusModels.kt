package com.forkmesh.nativeapp.insights

import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str

enum class ServiceState(val wire: String, val label: String, val tone: StatsTone, val severity: Int) {
    Operational("operational", "Operational", StatsTone.Good, 0),
    Degraded("degraded", "Degraded", StatsTone.Warning, 2),
    Down("down", "Down", StatsTone.Critical, 3),
    Unknown("unknown", "No data", StatsTone.None, 1),
    Future("future", "Pending", StatsTone.None, 0),
    ;

    companion object {
        fun of(raw: String): ServiceState = entries.firstOrNull { it.wire == raw } ?: Unknown
    }
}

data class StatusMinute(val ts: Long, val state: ServiceState)

internal fun Map<String, Any?>.optDouble(key: String): Double? = (this[key] as? Number)?.toDouble()

internal fun Map<String, Any?>.optInt(key: String): Int? = (this[key] as? Number)?.toInt()

internal fun Map<String, Any?>.optBoolean(key: String): Boolean? = this[key] as? Boolean

internal fun Map<String, Any?>.optText(key: String): String = when (val value = this[key]) {
    null -> ""
    is String -> value
    else -> value.toString()
}

data class StatusSystem(
    val id: String,
    val label: String,
    val state: ServiceState,
    val detail: String = "",
    val uptime24h: Double? = null,
    val uptime30d: Double? = null,
    val checks24h: Int = 0,
    val failures24h: Int = 0,
    val reason: String = "",
    val reasonTs: Long = 0,
    val observedAt: Long = 0,
    val minutes: List<StatusMinute> = emptyList(),
) {
    val isMirror: Boolean get() = id.startsWith("mirror:")

    companion object {
        fun fromJson(json: Map<String, Any?>) = StatusSystem(
            id = json.str("id"),
            label = json.str("label").ifEmpty { json.str("id") },
            state = ServiceState.of(json.str("status")),
            detail = json.str("checkDescription"),
            uptime24h = json.optDouble("uptime24hPct"),
            uptime30d = json.optDouble("uptimePct"),
            checks24h = json.int("checks24h"),
            failures24h = json.int("failures24h"),
            reason = json.optText("reason"),
            reasonTs = json.long("reasonTs"),
            observedAt = json.long("observedAt"),
            minutes = json.maps("minutes").takeLast(60).map { StatusMinute(it.long("minuteTs"), ServiceState.of(it.str("status"))) },
        )
    }
}

data class StatusCurrent(
    val catalogRepos: Int? = null,
    val onlineNodes: Int? = null,
    val errors24h: Int? = null,
    val mainnodeOnline: Boolean? = null,
    val mainnodeLastSeen: Long = 0,
    val monitoringState: String = "",
    val lastSampleTs: Long = 0,
) {
    companion object {
        fun fromJson(json: Map<String, Any?>) = StatusCurrent(
            catalogRepos = json.optInt("catalogRepos"),
            onlineNodes = json.optInt("onlineNodes"),
            errors24h = json.optInt("errors24h"),
            mainnodeOnline = json.optBoolean("mainnodeOnline"),
            mainnodeLastSeen = json.long("mainnodeLastSeenTs"),
            monitoringState = json.optText("monitoringState"),
            lastSampleTs = json.long("lastCronSampleTs"),
        )
    }
}

data class StatusReport(val now: Long, val systems: List<StatusSystem>, val current: StatusCurrent) {
    val overall: ServiceState
        get() {
            val relevant = systems.filter { !it.isMirror && it.state != ServiceState.Future }
            val worst = relevant.maxByOrNull { it.state.severity } ?: return ServiceState.Unknown
            if (worst.state == ServiceState.Unknown && relevant.any { it.state == ServiceState.Operational }) return ServiceState.Operational
            return worst.state
        }

    val down: List<StatusSystem> get() = systems.filter { it.state == ServiceState.Down && !it.isMirror }

    val coreSystems: List<StatusSystem> get() = systems.filter { !it.isMirror }

    val mirrorSystems: List<StatusSystem> get() = systems.filter { it.isMirror }

    val websiteUptime24h: Double? get() = (systems.firstOrNull { it.id == "website" } ?: coreSystems.firstOrNull())?.uptime24h

    val headline: String
        get() {
            if (current.monitoringState == "stale") return "Monitoring is behind"
            if (current.monitoringState == "empty") return "No monitoring samples yet"
            return when (overall) {
                ServiceState.Operational -> "All systems operational"
                ServiceState.Degraded -> "Some systems are degraded"
                ServiceState.Down -> "${down.size} system${if (down.size == 1) " is" else "s are"} down"
                ServiceState.Unknown, ServiceState.Future -> "Status unknown"
            }
        }

    companion object {
        fun fromJson(json: Map<String, Any?>) = StatusReport(
            now = json.long("now"),
            systems = json.maps("systems").map(StatusSystem::fromJson),
            current = StatusCurrent.fromJson(json.obj("current")),
        )
    }
}

enum class ConnectionLight {
    Live,
    Degraded,
    Offline,
    ;

    companion object {
        fun of(report: StatusReport?, reachable: Boolean): ConnectionLight {
            if (!reachable) return Offline
            if (report == null) return Degraded
            return if (report.overall == ServiceState.Operational) Live else Degraded
        }
    }
}
