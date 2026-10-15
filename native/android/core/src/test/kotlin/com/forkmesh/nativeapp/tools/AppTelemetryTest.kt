package com.forkmesh.nativeapp.tools

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import java.io.IOException
import java.nio.file.Files
import java.time.LocalDateTime
import java.time.ZoneId
import kotlin.math.abs

class AppTelemetryTest {
    private fun at(hour: Int, minute: Int = 0, second: Int = 0): Long =
        LocalDateTime.of(2026, 9, 14, hour, minute, second).atZone(ZoneId.systemDefault()).toInstant().toEpochMilli()

    fun testClassifiesPlatformLinesIntoFamilies() {
        assertEq(LogCategory.Stall, LogCategory.classify("[stall] frame frame 412ms"))
        assertEq(LogCategory.Perf, LogCategory.classify("[perf] operation api.GET 900ms error=x"))
        assertEq(LogCategory.Error, LogCategory.classify("Auth logout revocation failed: boom"))
        assertEq(LogCategory.Push, LogCategory.classify("[push] registered"))
        assertEq(LogCategory.Auth, LogCategory.classify("UserPresence: local_auth unavailable"))
        assertEq(LogCategory.Sync, LogCategory.classify("Node sync drained 3 items"))
        assertEq(LogCategory.Info, LogCategory.classify("hello"))
    }

    fun testKeepsTheNewestLinesAndCountsEveryLineEverAdded() {
        var clock = at(12)
        val log = AppLog(capacity = 3, now = { clock })
        for (i in 0 until 5) {
            clock += 1000
            log.add(if (i % 2 == 0) LogCategory.Net else LogCategory.Stall, "line $i")
        }
        assertEq(listOf("line 4", "line 3", "line 2"), log.entries().map { it.message })
        assertEq(5, log.totalLines)
        assertEq(3, log.totalFor(LogCategory.Net))
        assertEq(2, log.totalFor(LogCategory.Stall))
        assertEq(listOf("line 3"), log.entries(categories = setOf(LogCategory.Stall)).map { it.message })
        val since = at(12, 0, 4)
        assertEq(listOf("line 4", "line 3"), log.entries(sinceMs = since).map { it.message })
        assertEq(1, log.countsSince(since)[LogCategory.Stall.ordinal])
    }

    fun testAnEntryPrintsAsTimestampFamilyAndMessage() {
        val log = AppLog(now = { at(12, 0, 1) })
        log.add(LogCategory.Net, "GET forkmesh.test/api/repos 200 in 40ms")
        assertEq("2026-09-14T12:00:01.000 NET GET forkmesh.test/api/repos 200 in 40ms", log.entries().single().toString())
    }

    fun testAddLineTrimsAndSkipsBlankLines() {
        val log = AppLog()
        log.addLine("   ")
        log.addLine("Fetch failed: boom  \n")
        assertEq(1, log.totalLines)
        assertEq("Fetch failed: boom", log.entries().single().message)
        assertEq(LogCategory.Error, log.entries().single().category)
    }

    fun testRelayLinesMirrorAsRelayEventsOrErrors() {
        val log = AppLog()
        log.addRelayLine("Connected to wss://relay.test/ws")
        log.addRelayLine("Connect Failed: timeout")
        assertEq(1, log.totalFor(LogCategory.Relay))
        assertEq(1, log.totalFor(LogCategory.Error))
    }

    fun testCaptureProcessLogsRoutesPrintsAndUncaughtErrorsThenRestores() {
        val printed = mutableListOf<String?>()
        val previousSink = DebugPrint.sink
        val previousHandler = Thread.getDefaultUncaughtExceptionHandler()
        DebugPrint.sink = { printed.add(it) }
        try {
            val log = AppLog()
            val restore = log.captureProcessLogs()
            debugPrint("[stall] frame frame 300ms")
            assertEq(1, log.totalFor(LogCategory.Stall))
            assertEq(listOf<String?>("[stall] frame frame 300ms"), printed)
            Thread.getDefaultUncaughtExceptionHandler()!!.uncaughtException(Thread.currentThread(), IOException("boom"))
            assertEq("Uncaught: java.io.IOException: boom", log.entries(categories = setOf(LogCategory.Error)).single().message)
            restore()
            debugPrint("after restore")
            assertEq(2, log.totalLines)
            assertEq(2, printed.size)
            assertEq(previousHandler, Thread.getDefaultUncaughtExceptionHandler())
        } finally {
            DebugPrint.sink = previousSink
            Thread.setDefaultUncaughtExceptionHandler(previousHandler)
        }
    }

