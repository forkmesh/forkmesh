import FMPackages
import Foundation
import XCTest

final class DjVitalsTests: XCTestCase {
    let period = 1.0 / 60

    func run(_ vitals: inout DjVitals, from start: Double, frames: Int) -> Double {
        var now = start
        for _ in 0..<frames {
            now += period
            vitals.frame(at: now, period: period)
        }
        return now
    }

    func testSteadyFramesAreSmoothAndRaiseNoAlert() {
        var vitals = DjVitals()
        vitals.begin(at: 100)
        let now = run(&vitals, from: 100, frames: 600)
        let live = vitals.live(now: now)
        XCTAssertEqual(live.fps, 60, accuracy: 0.5)
        XCTAssertEqual(live.dropped, 0)
        XCTAssertEqual(live.hitchRatio, 0)
        XCTAssertEqual(live.probeHz, 60, accuracy: 0.5)
        XCTAssertTrue(live.smooth)
        XCTAssertFalse(vitals.alert(now: now).detected)
        XCTAssertTrue(vitals.longFrames.isEmpty)
    }

    func testTheFirstFrameAfterOpeningAndLongPausesAreNotCountedAsHitches() {
        var vitals = DjVitals()
        vitals.begin(at: 10)
        vitals.frame(at: 10.5, period: period)
        vitals.frame(at: 13, period: period)
        XCTAssertEqual(vitals.frames, 0)
        vitals.end(at: 13)
        vitals.begin(at: 20)
        vitals.frame(at: 20.2, period: period)
        XCTAssertEqual(vitals.frames, 0)
        XCTAssertEqual(vitals.dropped, 0)
    }

    func testAHeldFrameCountsDroppedFramesAndHitchTime() {
        var vitals = DjVitals()
        vitals.begin(at: 0)
        var now = run(&vitals, from: 0, frames: 10)
        now += 2 * period
        vitals.frame(at: now, period: period)
        XCTAssertEqual(vitals.dropped, 1)
        XCTAssertEqual(vitals.hitchMs, period * 1000, accuracy: 0.01)
        XCTAssertEqual(vitals.worstMs, 2 * period * 1000, accuracy: 0.01)
        XCTAssertTrue(vitals.longFrames.isEmpty)
    }

    func testALongFrameNamesTheWorkInsideItAndWhatHappenedJustBefore() {
        var vitals = DjVitals()
        vitals.begin(at: 0)
        var now = run(&vitals, from: 0, frames: 120)
        vitals.mark("start A", at: now + 0.01)
        vitals.work(.tick, ms: 0.2, at: now + 0.02)
        vitals.work(.wave, ms: 62, at: now + 0.07)
        now += 0.08
        vitals.frame(at: now, period: period)
        vitals.work(.save, ms: 9, at: now + 0.001)
        XCTAssertEqual(vitals.recentWork.map(\.kind), [.wave, .save])
        XCTAssertEqual(vitals.work[.tick]?.count, 1)
        let long = try? XCTUnwrap(vitals.longFrames.last)
        XCTAssertEqual(long?.ms ?? 0, 80, accuracy: 0.01)
        XCTAssertEqual(long?.work.map(\.kind), [.wave])
        XCTAssertEqual(long?.marks.map(\.text), ["start A"])
        let report = vitals.report(now: now, alert: .none, context: [])
        XCTAssertTrue(report.contains("80 ms, main thread busy 0 ms, layout and drawing 0 ms, 0 wakes, 0 studio view updates; inside: waveform draw 62.0 ms; around: start A"), report)
    }

