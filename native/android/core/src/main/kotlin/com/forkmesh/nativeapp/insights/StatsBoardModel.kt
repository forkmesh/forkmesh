package com.forkmesh.nativeapp.insights

import com.forkmesh.nativeapp.hosts.DesktopHealth
import com.forkmesh.nativeapp.hosts.DesktopHealthLevel
import com.forkmesh.nativeapp.hosts.RemoteFormat
import com.forkmesh.nativeapp.hosts.isUnsupportedControl
import java.util.Collections
import kotlin.concurrent.thread

object StatsTiles {
    val desktopIds: Set<String> = setOf("agents", "stalls", "desktopLoad", "desktopMemory")
    val networkIds: Set<String> = setOf("repos", "nodes", "hosts")
    val historyIds: Set<String> = setOf("nodeHours")
    val statusIds: Set<String> = setOf("uptime", "incidents", "errors")
    val releaseIds: Set<String> = LiveReleases.homepageIds + LiveReleases.IOS_ID
    val serverIds: Set<String> get() = networkIds + historyIds + statusIds + releaseIds
    val allIds: Set<String> get() = serverIds + desktopIds + DesktopTiles.ids

    fun network(snapshot: NetworkSnapshot): Map<String, StatsTileData> = mapOf(
        "repos" to StatsTileData(StatsFormat.compactNumber(snapshot.repos.toDouble()), "in the catalog"),
        "nodes" to StatsTileData(
            StatsFormat.compactNumber(snapshot.onlineNodes.size.toDouble()),
            "connected now",
            status = if (snapshot.onlineNodes.isEmpty()) StatsTone.Warning else StatsTone.Good,
        ),
        "hosts" to StatsTileData(StatsFormat.compactNumber(snapshot.hosts.toDouble()), "serving repositories"),
    )

    fun history(history: OnlineHistory): Map<String, StatsTileData> {
        val total = history.hours.sumOf { it.nodeMinutes }
        return mapOf(
            "nodeHours" to StatsTileData(
                StatsFormat.compactDuration(total.toLong() * 60_000),
                "node time online, 24 h",
                series = history.series,
                available = history.hours.isNotEmpty(),
                trend = StatsTrendPolarity.UpIsGood,
            ),
        )
    }

    fun status(report: StatusReport): Map<String, StatsTileData> {
        val down = report.down.size
        val uptime = report.websiteUptime24h
        val tiles = linkedMapOf(
            "uptime" to StatsTileData(
                StatsFormat.percent(uptime, 2),
                report.headline,
                available = uptime != null,
                status = when {
                    uptime == null -> StatsTone.None
                    uptime >= 99.5 -> StatsTone.Good
                    uptime >= 95 -> StatsTone.Warning
                    else -> StatsTone.Critical
                },
            ),
            "incidents" to StatsTileData(
                "$down",
                if (down == 0) "all core systems up" else report.down.take(2).joinToString(", ") { it.label },
                status = if (down == 0) StatsTone.Good else StatsTone.Critical,
            ),
        )
        val errors = report.current.errors24h
        tiles["errors"] = if (errors != null) {
            StatsTileData(
                StatsFormat.compactNumber(errors.toDouble()),
                "logged by the server",
                status = if (errors == 0) StatsTone.Good else if (errors < 50) StatsTone.Warning else StatsTone.Critical,
                trend = StatsTrendPolarity.DownIsGood,
            )
        } else {
            StatsTileData.unavailable
        }
        return tiles
    }