    fun testRollsSecondsIntoMinuteAveragesAndPeaks() {
        val history = MetricHistory()
        for (i in 0 until 120) history.add(if (i < 60) 2.0 else if (i == 90) 50.0 else 10.0)
        assertEq(listOf(10.0, 10.0, 10.0), history.lastSeconds(3).toList())
        assertEq(2.0, history.lastMinutes(2, peak = false)[0])
        assertEq(50.0, history.lastMinutes(2, peak = true)[1])
        expect(abs(history.lastMinutes(2, peak = false)[1] - 10.67) < 0.01)
    }

    fun testALongGapFillsWholeMinutesWithoutWalkingEverySecond() {
        val history = MetricHistory(secondCapacity = 120, minuteCapacity = 10)
        repeat(30) { history.add(1.0) }
        history.addGap(24 * 3600)
        history.add(7.0)
        assertEq(7.0, history.latest)
        expect(history.lastSeconds(2).first().isNaN())
        val minutes = history.lastMinutes(10, peak = false)
        assertEq(10, minutes.size)
        expect(minutes.all { it.isNaN() })
    }

    fun testAnEmptyHistoryHasNoLatestValue() {
        expect(MetricHistory().latest.isNaN())
        assertEq(0, MetricHistory().lastSeconds(30).size)
    }

    fun testSamplesFramesLagRequestsRelayFramesLogsAndStalls() {
        var clock = at(12)
        val log = AppLog(now = { clock })
        val monitor = PerformanceMonitorService(logSink = log::addLine, now = { clock })
        var relayFrames = 0L
        val telemetry = AppTelemetry(
            log = log,
            monitor = monitor,
            relayFrames = { relayFrames },
            now = { clock },
            rssMegabytes = { 123.0 },
        )

        telemetry.recordFrame(12_000)
        telemetry.recordFrame(40_000)
        relayFrames = 5
        telemetry.recordRequestFinished("GET", "https://forkmesh.test/api/repos?token=s", 200, 40, bytesIn = 512)
        telemetry.recordRequestFinished("GET", "https://forkmesh.test/missing", 404, 30, bytesIn = 20)
        monitor.recordSlowEvent(PerformanceEventKind.Frame, "frame", 300_000)

        clock += 1000
        telemetry.sampleNow()

        assertEq(2.0, telemetry.latest(TelemetryMetric.Fps))
        assertEq(40.0, telemetry.latest(TelemetryMetric.Frame))
        assertEq(123.0, telemetry.latest(TelemetryMetric.Mem))
        assertEq(2.0, telemetry.latest(TelemetryMetric.Net))
        assertEq(5.0, telemetry.latest(TelemetryMetric.Relay))
        assertEq(1.0, telemetry.latest(TelemetryMetric.Stall))
        assertEq(3.0, telemetry.latest(TelemetryMetric.Log))
        assertEq(1, log.totalFor(LogCategory.Stall))
        assertEq(1, log.totalFor(LogCategory.Error))
        assertEq(532L, telemetry.bytesReceived)
        val net = log.entries(categories = setOf(LogCategory.Net)).single().message
        expect(net.contains("forkmesh.test/api/repos"), net)
        expect(!net.contains("token"), net)

        clock += 4000
        telemetry.sampleNow()
        assertEq(0.0, telemetry.latest(TelemetryMetric.Stall))
        val recent = telemetry.series(TelemetryMetric.Fps, TelemetryRange.M1)
        assertEq(5, recent.size)
        assertEq(3, recent.count { it.isNaN() })
        val summary = telemetry.summary(TelemetryMetric.Net, TelemetryRange.M1)
        assertEq(2.0, summary.total)
        assertEq(2, summary.samples)
    }

    fun testHeartbeatLagFoldsIntoTheWorstOfTheSecond() {
        var clock = at(12)
        val telemetry = AppTelemetry(AppLog(), now = { clock }, rssMegabytes = { Double.NaN })
        telemetry.recordLoopLag(3_000)
        telemetry.recordLoopLag(80_500)
        clock += 1000
        telemetry.sampleNow()
        assertEq(80.5, telemetry.latest(TelemetryMetric.Loop))
        expect(telemetry.latest(TelemetryMetric.Mem).isNaN())
        assertEq("–", TelemetryMetric.Mem.format(telemetry.latest(TelemetryMetric.Mem)))
    }

