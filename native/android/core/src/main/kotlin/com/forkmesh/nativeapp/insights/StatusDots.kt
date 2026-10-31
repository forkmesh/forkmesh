package com.forkmesh.nativeapp.insights

import com.forkmesh.nativeapp.market.AccountCatalogEntry
import com.forkmesh.nativeapp.market.PackageMonitors
import com.forkmesh.nativeapp.market.StatusMonitor
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

data class StatusPageSettings(val onlyProblems: Boolean = false, val refreshMinutes: Int = DEFAULT_REFRESH_MINUTES) {
    val healthyIntervalMs: Long get() = refreshMinutes * 60_000L

    companion object {
        const val ONLY_PROBLEMS = "onlyProblems"
        const val REFRESH_MINUTES = "refreshMinutes"
        const val DEFAULT_REFRESH_MINUTES = 15
        val REFRESH_CHOICES = listOf(2, 5, 15, 30)

        fun of(settings: Map<String, Any?>): StatusPageSettings {
            val minutes = when (val raw = settings[REFRESH_MINUTES]) {
                is String -> raw.trim().toIntOrNull()
                is Number -> raw.toInt()
                else -> null
            }
            return StatusPageSettings(
                onlyProblems = settings[ONLY_PROBLEMS] == true,
                refreshMinutes = minutes?.takeIf { it in REFRESH_CHOICES } ?: DEFAULT_REFRESH_MINUTES,
            )
        }
    }
}

data class StatusDot(val id: String, val label: String, val state: ServiceState, val reason: String = "", val custom: Boolean = false) {
    val troubled: Boolean get() = state == ServiceState.Degraded || state == ServiceState.Down

    val description: String get() = "$label, ${state.label.lowercase()}"
}

data class StatusDotRow(
    val dots: List<StatusDot> = emptyList(),
    val troubled: Boolean = false,
    val refreshMs: Long = StatusDots.TROUBLED_REFRESH_MS,
    val fleet: List<FleetLight> = emptyList(),
) {
    val description: String get() = dots.joinToString("; ") { it.description }

    val services: List<StatusDot> get() = dots.filter { !it.id.startsWith(FleetLight.CORE_PREFIX) && !it.id.startsWith(FleetLight.RUNNER_PREFIX) }

    val servicesDescription: String get() = services.joinToString("; ") { it.description }

    companion object {
        val EMPTY = StatusDotRow()
    }
}

object StatusDots {
    const val STATUS_PAGE = "status-page"
    const val CUSTOM_PREFIX = "custom:"
    const val TROUBLED_REFRESH_MS = 2 * 60_000L
    const val PROBE_TIMEOUT_MS = 10_000

    fun installedMonitors(catalog: Map<String, AccountCatalogEntry>, installed: (String) -> Boolean): List<PackageMonitors>? {
        val checks = catalog.values.filter { !it.monitors.isEmpty }
        if (checks.isEmpty()) return null
        return checks.filter { installed(it.id) }.map { it.monitors }
    }

    fun customDot(monitor: StatusMonitor, ok: Boolean?): StatusDot = StatusDot(
        id = CUSTOM_PREFIX + monitor.id,
        label = monitor.name,
        state = when (ok) {
            true -> ServiceState.Operational
            false -> ServiceState.Down
            null -> ServiceState.Unknown
        },
        reason = if (ok == false) "${monitor.url} did not answer" else "",
        custom = true,
    )

    fun build(
        installed: Boolean,
        report: StatusReport?,
        monitors: List<PackageMonitors>?,
        settings: StatusPageSettings,
        custom: List<StatusDot> = emptyList(),
    ): StatusDotRow {
        if (!installed) return StatusDotRow.EMPTY
        val systems = report?.systems.orEmpty()
            .filter { it.state != ServiceState.Future && it.id.isNotEmpty() }
            .filter { system -> monitors == null || monitors.any { it.covers(system.id) } }
            .map { StatusDot(it.id, it.label, it.state, it.reason) }
        val all = systems + custom
        val troubled = all.any { it.state != ServiceState.Operational }
        val shown = if (settings.onlyProblems) all.filter { it.state != ServiceState.Operational } else all
        val fleet = FleetLight.lights(report, monitors, settings.onlyProblems)
        return StatusDotRow(shown, troubled, if (troubled) TROUBLED_REFRESH_MS else settings.healthyIntervalMs, fleet)
    }

    fun rowFor(
        report: StatusReport?,
        catalog: Map<String, AccountCatalogEntry>,
        installed: (String) -> Boolean,
        settings: Map<String, Any?>,
        custom: List<StatusDot> = emptyList(),
    ): StatusDotRow = build(installed(STATUS_PAGE), report, installedMonitors(catalog, installed), StatusPageSettings.of(settings), custom)
}

fun interface MonitorProbe {
    fun ok(url: String): Boolean
}

object HttpMonitorProbe : MonitorProbe {
    override fun ok(url: String): Boolean {
        var connection: HttpURLConnection? = null
        return try {
            connection = URL(url).openConnection() as? HttpURLConnection ?: return false
            connection.requestMethod = "GET"
            connection.connectTimeout = StatusDots.PROBE_TIMEOUT_MS
            connection.readTimeout = StatusDots.PROBE_TIMEOUT_MS
            connection.instanceFollowRedirects = true
            connection.useCaches = false
            connection.setRequestProperty("Cache-Control", "no-store")
            connection.responseCode in 1..399
        } catch (_: IOException) {
            false
        } catch (_: RuntimeException) {
            false
        } finally {
            connection?.disconnect()
        }
    }
}
