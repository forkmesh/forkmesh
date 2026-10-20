package com.forkmesh.nativeapp.tools

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import java.io.IOException
import java.nio.file.Files
import java.time.LocalDateTime
import java.time.ZoneId
import java.util.concurrent.Callable
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.ScheduledThreadPoolExecutor
import java.util.concurrent.TimeUnit

class PerformanceMonitorTest {
    private fun at(year: Int, month: Int, day: Int, hour: Int, minute: Int, second: Int): Long =
        LocalDateTime.of(year, month, day, hour, minute, second).atZone(ZoneId.systemDefault()).toInstant().toEpochMilli()

    private class TestLoop : AutoCloseable {
        private val executor = ScheduledThreadPoolExecutor(1)

        val scheduler = HeartbeatScheduler { delayMs, action ->
            val future = executor.schedule(Runnable { action() }, delayMs, TimeUnit.MILLISECONDS)
            val cancel: () -> Unit = { future.cancel(false) }
            cancel
        }

        fun <T> run(block: () -> T): T = executor.submit(Callable { block() }).get()

        override fun close() {
            executor.shutdownNow()
        }
    }

    private fun busyFor(millis: Long) {
        val started = System.nanoTime()
        while (System.nanoTime() - started < millis * 1_000_000) {
        }
    }

    fun testStallsNameTheScreenThatWasShowingAndOperationsDoNot() {
        val printed = mutableListOf<String>()
        val monitor = PerformanceMonitorService(logSink = { printed.add(it) }, now = { 1000L })
        var screen = "Code > forkmesh/forkmesh"
        monitor.screenName = { screen }
        monitor.recordSlowEvent(PerformanceEventKind.EventLoop, "ui-isolate blocked", 324_000)
        screen = "Inbox > Shortcuts"
        monitor.recordFrameDelay(buildMicros = 354_000, rasterMicros = 0)
        monitor.recordSlowEvent(PerformanceEventKind.Operation, "api.GET /api/tasks", 1_000_000)
        monitor.recordSlowEvent(PerformanceEventKind.Sync, "named", 250_000, mapOf("screen" to "kept"))
        screen = ""
        monitor.recordSlowEvent(PerformanceEventKind.EventLoop, "blank", 300_000)
        monitor.screenName = { error("gone") }
        monitor.recordSlowEvent(PerformanceEventKind.EventLoop, "thrown", 300_000)

        assertEq(listOf(null, null, "kept", null, "Inbox > Shortcuts", "Code > forkmesh/forkmesh"), monitor.events.map { it.details["screen"] })
        expect(printed.any { it.startsWith("[stall] frame frame 354ms") && it.contains("screen=\"Inbox > Shortcuts\"") }, printed.toString())
    }

    fun testTrackLogsOnlyOperationsOverTheThreshold() {
        val printed = mutableListOf<String>()
        val monitor = PerformanceMonitorService(
            thresholdMs = 500,
            logSink = { printed.add(it) },
            now = { 1000L },
            elapsedMicrosForTest = { 650_000L },
        )

        val result = monitor.track("repos.load", mapOf("screen" to "Code")) { 42 }

        assertEq(42, result)
        assertEq(1, monitor.events.size)
        val event = monitor.events.single()
        assertEq("repos.load", event.name)
        assertEq(PerformanceEventKind.Operation, event.kind)
        assertEq(650_000L, event.durationMicros)
        assertEq("Code", event.details["screen"])
        expect(printed.single().contains("repos.load"))
        expect(printed.single().contains("650ms"))
        assertEq("[perf] operation repos.load 650ms screen=Code", printed.single())
    }

    fun testTrackIgnoresFastOperationsAndDisabledMonitors() {
        val enabledMonitor = PerformanceMonitorService(thresholdMs = 500, elapsedMicrosForTest = { 120_000L }, logSink = {})
        val disabledMonitor = PerformanceMonitorService(enabled = false, thresholdMs = 500, elapsedMicrosForTest = { 2_000_000L })

        enabledMonitor.track("fast") {}
        disabledMonitor.track("disabled") {}

        assertEq(0, enabledMonitor.events.size)
        assertEq(0, disabledMonitor.events.size)
    }