    fun testFailedRequestsAreErrorsNamedByHostAndPath() {
        val log = AppLog()
        val telemetry = AppTelemetry(log)
        telemetry.recordRequestStarted()
        assertEq(1, telemetry.requestsInFlight)
        telemetry.recordRequestFinished("POST", "https://forkmesh.test/api/bug-reports?ticket=t", null, 12, IOException("no route to host"))
        assertEq(0, telemetry.requestsInFlight)
        assertEq(
            "POST forkmesh.test/api/bug-reports failed after 12ms: java.io.IOException: no route to host",
            log.entries().single().message,
        )
        telemetry.recordRequestFinished("GET", "https://forkmesh.test/api/x", 503, 7)
        assertEq("GET forkmesh.test/api/x HTTP 503 in 7ms", log.entries().first().message)
    }

    fun testTheStallMonitorReportsEveryHeartbeatLagAndCountsStalls() {
        val monitor = PerformanceMonitorService(logSink = {})
        monitor.recordSlowEvent(PerformanceEventKind.Operation, "slow", 1_000_000)
        monitor.recordSlowEvent(PerformanceEventKind.EventLoop, "ui-isolate blocked", 250_000)
        assertEq(1, monitor.stallCount)
    }

    fun testProcessMemoryReadsResidentSetSize() {
        val dir = Files.createTempDirectory("fm_rss").toFile()
        val status = File(dir, "status")
        try {
            status.writeText("Name:\tforkmesh\nVmRSS:\t  204800 kB\nThreads:\t9\n")
            assertEq(200.0, ProcessMemory.rssMegabytes(status))
            expect(ProcessMemory.rssMegabytes(File(dir, "missing")).isNaN())
        } finally {
            status.delete()
            dir.delete()
        }
    }

    fun testDownsampleKeepsPeaksOrAveragesFromTheNewestEnd() {
        assertEq(listOf(1.0, 5.0, 9.0), downsample(doubleArrayOf(1.0, 5.0, 2.0, 2.0, 9.0), 2, peak = true).toList())
        assertEq(listOf(1.0, 3.5, 5.5), downsample(doubleArrayOf(1.0, 5.0, 2.0, 2.0, 9.0), 2, peak = false).toList())
        expect(downsample(doubleArrayOf(Double.NaN, Double.NaN, 4.0), 2, peak = true)[0].isNaN())
    }

    fun testMetricValuesReadInTheirUnit() {
        assertEq("88MB", TelemetryMetric.Mem.format(88.4))
        assertEq("40ms", TelemetryMetric.Frame.format(40.4))
        assertEq("13ms", TelemetryMetric.Loop.format(12.5))
        assertEq("1.5k", TelemetryMetric.Net.format(1500.0))
        assertEq("2.5", TelemetryMetric.Net.format(2.5))
        assertEq("60", TelemetryMetric.Fps.format(60.0))
        assertEq("–", TelemetryMetric.Fps.format(Double.NaN))
    }

    fun testUptimeAndCountsStayCompact() {
        assertEq("42s", formatUptime(42_000))
        assertEq("3m7s", formatUptime(187_000))
        assertEq("2h5m", formatUptime(7_500_000))
        assertEq("1d3h", formatUptime(97_200_000))
        assertEq("999", compactCount(999))
        assertEq("1.2k", compactCount(1234))
        assertEq("1.3k", compactCount(1250))
        assertEq("12k", compactCount(12_345))
    }

    fun testChartGeometryGrowsInFromTheRightAndBreaksOnGaps() {
        assertEq(60.0, TelemetryChart.scaleTop(doubleArrayOf(1.0, Double.NaN, 20.0), 60.0))
        assertEq(110.00000000000001, TelemetryChart.scaleTop(doubleArrayOf(100.0), 60.0))
        assertEq(listOf(1..2, 4..4), TelemetryChart.runs(doubleArrayOf(Double.NaN, 1.0, 2.0, Double.NaN, 3.0)))
        val step = TelemetryChart.step(290.0, 3, 30)
        assertEq(10.0, step)
        assertEq(290.0, TelemetryChart.xAt(2, 3, 290.0, step))
        assertEq(270.0, TelemetryChart.xAt(0, 3, 290.0, step))
        assertEq(2, TelemetryChart.indexAt(290.0, 290.0, 3, 30))
        assertEq(1, TelemetryChart.indexAt(281.0, 290.0, 3, 30))
        assertEq(0, TelemetryChart.indexAt(0.0, 290.0, 3, 30))
        assertEq(null, TelemetryChart.indexAt(10.0, 290.0, 0, 30))
        assertEq(0, TelemetryChart.indexAt(10.0, 290.0, 1, 1))
        assertEq(10.0, TelemetryChart.yAt(-5.0, 10.0, 11.0, 1.0))
        assertEq(10.0, TelemetryChart.yAt(0.0, 10.0, 11.0, 1.0))
        assertEq(1.0, TelemetryChart.yAt(10.0, 10.0, 11.0, 1.0))
        assertEq(1.0, TelemetryChart.yAt(25.0, 10.0, 11.0, 1.0))
    }

