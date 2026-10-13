package com.forkmesh.nativeapp.insights

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.control.AgentSessionLookup
import com.forkmesh.nativeapp.control.AgentTransportBadge
import com.forkmesh.nativeapp.control.ControlCalling
import com.forkmesh.nativeapp.control.ControlEvent
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.control.UnifiedAgentRow
import com.forkmesh.nativeapp.hosts.DesktopHealth
import com.forkmesh.nativeapp.hosts.DesktopHealthLevel
import com.forkmesh.nativeapp.model.Issue
import com.forkmesh.nativeapp.model.IssueEvent
import com.forkmesh.nativeapp.model.Member
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.model.PullRequest
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import java.nio.file.Files
import kotlin.math.abs

private fun near(expected: Double, actual: Double?) = expect(actual != null && abs(expected - actual) < 0.01, "expected $expected but was $actual")

class FakeTransport : InsightsRequesting {
    val replies = HashMap<String, Map<String, Any?>>()
    val failures = HashMap<String, Exception>()
    val calls = ArrayList<Triple<String, String, Map<String, Any?>?>>()

    override fun call(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?> {
        synchronized(calls) { calls.add(Triple(method, path, body)) }
        failures[path]?.let { throw it }
        return replies[path] ?: emptyMap()
    }
}

class FakeStatsChannel : ControlCalling {
    val replies = HashMap<String, Map<String, Any?>>()
    val calls = ArrayList<Pair<String, Map<String, Any?>>>()
    var failure: String? = null

    override fun request(method: String, params: Map<String, Any?>, timeoutMs: Long): Map<String, Any?> {
        calls.add(method to params)
        failure?.let { throw LocalControlException(it) }
        return replies[method] ?: emptyMap()
    }

    override fun addEventTap(tap: (ControlEvent) -> Unit): Any = Any()

    override fun removeEventTap(token: Any) {}
}

class InsightsTest {
    fun testCompactNumbersMatchTheDesktop() {
        assertEq("1,284", StatsFormat.compactNumber(1284.0))
        assertEq("12.9K", StatsFormat.compactNumber(12900.0))
        assertEq("4.20M", StatsFormat.compactNumber(4_200_000.0))
        assertEq("42", StatsFormat.compactNumber(42.0))
        assertEq("2.2", StatsFormat.compactNumber(2.25))
        assertEq("250K", StatsFormat.compactNumber(250_000.0))
        assertEq("3.0B", StatsFormat.compactNumber(3_000_000_000.0))
        assertEq("1.5 KB", StatsFormat.compactBytes(1536))
        assertEq("512 B", StatsFormat.compactBytes(512))
        assertEq("50 MB", StatsFormat.compactBytes(50L * 1024 * 1024))
        assertEq("$12.50", StatsFormat.compactUsd(12.5))
        assertEq("-$12.50", StatsFormat.compactUsd(-12.5))
        assertEq("$1,284", StatsFormat.compactUsd(1284.0))
        assertEq("3h 5m", StatsFormat.compactDuration(3L * 3600 * 1000 + 5 * 60 * 1000))
        assertEq("59s", StatsFormat.compactDuration(59_000))
        assertEq("2d 2h", StatsFormat.compactDuration(50L * 3600 * 1000))
        assertEq("99.95%", StatsFormat.percent(99.95, 2))
        assertEq("–", StatsFormat.percent(null))
    }

    fun testTrendComparesTheNewerHalfWithTheOlderHalf() {
        near(180.0, StatsFormat.trendPercent(listOf(1.0, 4.0, 2.0, 12.0)))
        near(-50.0, StatsFormat.trendPercent(listOf(10.0, 10.0, 7.0, 5.0, 5.0)))
        assertEq(null, StatsFormat.trendPercent(listOf(0.0, 0.0, 3.0, 3.0)))
        assertEq(null, StatsFormat.trendPercent(listOf(1.0, 2.0, 3.0)))
        assertEq(null, StatsFormat.trendPercent(listOf(1.0, 4.0, 2.0, 12.0), categorical = true))
        val rising = StatsTileSeries(values = listOf(1.0, 1.0, 5.0, 5.0))
        assertEq(StatsTone.Good, StatsTileData(series = rising, trend = StatsTrendPolarity.UpIsGood).trendTone)
        assertEq(StatsTone.Warning, StatsTileData(series = rising, trend = StatsTrendPolarity.DownIsGood).trendTone)
        assertEq(StatsTone.None, StatsTileData(series = rising).trendTone)
    }