    func testLongFramesTellAMainThreadThatWasBusyFromOneThatWasWaiting() {
        var vitals = DjVitals()
        vitals.begin(at: 0)
        var now = run(&vitals, from: 0, frames: 60)
        vitals.awake(at: now + 0.001)
        vitals.asleep(at: now + 0.061)
        now += 0.07
        vitals.frame(at: now, period: period)
        vitals.awake(at: now + 0.002)
        vitals.asleep(at: now + 0.004)
        now += 0.07
        vitals.awake(at: now - 0.01)
        vitals.frame(at: now, period: period)
        XCTAssertEqual(vitals.longFrames.map(\.busyMs).first ?? 0, 60, accuracy: 0.01)
        XCTAssertEqual(vitals.longFrames.map(\.busyMs).last ?? 0, 12, accuracy: 0.01)
        vitals.asleep(at: now + 0.001)
        now += period
        vitals.frame(at: now, period: period)
        XCTAssertEqual(vitals.busyMs, 73, accuracy: 0.01)
        XCTAssertEqual(vitals.window(10, now: now).busyMs, 73, accuracy: 0.01)
        let report = vitals.report(now: now, alert: .none, context: [])
        XCTAssertTrue(report.contains("70 ms, main thread busy 60 ms"), report)
        XCTAssertTrue(report.contains("70 ms, main thread busy 12 ms"), report)
        XCTAssertTrue(report.contains("main thread busy ms/s"), report)
    }

    func testBusyTimeIsSplitIntoLayoutAndDrawingAndTheRest() {
        var vitals = DjVitals()
        vitals.begin(at: 0)
        var now = run(&vitals, from: 0, frames: 60)
        vitals.awake(at: now + 0.001)
        vitals.committing(at: now + 0.041)
        vitals.asleep(at: now + 0.061)
        now += 0.07
        vitals.frame(at: now, period: period)
        vitals.committing(at: now + 0.002)
        vitals.awake(at: now + 0.003)
        vitals.asleep(at: now + 0.005)
        now += period
        vitals.frame(at: now, period: period)
        XCTAssertEqual(vitals.longFrames.last?.busyMs ?? 0, 60, accuracy: 0.01)
        XCTAssertEqual(vitals.longFrames.last?.commitMs ?? 0, 20, accuracy: 0.01)
        XCTAssertEqual(vitals.commitMs, 20, accuracy: 0.01)
        XCTAssertEqual(vitals.window(10, now: now).commitMs, 20, accuracy: 0.01)
        let report = vitals.report(now: now, alert: .none, context: [])
        XCTAssertTrue(report.contains("70 ms, main thread busy 60 ms, layout and drawing 20 ms"), report)
        XCTAssertTrue(report.contains("of it layout and drawing ms/s"), report)
    }

    func testBusyTimeWithNoLayoutAndDrawingSaysItWasNotSeenInsteadOfZero() {
        var vitals = DjVitals()
        vitals.begin(at: 0)
        var now = run(&vitals, from: 0, frames: 60)
        vitals.awake(at: now + 0.001)
        vitals.asleep(at: now + 0.157)
        now += 0.16
        vitals.frame(at: now, period: period)
        now = run(&vitals, from: now, frames: 2)
        XCTAssertFalse(vitals.commitSeen)
        let report = vitals.report(now: now, alert: .none, context: [])
        XCTAssertTrue(report.contains("of it layout and drawing ms/s " + DjVitals.commitUnseen), report)
        XCTAssertTrue(report.contains("160 ms, main thread busy 156 ms, 1 wakes"), report)
        XCTAssertFalse(report.contains("layout and drawing 0 ms"), report)
    }

    func testACommitSliverInMinutesOfBusyTimeStillSaysNotSeenInsteadOfZero() {
        var vitals = DjVitals()
        vitals.begin(at: 0)
        var now = run(&vitals, from: 0, frames: 60)
        vitals.awake(at: now + 0.001)
        vitals.committing(at: now + 0.1555)
        vitals.asleep(at: now + 0.156)
        now += 0.16
        vitals.frame(at: now, period: period)
        now = run(&vitals, from: now, frames: 2)
        XCTAssertGreaterThan(vitals.commitMs, 0)
        XCTAssertFalse(vitals.commitSeen)
        let report = vitals.report(now: now, alert: .none, context: [])
        XCTAssertTrue(report.contains("of it layout and drawing ms/s " + DjVitals.commitUnseen), report)
        XCTAssertFalse(report.contains("layout and drawing 0 ms"), report)
    }