    fun testLogcatLinesKeepWhatTheProcessLoggedButNotOwnPrints() {
        assertEq("System.out: hello", LogcatLines.message("I/System.out: hello", "ForkMeshPrint"))
        assertEq("chatty: uid=10123 expire 3 lines", LogcatLines.message("I/chatty  : uid=10123 expire 3 lines", "ForkMeshPrint"))
        assertEq(
            "AndroidRuntime: FATAL EXCEPTION: main",
            LogcatLines.message("E/AndroidRuntime: FATAL EXCEPTION: main", "ForkMeshPrint"),
        )
        assertEq(null, LogcatLines.message("I/ForkMeshPrint: [stall] frame frame 300ms", "ForkMeshPrint"))
        assertEq(null, LogcatLines.message("--------- beginning of main", "ForkMeshPrint"))
        assertEq(null, LogcatLines.message("W/Tag: ", "ForkMeshPrint"))
        assertEq(null, LogcatLines.message("logcat: unknown option", "ForkMeshPrint"))
        assertEq(LogCategory.Error, LogCategory.classify(LogcatLines.message("E/AndroidRuntime: FATAL EXCEPTION: main", "x")!!))
    }

    private class Seeded(val telemetry: AppTelemetry, val log: AppLog, val clock: () -> Long, val advance: (Long) -> Unit)

    private fun seeded(): Seeded {
        var clock = at(12)
        val log = AppLog(now = { clock })
        val telemetry = AppTelemetry(log, now = { clock }, rssMegabytes = { 88.0 })
        log.add(LogCategory.Stall, "[stall] frame frame 412ms")
        log.add(LogCategory.Net, "GET forkmesh.test/api/repos 200 in 40ms")
        log.add(LogCategory.Net, "GET forkmesh.test/api/orgs 200 in 30ms")
        repeat(3) {
            clock += 1000
            telemetry.recordFrame(16_000)
            telemetry.sampleNow()
        }
        return Seeded(telemetry, log, { clock }, { clock += it })
    }

    fun testTheStripShowsLiveSquaresAndLogCounts() {
        val s = seeded()
        val mem = TelemetryMetric.Mem
        assertEq("MEM 88MB", "${mem.label} ${mem.format(s.telemetry.latest(mem))}")
        assertEq("2", compactCount(s.log.totalFor(LogCategory.Net)))
        assertEq(3, s.telemetry.lastSeconds(TelemetryMetric.Fps, TelemetryStripLayout.SPARK_SECONDS).size)
        var notified = 0
        val remove = s.telemetry.addListener { notified++ }
        s.log.add(LogCategory.Net, "GET forkmesh.test/x 200 in 1ms")
        assertEq(0, notified)
        s.advance(1000)
        s.telemetry.sampleNow()
        assertEq(1, notified)
        assertEq("3", compactCount(s.log.totalFor(LogCategory.Net)))
        remove()
    }

    fun testDiagnosticsOpensOnTheChosenLogFamily() {
        val s = seeded()
        val stallOnly = TelemetryScreenText.visibleEntries(s.log, setOf(LogCategory.Stall), TelemetryRange.M5, s.clock(), null)
        assertEq(listOf("[stall] frame frame 412ms"), stallOnly.map { it.message })
        val all = TelemetryScreenText.visibleEntries(s.log, emptySet(), TelemetryRange.M5, s.clock(), null)
        expect(all.any { it.message.contains("api/repos") })
        assertEq("All 3", TelemetryScreenText.allChipLabel(TelemetryScreenText.filterCounts(s.log, TelemetryRange.M5, s.clock(), null)))

        s.advance(3 * 60_000)
        assertEq(0, TelemetryScreenText.visibleEntries(s.log, emptySet(), TelemetryRange.M1, s.clock(), null).size)
        assertEq("Nothing logged in the last 1m", TelemetryScreenText.emptyLogs(TelemetryRange.M1))
    }

