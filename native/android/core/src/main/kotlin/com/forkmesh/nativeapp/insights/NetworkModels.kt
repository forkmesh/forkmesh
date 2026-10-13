package com.forkmesh.nativeapp.insights

import com.forkmesh.nativeapp.api.ServerUrl
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.json.list
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.model.Member

enum class NodeDiagnosticSeverity(val rank: Int, val label: String, val tone: StatsTone) {
    Ok(0, "OK", StatsTone.Good),
    Info(1, "Info", StatsTone.None),
    Warning(2, "Warning", StatsTone.Warning),
    Critical(3, "Critical", StatsTone.Critical),
    ;

    companion object {
        fun of(rank: Int): NodeDiagnosticSeverity = entries.first { it.rank == rank.coerceIn(0, 3) }
    }
}

data class NodeDiagnosticFinding(val id: String, val severity: NodeDiagnosticSeverity, val message: String) {
    companion object {
        const val MAX_FINDINGS = 8
        const val MAX_MESSAGE = 160

        fun parse(value: Any?): List<NodeDiagnosticFinding> {
            val findings = ArrayList<NodeDiagnosticFinding>()
            for (item in value as? List<*> ?: emptyList<Any?>()) {
                if (findings.size >= MAX_FINDINGS) break
                val json = item.asJsonMap()
                val id = json.str("i").take(32)
                val message = json.str("m").take(MAX_MESSAGE)
                if (id.isEmpty() || message.isEmpty()) continue
                findings.add(NodeDiagnosticFinding(id, NodeDiagnosticSeverity.of(json.int("s", NodeDiagnosticSeverity.Info.rank)), message))
            }
            return findings.sortedByDescending { it.severity.rank }
        }

        fun worst(findings: List<NodeDiagnosticFinding>): NodeDiagnosticSeverity =
            findings.maxByOrNull { it.severity.rank }?.severity ?: NodeDiagnosticSeverity.Ok
    }
}

data class NodeRow(
    val id: String,
    val name: String,
    val owner: String = "",
    val platform: String = "",
    val version: String = "",
    val online: Boolean,
    val isSelf: Boolean = false,
    val mirrors: Int = 0,
    val diagnostics: List<NodeDiagnosticFinding> = emptyList(),
) {
    val worst: NodeDiagnosticSeverity get() = NodeDiagnosticFinding.worst(diagnostics)

    val platformLabel: String
        get() = when (platform.lowercase()) {
            "linux" -> "Linux"
            "macos", "darwin", "osx" -> "macOS"
            "windows", "win" -> "Windows"
            "ios" -> "iPhone"
            "android" -> "Android"
            "web", "browser" -> "Web"
            "" -> "Unknown"
            else -> platform
        }

    val subtitle: String
        get() {
            val parts = mutableListOf(platformLabel)
            if (version.isNotEmpty()) parts.add("v$version")
            if (owner.isNotEmpty() && owner.lowercase() != name.lowercase()) parts.add("owner $owner")
            if (mirrors > 0) parts.add("$mirrors mirror${if (mirrors == 1) "" else "s"}")
            return parts.joinToString(" · ")
        }

    val accessibilityLabel: String
        get() {
            var text = "$name, ${if (online) "online" else "offline"}, $subtitle"
            if (worst.rank >= NodeDiagnosticSeverity.Warning.rank) {
                text += ", ${diagnostics.count { it.severity.rank >= NodeDiagnosticSeverity.Warning.rank }} health warnings"
            }
            return text
        }

    companion object {
        fun fromMember(member: Member, diagnostics: List<NodeDiagnosticFinding> = emptyList()) = NodeRow(
            id = member.id,
            name = member.nodeName.ifEmpty { member.name },
            owner = member.owner,
            platform = member.platform,
            version = member.version,
            online = member.online,
            isSelf = member.self,
            mirrors = member.mirrors.size,
            diagnostics = diagnostics,
        )
    }
}

object NodeDirectory {
    const val MAX_ROWS = 500

