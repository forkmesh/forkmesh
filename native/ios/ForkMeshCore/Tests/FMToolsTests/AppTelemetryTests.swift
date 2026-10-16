// App telemetry: what the strip and the Diagnostics screen are given to draw (TelemetryScreenModel,
// MetricCardState, the formatting helpers).

import FMAccount
import FMBase
import FMNet
import XCTest
@testable import FMTools

/// A settable clock the log and the sampler share.
final class TestClock: @unchecked Sendable {
    private let lock = NSLock()
    private var current: Date

    init(_ date: Date) { current = date }

    var now: Date {
        get { lock.lock(); defer { lock.unlock() }; return current }
        set { lock.lock(); current = newValue; lock.unlock() }
    }

    func advance(_ seconds: TimeInterval) { now = now.addingTimeInterval(seconds) }
}

/// 2026-09-14 12:00:00 in UTC.
let telemetryEpoch = Date(timeIntervalSince1970: 1_789_387_200)

final class AppTelemetryTests: XCTestCase {
    // MARK: AppLog

    func testClassifiesPlatformLinesIntoFamilies() {
        XCTAssertEqual(LogCategory.classify("[stall] frame frame 412ms"), .stall)
        XCTAssertEqual(LogCategory.classify("[perf] operation api.GET 900ms error=x"), .perf)
        XCTAssertEqual(LogCategory.classify("Auth logout revocation failed: boom"), .error)
        XCTAssertEqual(LogCategory.classify("[push] registered"), .push)
        XCTAssertEqual(LogCategory.classify("UserPresence: local_auth unavailable"), .auth)
        XCTAssertEqual(LogCategory.classify("hello"), .info)
    }

    func testKeepsTheNewestLinesAndCountsEveryLineEverAdded() {
        let clock = TestClock(telemetryEpoch)
        let log = AppLog(capacity: 3, now: { clock.now })
        for i in 0..<5 {
            clock.advance(1)
            log.add(i % 2 == 0 ? .net : .stall, "line \(i)")
        }
        XCTAssertEqual(log.entries().map(\.message), ["line 4", "line 3", "line 2"])
        XCTAssertEqual(log.totalLines, 5)
        XCTAssertEqual(log.totalFor(.net), 3)
        XCTAssertEqual(log.totalFor(.stall), 2)
        XCTAssertEqual(log.entries(categories: [.stall]).map(\.message), ["line 3"])
        let since = telemetryEpoch.addingTimeInterval(4)
        XCTAssertEqual(log.entries(since: since).map(\.message), ["line 4", "line 3"])
        XCTAssertEqual(log.countsSince(since)[LogCategory.stall.index], 1)
    }

    func testAddLineTrimsAndSkipsBlankLines() {
        let log = AppLog()
        log.addLine("   \n")
        log.addLine("[stall] frame  \n")
        XCTAssertEqual(log.totalLines, 1)
        XCTAssertEqual(log.entries().first?.message, "[stall] frame")
        XCTAssertEqual(log.entries().first?.category, .stall)
    }

    func testCaptureProcessLogsRoutesTheLogSinkAndRestoresIt() {
        let log = AppLog()
        let previous = Log.sink
        let restore = log.captureProcessLogs()
        Log.warning("auth", "Auth refresh failed: offline")
        restore()
        Log.info("x", "not captured")
        Log.sink = previous
        XCTAssertEqual(log.totalLines, 1)
        XCTAssertEqual(log.totalFor(.error), 1)
    }

    func testEntryPrintsLikeDartToString() {
        let entry = AppLogEntry(timestamp: telemetryEpoch.addingTimeInterval(1.5), category: .net, message: "GET x 200 in 1ms")
        let utc = TimeZone(identifier: "UTC")!
        XCTAssertEqual(entry.text(timeZone: utc), "2026-09-14T12:00:01.500 NET GET x 200 in 1ms")
        XCTAssertEqual(entry.stamp(timeZone: utc), "12:00:01")
    }

    // MARK: MetricHistory

    func testRollsSecondsIntoMinuteAveragesAndPeaks() {
        let history = MetricHistory()
        for i in 0..<120 { history.add(i < 60 ? 2 : (i == 90 ? 50 : 10)) }
        XCTAssertEqual(history.lastSeconds(3), [10, 10, 10])
        XCTAssertEqual(history.lastMinutes(2, peak: false)[0], 2)
        XCTAssertEqual(history.lastMinutes(2, peak: true)[1], 50)
        XCTAssertEqual(history.lastMinutes(2, peak: false)[1], 10.67, accuracy: 0.01)
    }

