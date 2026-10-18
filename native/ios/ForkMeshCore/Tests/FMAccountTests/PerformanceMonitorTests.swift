// The performance monitor's service cases (the overlay widget cases belong to the app target).

import FMBase
import Foundation
import XCTest
@testable import FMAccount

final class PerformanceMonitorTests: XCTestCase {
    struct Boom: Error, CustomStringConvertible {
        var description: String { "HTTP 400" }
    }

    func testTrackLogsOnlyOperationsOverTheThreshold() async throws {
        let printed = Lines()
        let monitor = PerformanceMonitorService(
            threshold: .milliseconds(500),
            logSink: { printed.add($0) },
            now: { Date(epochMillis: 1000) },
            elapsedForTest: { .milliseconds(650) })

        let result = await monitor.track("repos.load", details: ["screen": "Code"]) { 42 }

        XCTAssertEqual(result, 42)
        XCTAssertEqual(monitor.events.count, 1)
        let event = try XCTUnwrap(monitor.events.first)
        XCTAssertEqual(event.name, "repos.load")
        XCTAssertEqual(event.kind, .operation)
        XCTAssertEqual(event.duration, .milliseconds(650))
        XCTAssertEqual(event.details["screen"], "Code")
        XCTAssertFalse(event.isStall)
        XCTAssertEqual(monitor.stallCount, 0)
        XCTAssertEqual(printed.all.count, 1)
        XCTAssertTrue(printed.all[0].contains("repos.load"))
        XCTAssertTrue(printed.all[0].contains("650ms"))
        XCTAssertEqual(printed.all[0], "[perf] operation repos.load 650ms screen=Code")
        XCTAssertFalse(event.stackTrace.isEmpty, "a slow operation keeps the stack it was started from")
    }

    func testReturnAddressesAreSymbolicatedOnlyWhenAsked() {
        let addresses = PerformanceMonitorService.captureReturnAddresses()
        XCTAssertFalse(addresses.isEmpty)
        XCTAssertLessThanOrEqual(addresses.count, 8)
        XCTAssertEqual(PerformanceMonitorService.symbolicate([1]), "0 ??? 0x1")
        let lines = PerformanceMonitorService.symbolicate(addresses).split(separator: "\n")
        XCTAssertEqual(lines.count, addresses.count)
        XCTAssertTrue(lines.first?.hasPrefix("0 ") ?? false)
    }

    func testTrackIgnoresFastOperationsAndDisabledMonitors() async {
        let enabled = PerformanceMonitorService(threshold: .milliseconds(500), elapsedForTest: { .milliseconds(120) })
        let disabled = PerformanceMonitorService(enabled: false, threshold: .milliseconds(500), elapsedForTest: { .seconds(2) })
        await enabled.track("fast") {}
        await disabled.track("disabled") {}
        XCTAssertTrue(enabled.events.isEmpty)
        XCTAssertTrue(disabled.events.isEmpty)
    }

    func testTrackRecordsTheErrorAndRethrows() async {
        let monitor = PerformanceMonitorService(logSink: { _ in }, elapsedForTest: { .milliseconds(700) })
        do {
            _ = try await monitor.track("api.GET /api/repositories", details: ["path": "/api/repositories"]) { () async throws -> Int in
                throw Boom()
            }
            XCTFail("expected the error")
        } catch {
            XCTAssertTrue(error is Boom)
        }
        XCTAssertEqual(monitor.events.first?.name, "api.GET /api/repositories")
        XCTAssertEqual(monitor.events.first?.details["path"], "/api/repositories")
        XCTAssertTrue(monitor.events.first?.details.str("error").contains("HTTP 400") ?? false)
    }

    func testRecordsSlowFrameTimingWithUsefulDebugDetails() throws {
        let monitor = PerformanceMonitorService(threshold: .milliseconds(500), logSink: { _ in })
        monitor.recordFrameDelay(buildDuration: .milliseconds(320), rasterDuration: .milliseconds(260))
        XCTAssertEqual(monitor.events.count, 1)
        let event = try XCTUnwrap(monitor.events.first)
        XCTAssertEqual(event.kind, .frame)
        XCTAssertEqual(event.name, "frame")
        XCTAssertEqual(event.duration, .milliseconds(580))
        XCTAssertEqual(event.details["buildMs"], 320)
        XCTAssertEqual(event.details["rasterMs"], 260)
        XCTAssertEqual(monitor.stallCount, 1)
    }

