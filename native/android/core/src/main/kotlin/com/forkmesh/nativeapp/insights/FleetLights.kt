package com.forkmesh.nativeapp.insights

import com.forkmesh.nativeapp.market.PackageMonitors
import kotlin.math.roundToInt

data class NodeResources(val cpu: Double?, val memory: Double?, val disk: Double?) {
    val values: List<Double?> get() = listOf(cpu, memory, disk)

    val label: String get() = "CPU ${percent(cpu)}, memory ${percent(memory)}, disk ${percent(disk)}"

    companion object {
        const val WARNING = 75.0
        const val BAD = 90.0

        private fun percent(value: Double?): String = value?.let { "${it.roundToInt()}%" } ?: "unknown"

        private fun reading(raw: Any?): Double? {
            val value = (raw as? Number)?.toDouble() ?: return null
            if (value.isNaN() || value.isInfinite() || value < 0) return null
            return minOf(value, 100.0)
        }

        fun of(cpu: Double?, memory: Double?, disk: Double?): NodeResources? {
            val made = NodeResources(reading(cpu), reading(memory), reading(disk))
            return if (made.cpu == null && made.memory == null && made.disk == null) null else made
        }

        fun fromJson(json: Any?): NodeResources? {
            val map = json as? Map<*, *> ?: return null
            return of((map["cpu"] as? Number)?.toDouble(), (map["memory"] as? Number)?.toDouble(), (map["disk"] as? Number)?.toDouble())
        }
    }
}

enum class FleetLightKind { Core, Runner }

data class FleetLight(
    val id: String,
    val kind: FleetLightKind,
    val name: String,
    val healthy: Boolean,
    val reason: String = "",
    val resources: NodeResources? = null,
) {
    val label: String
        get() = when (kind) {
            FleetLightKind.Core -> {
                val state = "Core node $name, ${if (healthy) "operational" else "has an issue"}"
                resources?.let { "$state, ${it.label}" } ?: state
            }
            FleetLightKind.Runner -> "Runner $name, ${if (healthy) "signed in" else "not signed in or not responding"}"
        }

    companion object {
        const val CORE_PREFIX = "core:"
        const val RUNNER_PREFIX = "runner:"

        fun of(system: StatusSystem): FleetLight? {
            val kind = when {
                system.id.startsWith(CORE_PREFIX) -> FleetLightKind.Core
                system.id.startsWith(RUNNER_PREFIX) -> FleetLightKind.Runner
                else -> return null
            }
            val name = system.id.removePrefix(if (kind == FleetLightKind.Core) CORE_PREFIX else RUNNER_PREFIX)
            if (name.isEmpty()) return null
            return FleetLight(
                id = system.id,
                kind = kind,
                name = name,
                healthy = system.state == ServiceState.Operational,
                reason = system.reason,
                resources = if (kind == FleetLightKind.Core) system.resources else null,
            )
        }

        fun lights(report: StatusReport?, monitors: List<PackageMonitors>?, onlyProblems: Boolean): List<FleetLight> {
            val all = report?.systems.orEmpty()
                .filter { system -> monitors == null || monitors.any { it.covers(system.id) } }
                .mapNotNull(::of)
            val ordered = all.filter { it.kind == FleetLightKind.Core } + all.filter { it.kind == FleetLightKind.Runner }
            return if (onlyProblems) ordered.filter { !it.healthy } else ordered
        }
    }
}