    func testLongFramesTellComputingFromWaitingByTheMainThreadsCpuTime() {
        var vitals = DjVitals()
        vitals.begin(at: 0)
        var now = 0.0
        var cpu = 5.0
        var app = 20.0
        for _ in 0..<60 {
            now += period
            cpu += 0.002
            app += 0.006
            vitals.awake(at: now - 0.004)
            vitals.asleep(at: now - 0.002)
            vitals.frame(at: now, period: period, cpu: cpu, appCpu: app)
        }
        vitals.awake(at: now + 0.001)
        vitals.asleep(at: now + 0.151)
        now += 0.16
        cpu += 0.03
        app += 0.4
        vitals.frame(at: now, period: period, cpu: cpu, appCpu: app)
        let long = try? XCTUnwrap(vitals.longFrames.last)
        XCTAssertEqual(long?.busyMs ?? 0, 150, accuracy: 0.01)
        XCTAssertEqual(long?.cpuMs ?? 0, 30, accuracy: 0.01)
        XCTAssertEqual(long?.wakes, 1)
        XCTAssertTrue(vitals.cpuMeasured)
        XCTAssertEqual(vitals.cpuMs, 59 * 2 + 30, accuracy: 0.01)
        XCTAssertEqual(vitals.appCpuMs, 59 * 6 + 400, accuracy: 0.01)
        XCTAssertEqual(vitals.wakes, 60)
        let report = vitals.report(now: now, alert: .none, context: [])
        XCTAssertTrue(report.contains("160 ms, main thread busy 150 ms, on the CPU 30 ms, 1 wakes, 0 studio view updates"), report)
        XCTAssertTrue(report.contains("of it on the CPU ms/s "), report)
        XCTAssertTrue(report.contains("run-loop wakes a second "), report)
        XCTAssertTrue(report.contains("app CPU, all threads ms/s "), report)
    }

    func testStudioViewUpdatesAreCountedPerViewAndPerLongFrame() {
        var vitals = DjVitals()
        vitals.begin(at: 0)
        var now = 0.0
        for _ in 0..<120 {
            vitals.viewed("waveform")
            now += period
            vitals.frame(at: now, period: period)
        }
        for _ in 0..<40 { vitals.viewed("deck info") }
        vitals.viewed("waveform")
        now += 0.08
        vitals.frame(at: now, period: period)
        XCTAssertEqual(vitals.views["waveform"], 121)
        XCTAssertEqual(vitals.views["deck info"], 40)
        XCTAssertEqual(vitals.longFrames.last?.views, 41)
        let report = vitals.report(now: now, alert: .none, context: [])
        XCTAssertTrue(report.contains("## Studio view updates (count, a second measured)\nwaveform: 121, "), report)
        XCTAssertTrue(report.contains("\ndeck info: 40, "), report)
        XCTAssertTrue(report.contains("80 ms, main thread busy 0 ms, layout and drawing 0 ms, 0 wakes, 41 studio view updates"), report)
    }

    func testTheReportHasTheBusyTimeOfEachSecond() {
        var vitals = DjVitals()
        vitals.begin(at: 100)
        var now = 100.0
        for second in 0..<3 {
            for _ in 0..<60 {
                vitals.awake(at: now + 0.001)
                vitals.asleep(at: now + 0.001 + Double(second + 1) * 0.002)
                now += period
                vitals.frame(at: now, period: period)
            }
        }
        let report = vitals.report(now: now, alert: .none, context: [])
        let line = report.split(separator: "\n").first { $0.hasPrefix("main thread busy each second") }.map(String.init) ?? ""
        XCTAssertTrue(line.hasSuffix(" 120 240 360") || line.hasSuffix(" 120 240 360 ."), line)
    }

