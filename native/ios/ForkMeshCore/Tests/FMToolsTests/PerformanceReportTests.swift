import FMAccount
import FMBase
import XCTest
@testable import FMTools

final class PerformanceReportTests: XCTestCase {
    private let utc = TimeZone(identifier: "UTC")!

    @MainActor
    private func fixture() -> (telemetry: AppTelemetry, clock: TestClock) {
        let clock = TestClock(telemetryEpoch)
        let log = AppLog(now: { clock.now })
        let monitor = PerformanceMonitorService(logSink: { _ in }, now: { clock.now })
        let telemetry = AppTelemetry(log: log, monitor: monitor, now: { clock.now }, rssMegabytes: { 150 })
        return (telemetry, clock)
    }

    @MainActor
    func testReportCarriesContextMetricsEventsCulpritsAndLogs() async {
        let (telemetry, clock) = fixture()
        let monitor = telemetry.monitor!
        for _ in 0..<5 {
            clock.advance(1)
            telemetry.recordLoopLag(.milliseconds(40))
            telemetry.sampleNow()
        }
        monitor.recordSlowEvent(
            kind: .eventLoop, name: "ui-isolate blocked", duration: .milliseconds(450),
            details: ["finishedDuring": "api.GET /repos, chat.decode"])
        monitor.recordSlowEvent(
            kind: .eventLoop, name: "ui-isolate blocked", duration: .milliseconds(300),
            details: ["finishedDuring": "api.GET /repos"])
        monitor.recordSlowEvent(kind: .operation, name: "api.GET /repos", duration: .milliseconds(900))
        telemetry.log.add(.net, "GET app.forkmesh.com/api/repos/42 200 in 120ms")
        telemetry.log.add(.net, "GET app.forkmesh.com/api/repos/7 200 in 80ms")
        telemetry.log.add(.error, "POST app.forkmesh.com/api/chat HTTP 503 in 2000ms")
        telemetry.log.add(.error, "POST app.forkmesh.com/api/chat failed after 30ms: timeout")
        for i in 0..<3 { telemetry.log.add(.relay, "relay frame \(i) received") }
        telemetry.sampleNow()

        let text = PerformanceReport.text(
            telemetry: telemetry, context: [("app", "1.2.5+13 (release)"), ("device", "iPhone15,2")],
            metricKit: ["Hang 2026-09-30: The main thread hung for 2.1 s"], stallLog: "--- session start ---\nold stall\n",
            now: clock.now, timeZone: utc)

        XCTAssertTrue(text.hasPrefix("# \(PerformanceReport.title)\n"))
        XCTAssertTrue(text.contains("app: 1.2.5+13 (release)\ndevice: iPhone15,2"))
        XCTAssertTrue(text.contains("stalls since launch 2 (over 200ms), paused while visible 0 (longest 0ms), "), text)
        XCTAssertTrue(text.contains("## Stalls by name\n2x     total 750ms max 450ms last 12:00:05 eventLoop ui-isolate blocked\n"), text)
        XCTAssertTrue(text.contains("## Slow operations by name\n1x     total 900ms max 900ms operation api.GET /repos\n"), text)
        XCTAssertTrue(text.contains("2x     finishedDuring api.GET /repos\n1x     finishedDuring chat.decode"))
        XCTAssertTrue(text.contains("2x     avg 1015ms max 2000ms failed 2  POST app.forkmesh.com/api/chat\n2x     avg 100ms max 120ms failed 0  GET app.forkmesh.com/api/repos/#"))
        XCTAssertTrue(text.contains("3x     RELAY relay frame # received"))
        XCTAssertTrue(text.contains("\nLOOP   avg "))
        XCTAssertTrue(text.contains("max 40ms"))
        XCTAssertTrue(text.contains("MEM    avg 150MB max 150MB"))
        XCTAssertTrue(text.contains("The main thread hung for 2.1 s"))
        XCTAssertTrue(text.contains("old stall"))
        XCTAssertTrue(text.contains("2026-09-14T12:00:05.000 RELAY relay frame 2 received"))
        XCTAssertTrue(text.contains("450ms eventLoop ui-isolate blocked finishedDuring=api.GET /repos, chat.decode"))
        XCTAssertLessThanOrEqual(text.unicodeScalars.count, PerformanceReport.maxChars)
    }

    @MainActor
    func testEmptyReportSaysSoInsteadOfLeavingBlankSections() async {
        let (telemetry, clock) = fixture()
        let text = PerformanceReport.text(telemetry: telemetry, now: clock.now, timeZone: utc)
        XCTAssertTrue(text.contains("## Metrics (5m | 1h)\nno samples yet"))
        XCTAssertTrue(text.contains("## Newest slow events\nnone"))
        XCTAssertTrue(text.contains("## stalls.log tail\nempty"))
        XCTAssertTrue(text.contains("## MetricKit\nnone"))
    }