    fun desktop(health: DesktopHealth): Map<String, StatsTileData> {
        val tiles = linkedMapOf(
            "agents" to StatsTileData("${health.agentsRunning}", if (health.agentsQueued > 0) "${health.agentsQueued} queued" else "none queued"),
            "stalls" to StatsTileData("${health.stalls}", "recorded on the desktop", status = if (health.stalls == 0) StatsTone.Good else StatsTone.Warning),
        )
        tiles["desktopLoad"] = if (health.cpuPercent >= 0) {
            StatsTileData(RemoteFormat.fixed(health.cpuPercent, 0) + "%", if (health.level == DesktopHealthLevel.Unknown) "CPU" else health.level.label, status = health.tone)
        } else {
            StatsTileData.unavailable
        }
        val share = health.memoryUsedShareOrNull
        tiles["desktopMemory"] = if (share != null) {
            val used = (health.memoryTotal - maxOf(0.0, health.memoryAvailable)).toLong()
            StatsTileData(
                RemoteFormat.fixed(share * 100, 0) + "%",
                "${StatsFormat.compactBytes(used)} of ${StatsFormat.compactBytes(health.memoryTotal.toLong())}",
                status = if (share > 0.9) StatsTone.Critical else if (share > 0.75) StatsTone.Warning else StatsTone.Good,
            )
        } else {
            StatsTileData.unavailable
        }
        return tiles
    }
}