    @MainActor
    func testStallsNameTheScreenThatWasShowingAndOperationsDoNot() throws {
        let lines = Lines()
        let monitor = PerformanceMonitorService(logSink: { lines.add($0) })
        var screen = "Code > forkmesh/forkmesh"
        monitor.screenName = { screen }
        monitor.recordSlowEvent(kind: .eventLoop, name: "ui-isolate blocked", duration: .milliseconds(324))
        screen = "Inbox > Shortcuts"
        monitor.recordFrameDelay(buildDuration: .milliseconds(354), rasterDuration: .zero)
        monitor.recordSlowEvent(kind: .operation, name: "api.GET /api/tasks", duration: .seconds(1))
        monitor.recordSlowEvent(kind: .sync, name: "named", duration: .milliseconds(250), details: ["screen": "kept"])

        let events = monitor.events
        XCTAssertEqual(events.map { $0.details["screen"] }, ["kept", nil, "Inbox > Shortcuts", "Code > forkmesh/forkmesh"])
        XCTAssertTrue(lines.all.contains { $0.hasPrefix("[stall] frame frame 354ms") && $0.contains("screen=\"Inbox > Shortcuts\"") }, "\(lines.all)")
    }

    func testAStallRecordedOffTheMainThreadSkipsTheScreen() async {
        let monitor = PerformanceMonitorService(logSink: { _ in })
        await MainActor.run { monitor.screenName = { "Agents" } }
        await Task.detached {
            monitor.recordSlowEvent(kind: .eventLoop, name: "ui-isolate blocked", duration: .milliseconds(300))
        }.value
        XCTAssertNil(monitor.events.first?.details["screen"])
    }

    func testEventsAreCappedNewestFirstAndClearable() {
        let monitor = PerformanceMonitorService(maxEvents: 3, logSink: { _ in })
        for i in 0..<5 {
            monitor.recordSlowEvent(kind: .sync, name: "s\(i)", duration: .milliseconds(300))
        }
        XCTAssertEqual(monitor.events.map(\.name), ["s4", "s3", "s2"])
        XCTAssertEqual(monitor.stallCount, 5, "stallCount is never trimmed")
        XCTAssertEqual(monitor.latestEvent?.name, "s4")
        let counter = Counter()
        monitor.addListener { counter.increment() }
        monitor.clear()
        XCTAssertFalse(monitor.hasEvents)
        XCTAssertEqual(counter.value, 1)
        monitor.recordSlowEvent(kind: .sync, name: "short", duration: .milliseconds(10))
        XCTAssertFalse(monitor.hasEvents, "below the stall floor")
    }

    private func busyWait(milliseconds: Int) {
        let end = Date().addingTimeInterval(Double(milliseconds) / 1000)
        while Date() < end {}
    }

    private func sleep(_ ms: UInt64) async throws {
        try await Task.sleep(nanoseconds: ms * 1_000_000)
    }

    // The heartbeat watches its queue (the main queue in the app). Here it is a private serial
    // queue standing in for the main thread, blocked by work submitted to it.
    func testHeartbeatRecordsABlockedMainThreadWithAttribution() async throws {
        let printed = Lines()
        let ui = DispatchQueue(label: "test.ui")
        let monitor = PerformanceMonitorService(
            stallThreshold: .milliseconds(120), heartbeatInterval: .milliseconds(20), logSink: { printed.add($0) },
            heartbeatQueue: ui)
        monitor.startHeartbeat()
        try await sleep(50)

        await monitor.track("api.GET /api/repositories") {}
        ui.async { self.busyWait(milliseconds: 250) }
        try await sleep(400)
        monitor.stopHeartbeat()

        let stall = try XCTUnwrap(monitor.events.first { $0.kind == .eventLoop })
        XCTAssertTrue(stall.isStall)
        XCTAssertGreaterThanOrEqual(stall.durationMs, 120)
        XCTAssertEqual(stall.details["finishedDuring"], "api.GET /api/repositories")
        XCTAssertTrue(printed.all.last?.hasPrefix("[stall] eventLoop ui-isolate blocked") ?? false, printed.all.last ?? "")
    }