    fun merge(live: List<NodeRow>, onlineNames: List<String>): List<NodeRow> {
        val rows = ArrayList<NodeRow>()
        val seen = HashSet<String>()
        for (row in live) {
            val key = row.name.lowercase()
            if (key.isEmpty() || !seen.add(key)) continue
            rows.add(row)
        }
        for (name in onlineNames) {
            val key = name.lowercase()
            if (key.isEmpty() || !seen.add(key)) continue
            rows.add(NodeRow(id = "relay:$key", name = name, online = true))
        }
        return rows.sortedWith(order).take(MAX_ROWS)
    }

    private val order = Comparator<NodeRow> { a, b ->
        when {
            a.isSelf != b.isSelf -> if (a.isSelf) -1 else 1
            a.worst != b.worst -> b.worst.rank - a.worst.rank
            a.online != b.online -> if (a.online) -1 else 1
            else -> a.name.lowercase().compareTo(b.name.lowercase())
        }
    }

    fun filter(rows: List<NodeRow>, query: String): List<NodeRow> {
        val needle = query.trim().lowercase()
        if (needle.isEmpty()) return rows
        return rows.filter {
            it.name.lowercase().contains(needle) || it.owner.lowercase().contains(needle) || it.platformLabel.lowercase().contains(needle)
        }
    }
}

data class NetworkSnapshot(val repos: Int, val hosts: Int, val clients: Int, val onlineNodes: List<String>) {
    companion object {
        fun fromJson(json: Map<String, Any?>) = NetworkSnapshot(
            repos = json.int("repos"),
            hosts = json.int("hosts"),
            clients = json.int("clients"),
            onlineNodes = json.list("onlineNodes").map { jsonText(it).trim() }.filter { it.isNotEmpty() },
        )
    }
}

data class OnlineHistory(val hours: List<Hour>, val nodes: List<Node>) {
    data class Hour(val hourTs: Long, val nodeMinutes: Int)

    data class Node(val id: String, val label: String, val totalMinutes: Int)

    val series: StatsTileSeries get() = StatsTileSeries(hours.map { it.hourTs.toString() }, hours.map { it.nodeMinutes.toDouble() })

    val peakHour: Hour? get() = hours.maxByOrNull { it.nodeMinutes }

    companion object {
        fun fromJson(json: Map<String, Any?>) = OnlineHistory(
            hours = json.maps("hours").map { Hour(it.long("hourTs"), it.int("nodeMinutes")) },
            nodes = json.maps("nodes").take(50).map { Node(it.str("id"), it.str("label"), it.int("totalMinutes")) },
        )
    }
}

data class RelayRow(
    val id: String,
    val label: String,
    val host: String,
    val active: Boolean,
    val verifiedAt: Long = 0,
    val declared: Boolean = false,
    val missing: List<String> = emptyList(),
)

data class EndpointRow(val name: String, val url: String, val purpose: String) {
    companion object {
        fun forRelay(serverUrl: String): List<EndpointRow> {
            val parsed = ServerUrl.parse(serverUrl)
            if (parsed.host.isEmpty()) return emptyList()
            val scheme = serverUrl.substringBefore("://", "").lowercase()
            val secure = scheme == "wss" || scheme == "https"
            val port = parsed.port?.takeIf { it > 0 && it != (if (secure) 443 else 80) }?.let { ":$it" }.orEmpty()
            val http = "${if (secure) "https" else "http"}://${parsed.host}$port"
            return listOf(
                EndpointRow("API", "$http/api", "Accounts, repositories, issues and pings"),
                EndpointRow("Relay socket", serverUrl, "Chat, presence and desktop control"),
                EndpointRow("Git", http, "Clone and fetch over HTTPS"),
                EndpointRow("Status", "$http/api/status", "Health checks shown on the Status page"),
            )
        }
    }
}

enum class NetworkSection(val wire: String, val title: String) {
    Nodes("nodes", "Nodes"),
    Relays("relays", "Relays"),
    Endpoints("endpoints", "Endpoints"),
    Activity("activity", "Activity"),
    ;

    companion object {
        fun of(route: String?): NetworkSection = entries.firstOrNull { it.wire == route?.lowercase() } ?: Nodes
    }
}