    func testALongGapFillsWholeMinutesWithoutWalkingEverySecond() {
        let history = MetricHistory(secondCapacity: 120, minuteCapacity: 10)
        for _ in 0..<30 { history.add(1) }
        history.addGap(24 * 3600)
        history.add(7)
        XCTAssertEqual(history.latest, 7)
        XCTAssertTrue(history.lastSeconds(2).first!.isNaN)
        let minutes = history.lastMinutes(10, peak: false)
        XCTAssertEqual(minutes.count, 10)
        XCTAssertTrue(minutes.allSatisfy(\.isNaN))
    }

    // MARK: AppTelemetry

    @MainActor
    func testSamplesFramesLagRequestsRelayFramesLogsAndStalls() async throws {
        let clock = TestClock(telemetryEpoch)
        let log = AppLog(now: { clock.now })
        let monitor = PerformanceMonitorService(logSink: { line in log.addLine(line) })
        var relayFrames = 0
        let telemetry = AppTelemetry(log: log, monitor: monitor, relayFrames: { relayFrames }, now: { clock.now },
                                     rssMegabytes: { 123 })
        let fake = FakeHTTPClient { request in
            HTTPResponse(status: request.url.path == "/missing" ? 404 : 200, text: "")
        }
        let client = telemetry.wrap(fake)

        telemetry.recordFrame(.milliseconds(12))
        telemetry.recordFrame(.milliseconds(40))
        monitor.onHeartbeatLag = nil
        relayFrames = 5
        _ = try await client.get(URL(string: "https://forkmesh.test/api/repos?token=s")!)
        _ = try await client.get(URL(string: "https://forkmesh.test/missing")!)
        monitor.recordSlowEvent(kind: .frame, name: "frame", duration: .milliseconds(300))

        clock.advance(1)
        telemetry.sampleNow()

        XCTAssertEqual(telemetry.latest(.fps), 2)
        XCTAssertEqual(telemetry.latest(.frame), 40)
        XCTAssertEqual(telemetry.latest(.mem), 123)
        XCTAssertEqual(telemetry.latest(.net), 2)
        XCTAssertEqual(telemetry.latest(.relay), 5)
        XCTAssertEqual(telemetry.latest(.stall), 1)
        XCTAssertEqual(telemetry.latest(.log), 3)
        XCTAssertEqual(log.totalFor(.stall), 1)
        XCTAssertEqual(log.totalFor(.error), 1)
        let net = log.entries(categories: [.net])
        XCTAssertEqual(net.count, 1)
        XCTAssertTrue(net[0].message.contains("forkmesh.test/api/repos"))
        XCTAssertFalse(net[0].message.contains("token"))

        // The next second starts from zero, and a pause leaves NaN seconds.
        clock.advance(4)
        telemetry.sampleNow()
        XCTAssertEqual(telemetry.latest(.stall), 0)
        XCTAssertEqual(telemetry.latest(.fps), 0)
        let recent = telemetry.series(.fps, .m1)
        XCTAssertEqual(recent.count, 5)
        XCTAssertEqual(recent.filter(\.isNaN).count, 3)
        let summary = telemetry.summary(.net, .m1)
        XCTAssertEqual(summary.total, 2)
        XCTAssertEqual(summary.samples, 2)
        telemetry.dispose()
        monitor.dispose()
    }

    @MainActor
    func testAFailedRequestIsLoggedAsAnErrorAndStillCounted() async {
        let log = AppLog()
        let telemetry = AppTelemetry(log: log, rssMegabytes: { 1 })
        let fake = FakeHTTPClient()
        fake.enqueue(error: HTTPError.timedOut)
        do {
            _ = try await telemetry.wrap(fake).get(URL(string: "https://forkmesh.test/api/x?ticket=t")!)
            XCTFail("expected a throw")
        } catch {}
        XCTAssertEqual(telemetry.requestsInFlight, 0)
        let line = log.entries(categories: [.error]).first?.message ?? ""
        XCTAssertTrue(line.hasPrefix("GET forkmesh.test/api/x failed after "), line)
        XCTAssertFalse(line.contains("ticket"))
        telemetry.sampleNow()
        XCTAssertEqual(telemetry.latest(.net), 1)
    }