    func testAppWorkInsideALongFrameIsNamedAndSummed() {
        var vitals = DjVitals()
        vitals.begin(at: 0)
        var now = run(&vitals, from: 0, frames: 60)
        vitals.section("surfaces.publish", ms: 120, at: now + 0.13)
        vitals.section("agents.eagle", ms: 0.5, at: now + 0.131)
        now += 0.14
        vitals.frame(at: now, period: period)
        vitals.section("surfaces.publish", ms: 30, at: now + 0.01)
        XCTAssertEqual(vitals.longFrames.last?.work.map(\.name), ["app surfaces.publish"])
        XCTAssertEqual(vitals.appWork["surfaces.publish"]?.count, 2)
        XCTAssertEqual(vitals.appWork["surfaces.publish"]?.maxMs ?? 0, 120, accuracy: 0.01)
        XCTAssertNil(vitals.work[.app])
        let report = vitals.report(now: now, alert: .none, context: [])
        XCTAssertTrue(report.contains("inside: app surfaces.publish 120.0 ms"), report)
        XCTAssertTrue(report.contains("## App work on the main thread while the studio was on screen"), report)
        XCTAssertTrue(report.contains("surfaces.publish: 2, 75.00, 120.0, 150"), report)
        XCTAssertTrue(report.contains("agents.eagle: 1, 0.50, 0.5, 1"), report)
    }

    func testTheReportSaysWhatTheCountersCover() {
        var vitals = DjVitals()
        vitals.tick(.a, at: 10, playing: true, drift: nil)
        vitals.work(.automix, ms: 200, at: 8.2)
        vitals.begin(at: 300)
        let now = run(&vitals, from: 300, frames: 120)
        let report = vitals.report(now: now, alert: .none, context: [])
        XCTAssertEqual(vitals.counting ?? 0, 8, accuracy: 0.0001)
        XCTAssertTrue(report.contains("measured 2s of frames over 2s since the studio first opened"), report)
        XCTAssertTrue(report.contains("counted over 4m54s, with the studio open or closed"), report)
    }

    func testAFreezeRaisesAnAlertThatFadesAfterHalfAMinute() {
        var vitals = DjVitals()
        vitals.begin(at: 0)
        var now = run(&vitals, from: 0, frames: 30)
        now += 0.4
        vitals.frame(at: now, period: period)
        XCTAssertEqual(vitals.alert(now: now).reason, "the studio froze for 400 ms")
        let later = run(&vitals, from: now, frames: 60 * 31)
        XCTAssertFalse(vitals.alert(now: later).detected)
    }

    func testSteadyStutterRaisesTheHitchRatioAlert() {
        var vitals = DjVitals()
        vitals.begin(at: 0)
        var now = 0.0
        for _ in 0..<200 {
            now += period
            vitals.frame(at: now, period: period)
            now += 2 * period
            vitals.frame(at: now, period: period)
        }
        let alert = vitals.alert(now: now)
        XCTAssertTrue(alert.reason.hasPrefix("the studio dropped"), alert.reason)
        XCTAssertFalse(vitals.live(now: now).smooth)
    }

    func testLatePlayerTicksAreCountedOnlyWhilePlaying() {
        var vitals = DjVitals()
        vitals.tick(.a, at: 1, playing: true, drift: 0.01)
        vitals.tick(.a, at: 1.25, playing: true, drift: -0.02)
        vitals.tick(.a, at: 2.25, playing: true, drift: 0.4)
        vitals.tick(.b, at: 1, playing: false, drift: nil)
        vitals.tick(.b, at: 9, playing: false, drift: nil)
        let a = vitals.ticks[.a]
        XCTAssertEqual(a?.count, 3)
        XCTAssertEqual(a?.late, 1)
        XCTAssertEqual(a?.maxGap ?? 0, 1, accuracy: 0.0001)
        XCTAssertEqual(a?.maxDrift ?? 0, 0.4, accuracy: 0.0001)
        XCTAssertEqual(a?.snaps, 1)
        XCTAssertEqual(vitals.ticks[.b]?.late, 0)
        XCTAssertEqual(vitals.alert(now: 3).reason, "Deck A's player went 1.0 s without reporting its position")
    }

    func testWorkMetersCountTheBudget() {
        var vitals = DjVitals()
        vitals.work(.fade, ms: 1, at: 1)
        vitals.work(.fade, ms: 6, at: 2)
        let meter = vitals.work[.fade]
        XCTAssertEqual(meter?.count, 2)
        XCTAssertEqual(meter?.averageMs ?? 0, 3.5, accuracy: 0.0001)
        XCTAssertEqual(meter?.maxMs, 6)
        XCTAssertEqual(meter?.over, 1)
    }