    fun testLayoutMerge() {
        val catalog = listOf("cost", "repos", "issues", "tokens", "peers").map { StatsTileCatalogEntry(it, it, "g", "", "") }
        val defaults = listOf(StatsTilePlacement("repos"), StatsTilePlacement("issues"))
        val layout = listOf(StatsTilePlacement("x", 2, 1))
        assertEq(layout, StatsLayout.fromJson(StatsLayout.toJson(layout)))
        assertEq(listOf(StatsTilePlacement("x", 2, 1)), StatsLayout.fromJson(listOf(mapOf("id" to "x", "w" to 9L, "h" to 0L), mapOf("w" to 1L))))
        val first = StatsLayout.merge(emptyList(), false, defaults, catalog, emptyList())
        assertEq(listOf("repos", "issues"), first.layout.map { it.id })
        expect(first.changed)
        assertEq(catalog.size, first.offered.size)
        val saved = listOf(StatsTilePlacement("cost", 2, 1), StatsTilePlacement("repos"))
        val grown = StatsLayout.merge(saved, true, defaults + StatsTilePlacement("tokens"), catalog, listOf("cost", "repos"))
        assertEq(listOf("cost", "repos", "issues", "tokens"), grown.layout.map { it.id })
        assertEq(2, grown.layout[0].width)
        expect(grown.changed)
        val again = StatsLayout.merge(grown.layout, true, defaults, catalog, grown.offered)
        assertEq(grown.layout, again.layout)
        expect(!again.changed)
        val removed = StatsLayout.merge(grown.layout.filter { it.id != "tokens" }, true, defaults + StatsTilePlacement("tokens"), catalog, grown.offered)
        expect("tokens" !in removed.layout.map { it.id })
        expect(!removed.changed)
        val preserved = grown.layout + StatsTilePlacement("from-a-newer-build")
        assertEq(grown.layout.size, StatsLayout.visible(preserved, catalog).size)
        assertEq(listOf("repos", "cost"), StatsLayout.move(saved, "repos", 0).map { it.id })
        assertEq(saved, StatsLayout.move(saved, "missing", 0))
    }

    fun testStoreKeepsTheBoardPerScopeAndResetRestoresDefaults() {
        val prefs = MemoryPrefStore()
        val store = StatsLayoutStore(prefs, "p1")
        val first = store.load()
        assertEq(StatsCatalog.defaults, first.layout)
        store.save(first.layout.drop(1), first.offered)
        assertEq(StatsCatalog.defaults.drop(1), store.load().layout)
        assertEq(StatsCatalog.defaults, StatsLayoutStore(prefs, "other").load().layout)
        store.reset()
        assertEq(StatsCatalog.defaults, store.load().layout)
    }

    private val statusJson: Map<String, Any?> = mapOf(
        "now" to 1000L,
        "systems" to listOf(
            mapOf("id" to "website", "label" to "Website", "status" to "operational", "uptime24hPct" to 99.9, "checks24h" to 1440L),
            mapOf("id" to "relay", "label" to "Relay", "status" to "down", "reason" to "timeout"),
            mapOf("id" to "mirror:mirror35", "status" to "down"),
            mapOf("id" to "later", "status" to "future"),
        ),
        "current" to mapOf("errors24h" to 12L, "onlineNodes" to 5L, "monitoringState" to null),
    )

