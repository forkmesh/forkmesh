package com.forkmesh.nativeapp.reticulum

import java.util.Locale

enum class MapGroup(val label: String) {
    Peer("People"),
    Node("Nodes and services"),
    Propagation("Propagation nodes"),
    Transport("Transport"),
    Other("Other"),
    ;

    companion object {
        fun of(kind: String): MapGroup = when (kind) {
            "peer", "forkmesh" -> Peer
            "node", "service" -> Node
            "propagation" -> Propagation
            "transport", "discovery" -> Transport
            else -> Other
        }
    }
}

enum class DestinationSort(val label: String) { LastHeard("Last heard"), Hops("Hops"), Announces("Announces"), Name("Name") }

object ReticulumFormat {
    val announceIntervals: List<Pair<Int, String>> = listOf(
        0 to "Only when I tap Announce",
        15 to "Every 15 minutes",
        30 to "Every 30 minutes",
        60 to "Every hour",
        180 to "Every 3 hours",
    )

    fun bytes(value: Double): String {
        val units = arrayOf("B", "KB", "MB", "GB", "TB")
        var amount = value
        var unit = 0
        while (amount >= 1000 && unit < 4) {
            amount /= 1000
            unit++
        }
        return if (unit == 0) "${amount.toLong()} B" else String.format(Locale.ROOT, if (amount < 10) "%.1f %s" else "%.0f %s", amount, units[unit])
    }

    fun rate(bytesPerSecond: Double): String = bytes(bytesPerSecond) + "/s"

    fun duration(seconds: Long): String = when {
        seconds < 60 -> "$seconds s"
        seconds < 3600 -> "${seconds / 60} min"
        seconds < 86400 -> "${seconds / 3600} h ${(seconds % 3600) / 60} min"
        else -> "${seconds / 86400} d ${(seconds % 86400) / 3600} h"
    }

    fun age(whenMs: Long, nowMs: Long): String {
        if (whenMs <= 0) return "never"
        val seconds = maxOf(0L, (nowMs - whenMs) / 1000)
        if (seconds < 5) return "just now"
        return duration(seconds) + " ago"
    }

    fun hops(count: Int): String = if (count == 1) "1 hop" else "$count hops"

    fun plural(count: Number, word: String): String = "$count $word${if (count.toLong() == 1L) "" else "s"}"

    fun matches(record: DestinationRecord, filter: String): Boolean {
        val needle = filter.trim()
        if (needle.isEmpty()) return true
        return record.displayName.contains(needle, ignoreCase = true) || record.label.contains(needle, ignoreCase = true) ||
            record.aspect.contains(needle, ignoreCase = true) || record.hex.contains(needle, ignoreCase = true)
    }

    fun visibleDestinations(all: Collection<DestinationRecord>, filter: String, group: MapGroup?, sort: DestinationSort): List<DestinationRecord> {
        val shown = all.filter { (group == null || MapGroup.of(it.kind) == group) && matches(it, filter) }
        return when (sort) {
            DestinationSort.LastHeard -> shown.sortedByDescending { it.lastHeard }
            DestinationSort.Hops -> shown.sortedWith(compareBy<DestinationRecord> { it.hops }.thenByDescending { it.lastHeard })
            DestinationSort.Announces -> shown.sortedByDescending { it.announces }
            DestinationSort.Name -> shown.sortedWith(compareBy<DestinationRecord> { it.displayName.isEmpty() }.thenBy(String.CASE_INSENSITIVE_ORDER) { it.displayName })
        }
    }

    data class Tiles(
        val status: String,
        val statusDetail: String,
        val statusTone: Int,
        val destinations: String,
        val destinationsDetail: String,
        val announces: String,
        val announcesDetail: String,
        val announceSpark: List<Double>,
        val traffic: String,
        val trafficDetail: String,
        val trafficSpark: List<Double>,
        val paths: String,
        val pathsDetail: String,
    )