    func testRecentMomentsCoverTheAlertsThirtySecondsNotJustTheLastSixteen() {
        var vitals = DjVitals()
        vitals.begin(at: 0)
        for index in 0..<24 { vitals.mark("load \(index)", at: 100 + Double(index)) }
        let report = vitals.report(now: 124, alert: .none, context: [])
        XCTAssertTrue(report.contains("-24.0 s load 0"), report)
        XCTAssertTrue(report.contains("-1.0 s load 23"), report)
        for index in 0..<10 { vitals.mark("late \(index)", at: 200 + Double(index)) }
        let later = vitals.report(now: 260, alert: .none, context: [])
        XCTAssertTrue(later.contains("load 18"), later)
        XCTAssertFalse(later.contains("load 17"), later)
    }

    func testRingsStayBounded() {
        var vitals = DjVitals()
        vitals.begin(at: 0)
        for index in 0..<500 {
            vitals.mark("m\(index)", at: Double(index))
            vitals.work(.tick, ms: 2, at: Double(index))
        }
        var now = 0.0
        for _ in 0..<40 {
            now += 0.1
            vitals.frame(at: now, period: period)
        }
        _ = run(&vitals, from: now, frames: 60 * 200)
        XCTAssertEqual(vitals.marks.count, DjVitals.marksKept)
        XCTAssertEqual(vitals.recentWork.count, DjVitals.workKept)
        XCTAssertEqual(vitals.longFrames.count, DjVitals.longFramesKept)
        XCTAssertLessThanOrEqual(vitals.seconds.count, DjVitals.secondsKept)
    }

    func testTheReportHasEverySectionAndTheContext() {
        var vitals = DjVitals()
        vitals.begin(at: 0)
        let now = run(&vitals, from: 0, frames: 120)
        vitals.scriptSent()
        vitals.scriptReturned(ms: 12)
        let report = vitals.report(now: now, alert: DjVitalsAlert(reason: "x"), context: [("decks", "A playing")])
        for heading in ["# DJ studio vitals", "alert: x", "## Frames", "## Long frames", "## Main-thread work", "## Player ticks",
                        "## Web view scripts", "## Recent moments", "## Studio and device", "decks: A playing",
                        "## App work on the main thread", "## Studio view updates"] {
            XCTAssertTrue(report.contains(heading), heading)
        }
        XCTAssertTrue(report.contains("probe 60 Hz"), report)
        XCTAssertLessThan(report.count, 3000)
    }

    func testTheLeadSendsTheAgentToThePlaybookAndAsksItToImproveIt() throws {
        let lead = DjVitals.lead(reason: "the studio froze for 400 ms")
        XCTAssertTrue(lead.hasPrefix("The DJ studio detected a performance issue: the studio froze for 400 ms."))
        XCTAssertTrue(lead.contains("First read \(DjVitals.playbook)"))
        XCTAssertTrue(lead.contains("add this check to that page"))
        XCTAssertTrue(lead.contains("make the studio collect it"))
        XCTAssertTrue(DjVitals.lead(reason: "").hasPrefix("The user asked for a DJ studio performance check."))
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0..<6 { root.deleteLastPathComponent() }
        let page = try String(contentsOf: root.appendingPathComponent(DjVitals.playbook), encoding: .utf8)
        for heading in ["## Budgets", "## Reading the bundle", "## Checks so far", "## Adding a check"] {
            XCTAssertTrue(page.contains(heading), heading)
        }
    }

    func testTheRecorderIsSafeFromManyThreads() {
        let recorder = DjVitalsRecorder()
        DispatchQueue.concurrentPerform(iterations: 200) { index in
            recorder.work(.wave, from: Double(index), to: Double(index) + 0.001)
            recorder.mark("m", at: Double(index))
        }
        XCTAssertEqual(recorder.snapshot.work[.wave]?.count, 200)
        DispatchQueue.concurrentPerform(iterations: 200) { _ in recorder.viewed("record") }
        XCTAssertEqual(recorder.snapshot.views["record"], 200)
    }
}