    func testTimeSyncNamesTheStallAndTheHeartbeatDoesNotRepeatIt() async throws {
        let ui = DispatchQueue(label: "test.ui")
        let monitor = PerformanceMonitorService(
            stallThreshold: .milliseconds(120), heartbeatInterval: .milliseconds(20), logSink: { _ in }, heartbeatQueue: ui)
        let lags = Lines()
        monitor.onHeartbeatLag = { lags.add("\($0)") }
        monitor.startHeartbeat()
        try await sleep(50)

        let value = ui.sync {
            monitor.timeSync("chat.decode") { () -> Int in
                busyWait(milliseconds: 200)
                return 7
            }
        }
        try await sleep(100)
        monitor.stopHeartbeat()

        XCTAssertEqual(value, 7)
        XCTAssertEqual(monitor.events.count, 1)
        XCTAssertEqual(monitor.events.first?.kind, .sync)
        XCTAssertEqual(monitor.events.first?.name, "chat.decode")
        XCTAssertFalse(lags.all.isEmpty, "every beat reports its lag")
    }

    func testAStallNamesTheShorterSectionsThatRanInsideIt() async throws {
        let ui = DispatchQueue(label: "test.ui")
        let monitor = PerformanceMonitorService(
            stallThreshold: .milliseconds(120), heartbeatInterval: .milliseconds(20), sectionFloor: .milliseconds(30),
            logSink: { _ in }, heartbeatQueue: ui)
        monitor.startHeartbeat()
        try await sleep(50)

        ui.async {
            monitor.timeSync("agents.lines") { self.busyWait(milliseconds: 50) }
            monitor.timeSync("watch.fit") { self.busyWait(milliseconds: 90) }
            monitor.timeSync("agents.eagle") { self.busyWait(milliseconds: 2) }
        }
        try await sleep(400)
        monitor.stopHeartbeat()

        XCTAssertFalse(monitor.events.contains { $0.kind == .sync }, "each section alone is under the stall budget")
        let stall = try XCTUnwrap(monitor.events.first { $0.kind == .eventLoop })
        let sections = try XCTUnwrap(stall.details["sections"]?.stringValue)
        let names = sections.components(separatedBy: ", ").map { $0.components(separatedBy: " ").first ?? "" }
        XCTAssertEqual(names, ["watch.fit", "agents.lines"], sections)

        monitor.timeSync("agents.desktopList") { busyWait(milliseconds: 40) }
        monitor.recordFrameDelay(buildDuration: .milliseconds(250), rasterDuration: .zero)
        let frame = try XCTUnwrap(monitor.events.first { $0.kind == .frame })
        XCTAssertTrue(try XCTUnwrap(frame.details["sections"]?.stringValue).hasPrefix("agents.desktopList "))
    }

    @MainActor
    func testAWatcherHearsMainThreadSectionsOfAMillisecondOrMoreOnTheUptimeClock() async {
        let monitor = PerformanceMonitorService(logSink: { _ in })
        let heard = Lines()
        let before = Double(DispatchTime.now().uptimeNanoseconds) / 1_000_000_000
        monitor.watchSections { name, start, end in
            heard.add("\(name) \(start >= before - 0.001) \(end - start >= 0.003) \(end <= Double(DispatchTime.now().uptimeNanoseconds) / 1_000_000_000)")
        }
        monitor.timeSync("surfaces.publish") { busyWait(milliseconds: 4) }
        monitor.timeSync("agents.lines") {}
        await Task.detached {
            monitor.timeSync("off.main") {
                let until = Date().addingTimeInterval(0.004)
                while Date() < until {}
            }
        }.value
        monitor.watchSections(nil)
        monitor.timeSync("after") { busyWait(milliseconds: 4) }
        XCTAssertEqual(heard.all, ["surfaces.publish true true true"])
    }

    @MainActor
    func testNestedSectionsRecordOneStallThatNamesTheInnerWork() throws {
        let monitor = PerformanceMonitorService(
            stallThreshold: .milliseconds(100), sectionFloor: .milliseconds(20), logSink: { _ in })
        monitor.timeSync("relay.event") {
            monitor.timeSync("relay.control") {
                monitor.timeSync("agents.eagle") { busyWait(milliseconds: 130) }
            }
        }
        XCTAssertEqual(monitor.events.map(\.name), ["relay.event"])
        XCTAssertEqual(monitor.stallCount, 1)
        let sections = try XCTUnwrap(monitor.events.first?.details["sections"]?.stringValue)
        XCTAssertEqual(sections.components(separatedBy: ", ").count, 2, sections)
        XCTAssertTrue(sections.contains("agents.eagle ") && sections.contains("relay.control "), sections)
    }