    func testTheStallMonitorReportsEveryHeartbeatLagAndCountsStalls() {
        let monitor = PerformanceMonitorService(logSink: { _ in })
        monitor.onHeartbeatLag = { _ in }
        monitor.recordSlowEvent(kind: .operation, name: "slow", duration: .seconds(1))
        monitor.recordSlowEvent(kind: .eventLoop, name: "ui-isolate blocked", duration: .milliseconds(250))
        XCTAssertEqual(monitor.stallCount, 1)
        monitor.dispose()
    }

    @MainActor
    func testStartHooksTheHeartbeatAndStopReleasesIt() {
        let monitor = PerformanceMonitorService(logSink: { _ in })
        let telemetry = AppTelemetry(log: AppLog(), monitor: monitor, rssMegabytes: { 1 })
        telemetry.start(lifecycleState: .paused)
        XCTAssertFalse(telemetry.running)
        XCTAssertNotNil(monitor.onHeartbeatLag)
        monitor.onHeartbeatLag?(.milliseconds(70))
        telemetry.appLifecycleChanged(.resumed)
        XCTAssertTrue(telemetry.running)
        telemetry.appLifecycleChanged(.paused)
        XCTAssertFalse(telemetry.running)
        telemetry.sampleNow()
        XCTAssertEqual(telemetry.latest(.loop), 70)
        telemetry.stop()
        XCTAssertNil(monitor.onHeartbeatLag)
    }

    func testDownsampleKeepsPeaksOrAveragesFromTheNewestEnd() {
        XCTAssertEqual(telemetryDownsample([1, 5, 2, 2, 9], 2, peak: true), [1, 5, 9])
        XCTAssertEqual(telemetryDownsample([1, 5, 2, 2, 9], 2, peak: false), [1, 3.5, 5.5])
        XCTAssertTrue(telemetryDownsample([.nan, .nan, 4], 2, peak: true)[0].isNaN)
    }

    // MARK: Formatting (the strip)

    func testMetricFormatting() {
        XCTAssertEqual(TelemetryMetric.mem.format(88), "88MB")
        XCTAssertEqual(TelemetryMetric.mem.formatBare(88), "88")
        XCTAssertEqual(TelemetryMetric.frame.format(40.4), "40ms")
        XCTAssertEqual(TelemetryMetric.net.format(1500), "1.5k")
        XCTAssertEqual(TelemetryMetric.net.format(2.5), "2.5")
        XCTAssertEqual(TelemetryMetric.net.format(3), "3")
        XCTAssertEqual(TelemetryMetric.fps.format(.nan), "–")
        XCTAssertEqual(TelemetryMetric.frame.formatBare(.nan), "–")
        for metric in TelemetryMetric.allCases { XCTAssertEqual(metric.short.count, 3, metric.label) }
        XCTAssertNil(TelemetryMetric.mem.health([]))
        XCTAssertNil(TelemetryMetric.mem.health([.nan, .nan]))
        XCTAssertEqual(TelemetryMetric.fps.health([60, 59, .nan]), .good)
        XCTAssertEqual(TelemetryMetric.fps.health([60, 30]), .warning)
        XCTAssertEqual(TelemetryMetric.fps.health([60, 12]), .bad)
        XCTAssertEqual(TelemetryMetric.loop.health([3, 49]), .good)
        XCTAssertEqual(TelemetryMetric.loop.health([3, 120]), .warning)
        XCTAssertEqual(TelemetryMetric.loop.health([3, 250]), .bad)
        XCTAssertEqual(TelemetryMetric.mem.health([180]), .good)
        XCTAssertEqual(TelemetryMetric.stall.health([0, 0]), .good)
        XCTAssertEqual(TelemetryMetric.stall.health([0, 1]), .bad)
        XCTAssertEqual(TelemetryFormat.compactCount(999), "999")
        XCTAssertEqual(TelemetryFormat.compactCount(1250), "1.3k")
        XCTAssertEqual(TelemetryFormat.compactCount(12500), "12k")
        XCTAssertEqual(TelemetryFormat.uptime(12), "12s")
        XCTAssertEqual(TelemetryFormat.uptime(250), "4m10s")
        XCTAssertEqual(TelemetryFormat.uptime(3 * 3600 + 5 * 60), "3h5m")
        XCTAssertEqual(TelemetryFormat.uptime(2 * 86400 + 3600), "2d1h")
        XCTAssertEqual(LogCategoryInk.of(.error, count: 0), .tertiary)
        XCTAssertEqual(LogCategoryInk.of(.error, count: 2), .danger)
        XCTAssertEqual(LogCategoryInk.of(.stall, count: 1), .warning)
        XCTAssertEqual(LogCategoryInk.of(.net, count: 1), .primary)
    }