    @MainActor
    func testBusyLaunchStaysUnderTheCharacterBudget() async {
        let (telemetry, clock) = fixture()
        let monitor = telemetry.monitor!
        let filler = String(repeating: "x", count: 400)
        for i in 0..<120 {
            clock.advance(1)
            telemetry.recordLoopLag(.milliseconds(i))
            telemetry.sampleNow()
            monitor.recordSlowEvent(
                kind: .eventLoop, name: "blocked \(filler) \(i)", duration: .milliseconds(300),
                details: ["finishedDuring": .string("op\(i), \(filler)")])
            telemetry.log.add(.net, "GET app.forkmesh.com/api/r\(i)/\(filler) 200 in 12ms")
            telemetry.log.add(.relay, "frame \(filler) \(i)")
        }
        let text = PerformanceReport.text(
            telemetry: telemetry, context: [("screen", filler)],
            metricKit: Array(repeating: "Hang \(filler)\n\(filler)", count: 10),
            stallLog: Array(repeating: filler, count: 80).joined(separator: "\n"),
            now: clock.now, timeZone: utc)
        XCTAssertTrue(text.hasPrefix("# \(PerformanceReport.title)\n"))
        XCTAssertLessThanOrEqual(text.unicodeScalars.count, PerformanceReport.maxChars)
        XCTAssertFalse(text.contains("[cut at"))
        XCTAssertTrue(text.contains("trimmed to fit"))
        for heading in ["## Metrics", "## Stalls by name", "## Slow operations by name", "## HTTP endpoints", "## MetricKit", "## App log"] {
            XCTAssertTrue(text.contains(heading), heading)
        }
    }

    @MainActor
    func testTrimmingKeepsTheNewestLogLinesAndEveryHeading() async {
        let (telemetry, clock) = fixture()
        let filler = String(repeating: "y", count: 100)
        for i in 0..<40 {
            clock.advance(1)
            telemetry.log.add(.info, "line \(i) \(filler)")
        }
        let stallLog = (0..<20).map { "stall \($0) \(filler)" }.joined(separator: "\n")
        let text = PerformanceReport.text(
            telemetry: telemetry, stallLog: stallLog, now: clock.now, timeZone: utc, limit: 1500)
        XCTAssertLessThanOrEqual(text.unicodeScalars.count, 1500)
        XCTAssertFalse(text.contains("[cut at"))
        XCTAssertTrue(text.contains("## App log"))
        XCTAssertTrue(text.contains("## Log volume"))
        XCTAssertTrue(text.contains("older lines trimmed to fit\n"))
        if text.contains("line 3") {
            XCTAssertTrue(text.contains("line 39"))
        }
    }

    @MainActor
    func testLimitIsClampedBetweenMinAndMaxChars() async {
        let (telemetry, clock) = fixture()
        let filler = String(repeating: "z", count: 300)
        for i in 0..<30 { telemetry.log.add(.info, "\(i) \(filler)") }
        let tiny = PerformanceReport.text(telemetry: telemetry, now: clock.now, timeZone: utc, limit: 10)
        XCTAssertLessThanOrEqual(tiny.unicodeScalars.count, PerformanceReport.minChars)
        XCTAssertGreaterThan(tiny.unicodeScalars.count, PerformanceReport.minChars / 2)
        let huge = PerformanceReport.text(telemetry: telemetry, now: clock.now, timeZone: utc, limit: 100_000)
        XCTAssertLessThanOrEqual(huge.unicodeScalars.count, PerformanceReport.maxChars)
    }

    @MainActor
    func testFrameEventsAreNamedOnceAndMetricKitStacksKeepTheirLine() async {
        let (telemetry, clock) = fixture()
        telemetry.monitor!.recordFrameDelay(buildDuration: .milliseconds(300), rasterDuration: .zero)
        let text = PerformanceReport.text(
            telemetry: telemetry, metricKit: ["Crash 2026-10-02: signal 11\nstack: ForkMesh+0x1a < UIKitCore+0x2b"],
            now: clock.now, timeZone: utc)
        XCTAssertTrue(text.contains("## Stalls by name\n1x     total 300ms max 300ms last 12:00:00 frame\n"), text)
        XCTAssertFalse(text.contains("frame frame"))
        XCTAssertTrue(text.contains("## MetricKit\nCrash 2026-10-02: signal 11\nstack: ForkMesh+0x1a < UIKitCore+0x2b"))
    }

