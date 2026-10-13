package com.forkmesh.nativeapp.hosts

import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.double
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings

data class FleetCostRow(val key: String, val count: Int, val monthlyCost: Double)

data class FleetCosts(
    val hosts: Int = 0,
    val vultr: Int = 0,
    val monthlyCost: Double = 0.0,
    val byRegion: List<FleetCostRow> = emptyList(),
    val byPlan: List<FleetCostRow> = emptyList(),
) {
    val yearlyCost: Double get() = monthlyCost * 12

    val line: String
        get() {
            if (vultr == 0) return "No Vultr servers"
            return "$vultr Vultr server${if (vultr == 1) "" else "s"} · ${RemoteFormat.money(monthlyCost)}/month"
        }

    companion object {
        fun fromJson(json: Map<String, Any?>) = FleetCosts(
            hosts = json.int("hosts"),
            vultr = json.int("vultr"),
            monthlyCost = json.double("monthlyCost"),
            byRegion = json.maps("byRegion").map { FleetCostRow(it.str("region"), it.int("count"), it.double("monthlyCost")) },
            byPlan = json.maps("byPlan").map { FleetCostRow(it.str("plan"), it.int("count"), it.double("monthlyCost")) },
        )
    }
}

data class FleetTarget(
    val available: Boolean = false,
    val enabled: Boolean = false,
    val desired: Int = 0,
    val status: String = "",
    val nextCheckInMs: Long = -1,
    val reconciling: Boolean = false,
) {
    val line: String
        get() {
            if (!available) return "Open the desktop's Hosts page once to manage the fleet size."
            if (!enabled) return "The desktop does not manage the fleet size."
            val parts = mutableListOf("Keeps $desired healthy mirror${if (desired == 1) "" else "s"}")
            if (reconciling) parts.add("checking now")
            else if (nextCheckInMs >= 0) parts.add("next check in ${RemoteFormat.duration(nextCheckInMs)}")
            return parts.joinToString(" · ")
        }

    fun countdown(elapsedMs: Long): Long = if (nextCheckInMs < 0) -1 else maxOf(0L, nextCheckInMs - maxOf(0L, elapsedMs))

    companion object {
        fun fromJson(json: Map<String, Any?>) = FleetTarget(
            available = json.bool("available"),
            enabled = json.bool("enabled"),
            desired = json.int("desired"),
            status = json.str("status"),
            nextCheckInMs = json.long("nextCheckInMs", -1),
            reconciling = json.bool("reconciling"),
        )
    }
}

enum class ProvisionStageState(val wire: String) {
    Pending("pending"),
    Active("active"),
    Done("done"),
    Failed("failed"),
    ;

    companion object {
        fun of(wire: String): ProvisionStageState = entries.firstOrNull { it.wire == wire } ?: Pending
    }
}

data class ProvisionStage(val number: Int, val name: String, val state: ProvisionStageState, val detail: String) {
    companion object {
        fun fromJson(json: Map<String, Any?>) =
            ProvisionStage(json.int("number"), json.str("name"), ProvisionStageState.of(json.str("status")), json.str("detail"))
    }
}

data class ProvisionStatus(
    val active: Boolean = false,
    val node: String = "",
    val stage: Int = 0,
    val stageCount: Int = 6,
    val state: String = "",
    val detail: String = "",
    val message: String = "",
    val gap: String = "",
    val resumable: Boolean = false,
    val attempts: Int = 0,
    val stages: List<ProvisionStage> = emptyList(),
    val log: List<String> = emptyList(),
) {
    val isVisible: Boolean get() = active || resumable || (node.isNotEmpty() && state == "failed")

    val progress: Double
        get() {
            if (stageCount <= 0) return 0.0
            val done = stages.count { it.state == ProvisionStageState.Done }
            return minOf(1.0, done.toDouble() / stageCount)
        }

    val currentStage: ProvisionStage? get() = stages.firstOrNull { it.number == stage }

    val headline: String
        get() {
            val name = node.ifEmpty { "New mirror" }
            if (active) {
                val current = currentStage?.name.orEmpty()
                return if (current.isEmpty()) "Deploying $name" else "Deploying $name: $current"
            }
            if (resumable) return "$name is paused"
            if (state == "failed") return "$name failed"
            if (state == "done") return "$name is live"
            return name
        }

    companion object {
        fun fromJson(json: Map<String, Any?>) = ProvisionStatus(
            active = json.bool("active"),
            node = json.str("node"),
            stage = json.int("stage"),
            stageCount = json.int("stageCount", 6),
            state = json.str("state"),
            detail = json.str("detail"),
            message = json.str("message"),
            gap = json.str("gap"),
            resumable = json.bool("resumable"),
            attempts = json.int("attempts"),
            stages = json.maps("stages").map(ProvisionStage::fromJson),
            log = json.strings("log"),
        )
    }
}