    fun testStatusReport() {
        val report = StatusReport.fromJson(statusJson)
        assertEq(ServiceState.Down, report.overall)
        assertEq("1 system is down", report.headline)
        assertEq(listOf("relay"), report.down.map { it.id })
        assertEq(1, report.mirrorSystems.size)
        val withNodes = StatusReport.fromJson(statusJson + ("systems" to listOf(
            mapOf("id" to "website", "status" to "operational"),
            mapOf("id" to "core:n2", "status" to "operational"),
            mapOf("id" to "runner:fmnext-r1", "status" to "operational"),
            mapOf("id" to "mirror:m1", "status" to "down"),
        )))
        assertEq(listOf("website"), withNodes.serviceSystems.map { it.id })
        assertEq(listOf("core:n2", "runner:fmnext-r1"), withNodes.nodeSystems.map { it.id })
        near(99.9, report.websiteUptime24h)
        assertEq("timeout", report.systems[1].reason)
        assertEq("", report.current.monitoringState)
        assertEq(12, report.current.errors24h)
        assertEq(ConnectionLight.Degraded, ConnectionLight.of(report, true))
        assertEq(ConnectionLight.Offline, ConnectionLight.of(report, false))
        val healthy = StatusReport(0, listOf(StatusSystem("a", "A", ServiceState.Operational), StatusSystem("b", "B", ServiceState.Unknown)), StatusCurrent())
        assertEq(ServiceState.Operational, healthy.overall)
        assertEq(ConnectionLight.Live, ConnectionLight.of(healthy, true))
        assertEq("Monitoring is behind", healthy.copy(current = StatusCurrent(monitoringState = "stale")).headline)
        val tiles = StatsTiles.status(report)
        assertEq("99.90%", tiles["uptime"]!!.value)
        assertEq(StatsTone.Good, tiles["uptime"]!!.status)
        assertEq("1", tiles["incidents"]!!.value)
        assertEq("Relay", tiles["incidents"]!!.note)
        assertEq(StatsTone.Warning, tiles["errors"]!!.status)
        expect(!StatsTiles.status(report.copy(current = StatusCurrent())).getValue("errors").available)
    }

    fun testNodeDirectory() {
        val findings = NodeDiagnosticFinding.parse(
            listOf(
                mapOf("i" to "disk", "s" to 2L, "m" to "Disk 91% full"),
                mapOf("i" to "", "m" to "dropped"),
                mapOf("i" to "cpu", "s" to 9L, "m" to "Load high"),
                mapOf("i" to "x", "m" to "info"),
            ),
        )
        assertEq(listOf("cpu", "disk", "x"), findings.map { it.id })
        assertEq(NodeDiagnosticSeverity.Critical, findings[0].severity)
        assertEq(NodeDiagnosticSeverity.Critical, NodeDiagnosticFinding.worst(findings))
        assertEq(NodeDiagnosticSeverity.Ok, NodeDiagnosticFinding.worst(emptyList()))
        val me = NodeRow.fromMember(Member(id = "1", name = "phone", self = true, online = true, platform = "android", version = "1.4.0"))
        val warn = NodeRow(id = "2", name = "Box", owner = "fred", platform = "linux", online = false, diagnostics = findings, mirrors = 2)
        val calm = NodeRow(id = "3", name = "alpha", online = true)
        val merged = NodeDirectory.merge(listOf(calm, warn, me, calm.copy(id = "dup")), listOf("box", "zeta", ""))
        assertEq(listOf("phone", "Box", "alpha", "zeta"), merged.map { it.name })
        assertEq("relay:zeta", merged.last().id)
        assertEq("Linux · owner fred · 2 mirrors", warn.subtitle)
        assertEq("Android · v1.4.0", me.subtitle)
        expect(warn.accessibilityLabel.endsWith("2 health warnings"))
        assertEq(listOf("Box"), NodeDirectory.filter(merged, "LINUX").map { it.name })
        assertEq(merged, NodeDirectory.filter(merged, " "))
        val snapshot = NetworkSnapshot.fromJson(mapOf("repos" to 10L, "hosts" to 3L, "onlineNodes" to listOf(" a ", "", "b")))
        assertEq(listOf("a", "b"), snapshot.onlineNodes)
        assertEq(NetworkSection.Relays, NetworkSection.of("RELAYS"))
        assertEq(NetworkSection.Nodes, NetworkSection.of(null))
        val endpoints = EndpointRow.forRelay("wss://app.forkmesh.com/ws")
        assertEq("https://app.forkmesh.com/api", endpoints[0].url)
        assertEq("https://app.forkmesh.com/api/status", endpoints[3].url)
        assertEq("http://10.0.0.2:8787", EndpointRow.forRelay("ws://10.0.0.2:8787/ws")[2].url)
        assertEq(emptyList<EndpointRow>(), EndpointRow.forRelay(""))
    }