    fun tiles(snapshot: ReticulumSnapshot): Tiles {
        val online = snapshot.onlineCount
        val enabled = snapshot.enabledCount
        val (status, detail, tone) = when {
            !snapshot.running -> Triple("Off", "Disconnected by you", 0)
            online > 0 -> Triple("Online", "$online of $enabled interfaces up", 1)
            enabled > 0 -> Triple("Connecting", "No interface is up yet", 2)
            else -> Triple("No interfaces", "Add one in Interfaces", 0)
        }
        val now = snapshot.at
        var recent = 0
        var nearest = 0
        for (record in snapshot.destinations.values) {
            if ((now - record.firstHeard) / 1000 < 600) recent++
            if (record.hops <= 1) nearest++
        }
        val history = snapshot.history
        var lastMinute = 0
        var rx = 0L
        var tx = 0L
        history.forEachIndexed { index, sample ->
            if (index >= history.size - 60) lastMinute += sample.announces
            if (index >= history.size - 10) {
                rx += sample.rx
                tx += sample.tx
            }
        }
        val window = history.size.coerceIn(1, 10).toDouble()
        var answered = 0
        var pending = 0
        var fastest = -1L
        for (lookup in snapshot.lookups.values) {
            if (lookup.answered > 0) {
                answered++
                val ms = lookup.answered - lookup.started
                fastest = if (fastest < 0) ms else minOf(fastest, ms)
            } else if (!lookup.timedOut) {
                pending++
            }
        }
        return Tiles(
            status = status,
            statusDetail = detail,
            statusTone = tone,
            destinations = "${snapshot.destinations.size}",
            destinationsDetail = "$recent new in 10 min · $nearest one hop away",
            announces = "$lastMinute/min",
            announcesDetail = "${snapshot.counters.announces} heard · ${snapshot.counters.rejectedAnnounces} rejected",
            announceSpark = history.map { it.announces.toDouble() },
            traffic = rate((rx + tx) / window),
            trafficDetail = "↓ ${rate(rx / window)} · ↑ ${rate(tx / window)}",
            trafficSpark = history.map { (it.rx + it.tx).toDouble() },
            paths = "$answered found",
            pathsDetail = when {
                pending > 0 -> "$pending waiting for an answer"
                fastest >= 0 -> "Fastest answer $fastest ms"
                else -> "Ask for one in Destinations"
            },
        )
    }

    fun summary(snapshot: ReticulumSnapshot): String =
        if (snapshot.running) "${snapshot.onlineCount} of ${snapshot.enabledCount} interfaces online · you are ${Rns.shortHash(snapshot.destinationHash)}" else "Disconnected"

    fun detailRows(record: DestinationRecord, snapshot: ReticulumSnapshot, zone: java.time.ZoneId = java.time.ZoneId.systemDefault()): List<Pair<String, String>> {
        val rows = ArrayList<Pair<String, String>>()
        rows += "Application" to if (record.aspect.isEmpty()) record.label else "${record.label} (${record.aspect})"
        rows += "Destination" to Rns.hex(record.hash)
        rows += "Identity" to Rns.hex(record.identityHash)
        rows += "Distance" to hops(record.hops)
        rows += "Heard via" to snapshot.interfaceName(record.interfaceId)
        if (record.transportId.isNotEmpty()) rows += "Next hop" to Rns.hex(record.transportId)
        rows += "First heard" to age(record.firstHeard, snapshot.at)
        rows += "Last heard" to age(record.lastHeard, snapshot.at)
        if (record.emittedAt > 0) {
            rows += "Announced at" to java.time.format.DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss")
                .format(java.time.Instant.ofEpochSecond(record.emittedAt).atZone(zone))
        }
        rows += "Announces" to "${record.announces}"
        rows += "Forward secrecy" to if (record.ratchet) "Ratchet key announced" else "No ratchet"
        record.discovery?.let { found ->
            rows += "Offers" to "${found.type} ${found.reachableOn}${if (found.port > 0) ":${found.port}" else ""}"
            rows += "Proof of work" to "${found.stampValue} bits${if (found.stampValid) "" else " (below 16, not trusted)"}"
        }
        snapshot.lookups[record.hex]?.let { lookup ->
            rows += "Path lookup" to when {
                lookup.answered > 0 -> "Answered in ${lookup.answered - lookup.started} ms"
                lookup.timedOut -> "No answer"
                else -> "Waiting…"
            }
        }
        return rows
    }

    fun interfaceLines(view: InterfaceView, snapshot: ReticulumSnapshot): List<String> {
        val status = view.status
        val config = view.config
        val lines = ArrayList<String>()
        lines += "${config.kindLabel} · ${config.endpoint}"
        if (config.usesAccessCode) lines += "Access code: ${if (config.networkName.isEmpty()) "passphrase only" else "network ${config.networkName}"}, ${config.accessCodeSize} bytes"
        lines += status.state.label + if (status.detail.isEmpty()) "" else ": ${status.detail}"
        if (status.state == LinkState.Waiting && status.nextAttempt > 0) lines += "Next try in ${duration(maxOf(0L, (status.nextAttempt - snapshot.at) / 1000))}"
        if (status.connectMs >= 0) lines += "Last connect took ${status.connectMs} ms"
        lines += "Sessions ${status.sessions} · invalid frames ${status.invalidFrames} · rejected announces ${status.rejected}"
        lines += "${snapshot.destinations.values.count { it.interfaceId == config.id }} destinations best reached through it"
        return lines
    }