    fun testAFailedOperationRecordsItsErrorAndStillThrows() {
        val monitor = PerformanceMonitorService(elapsedMicrosForTest = { 900_000L }, logSink = {})
        assertFails<IOException> { monitor.track("api.GET /api/repositories") { throw IOException("HTTP 400") } }
        val event = monitor.events.single()
        assertEq("java.io.IOException: HTTP 400", event.details["error"])
        expect(event.stackTrace.isNotEmpty())
    }

    fun testRecordsSlowFrameTimingWithUsefulDebugDetails() {
        val monitor = PerformanceMonitorService(thresholdMs = 500, logSink = {})

        monitor.recordFrameDelay(buildMicros = 320_000, rasterMicros = 260_000)

        assertEq(1, monitor.events.size)
        val event = monitor.events.single()
        assertEq(PerformanceEventKind.Frame, event.kind)
        assertEq("frame", event.name)
        assertEq(580_000L, event.durationMicros)
        assertEq(320L, event.details["buildMs"])
        assertEq(260L, event.details["rasterMs"])
        expect(!event.details.containsKey("vsyncMs"))
    }

    fun testHeartbeatRecordsABlockedMainThreadWithAttribution() {
        TestLoop().use { loop ->
            val printed = CopyOnWriteArrayList<String>()
            val monitor = PerformanceMonitorService(
                stallThresholdMs = 120,
                heartbeatIntervalMs = 20,
                logSink = { printed.add(it) },
                scheduler = loop.scheduler,
            )
            val lags = CopyOnWriteArrayList<Long>()
            monitor.onHeartbeatLag = { lags.add(it) }
            loop.run { monitor.startHeartbeat() }
            Thread.sleep(50)

            loop.run {
                monitor.track("api.GET /api/repositories") {}
                busyFor(250)
            }
            Thread.sleep(60)

            val stall = monitor.events.first { it.kind == PerformanceEventKind.EventLoop }
            expect(stall.isStall)
            expect(stall.durationMs >= 120, "lag ${stall.durationMs}ms")
            assertEq("api.GET /api/repositories", stall.details["finishedDuring"])
            expect(printed.last().startsWith("[stall] eventLoop ui-isolate blocked"), printed.last())
            expect(lags.isNotEmpty() && lags.all { it >= 0 })
            expect(monitor.heartbeatRunning)
            loop.run { monitor.dispose() }
            expect(!monitor.heartbeatRunning)
        }
    }

    fun testTimeSyncNamesTheStallAndTheHeartbeatDoesNotRepeatIt() {
        TestLoop().use { loop ->
            val monitor = PerformanceMonitorService(
                stallThresholdMs = 120,
                heartbeatIntervalMs = 20,
                logSink = {},
                scheduler = loop.scheduler,
            )
            loop.run { monitor.startHeartbeat() }
            Thread.sleep(50)

            val value = loop.run {
                monitor.timeSync("chat.decode") {
                    busyFor(200)
                    7
                }
            }
            Thread.sleep(60)
            loop.run { monitor.dispose() }

            assertEq(7, value)
            assertEq(1, monitor.events.size)
            assertEq(PerformanceEventKind.Sync, monitor.events.single().kind)
            assertEq("chat.decode", monitor.events.single().name)
        }
    }

    fun testAStoppedHeartbeatNeverFires() {
        TestLoop().use { loop ->
            val lags = CopyOnWriteArrayList<Long>()
            val monitor = PerformanceMonitorService(heartbeatIntervalMs = 20, logSink = {}, scheduler = loop.scheduler)
            monitor.onHeartbeatLag = { lags.add(it) }
            loop.run {
                monitor.startHeartbeat()
                monitor.stopHeartbeat()
            }
            Thread.sleep(60)
            assertEq(0, lags.size)
        }
    }