    fun testOnlineHistoryAndNetworkTiles() {
        val history = OnlineHistory.fromJson(
            mapOf("hours" to listOf(mapOf("hourTs" to 1L, "nodeMinutes" to 30L), mapOf("hourTs" to 2L, "nodeMinutes" to 90L))),
        )
        assertEq(2L, history.peakHour!!.hourTs)
        val tile = StatsTiles.history(history).getValue("nodeHours")
        assertEq("2h 0m", tile.value)
        expect(tile.available)
        expect(!StatsTiles.history(OnlineHistory(emptyList(), emptyList())).getValue("nodeHours").available)
        val net = StatsTiles.network(NetworkSnapshot(1284, 3, 0, emptyList()))
        assertEq("1,284", net.getValue("repos").value)
        assertEq(StatsTone.Warning, net.getValue("nodes").status)
    }

    fun testDesktopTiles() {
        val health = DesktopHealth(
            level = DesktopHealthLevel.Strained, cpuPercent = 87.4, memoryTotal = 1000.0, memoryAvailable = 50.0,
            agentsRunning = 2, agentsQueued = 3, stalls = 1,
        )
        val tiles = StatsTiles.desktop(health)
        assertEq("87%", tiles.getValue("desktopLoad").value)
        assertEq(StatsTone.Critical, tiles.getValue("desktopLoad").status)
        assertEq("95%", tiles.getValue("desktopMemory").value)
        assertEq(StatsTone.Critical, tiles.getValue("desktopMemory").status)
        assertEq("3 queued", tiles.getValue("agents").note)
        assertEq(StatsTone.Warning, tiles.getValue("stalls").status)
        expect(!StatsTiles.desktop(DesktopHealth()).getValue("desktopLoad").available)
        expect(!StatsTiles.desktop(DesktopHealth()).getValue("desktopMemory").available)
    }

    fun testBoardServerTilesFillWhileTheDesktopIsStillAnswering() {
        val transport = FakeTransport()
        transport.replies["/api/network/stats"] = mapOf("repos" to 3L, "hosts" to 1L, "onlineNodes" to listOf("a"))
        transport.replies["/api/status"] = statusJson
        val gate = java.util.concurrent.CountDownLatch(1)
        val model = StatsBoardModel(transport, StatsLayoutStore(MemoryPrefStore()), desktop = {
            gate.await()
            DesktopHealth(cpuPercent = 10.0, agentsRunning = 1)
        })
        val changes = java.util.concurrent.atomic.AtomicInteger()
        val reload = Thread { model.reload { changes.incrementAndGet() } }
        reload.start()
        val deadline = System.currentTimeMillis() + 5_000
        while ((model.data("repos") == null || model.isLoading("repos")) && System.currentTimeMillis() < deadline) Thread.sleep(5)
        assertEq("3", model.data("repos")!!.value)
        expect(model.isLoading("agents"))
        expect(model.data("agents") == null)
        expect(model.loading)
        gate.countDown()
        reload.join(5_000)
        assertEq("1", model.data("agents")!!.value)
        expect(model.pending.isEmpty())
        expect(!model.loading)
        expect(changes.get() >= 5)
        assertEq(
            "Your desktop took too long to answer. Refresh to try again.",
            DesktopStatsClient.explain(LocalControlException("timeout")),
        )
    }