data class GatewayHealthRow(val host: String, val downSinceMs: Long, val downForMs: Long, val retired: Boolean) {
    val line: String
        get() = if (retired) {
            "Down ${RemoteFormat.duration(downForMs)} · retired from the automatic push"
        } else {
            "Down ${RemoteFormat.duration(downForMs)}"
        }

    companion object {
        fun fromJson(json: Map<String, Any?>) =
            GatewayHealthRow(json.str("host"), json.long("downSinceMs"), json.long("downForMs"), json.bool("retired"))
    }
}

data class FleetSummary(
    val costs: FleetCosts = FleetCosts(),
    val fleet: FleetTarget = FleetTarget(),
    val provision: ProvisionStatus = ProvisionStatus(),
    val gateways: List<GatewayHealthRow> = emptyList(),
    val mirrorClientStatus: String = "",
    val mirrorClientHealth: String = "",
    val pushing: Int = 0,
    val pushPending: Int = 0,
    val canCreate: Boolean = false,
    val busy: Boolean = false,
    val deploying: Int = 0,
    val admin: Boolean = false,
    val nodeDeleteInProgress: Boolean = false,
    val nodeDeleteTarget: String = "",
    val lastResult: String = "",
    val running: List<String> = emptyList(),
) {
    val gatewaysDown: Int get() = gateways.count { !it.retired }

    companion object {
        fun fromJson(json: Map<String, Any?>): FleetSummary {
            val mirrorClient = json.obj("mirrorClient")
            val mirrorPush = json.obj("mirrorPush")
            val nodeDelete = json.obj("nodeDelete")
            return FleetSummary(
                costs = FleetCosts.fromJson(json.obj("costs")),
                fleet = FleetTarget.fromJson(json.obj("fleet")),
                provision = ProvisionStatus.fromJson(json.obj("provision")),
                gateways = json.maps("gateways").map(GatewayHealthRow::fromJson),
                mirrorClientStatus = mirrorClient.str("status"),
                mirrorClientHealth = mirrorClient.str("health"),
                pushing = mirrorPush.int("pushing"),
                pushPending = mirrorPush.int("pending"),
                canCreate = json.bool("canCreate"),
                busy = json.bool("busy"),
                deploying = json.int("deploying"),
                admin = json.bool("admin"),
                nodeDeleteInProgress = nodeDelete.bool("inProgress"),
                nodeDeleteTarget = nodeDelete.str("target"),
                lastResult = json.str("lastResult"),
                running = json.strings("running"),
            )
        }
    }
}

data class FleetRepo(val repo: String, val isPrivate: Boolean, val published: Boolean, val pushing: Boolean) {
    companion object {
        fun fromJson(json: Map<String, Any?>) =
            FleetRepo(json.str("repo"), json.bool("private"), json.bool("published"), json.bool("pushing"))
    }
}

data class DiskUsageEntry(val name: String, val path: String, val bytes: Double, val isTotal: Boolean) {
    companion object {
        fun fromJson(json: Map<String, Any?>) =
            DiskUsageEntry(json.str("name"), json.str("path"), json.double("bytes"), json.bool("total"))
    }
}

data class DiskUsage(val host: String, val path: String, val entries: List<DiskUsageEntry>) {
    val children: List<DiskUsageEntry> get() = entries.filter { !it.isTotal }

    val total: Double get() = entries.firstOrNull { it.isTotal }?.bytes ?: children.sumOf { it.bytes }

    val sortedChildren: List<DiskUsageEntry> get() = children.sortedWith(compareByDescending<DiskUsageEntry> { it.bytes }.thenBy { it.name })

    fun share(entry: DiskUsageEntry): Double = if (total > 0) minOf(1.0, entry.bytes / total) else 0.0

    val parentPath: String?
        get() {
            if (path == "/" || path.isEmpty()) return null
            val parts = path.split('/').filter { it.isNotEmpty() }.toMutableList()
            if (parts.isNotEmpty()) parts.removeAt(parts.size - 1)
            return "/" + parts.joinToString("/")
        }

    companion object {
        fun fromJson(json: Map<String, Any?>) =
            DiskUsage(json.str("host"), json.str("path"), json.maps("entries").map(DiskUsageEntry::fromJson))
    }
}