    fun testPausedLogsStopAtThePause() {
        val s = seeded()
        val pausedAt = s.clock()
        s.advance(1000)
        s.log.add(LogCategory.Net, "GET forkmesh.test/late 200 in 1ms")
        val visible = TelemetryScreenText.visibleEntries(s.log, emptySet(), TelemetryRange.M5, s.clock(), pausedAt)
        assertEq(3, visible.size)
        assertEq(
            "GET forkmesh.test/api/orgs 200 in 30ms",
            visible.first().message,
        )
        val copied = TelemetryScreenText.copyText(visible).lines()
        expect(copied.first().endsWith("STALL [stall] frame frame 412ms"), copied.first())
        assertEq("Copied 3 log lines", TelemetryScreenText.copiedLines(visible.size))
    }

    fun testLogsCaptionNamesTheFamilyOrTheRange() {
        val s = seeded()
        assertEq(
            "UI stalls: frames, event-loop blocks, named slow sections. 1 since launch. Also written to /data/files/diagnostics/stalls.log",
            TelemetryScreenText.logsCaption(s.log, setOf(LogCategory.Stall), 1, TelemetryRange.M5, "/data/files/diagnostics/stalls.log"),
        )
        assertEq(
            "HTTP requests to ForkMesh services. 2 since launch.",
            TelemetryScreenText.logsCaption(s.log, setOf(LogCategory.Net), 2, TelemetryRange.M5, "/x"),
        )
        assertEq("3 lines in the last 5m · 3 since launch", TelemetryScreenText.logsCaption(s.log, emptySet(), 3, TelemetryRange.M5, null))
    }

    fun testStatsDrawsASquarePerMetricAndFocusesOne() {
        val s = seeded()
        assertEq(2, TelemetryScreenText.statsColumns(328f))
        assertEq(3, TelemetryScreenText.statsColumns(480f))
        assertEq(4, TelemetryScreenText.statsColumns(720f))
        assertEq("Up 3s · sampled every second while the app is open · history since launch", TelemetryScreenText.statsHeader(s.telemetry.uptimeMs))
        for (range in TelemetryRange.entries) {
            for (metric in TelemetryMetric.entries) {
                val card = MetricCardModel.of(s.telemetry, metric, range)
                expect(card.points.size <= MetricCardModel.MAX_POINTS, "$metric $range")
                expect(card.slots <= MetricCardModel.MAX_POINTS, "$metric $range")
            }
        }
        val day = MetricCardModel.of(s.telemetry, TelemetryMetric.Mem, TelemetryRange.H24)
        assertEq(180, day.slots)
        assertEq(480, day.secondsPerPoint)
        assertEq("now", day.whenText(null))
        val fps = MetricCardModel.of(s.telemetry, TelemetryMetric.Fps, TelemetryRange.M5)
        assertEq(150, fps.slots)
        assertEq(2, fps.points.size)
        assertEq("1", fps.valueText(null))
        assertEq("2s ago", fps.whenText(0))
        assertEq("now", fps.whenText(5))
        assertEq("min 1 · avg 1 · max 1", fps.footer)
        assertEq("total 0 · peak 0/s", MetricCardModel.of(s.telemetry, TelemetryMetric.Net, TelemetryRange.M5).footer)
    }

    fun testAgoLabelsReadLikeTheDesktop() {
        assertEq("now", agoLabel(0))
        assertEq("45s ago", agoLabel(45))
        assertEq("2m ago", agoLabel(120))
        assertEq("1h5m ago", agoLabel(3900))
    }

    fun testHealthMatchesTheIosLimits() {
        assertEq(TelemetryHealth.Good, TelemetryMetric.Fps.health(doubleArrayOf(60.0, 58.0)))
        assertEq(TelemetryHealth.Warning, TelemetryMetric.Fps.health(doubleArrayOf(60.0, 30.0)))
        assertEq(TelemetryHealth.Bad, TelemetryMetric.Fps.health(doubleArrayOf(Double.NaN, 20.0)))
        assertEq(TelemetryHealth.Good, TelemetryMetric.Frame.health(doubleArrayOf(16.0, 49.0)))
        assertEq(TelemetryHealth.Warning, TelemetryMetric.Frame.health(doubleArrayOf(16.0, 50.0)))
        assertEq(TelemetryHealth.Bad, TelemetryMetric.Stall.health(doubleArrayOf(0.0, 1.0)))
        assertEq(null, TelemetryMetric.Mem.health(doubleArrayOf(Double.NaN)))
        assertEq(null, TelemetryMetric.Net.health(DoubleArray(0)))
    }
}