    fun testBoardModelReloadsEveryPart() {
        val transport = FakeTransport()
        transport.replies["/api/network/stats"] = mapOf("repos" to 10L, "hosts" to 2L, "onlineNodes" to listOf("a"))
        transport.failures["/api/network/online-history"] = OrgApiException("http_500", "boom")
        transport.replies["/api/status"] = statusJson
        var clock = 5L
        val model = StatsBoardModel(transport, StatsLayoutStore(MemoryPrefStore()), desktop = { throw LocalControlException("not_paired") }) { clock }
        model.reload()
        assertEq("10", model.data("repos")!!.value)
        assertEq(listOf("activity"), model.failures)
        assertEq("Your desktop is not reachable right now.", model.desktopNote)
        expect(!model.data("agents")!!.available)
        assertEq(5L, model.updatedAtMs)
        clock = 9
        val paired = StatsBoardModel(transport, StatsLayoutStore(MemoryPrefStore()), desktop = { DesktopHealth(cpuPercent = 10.0, agentsRunning = 4) }) { clock }
        paired.reload()
        assertEq("4", paired.data("agents")!!.value)
        assertEq("", paired.desktopNote)
        val unpaired = StatsBoardModel(transport, StatsLayoutStore(MemoryPrefStore()))
        unpaired.reload()
        assertEq("Pair a desktop to see its load and agents.", unpaired.desktopNote)
        expect(unpaired.addable.map { it.id }.containsAll(listOf("desktopMemory", "stalls")))
        unpaired.add("stalls")
        expect(unpaired.layout.any { it.id == "stalls" })
        unpaired.add("bogus")
        expect(unpaired.layout.none { it.id == "bogus" })
        unpaired.toggleWide("nodes")
        assertEq(2, unpaired.layout.first { it.id == "nodes" }.width)
        unpaired.move("incidents", 0)
        assertEq("incidents", unpaired.layout.first().id)
        unpaired.remove("incidents")
        expect(unpaired.layout.none { it.id == "incidents" })
        unpaired.reset()
        assertEq(StatsCatalog.defaults, unpaired.layout)
    }

    private fun issue(number: Int, created: Long, vararg changes: Pair<Long, String>) = Issue(
        number = number,
        title = "i$number",
        status = changes.lastOrNull()?.second ?: "open",
        events = listOf(IssueEvent(type = "open", ts = created)) + changes.map { IssueEvent(type = "status", status = it.second, ts = it.first) },
    )

    fun testBurnupSeries() {
        val issues = listOf(issue(1, 100, 300L to "closed"), issue(2, 200), issue(3, 250, 260L to "closed", 280L to "open"), issue(4, 0))
        assertEq(100L, IssueBurnup.createdAt(issues[0]))
        assertEq("closed", IssueBurnup.status(issues[0], 300))
        assertEq("open", IssueBurnup.status(issues[0], 299))
        val series = IssueBurnup.series(issues, 400, 0, 4)
        assertEq(listOf(0L, 100L, 200L, 300L, 400L), series.map { it.timestampMs })
        assertEq(listOf(0, 1, 2, 2, 2), series.map { it.openCount })
        assertEq(listOf(0, 0, 0, 1, 1), series.map { it.closedCount })
        assertEq(100L, IssueBurnup.firstTimestamp(issues, 7))
        assertEq(7L, IssueBurnup.firstTimestamp(listOf(issue(9, 0)), 7))
    }

    fun testRepoInsightsSummary() {
        val week = 7L * 24 * 60 * 60 * 1000
        val now = 1_790_000_000_000L
        val currentWeek = now - now % week
        val commits = listOf(
            mapOf("author" to mapOf("name" to "Ann"), "date" to java.time.Instant.ofEpochMilli(currentWeek + 86_400_000).toString()),
            mapOf("author" to "Bob", "timestamp" to (now / 1000)),
            mapOf("authorName" to "Bob", "ts" to now - week),
            mapOf("author" to mapOf("email" to "c@x"), "date" to "not a date"),
            mapOf("committer" to "Old", "date" to (now - 30 * week).toString()),
        )
        val summary = RepoInsightsSummary.build(
            commits,
            listOf(issue(1, 1, 5L to "closed"), issue(2, 2)),
            listOf(PullRequest(1, "a", status = "open"), PullRequest(2, "b", status = "merged"), PullRequest(3, "c", status = "closed")),
            Repository("f", "r", language = "Kotlin", stars = 4, forks = 1, mirrors = 3),
            now,
        )
        assertEq(5, summary.commitsSeen)
        assertEq(RepoInsightsSummary.WEEKS, summary.weekly.size)
        assertEq(2, summary.weekly.last().commits)
        assertEq(1, summary.weekly[RepoInsightsSummary.WEEKS - 2].commits)
        assertEq(3, summary.commitsInWindow)
        assertEq(currentWeek, summary.weekly.last().weekStartMs)
        assertEq(listOf("Bob", "Ann", "c@x", "Old"), summary.contributors.map { it.name })
        assertEq(1, summary.openIssues)
        assertEq(1, summary.closedIssues)
        assertEq(Triple(1, 1, 1), Triple(summary.openPulls, summary.mergedPulls, summary.closedPulls))
        assertEq(13, summary.burnup.size)
        assertEq("Kotlin", summary.language)
        assertEq(3, summary.mirrors)
        assertEq(1_500_000_000_000L, RepoInsightsSummary.dateMs(mapOf("date" to 1_500_000_000L)))
        assertEq(1_500_000_000_123L, RepoInsightsSummary.dateMs(mapOf("date" to "1500000000123")))
        assertEq(1_600_000_000_500L, RepoInsightsSummary.dateMs(mapOf("date" to "2020-09-13T12:26:40.500Z")))
        assertEq(null, RepoInsightsSummary.dateMs(emptyMap()))
    }