class StatsBoardModel(
    private val transport: InsightsRequesting,
    private val layoutStore: StatsLayoutStore,
    private val desktop: (() -> DesktopHealth)? = null,
    private val desktopTiles: (() -> DesktopTilesReply)? = null,
    private val now: () -> Long = System::currentTimeMillis,
) {
    @Volatile
    var layout: List<StatsTilePlacement>
        private set

    @Volatile
    var offered: List<String>
        private set

    @Volatile
    var tiles: Map<String, StatsTileData> = emptyMap()
        private set

    @Volatile
    var loading: Boolean = false
        private set

    @Volatile
    var pending: Set<String> = StatsTiles.allIds
        private set

    @Volatile
    var desktopOffered: Set<String> = emptySet()
        private set

    @Volatile
    var failures: List<String> = emptyList()
        private set

    @Volatile
    var desktopNote: String = ""
        private set

    @Volatile
    var desktopTilesNote: String = ""
        private set

    @Volatile
    var updatedAtMs: Long = 0
        private set

    init {
        val merge = layoutStore.load()
        layout = merge.layout
        offered = merge.offered
        desktopOffered = layoutStore.loadDesktopTiles()
    }

    fun isShown(id: String): Boolean = !DesktopTiles.isDesktopTile(id) || id in desktopOffered

    val visible: List<StatsTilePlacement> get() = StatsLayout.visible(layout, StatsCatalog.entries).filter { isShown(it.id) }

    val addable: List<StatsTileCatalogEntry>
        get() {
            val placed = layout.map { it.id }.toSet()
            return StatsCatalog.entries.filter { it.id !in placed && isShown(it.id) }
        }

    fun data(id: String): StatsTileData? = tiles[id]

    fun isLoading(id: String): Boolean = id in pending

    fun reload(changed: () -> Unit = {}) {
        loading = true
        pending = StatsTiles.allIds
        changed()
        try {
            val failed = Collections.synchronizedSet(HashSet<String>())
            val parts = listOf(
                thread(name = "stats-network") {
                    load(StatsTiles.networkIds, "network", failed, changed) {
                        StatsTiles.network(NetworkSnapshot.fromJson(transport.call("GET", "/api/network/stats", null)))
                    }
                },
                thread(name = "stats-activity") {
                    load(StatsTiles.historyIds, "activity", failed, changed) {
                        StatsTiles.history(OnlineHistory.fromJson(transport.call("GET", "/api/network/online-history", null)))
                    }
                },
                thread(name = "stats-status") {
                    load(StatsTiles.statusIds, "status", failed, changed) {
                        StatsTiles.status(StatusReport.fromJson(transport.call("GET", "/api/status", null)))
                    }
                },
                thread(name = "stats-releases") {
                    load(LiveReleases.homepageIds, "releases", failed, changed) { LiveReleases.tiles(LiveReleases.load(transport)) }
                },
                thread(name = "stats-app-store") { loadAppStore(changed) },
                thread(name = "stats-desktop") { loadDesktop(changed) },
                thread(name = "stats-desktop-tiles") { loadDesktopTiles(changed) },
            )
            parts.forEach { it.join() }
            failures = listOf("network", "activity", "status", "releases").filter { it in failed }
            updatedAtMs = now()
        } finally {
            pending = emptySet()
            loading = false
        }
    }

    private fun load(ids: Set<String>, failure: String, failed: MutableSet<String>, changed: () -> Unit, fetch: () -> Map<String, StatsTileData>) {
        try {
            publish(fetch())
        } catch (_: Exception) {
            failed.add(failure)
        } finally {
            finish(ids)
            changed()
        }
    }

    private fun loadDesktop(changed: () -> Unit) {
        try {
            val source = desktop
            if (source == null) {
                desktopNote = "Pair a desktop to see its load and agents."
                publish(StatsTiles.desktopIds.associateWith { StatsTileData.unavailable })
                return
            }
            try {
                publish(StatsTiles.desktop(source()))
                desktopNote = ""
            } catch (error: Exception) {
                desktopNote = DesktopStatsClient.explain(error)
                publishMissing(StatsTiles.desktopIds)
            }
        } finally {
            finish(StatsTiles.desktopIds)
            changed()
        }
    }

    private fun loadAppStore(changed: () -> Unit) {
        try {
            publish(mapOf(LiveReleases.IOS_ID to LiveReleases.loadAppStore(transport)))
        } catch (error: Exception) {
            val why = InsightsException.from(error).message
            publishMissing(setOf(LiveReleases.IOS_ID), StatsTileData(note = "App Store status unavailable: $why", available = false))
        } finally {
            finish(setOf(LiveReleases.IOS_ID))
            changed()
        }
    }

    private fun loadDesktopTiles(changed: () -> Unit) {
        try {
            val source = desktopTiles
            if (source == null) {
                offerDesktopTiles(emptySet())
                desktopTilesNote = ""
                return
            }
            try {
                val reply = source()
                offerDesktopTiles(reply.offered intersect DesktopTiles.ids)
                publish(reply.tiles.filterKeys { it in DesktopTiles.ids })
                desktopTilesNote = ""
            } catch (error: Exception) {
                if (isUnsupportedControl(error)) {
                    offerDesktopTiles(emptySet())
                    desktopTilesNote = "Update ForkMesh on your desktop to see all of its stats here."
                } else {
                    desktopTilesNote = ""
                    publishMissing(desktopOffered, StatsTileData(note = DesktopStatsClient.explain(error), available = false))
                }
            }
        } finally {
            finish(DesktopTiles.ids)
            changed()
        }
    }

    private fun offerDesktopTiles(ids: Set<String>) {
        if (ids == desktopOffered) return
        desktopOffered = ids
        layoutStore.saveDesktopTiles(ids)
    }

    @Synchronized
    private fun publish(fresh: Map<String, StatsTileData>) {
        tiles = LinkedHashMap(tiles).apply { putAll(fresh) }
    }

    @Synchronized
    private fun publishMissing(ids: Set<String>, missing: StatsTileData = StatsTileData.unavailable) {
        tiles = LinkedHashMap(tiles).apply { for (id in ids) if (this[id]?.available != true) put(id, missing) }
    }

    @Synchronized
    private fun finish(ids: Set<String>) {
        pending = pending - ids
    }

    fun move(id: String, to: Int) {
        layout = StatsLayout.move(layout, id, to)
        persist()
    }

    fun remove(id: String) {
        layout = layout.filter { it.id != id }
        persist()
    }

    fun add(id: String) {
        if (StatsCatalog.entry(id) == null || !isShown(id) || layout.any { it.id == id }) return
        layout = layout + StatsTilePlacement(id)
        persist()
    }

    fun toggleWide(id: String) {
        layout = layout.map { if (it.id == id) it.copy(width = if (it.width == 1) 2 else 1) else it }
        persist()
    }

    fun reset() {
        layoutStore.reset()
        val merge = layoutStore.load()
        layout = merge.layout
        offered = merge.offered
    }

    private fun persist() = layoutStore.save(layout, offered)
}
