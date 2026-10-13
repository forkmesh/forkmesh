package com.forkmesh.nativeapp.hosts

import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.double
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import java.util.Locale

enum class HostHealth(val wire: String, val label: String, val urgency: Int) {
    Online("online", "Online", 4),
    Attention("attention", "Needs attention", 1),
    Offline("offline", "Offline", 0),
    Provisioning("provisioning", "Provisioning", 2),
    Unknown("unknown", "Checking", 3),
    ;

    companion object {
        fun fromWire(wire: String): HostHealth = entries.firstOrNull { it.wire == wire } ?: Unknown
    }
}

enum class HostAction(val wire: String, val title: String) {
    Update("update", "Update ForkMesh"),
    InstallBinary("installBinary", "Install the desktop's build"),
    Restart("restart", "Restart service"),
    Stop("stop", "Stop service"),
    Start("start", "Start service"),
    Install("install", "Create host"),
    Destroy("destroy", "Destroy server"),
    Terminal("terminal", "SSH terminal"),
    ;

    val isDestructive: Boolean get() = this == Destroy || this == Stop
    val needsTypedName: Boolean get() = this == Destroy
    val opensTerminal: Boolean get() = this == Terminal

    companion object {
        fun fromWire(wire: String): HostAction? = entries.firstOrNull { it.wire == wire }
    }
}

data class HostRow(
    val name: String,
    val address: String = "",
    val user: String = "",
    val status: String = "",
    val provider: String = "",
    val region: String = "",
    val plan: String = "",
    val monthlyCost: Double = 0.0,
    val ramMb: Int = 0,
    val diskGb: Int = 0,
    val dnsHostname: String = "",
    val health: HostHealth = HostHealth.Unknown,
    val healthLabel: String = "",
    val version: String = "",
    val online: Boolean = false,
    val actions: List<HostAction> = emptyList(),
    val recentLog: List<String> = emptyList(),
    val lastResult: String = "",
) {
    val id: String get() = name.lowercase(Locale.ROOT)

    val isVultr: Boolean get() = provider.lowercase(Locale.ROOT) == "vultr"

    val planLine: String
        get() = listOfNotNull(
            plan.ifEmpty { null },
            region.ifEmpty { null },
            if (monthlyCost > 0) HostFormat.monthly(monthlyCost) else null,
        ).joinToString(" · ")

    val healthLine: String
        get() {
            val probe = healthLabel.trim { it == '●' || it == '◌' || it.isWhitespace() }
            return probe.ifEmpty { health.label }
        }

    companion object {
        fun fromJson(json: Map<String, Any?>): HostRow = HostRow(
            name = json.str("name"),
            address = json.str("address"),
            user = json.str("user"),
            status = json.str("status"),
            provider = json.str("provider"),
            region = json.str("region"),
            plan = json.str("plan"),
            monthlyCost = json.double("monthlyCost"),
            ramMb = json.int("ramMb"),
            diskGb = json.int("diskGb"),
            dnsHostname = json.str("dnsHostname"),
            health = HostHealth.fromWire(json.str("health")),
            healthLabel = json.str("healthLabel"),
            version = json.str("version"),
            online = json.bool("online"),
            actions = json.strings("actions").mapNotNull(HostAction::fromWire),
            recentLog = json.strings("recentLog"),
            lastResult = json.str("lastResult"),
        )
    }
}

data class HostsSnapshot(
    val hosts: List<HostRow> = emptyList(),
    val online: Int = 0,
    val attention: Int = 0,
    val canCreate: Boolean = false,
    val busy: Boolean = false,
    val provisioning: String = "",
    val desktop: String = "",
) {
    val sorted: List<HostRow>
        get() = hosts.sortedWith(compareBy<HostRow> { it.health.urgency }.thenBy(String.CASE_INSENSITIVE_ORDER) { it.name })

    val summary: String
        get() {
            if (hosts.isEmpty()) return "No hosts yet"
            if (attention == 0 && online == hosts.size) {
                return "All ${hosts.size} host${if (hosts.size == 1) "" else "s"} online"
            }
            val parts = mutableListOf("${hosts.size} saved", "$online online")
            if (attention > 0) parts.add("$attention need${if (attention == 1) "s" else ""} attention")
            return parts.joinToString(" · ")
        }

    companion object {
        fun fromJson(json: Map<String, Any?>): HostsSnapshot = HostsSnapshot(
            hosts = json.maps("hosts").map(HostRow::fromJson),
            online = json.int("online"),
            attention = json.int("attention"),
            canCreate = json.bool("canCreate"),
            busy = json.bool("busy"),
            provisioning = json.str("provisioning"),
            desktop = json.str("desktop"),
        )
    }
}