    fun testDesktopStatsClient() {
        val channel = FakeStatsChannel()
        channel.replies[DesktopStatsMethod.TIMELINE] = mapOf(
            "days" to (1L..35L).map { mapOf("dayTs" to it, "errors" to it % 3, "warnings" to 1L, "info" to if (it == 20L) 100L else 0L) },
        )
        val client = DesktopStatsClient(channel)
        val timeline = client.timeline(90)
        assertEq(30L, channel.calls.last().second["days"])
        assertEq(30, timeline.days.size)
        assertEq(6L, timeline.days.first().dayTs)
        assertEq(20L, timeline.busiest!!.dayTs)
        assertEq(30, timeline.errorSeries.values.size)
        channel.replies[DesktopStatsMethod.QUALITY] = mapOf(
            "score" to 72L, "grade" to "C",
            "files" to listOf(
                mapOf("path" to "b.kt", "score" to 40L, "findings" to (1..12).map { "f$it" }),
                mapOf("path" to "a.kt", "score" to 40L),
                mapOf("path" to "c.kt", "score" to 90L),
            ),
        )
        val quality = client.quality("f/r")
        assertEq(listOf("a.kt", "b.kt", "c.kt"), quality.files.map { it.path })
        assertEq(10, quality.files[1].findings.size)
        assertEq(StatsTone.Warning, quality.tone)
        channel.replies[DesktopStatsMethod.RATCHET_PREPARE] = mapOf("summary" to "no token")
        assertFails<InsightsException> { client.prepareRatchet("f/r", true) }
        channel.replies[DesktopStatsMethod.RATCHET_PREPARE] = mapOf("token" to "t", "summary" to "Turn on")
        channel.replies[DesktopStatsMethod.RATCHET_RUN] = mapOf("commits" to 3L, "ratchet" to mapOf("enabled" to true, "note" to "on"))
        val confirmation = client.prepareRatchet("f/r", true)
        val stats = client.runRatchet(confirmation)
        expect(stats.ratchetEnabled)
        assertEq("t", channel.calls.last().second["token"])
        channel.replies[DesktopStatsMethod.SENTRY] = mapOf("issues" to listOf(mapOf("id" to "1", "level" to "fatal", "count" to 3L)))
        assertEq(StatsTone.Critical, client.sentry().single().tone)
        channel.failure = "unknown_method"
        assertEq(DesktopLoad.Unsupported, DesktopLoad.attempt { client.sentry() })
        assertEq("Your desktop does not offer this yet. Update ForkMesh on the desktop.", DesktopStatsClient.explain(LocalControlException("unsupported")))
        assertEq("The confirmation expired. Try again.", DesktopStatsClient.explain(LocalControlException("confirmation_expired")))
    }

    fun testInsightsErrorsAndQueries() {
        expect(InsightsException("http_403").needsAdmin)
        assertEq("This needs an administrator account.", InsightsException("not_admin").message)
        assertEq("some code", InsightsException("some_code").message)
        assertEq("http_404", InsightsException.from(OrgApiException("http_404", "x")).code)
        assertEq("boom", InsightsException.from(IllegalStateException("boom")).message)
        assertEq("/a?x=1%202&z=%2F", InsightsQuery.path("/a", listOf("x" to "1 2", "y" to "", "z" to "/")))
        assertEq("/a", InsightsQuery.path("/a", listOf("y" to "")))
        val current = InsightsLoad.Loaded(3)
        assertEq(current, InsightsLoad.attempt(current) { throw OrgApiException("http_500", "x") })
        val failed = InsightsLoad.attempt<Int>(InsightsLoad.Idle) { throw OrgApiException("http_403", "x") }
        expect(failed.errorOrNull!!.needsAdmin)
    }