    fun lookupText(lookup: PathLookup?, snapshot: ReticulumSnapshot): String = when {
        lookup == null -> ""
        lookup.answered > 0 -> "Path found: ${hops(lookup.hops)} via ${snapshot.interfaceName(lookup.interfaceId)}, answered in ${lookup.answered - lookup.started} ms."
        lookup.timedOut -> "No path within ${ReticulumNode.PATH_TIMEOUT_SECONDS} seconds. The destination may be offline, or no transport node you reach knows it."
        else -> "Asking the network…"
    }

    fun counterRows(counters: PacketCounters): List<Pair<String, Long>> = listOf(
        "Announces" to counters.announces,
        "Rejected announces" to counters.rejectedAnnounces,
        "Duplicates" to counters.duplicates,
        "Data packets" to counters.data,
        "Link requests" to counters.linkRequests,
        "Proofs" to counters.proofs,
        "Unreadable frames" to counters.invalid,
        "Path requests heard" to counters.pathRequestsSeen,
        "Path requests answered" to counters.pathRequestsAnswered,
        "Announces sent" to counters.announcesSent,
    )

    fun discoveredAddress(found: DiscoveredInterface): String = when {
        found.connectable -> "${found.reachableOn}:${found.port}"
        found.frequency > 0 -> String.format(Locale.ROOT, "%.3f MHz", found.frequency / 1e6)
        else -> "Not reachable over IP"
    }

    fun location(found: DiscoveredInterface): String =
        if (found.hasLocation) String.format(Locale.ROOT, "%.3f, %.3f", found.latitude, found.longitude) else "—"

    fun interfaceFromDiscovery(found: DiscoveredInterface): InterfaceConfig = InterfaceConfig(
        id = InterfaceConfig.newId(),
        name = found.name.ifEmpty { found.reachableOn }.take(80),
        host = found.reachableOn,
        port = found.port,
        kiss = found.type == "KISSInterface",
    )

    fun messageMeta(message: ChatMessage, zone: java.time.ZoneId = java.time.ZoneId.systemDefault()): String {
        val meta = ArrayList<String>()
        meta += java.time.format.DateTimeFormatter.ofPattern("d MMM HH:mm", Locale.ENGLISH)
            .format(java.time.Instant.ofEpochMilli((message.timestamp * 1000).toLong()).atZone(zone))
        if (message.outgoing) {
            meta += when {
                message.state == MessageState.Delivered -> "✓✓ Delivered"
                message.state == MessageState.Sent -> "✓ Sent"
                message.state == MessageState.Sending && message.progress > 0 -> "Sending ${message.progress}%"
                else -> message.state.label
            }
        } else {
            meta += if (message.verified) "Signature checked" else "Signature not checked"
        }
        if (message.method.isNotEmpty()) {
            meta += when (message.method) {
                "opportunistic" -> "single packet"
                "link" -> "over a link"
                else -> "as a resource"
            }
        }
        return meta.joinToString(" · ")
    }

    fun composerHint(text: String, online: Boolean): String {
        if (text.isEmpty()) return if (online) "End-to-end encrypted with the recipient's key" else "Connect an interface to send"
        val bytes = text.toByteArray(Charsets.UTF_8).size
        val size = bytes + 16
        val route = when {
            size <= Lxmf.OPPORTUNISTIC_MAX_CONTENT -> "fits in a single packet"
            size <= Lxmf.LINK_PACKET_MAX_CONTENT -> "goes over an encrypted link"
            else -> "goes over a link as a resource"
        }
        return "$bytes bytes · $route"
    }

    fun conversationPreview(conversation: Conversation): String {
        val text = conversation.last.content.replace('\n', ' ').take(80)
        return if (conversation.last.outgoing) "You: $text" else text
    }

    fun peerSubtitle(peer: ByteArray, snapshot: ReticulumSnapshot): String {
        var subtitle = Rns.hex(peer)
        val record = snapshot.destinations[Rns.hex(peer)]
        subtitle += if (record != null) " · ${hops(record.hops)} via ${snapshot.interfaceName(record.interfaceId)} · heard ${age(record.lastHeard, snapshot.at)}" else " · not heard yet"
        val cost = record?.let { Lxmf.stampCostFromAppData(it.appData) } ?: 0
        if (cost > 0) subtitle += " · asks for $cost bits of proof of work"
        return subtitle
    }
}