data class HostPlanChoice(
    val id: String,
    val monthlyCost: Double = 0.0,
    val ramMb: Int = 0,
    val diskGb: Int = 0,
    val vcpus: Int = 0,
    val regions: List<String> = emptyList(),
) {
    val label: String
        get() {
            val parts = mutableListOf(HostFormat.monthly(monthlyCost))
            if (vcpus > 0) parts.add("$vcpus vCPU")
            if (ramMb > 0) parts.add(HostFormat.memory(ramMb))
            if (diskGb > 0) parts.add("$diskGb GB disk")
            return parts.joinToString(" · ")
        }

    companion object {
        fun fromJson(json: Map<String, Any?>): HostPlanChoice = HostPlanChoice(
            id = json.str("id"),
            monthlyCost = json.double("monthlyCost"),
            ramMb = json.int("ramMb"),
            diskGb = json.int("diskGb"),
            vcpus = json.int("vcpus"),
            regions = json.strings("regions"),
        )
    }
}

data class HostCreateOptions(
    val vultr: Boolean = false,
    val plans: List<HostPlanChoice> = emptyList(),
    val defaultPlan: String = "",
    val defaultRegion: String = "",
    val suggestedName: String = "",
) {
    companion object {
        fun fromJson(json: Map<String, Any?>): HostCreateOptions = HostCreateOptions(
            vultr = json.bool("vultr"),
            plans = json.maps("plans").map(HostPlanChoice::fromJson),
            defaultPlan = json.str("defaultPlan"),
            defaultRegion = json.str("defaultRegion"),
            suggestedName = json.str("suggestedName"),
        )
    }
}

data class HostConfirmation(
    val token: String,
    val action: HostAction,
    val host: String,
    val summary: String,
    val typeToConfirm: String = "",
    val expiresInMs: Int = 60_000,
) {
    val needsTypedName: Boolean get() = typeToConfirm.isNotEmpty()

    fun accepts(typed: String): Boolean = !needsTypedName || typed.trim() == typeToConfirm

    companion object {
        fun fromJson(json: Map<String, Any?>): HostConfirmation? {
            val action = HostAction.fromWire(json.str("action")) ?: return null
            val token = json.str("token")
            if (token.isEmpty()) return null
            return HostConfirmation(
                token = token,
                action = action,
                host = json.str("host"),
                summary = json.str("summary"),
                typeToConfirm = json.str("typeToConfirm"),
                expiresInMs = json.int("expiresInMs", 60_000),
            )
        }
    }
}

data class HostActionOutcome(
    val action: String,
    val host: String,
    val ok: Boolean,
    val message: String,
    val atMs: Long,
) {
    val id: String get() = "$action:$host:$atMs"

    val headline: String
        get() {
            val what = HostAction.fromWire(action)?.title ?: action
            return "$what ${if (host.isEmpty()) "" else "of $host "}${if (ok) "finished" else "failed"}"
        }

    companion object {
        fun fromJson(json: Map<String, Any?>, atMs: Long): HostActionOutcome =
            HostActionOutcome(json.str("action"), json.str("host"), json.bool("ok"), json.str("message"), atMs)
    }
}

object HostFormat {
    fun monthly(cost: Double): String =
        if (cost == Math.rint(cost)) "$${cost.toLong()}/mo" else String.format(Locale.ROOT, "$%.2f/mo", cost)

    fun memory(mb: Int): String = if (mb >= 1024 && mb % 1024 == 0) "${mb / 1024} GB RAM" else "$mb MB RAM"

    private val regionNames = mapOf(
        "ewr" to "New Jersey", "atl" to "Atlanta", "ord" to "Chicago", "dfw" to "Dallas", "mia" to "Miami",
        "lax" to "Los Angeles", "sea" to "Seattle", "sjc" to "Silicon Valley", "hon" to "Honolulu",
    )

    fun region(code: String): String = regionNames[code]?.let { "$it ($code)" } ?: code
}