    fun testExitReports() {
        assertEq(null, ExitReport.fromExit(1, 10, 1, "", "", "", ""))
        val crash = ExitReport.fromExit(ExitKind.REASON_CRASH, 1000, 42, "", "\n  java.lang.IllegalStateException: boom\n at x", "1.4.0", "Android 15")!!
        assertEq("1000-42", crash.id)
        assertEq("java.lang.IllegalStateException: boom", crash.summary)
        expect(crash.stallLogLine.startsWith("[exitinfo] crash java.lang"))
        expect(crash.agentPrompt().contains("native/android"))
        val anr = ExitReport.fromExit(ExitKind.REASON_ANR, 2000, 7, "Input dispatching timed out", "", "", "")!!
        assertEq(ExitKind.Anr, anr.kind)
        assertEq("Input dispatching timed out", anr.summary)
        assertEq(ExitKind.LowMemory.title, ExitReport.fromExit(ExitKind.REASON_LOW_MEMORY, 3, 1, "", "", "", "")!!.summary)
        val long = ExitReport.create("x", ExitKind.Crash, 1, "v".repeat(99), "", "s".repeat(999), "d".repeat(99_999))
        assertEq(40, long.appVersion.length)
        assertEq(ExitReport.SUMMARY_LIMIT, long.summary.length)
        assertEq(ExitReport.DETAIL_LIMIT, long.detail.length)
        val dir = Files.createTempDirectory("exit-reports").toFile()
        try {
            val store = ExitReportStore { File(dir, "diagnostics") }
            assertEq(emptyList<ExitReport>(), store.load())
            store.append(listOf(crash, anr))
            store.append(listOf(crash))
            assertEq(listOf("2000-7", "1000-42"), store.load().map { it.id })
            assertEq(2000L, store.newestTimestamp())
            store.append((1..30).map { ExitReport.create("r$it", ExitKind.Crash, 10_000L + it, summary = "s") })
            assertEq(ExitReportStore.MAX_REPORTS, store.load().size)
            assertEq("r30", store.load().first().id)
            assertEq(listOf(ExitKind.Crash to 20), ExitReportStore.counts(store.load()))
            store.remove("r30")
            expect(store.load().none { it.id == "r30" })
            store.clear()
            assertEq(0, store.load().size)
            File(File(dir, "diagnostics"), ExitReportStore.FILE_NAME).writeText("{not json")
            assertEq(0, store.load().size)
        } finally {
            dir.deleteRecursively()
        }
    }

    fun testAgentSessionLookup() {
        fun row(id: String, badge: AgentTransportBadge, local: Int = 0, cloud: String = "") = UnifiedAgentRow(
            id = id, badge = badge, title = "", repoOwner = "f", repoName = "r", node = "", status = "", statusLabel = "",
            isActive = false, localId = local, cloudSessionId = cloud,
        )
        val desktop = row("desktop:12", AgentTransportBadge.entries.first { it != AgentTransportBadge.Node }, local = 12)
        val node = row("node:abc", AgentTransportBadge.Node, cloud = "abc")
        val rows = listOf(desktop, node)
        assertEq(desktop, AgentSessionLookup.find(rows, "desktop:12"))
        assertEq(desktop, AgentSessionLookup.find(rows, "12"))
        assertEq(node, AgentSessionLookup.find(rows, "abc"))
        assertEq(node, AgentSessionLookup.find(rows, "node:abc"))
        assertEq(null, AgentSessionLookup.find(rows, " "))
        assertEq(null, AgentSessionLookup.find(rows, "99"))
        assertEq(12, AgentSessionLookup.desktopSessionNumber("desktop:12"))
        assertEq(null, AgentSessionLookup.desktopSessionNumber("abc"))
    }
}