    func testChartGeometry() {
        XCTAssertEqual(TelemetryChartGeometry.scaleTop([1, 2], floor: 60), 60)
        XCTAssertEqual(TelemetryChartGeometry.scaleTop([100, .nan], floor: 60), 110, accuracy: 1e-9)
        // Right-aligned: with 30 slots and 3 points, the newest sits on the right edge.
        let geometry = TelemetryChartGeometry(values: [1, .nan, 3], slots: 30, floor: 10, width: 290, height: 20)
        XCTAssertEqual(geometry.x(2), 290)
        XCTAssertEqual(geometry.x(0), 270)
        XCTAssertEqual(geometry.segments, [0...0, 2...2])
        XCTAssertEqual(TelemetryChartGeometry.indexAt(x: 290, width: 290, count: 3, slots: 30), 2)
        XCTAssertEqual(TelemetryChartGeometry.indexAt(x: 0, width: 290, count: 3, slots: 30), 0)
        XCTAssertNil(TelemetryChartGeometry.indexAt(x: 0, width: 290, count: 0, slots: 30))
    }

    // MARK: Widgets (as their models)

    @MainActor
    private func seeded() -> (TestClock, AppLog, AppTelemetry) {
        let clock = TestClock(telemetryEpoch)
        let log = AppLog(now: { clock.now })
        let telemetry = AppTelemetry(log: log, now: { clock.now }, rssMegabytes: { 88 })
        log.add(.stall, "[stall] frame frame 412ms")
        log.add(.net, "GET forkmesh.test/api/repos 200 in 40ms")
        log.add(.net, "GET forkmesh.test/api/orgs 200 in 30ms")
        for _ in 0..<3 {
            clock.advance(1)
            telemetry.recordFrame(.milliseconds(16))
            telemetry.sampleNow()
        }
        return (clock, log, telemetry)
    }

    @MainActor
    func testTheStripShowsLiveSquaresAndLogCounts() async {
        let (clock, log, telemetry) = seeded()
        XCTAssertEqual(TelemetryMetric.mem.short, "MEM")
        XCTAssertEqual(TelemetryMetric.mem.formatBare(telemetry.latest(.mem)), "88")
        XCTAssertEqual("\(TelemetryMetric.mem.title) \(TelemetryMetric.mem.format(telemetry.latest(.mem)))", "Memory 88MB")
        XCTAssertEqual(telemetry.history(.mem).lastSeconds(30).count, 3)
        XCTAssertEqual(TelemetryFormat.compactCount(log.totalFor(.net)), "2")

        // New samples arrive on the tick.
        let before = telemetry.revision
        log.add(.net, "GET forkmesh.test/x 200 in 1ms")
        clock.advance(1)
        telemetry.sampleNow()
        XCTAssertEqual(telemetry.revision, before + 1)
        XCTAssertEqual(TelemetryFormat.compactCount(log.totalFor(.net)), "3")
    }

    @MainActor
    func testDiagnosticsOpensOnTheChosenLogFamily() async {
        let (clock, _, telemetry) = seeded()
        let model = TelemetryScreenModel(telemetry: telemetry, logCategory: .stall, now: { clock.now })
        XCTAssertEqual(model.tab, 1)
        XCTAssertEqual(model.visibleEntries.map(\.message), ["[stall] frame frame 412ms"])
        XCTAssertTrue(model.caption.hasPrefix("UI stalls: frames, event-loop blocks, named slow sections. 1 since launch."))

        model.selectAll()
        XCTAssertEqual(model.visibleEntries.filter { $0.message.contains("api/repos") }.count, 1)
        XCTAssertEqual(model.allChipLabel, "All 3")
        XCTAssertEqual(model.chipLabel(.net), "NET 2")
        XCTAssertEqual(model.caption, "3 lines in the last 5m · 3 since launch")

        // A range that ends before the lines were written hides them.
        clock.advance(180)
        model.range = .m1
        XCTAssertTrue(model.visibleEntries.isEmpty)
        XCTAssertEqual(model.emptyText, "Nothing logged in the last 1m")
    }