    @MainActor
    func testStallsAreGroupedByTheScreenThatWasShowing() async {
        let (telemetry, clock) = fixture()
        let monitor = telemetry.monitor!
        monitor.recordSlowEvent(kind: .eventLoop, name: "ui-isolate blocked", duration: .milliseconds(250))
        clock.advance(30)
        monitor.recordSlowEvent(
            kind: .eventLoop, name: "ui-isolate blocked", duration: .milliseconds(324), details: ["screen": "Inbox > Shortcuts"])
        monitor.recordSlowEvent(kind: .frame, name: "frame", duration: .milliseconds(354), details: ["screen": "Inbox > Shortcuts"])
        let text = PerformanceReport.text(telemetry: telemetry, now: clock.now, timeZone: utc)
        XCTAssertTrue(text.contains(
            "## Stalls by screen\n2x     total 678ms max 354ms last 12:00:30 Inbox > Shortcuts\n"
                + "1x     total 250ms max 250ms last 12:00:00 not recorded\n"), text)
    }

    @MainActor
    func testStallsSumTheNamedMainActorWorkThatRanInsideThem() async {
        let (telemetry, clock) = fixture()
        let monitor = telemetry.monitor!
        monitor.recordSlowEvent(
            kind: .eventLoop, name: "ui-isolate blocked", duration: .milliseconds(318),
            details: ["sections": "watch.fit 180ms, agents.lines 40ms"])
        monitor.recordSlowEvent(
            kind: .frame, name: "frame", duration: .milliseconds(246), details: ["sections": "watch.fit 120ms"])
        monitor.recordSlowEvent(kind: .eventLoop, name: "ui-isolate blocked", duration: .milliseconds(250))
        let text = PerformanceReport.text(telemetry: telemetry, now: clock.now, timeZone: utc)
        XCTAssertTrue(text.contains(
            "## Main-actor work during stalls\n2x     total 300ms max 180ms watch.fit\n"
                + "1x     total 40ms max 40ms  agents.lines\n"), text)
    }

    @MainActor
    func testStallsKeepTheirOwnSectionWhenOperationsCrowdTheReport() async {
        let (telemetry, clock) = fixture()
        let monitor = telemetry.monitor!
        monitor.recordSlowEvent(kind: .eventLoop, name: "ui-isolate blocked", duration: .milliseconds(250))
        clock.advance(90)
        monitor.recordSlowEvent(kind: .sync, name: "genie.scan", duration: .milliseconds(220))
        monitor.recordSlowEvent(kind: .eventLoop, name: "ui-isolate blocked", duration: .milliseconds(600))
        for i in 0..<12 {
            monitor.recordSlowEvent(kind: .operation, name: "api.GET /api/r\(i)", duration: .milliseconds(5000 + i))
        }
        let text = PerformanceReport.text(telemetry: telemetry, now: clock.now, timeZone: utc)
        XCTAssertTrue(text.contains(
            "## Stalls by name\n2x     total 850ms max 600ms last 12:01:30 eventLoop ui-isolate blocked\n"
                + "1x     total 220ms max 220ms last 12:01:30 sync genie.scan\n"), text)
        XCTAssertTrue(text.contains("## Slow operations by name\n1x     total 5011ms max 5011ms operation api.GET /api/r11\n"), text)
        XCTAssertTrue(text.contains("... 6 more"))
        XCTAssertFalse(text.components(separatedBy: "## Slow operations by name")[1].contains("ui-isolate"))
    }

    @MainActor
    func testFitCutsAtTheLimitAndSaysSo() async {
        let text = PerformanceReport.fit(String(repeating: "a", count: 50), 30)
        XCTAssertEqual(text.unicodeScalars.count, 30)
        XCTAssertTrue(text.hasSuffix("[cut at 30 chars]\n"))
        XCTAssertEqual(PerformanceReport.fit("short\n", 30), "short\n")
    }

    @MainActor
    func testRequestLinesParseIntoEndpointAndDuration() async {
        XCTAssertEqual(PerformanceReport.parseRequest("GET h/api/x/12 200 in 33ms")?.key, "GET h/api/x/#")
        XCTAssertEqual(PerformanceReport.parseRequest("GET h/api/x 200 in 33ms")?.ms, 33)
        XCTAssertEqual(PerformanceReport.parseRequest("PUT h/a HTTP 500 in 7ms")?.ms, 7)
        XCTAssertEqual(PerformanceReport.parseRequest("DELETE h/a failed after 9ms: gone")?.ms, 9)
        XCTAssertNil(PerformanceReport.parseRequest("relay connected in 30ms"))
    }
}