    func testStallsAreAppendedToDiagnosticsStallsLog() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("stall_log_test-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: root) }
        let log = StallLogFile(directory: { root }, timeZone: TimeZone(identifier: "UTC")!)
        let when = Date(epochMillis: 1_789_387_201_000) // 2026-09-14T12:00:01Z
        let monitor = PerformanceMonitorService(logSink: { _ in }, stallLog: log, now: { when })

        monitor.recordFrameDelay(
            buildDuration: .milliseconds(40), rasterDuration: .milliseconds(190), totalSpan: .milliseconds(260))
        monitor.recordFrameDelay(buildDuration: .milliseconds(5), rasterDuration: .milliseconds(5))
        await log.flush()

        let file = root.appendingPathComponent("diagnostics/stalls.log")
        XCTAssertEqual(log.path, file.path)
        let lines = try String(contentsOf: file, encoding: .utf8).split(separator: "\n").map(String.init)
        XCTAssertTrue(lines.first?.hasPrefix("--- session start") ?? false)
        XCTAssertEqual(lines.count, 2)
        // The stall line uses the local time zone of the process (DartDateFormat.iso8601).
        XCTAssertTrue(lines.last?.hasSuffix(" [stall] frame frame 260ms buildMs=40 rasterMs=190") ?? false, lines.last ?? "")
        XCTAssertTrue(lines.last?.hasPrefix(DartDateFormat.iso8601(when)) ?? false)
    }

    func testStackTracesAreIndentedInTheStallLog() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("stall_log_stack-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: root) }
        let log = StallLogFile(directory: { root })
        let monitor = PerformanceMonitorService(logSink: { _ in }, stallLog: log)
        monitor.recordSlowEvent(
            kind: .operation, name: "repos.load", duration: .milliseconds(820), details: ["screen": "Repos"],
            stackTrace: "stack line 1\nstack line 2")
        await log.flush()
        let text = try String(contentsOf: root.appendingPathComponent("diagnostics/stalls.log"), encoding: .utf8)
        XCTAssertTrue(text.contains("[perf] operation repos.load 820ms screen=Repos\n    stack line 1\n    stack line 2"))
    }

    func testFrameSourceAndLifecycleDriveTheMonitors() {
        final class Source: FrameTimingSource {
            var handler: (([FrameTiming]) -> Void)?
            var stopped = false
            func startReporting(_ handler: @escaping ([FrameTiming]) -> Void) { self.handler = handler }
            func stopReporting() { stopped = true }
        }
        let source = Source()
        let monitor = PerformanceMonitorService(stallThreshold: .milliseconds(20), heartbeatInterval: .seconds(10), logSink: { _ in })
        monitor.installStallMonitors(frameSource: source, lifecycleState: .paused)
        monitor.installStallMonitors(frameSource: Source(), lifecycleState: .resumed) // idempotent
        let slow = FrameTiming(buildDuration: .milliseconds(20), rasterDuration: .milliseconds(10), vsyncOverhead: .milliseconds(3))
        source.handler?([slow])
        XCTAssertTrue(monitor.events.isEmpty, "a hidden app has no frames to be late")
        monitor.appLifecycleChanged(.resumed)
        Thread.sleep(forTimeInterval: 0.08)
        source.handler?([slow, FrameTiming(buildDuration: .milliseconds(1), rasterDuration: .milliseconds(1))])
        XCTAssertEqual(monitor.events.count, 1)
        XCTAssertEqual(monitor.events.first?.details["vsyncMs"], 3)
        monitor.appLifecycleChanged(.paused)
        monitor.uninstallStallMonitors()
        XCTAssertTrue(source.stopped)
    }
}

extension PerformanceMonitorTests {
    func testAFrameGapReachingBackIntoTheBackgroundIsNotAStall() {
        final class Source: FrameTimingSource {
            var handler: (([FrameTiming]) -> Void)?
            func startReporting(_ handler: @escaping ([FrameTiming]) -> Void) { self.handler = handler }
            func stopReporting() {}
        }
        let source = Source()
        let monitor = PerformanceMonitorService(heartbeatInterval: .seconds(10), logSink: { _ in })
        monitor.installStallMonitors(frameSource: source, lifecycleState: .resumed)
        monitor.appLifecycleChanged(.paused)
        monitor.appLifecycleChanged(.resumed)
        let suspended = Duration.milliseconds(54_137)
        source.handler?([FrameTiming(buildDuration: suspended, rasterDuration: .zero, totalSpan: suspended)])
        XCTAssertTrue(monitor.events.isEmpty)
        XCTAssertEqual(monitor.stallCount, 0)
        monitor.uninstallStallMonitors()
    }