    fun testStallsAreAppendedToDiagnosticsStallsLog() {
        val root = Files.createTempDirectory("stall_log_test").toFile()
        val file = File(root, "diagnostics/stalls.log")
        try {
            val log = StallLogFile(directory = { root })
            val monitor = PerformanceMonitorService(logSink = {}, stallLog = log, now = { at(2026, 9, 14, 12, 0, 1) })

            monitor.recordFrameDelay(buildMicros = 40_000, rasterMicros = 190_000, totalMicros = 260_000)
            monitor.recordFrameDelay(buildMicros = 5_000, rasterMicros = 5_000)
            log.flush()

            assertEq(file.path, log.path)
            assertEq(file.path, monitor.stallLogPath)
            val lines = file.readLines()
            expect(lines.first().startsWith("--- session start"), lines.first())
            assertEq(2, lines.size)
            assertEq("2026-09-14T12:00:01.000 [stall] frame frame 260ms buildMs=40 rasterMs=190", lines.last())
        } finally {
            file.delete()
            File(root, "diagnostics").delete()
            root.delete()
        }
    }

    fun testStallRecordsCarryTheirStackIndented() {
        val root = Files.createTempDirectory("stall_log_stack").toFile()
        val file = File(root, "diagnostics/stalls.log")
        try {
            val log = StallLogFile(
                directory = { root },
                build = AppBuild(version = "0.1.0+1", platform = "android", osVersion = "Android 16 (API 36)", mode = "debug"),
                now = { at(2026, 9, 14, 12, 0, 0) },
            )
            val monitor = PerformanceMonitorService(logSink = {}, stallLog = log, now = { at(2026, 9, 14, 12, 0, 2) })
            monitor.recordSlowEvent(PerformanceEventKind.Operation, "repos.load", 820_000, stackTrace = "stack line 1\nstack line 2")
            log.flush()
            assertEq(
                listOf(
                    "--- session start 2026-09-14T12:00:00.000 build=0.1.0+1 mode=debug platform=android os=\"Android 16 (API 36)\" ---",
                    "2026-09-14T12:00:02.000 [perf] operation repos.load 820ms",
                    "    stack line 1",
                    "    stack line 2",
                ),
                file.readLines(),
            )
        } finally {
            file.delete()
            File(root, "diagnostics").delete()
            root.delete()
        }
    }

    fun testStallLogRotatesPastItsSizeCap() {
        val root = Files.createTempDirectory("stall_log_rotate").toFile()
        val current = File(root, "diagnostics/stalls.log")
        val rotated = File(root, "diagnostics/stalls.log.1")
        try {
            val log = StallLogFile(directory = { root }, maxBytes = 400)
            for (i in 0 until 10) log.append("record $i ${"x".repeat(60)}")
            log.flush()

            expect(rotated.exists())
            expect(current.length() <= 400, "${current.length()} bytes")
            expect(current.readText().contains("record 9"))
            expect(current.readText().startsWith("--- rotated"))
        } finally {
            current.delete()
            rotated.delete()
            File(root, "diagnostics").delete()
            root.delete()
        }
    }

    fun testStallLogWithoutStorageDropsRecordsQuietly() {
        val log = StallLogFile(directory = { throw IllegalStateException("none") })
        log.append("lost")
        log.flush()
        assertEq(null, log.path)
    }

    fun testSlowRestRequestsAreReported() {
        val monitor = PerformanceMonitorService(elapsedMicrosForTest = { 700_000L }, logSink = {})

        monitor.recordExchange("GET", "http://localhost:8787/api/repositories", 400, 700_000)

        assertEq(1, monitor.events.size)
        val event = monitor.events.single()
        assertEq("api.GET /api/repositories", event.name)
        assertEq("/api/repositories", event.details["path"])
        assertEq("localhost", event.details["host"])
        expect((event.details["error"] as String).contains("HTTP 400"))
    }