enum class FleetDeployMode(val wire: String, val title: String) {
    Install("install", "Install the latest release"),
    Reinstall("reinstall", "Uninstall and reinstall all"),
    Source("source", "Update all from source"),
}

sealed class FleetAction {
    data class Add(val name: String, val address: String, val user: String) : FleetAction()
    data class Forget(val host: String) : FleetAction()
    data class DeployAll(val mode: FleetDeployMode) : FleetAction()
    data class Uninstall(val host: String) : FleetAction()
    data class AgentClis(val host: String) : FleetAction()
    data class Actions(val host: String, val enabled: Boolean) : FleetAction()
    data class LinkCode(val code: String) : FleetAction()
    data object Resume : FleetAction()
    data class Target(val enabled: Boolean, val desired: Int) : FleetAction()
    data object Reconcile : FleetAction()
    data class MirrorPush(val repo: String) : FleetAction()
    data class DeleteNode(val node: String) : FleetAction()

    val wireName: String
        get() = when (this) {
            is Add -> "add"
            is Forget -> "forget"
            is DeployAll -> "deployAll"
            is Uninstall -> "uninstall"
            is AgentClis -> "agentClis"
            is Actions -> "actions"
            is LinkCode -> "linkCode"
            Resume -> "resume"
            is Target -> "fleetTarget"
            Reconcile -> "reconcile"
            is MirrorPush -> "mirrorPush"
            is DeleteNode -> "deleteNode"
        }

    val params: Map<String, Any?>
        get() {
            val out = linkedMapOf<String, Any?>("action" to wireName)
            when (this) {
                is Add -> {
                    out["host"] = name.trim()
                    out["address"] = address.trim()
                    out["user"] = user.trim()
                }
                is Forget -> out["host"] = host
                is Uninstall -> out["host"] = host
                is AgentClis -> out["host"] = host
                is DeleteNode -> out["host"] = node
                is DeployAll -> out["mode"] = mode.wire
                is Actions -> {
                    out["host"] = host
                    out["enabled"] = enabled
                }
                is LinkCode -> out["code"] = code.trim()
                is Target -> {
                    out["enabled"] = enabled
                    out["desired"] = desired.toLong()
                }
                is MirrorPush -> out["repo"] = repo
                Resume, Reconcile -> Unit
            }
            return out
        }

    val title: String
        get() = when (this) {
            is Add -> "Add host"
            is Forget -> "Forget host"
            is DeployAll -> mode.title
            is Uninstall -> "Uninstall ForkMesh"
            is AgentClis -> "Install agent CLIs"
            is Actions -> if (enabled) "Turn Actions on" else "Turn Actions off"
            is LinkCode -> "Link a node"
            Resume -> "Resume deployment"
            is Target -> "Fleet target"
            Reconcile -> "Check the fleet now"
            is MirrorPush -> "Mirror push"
            is DeleteNode -> "Delete node"
        }

    val isDestructive: Boolean
        get() = when (this) {
            is Uninstall, is DeleteNode, is Forget -> true
            is DeployAll -> mode == FleetDeployMode.Reinstall
            else -> false
        }
}

object FleetInput {
    private fun asciiDigit(c: Char) = c in '0'..'9'

    private fun asciiAlnum(c: Char) = asciiDigit(c) || c in 'a'..'z' || c in 'A'..'Z'

    fun validLinkCode(code: String): Boolean {
        val trimmed = code.trim()
        return trimmed.length == 6 && trimmed.all(::asciiDigit)
    }

    fun validHostName(name: String): Boolean {
        val trimmed = name.trim()
        if (trimmed.isEmpty() || trimmed.length > 63 || !asciiAlnum(trimmed[0])) return false
        return trimmed.all { asciiAlnum(it) || it == '.' || it == '_' || it == '-' }
    }

    fun validAddress(address: String): Boolean {
        val trimmed = address.trim()
        if (trimmed.isEmpty() || trimmed.length > 253) return false
        return trimmed.all { asciiAlnum(it) || it == '.' || it == '-' || it == ':' }
    }

    fun validUser(user: String): Boolean {
        val trimmed = user.trim()
        if (trimmed.isEmpty() || trimmed.length > 32) return false
        val first = trimmed[0]
        if (!(first in 'a'..'z' || first == '_')) return false
        return trimmed.all { it in 'a'..'z' || asciiDigit(it) || it == '_' || it == '-' }
    }
}