    func testALateCallbackCountsOnlyTheTimePastItsPeriod() throws {
        XCTAssertNil(FrameTiming.lateCallback(interval: .milliseconds(100), period: .milliseconds(100)))
        XCTAssertNil(FrameTiming.lateCallback(interval: .milliseconds(200), period: .milliseconds(100)))
        XCTAssertNil(FrameTiming.lateCallback(interval: .milliseconds(200), period: .zero))
        let late = try XCTUnwrap(FrameTiming.lateCallback(interval: .milliseconds(221), period: .milliseconds(100)))
        XCTAssertEqual(late.buildDuration, .milliseconds(121))
        XCTAssertEqual(late.rasterDuration, .zero)
        XCTAssertEqual(late.vsyncOverhead, .milliseconds(100))
        XCTAssertNil(late.totalSpan)
    }

    func testOneSkippedTickOfTheTenHertzProbeIsNotAStall() {
        final class Source: FrameTimingSource {
            var handler: (([FrameTiming]) -> Void)?
            func startReporting(_ handler: @escaping ([FrameTiming]) -> Void) { self.handler = handler }
            func stopReporting() {}
        }
        let source = Source()
        let monitor = PerformanceMonitorService(heartbeatInterval: .seconds(10), logSink: { _ in })
        monitor.installStallMonitors(frameSource: source, lifecycleState: .resumed)
        Thread.sleep(forTimeInterval: 0.4)
        let period = Duration.milliseconds(100)
        let reported = [201, 215, 221].compactMap { FrameTiming.lateCallback(interval: .milliseconds($0), period: period) }
        source.handler?(reported)
        XCTAssertTrue(monitor.events.isEmpty, "a 100-121 ms hold of the main thread is under the 200 ms stall budget")
        let twoTicks = FrameTiming.lateCallback(interval: .milliseconds(320), period: period)
        source.handler?(twoTicks.map { [$0] } ?? [])
        XCTAssertEqual(monitor.events.count, 1)
        XCTAssertEqual(monitor.events.first?.durationMs, 220)
        XCTAssertEqual(monitor.events.first?.details["buildMs"], 220)
        XCTAssertEqual(monitor.events.first?.details["vsyncMs"], 100)
        monitor.uninstallStallMonitors()
    }
}

final class StallLogFileTests: XCTestCase {
    func testStallLogRotatesPastItsSizeCap() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("stall_log_rotate-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: root) }
        let log = StallLogFile(directory: { root }, maxBytes: 400)
        for i in 0..<10 { log.append("record \(i) \(String(repeating: "x", count: 60))") }
        await log.flush()

        let current = root.appendingPathComponent("diagnostics/stalls.log")
        let rotated = root.appendingPathComponent("diagnostics/stalls.log.1")
        XCTAssertTrue(FileManager.default.fileExists(atPath: rotated.path))
        let size = (try FileManager.default.attributesOfItem(atPath: current.path)[.size] as? NSNumber)?.intValue ?? 0
        XCTAssertLessThanOrEqual(size, 400)
        XCTAssertTrue(try String(contentsOf: current, encoding: .utf8).contains("record 9"))
    }

    func testStallLogWithoutStorageDropsRecordsQuietly() async {
        struct NoStorage: Error {}
        let log = StallLogFile(directory: { throw NoStorage() })
        log.append("lost")
        await log.flush()
        XCTAssertNil(log.path)
        let tail = await log.tail(maxBytes: 1024)
        XCTAssertEqual(tail, "")
    }

    func testTailReadsTheRotatedFileThenTheCurrentOneFromAWholeLine() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("stall_log_tail-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: root) }
        let log = StallLogFile(directory: { root }, maxBytes: 400)
        for i in 0..<10 { log.append("record \(i) \(String(repeating: "x", count: 60))") }
        let whole = await log.tail(maxBytes: 1 << 20)
        XCTAssertTrue(whole.contains("record 9"))
        XCTAssertLessThan(whole.range(of: "record 4")!.lowerBound, whole.range(of: "record 9")!.lowerBound)
        let short = await log.tail(maxBytes: 100)
        XCTAssertLessThanOrEqual(short.utf8.count, 100)
        XCTAssertTrue(short.hasPrefix("record 9"))
    }

    func testTailOfALogNeverWrittenIsEmpty() async {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("stall_log_none-\(UUID().uuidString)")
        let tail = await StallLogFile(directory: { root }).tail(maxBytes: 1024)
        XCTAssertEqual(tail, "")
    }
}