    fun testFastRequestsStillAttributeALaterStall() {
        TestLoop().use { loop ->
            val monitor = PerformanceMonitorService(stallThresholdMs = 120, heartbeatIntervalMs = 20, logSink = {}, scheduler = loop.scheduler)
            loop.run { monitor.startHeartbeat() }
            Thread.sleep(50)
            loop.run {
                monitor.recordExchange("POST", "https://relay.test/api/sync?x=1", 200, 3_000)
                busyFor(250)
            }
            Thread.sleep(60)
            loop.run { monitor.dispose() }
            val stall = monitor.events.first { it.kind == PerformanceEventKind.EventLoop }
            assertEq("api.POST /api/sync", stall.details["finishedDuring"])
            assertEq(1, monitor.events.size)
        }
    }

    fun testDevOverlayCopiesTheSlowEventLog() {
        val monitor = PerformanceMonitorService(thresholdMs = 500, logSink = {}, now = { at(2026, 7, 3, 12, 34, 56) })
        monitor.recordSlowEvent(
            PerformanceEventKind.Operation,
            "repos.load",
            820_000,
            details = mapOf("screen" to "Repos"),
            stackTrace = "stack line 1\nstack line 2",
        )

        val copied = DevPerformanceText.formatLog(monitor.events)

        expect(copied.contains("repos.load"))
        expect(copied.contains("820ms"))
        expect(copied.contains("screen: Repos"))
        expect(copied.contains("stack line 1"))
        assertEq(
            "[operation] repos.load\nduration: 820ms\ntimestamp: 2026-07-03T12:34:56.000\nscreen: Repos\nstack:\nstack line 1\nstack line 2",
            copied,
        )
        assertEq("operation at 12:34:56", DevPerformanceText.cardSubtitle(monitor.events.single()))
        assertEq("screen: Repos", DevPerformanceText.cardDetails(monitor.events.single()))
        assertEq("820ms", DevPerformanceText.durationLabel(monitor.events.single()))
        monitor.clear()
        assertEq("No slow performance events recorded.", DevPerformanceText.formatLog(monitor.events))
    }

    fun testDevOverlayIsHiddenWhenDisabledOrEmpty() {
        val disabled = PerformanceMonitorService(enabled = false)
        val empty = PerformanceMonitorService()
        val busy = PerformanceMonitorService(logSink = {})
        busy.recordSlowEvent(PerformanceEventKind.Operation, "auth.login", 700_000, details = mapOf("screen" to "Login"))
        disabled.recordSlowEvent(PerformanceEventKind.Operation, "auth.login", 700_000)

        expect(!DevPerformanceText.showsIndicator(disabled))
        expect(!DevPerformanceText.showsIndicator(empty))
        expect(DevPerformanceText.showsIndicator(busy))
        assertEq("700ms", DevPerformanceText.durationLabel(busy.latestEvent!!))
        assertEq(
            "Stalls over 200 ms and operations over 500 ms will appear here.",
            DevPerformanceText.emptyMessage(empty),
        )
    }

    fun testDevOverlayIndicatorCanBeDraggedButStaysInside() {
        val start = 800f - 12f - 90f to 8f
        assertEq(
            start.first - 120 to start.second + 80,
            DevPerformanceText.clampIndicator(start.first - 120, start.second + 80, 800f, 600f, 90f, 32f, 0f, 0f, 8f),
        )
        assertEq(8f to 32f, DevPerformanceText.clampIndicator(-500f, -500f, 800f, 600f, 90f, 32f, 24f, 16f, 8f))
        assertEq(702f to 544f, DevPerformanceText.clampIndicator(5000f, 5000f, 800f, 600f, 90f, 32f, 24f, 16f, 8f))
        assertEq(8f to 32f, DevPerformanceText.clampIndicator(40f, 40f, 50f, 40f, 90f, 32f, 24f, 16f, 8f))
    }
}