    @MainActor
    func testPauseFreezesTheLogAndCopyWritesOldestFirst() async {
        let (clock, log, telemetry) = seeded()
        let model = TelemetryScreenModel(telemetry: telemetry, now: { clock.now })
        XCTAssertEqual(model.tab, 0)
        let logs = TelemetryScreenModel(telemetry: telemetry, initialTab: 1, now: { clock.now })
        XCTAssertEqual(logs.tab, 1)
        model.togglePause()
        XCTAssertEqual(model.pauseLabel, "Resume")
        clock.advance(1)
        log.add(.info, "after the pause")
        XCTAssertEqual(model.visibleEntries.count, 3)
        let copied = model.copyText()
        XCTAssertEqual(copied.confirmation, "Copied 3 log lines")
        XCTAssertTrue(copied.text.components(separatedBy: "\n").first!.hasSuffix("STALL [stall] frame frame 412ms"))
        model.togglePause()
        XCTAssertEqual(model.visibleEntries.count, 4)
    }

    @MainActor
    func testStatsCardsFocusAndTheirNumbers() async {
        let (clock, _, telemetry) = seeded()
        let model = TelemetryScreenModel(telemetry: telemetry, focusMetric: .fps, now: { clock.now })
        XCTAssertEqual(model.focus, .fps)
        XCTAssertEqual(TelemetryScreenModel.columns(width: 398), 2)
        XCTAssertEqual(TelemetryScreenModel.columns(width: 500), 3)
        XCTAssertEqual(TelemetryScreenModel.columns(width: 800), 4)
        let fps = model.card(.fps)
        // 300 seconds in at most 180 points: buckets of two, counted back from the newest.
        XCTAssertEqual(fps.points, [1, 1])
        XCTAssertEqual(fps.slots, 150)
        XCTAssertEqual(fps.value(scrub: nil), "1")
        XCTAssertEqual(fps.when(scrub: nil), "now")
        XCTAssertEqual(fps.when(scrub: 0), "2s ago")
        XCTAssertEqual(fps.when(scrub: 9), "now")
        XCTAssertEqual(fps.footer, "min 1 · avg 1 · max 1")
        let net = model.card(.net)
        XCTAssertEqual(net.footer, "total 0 · peak 0/s")

        model.range = .h24
        let mem = model.card(.mem)
        XCTAssertEqual(mem.secondsPerPoint, 480)
        XCTAssertTrue(mem.points.isEmpty)
        XCTAssertEqual(mem.value(scrub: nil), "88MB")
        model.focus = .mem
        XCTAssertEqual(model.focus, .mem)
        XCTAssertTrue(model.uptimeLine.hasPrefix("Up 3s · sampled every second"))
        XCTAssertEqual(TelemetryFormat.ago(3900), "1h5m ago")
    }

    // MARK: Performance overlay

    func testPerformanceLogText() {
        let monitor = PerformanceMonitorService(logSink: { _ in }, now: { telemetryEpoch })
        XCTAssertEqual(PerformanceLogText.log([]), "No slow performance events recorded.")
        monitor.recordSlowEvent(kind: .frame, name: "frame", duration: .milliseconds(300), details: ["build": "x"])
        let utc = TimeZone(identifier: "UTC")!
        let event = monitor.events[0]
        XCTAssertEqual(PerformanceLogText.event(event, timeZone: utc),
                       "[frame] frame\nduration: 300ms\ntimestamp: 2026-09-14T12:00:00.000\nbuild: x")
        XCTAssertEqual(PerformanceLogText.kindLine(event, timeZone: utc), "frame at 12:00:00")
        XCTAssertEqual(PerformanceLogText.icon(event), "layers_outlined")
        XCTAssertEqual(PerformanceLogText.emptyMessage(monitor),
                       "Stalls over 200 ms and operations over 500 ms will appear here.")
        let clamped = PerformanceLogText.clampIndicator(x: -40, y: 5000, boundsWidth: 400, boundsHeight: 800,
                                                        width: 80, height: 30, topInset: 44, bottomInset: 34)
        XCTAssertEqual(clamped.x, 8)
        XCTAssertEqual(clamped.y, 800 - 30 - 34 - 8)
        monitor.dispose()
    }
}
