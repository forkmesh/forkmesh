import FMBase
import Foundation
import XCTest

@testable import FMChat

final class CallDiagnosticsTests: XCTestCase {
    static let fixture: JSONObject = {
        var url = URL(fileURLWithPath: #filePath)
        url.deleteLastPathComponent()
        url.appendPathComponent("Fixtures/call-diagnostics-parity.json")
        guard let data = try? Data(contentsOf: url) else { return [:] }
        return JSON.parseObject(data)
    }()

    func sample(_ entry: JSONObject) -> CallQualitySample {
        func number(_ key: String) -> Double? { entry[key]?.doubleValue }
        func whole(_ key: String) -> Int? { number(key).map { Int($0) } }
        return CallQualitySample(
            at: number("at") ?? 0, rttMs: number("rttMs"), lossPct: number("lossPct"), jitterMs: number("jitterMs"),
            sendKbps: number("sendKbps"), recvKbps: number("recvKbps"), inWidth: whole("inWidth"),
            inHeight: whole("inHeight"), inFps: number("inFps"), outWidth: whole("outWidth"),
            outHeight: whole("outHeight"), outFps: number("outFps"), limit: entry.str("limit"),
            faceFps: number("faceFps"), faceMs: number("faceMs"), faceLevel: number("faceLevel"))
    }

    func memory(_ value: JSONValue) -> CallDiagnosticsMemory {
        CallDiagnosticsMemory(json: JSON.encode(value))
    }

    func environment(_ entry: JSONObject) -> CallDiagnosticsEnvironment {
        CallDiagnosticsEnvironment(
            userAgent: entry.str("userAgent"), cores: entry.int("cores"), memory: entry.double("memory"),
            connection: entry.str("connection"))
    }

    func same(_ expected: JSONValue?, _ actual: Double?, _ label: String) {
        guard let expected, expected != .null else {
            XCTAssertNil(actual, label)
            return
        }
        guard let want = expected.doubleValue, let got = actual else {
            XCTFail("\(label): expected \(expected), got \(String(describing: actual))")
            return
        }
        XCTAssertEqual(got, want, accuracy: 1e-9, label)
    }

    func testTheFixtureCoversTheWebModule() throws {
        let cases = Self.fixture.maps("cases")
        XCTAssertGreaterThanOrEqual(cases.count, 15, "run node tools/call_diagnostics_parity.mjs")
        let severities = Set(cases.map { $0.obj("diagnosis").str("severity") })
        XCTAssertEqual(severities, ["unknown", "ok", "warn", "bad"])
        let ids = Set(cases.flatMap { $0.obj("diagnosis").maps("issues").map { $0.str("id") } })
        XCTAssertEqual(ids, Set(CallDiagnostics.kinds.keys))
        let constants = Self.fixture.obj("constants")
        XCTAssertEqual(constants.int("window"), CallDiagnostics.window)
        XCTAssertEqual(constants.int("minSamples"), CallDiagnostics.minSamples)
        XCTAssertEqual(constants.int("reports"), CallDiagnostics.reports)
        XCTAssertEqual(constants.int("notes"), CallDiagnostics.notes)
        XCTAssertEqual(constants.int("noteChars"), CallDiagnostics.noteChars)
        XCTAssertEqual(constants.double("videoMinFps"), CallDiagnostics.videoMinFps)
        XCTAssertEqual(constants.double("videoMinHeight"), CallDiagnostics.videoMinHeight)
        XCTAssertEqual(constants.double("videoMinKbps"), CallDiagnostics.videoMinKbps)
        XCTAssertEqual(constants.double("faceSlowMs"), CallDiagnostics.faceSlowMs)
        XCTAssertEqual(constants.double("faceBadMs"), CallDiagnostics.faceBadMs)
        XCTAssertEqual(constants.str("key"), CallDiagnostics.key)
    }

    func testDiagnosesMatchTheWebModule() throws {
        let memories = Self.fixture.list("memories").map(memory)
        let environments = Self.fixture.maps("environments").map(environment)
        XCTAssertEqual(memories.count, 3)
        for entry in Self.fixture.maps("cases") {
            let name = entry.str("name")
            let options = entry.obj("options")
            let samples = entry.maps("samples").map(sample)
            let diagnosis = CallDiagnostics.diagnose(
                samples, video: options.bool("video"), sending: options.optBool("sending") ?? true,
                window: options.int("window", CallDiagnostics.window))
            let expected = entry.obj("diagnosis")
            same(expected["at"], diagnosis.at, "\(name) at")
            XCTAssertEqual(diagnosis.severity.rawValue, expected.str("severity"), name)
            let issues = expected.maps("issues")
            XCTAssertEqual(diagnosis.issues.map(\.id), issues.map { $0.str("id") }, name)
            for (issue, want) in zip(diagnosis.issues, issues) {
                XCTAssertEqual(issue.severity.rawValue, want.str("severity"), "\(name) \(issue.id)")
                XCTAssertEqual(issue.title, want.str("title"))
                XCTAssertEqual(issue.area, want.str("area"))
                XCTAssertEqual(issue.cause, want.str("cause"))
                XCTAssertEqual(issue.detail, want.str("detail"), "\(name) \(issue.id)")
                XCTAssertEqual(issue.fixes.map(\.id), want.maps("fixes").map { $0.str("id") })
                XCTAssertEqual(issue.fixes.map(\.text), want.maps("fixes").map { $0.str("text") })
            }
            let metrics = expected.obj("metrics")
            XCTAssertEqual(diagnosis.metrics.samples, metrics.int("samples"), name)
            for (key, value) in diagnosis.metrics.entries { same(metrics[key], value, "\(name) \(key)") }
            XCTAssertEqual(CallDiagnostics.describe(diagnosis.metrics), entry.strings("lines"), name)
            for (index, ranked) in entry.list("ranked").enumerated() {
                let lists = ranked.arrayOrEmpty
                for (issue, list) in zip(diagnosis.issues, lists) {
                    let mine = CallDiagnostics.rankFixes(issue, memories[index])
                    XCTAssertEqual(mine.map(\.id), list.objects.map { $0.str("id") }, "\(name) \(issue.id) memory \(index)")
                    XCTAssertEqual(mine.map(\.worked), list.objects.map { $0.int("worked") })
                    XCTAssertEqual(mine.map(\.failed), list.objects.map { $0.int("failed") })
                }
            }
            for (index, prompt) in entry.strings("prompts").enumerated() {
                let mine = CallDiagnostics.prompt(
                    diagnosis, memories[index], environment: environments[index % environments.count])
                XCTAssertEqual(mine, prompt, "\(name) prompt \(index)")
            }
            var remembered = memories[1]
            CallDiagnostics.rememberReport(&remembered, diagnosis, now: -1)
            let report = entry.obj("remembered")
            let last = try XCTUnwrap(remembered.reports.last)
            if diagnosis.at != nil { same(report["at"], last.at, "\(name) remembered at") }
            XCTAssertEqual(last.issues, report.strings("issues"), name)
            let wanted = report.obj("metrics")
            XCTAssertEqual(Set(last.metrics.keys), Set(wanted.keys), name)
            for key in wanted.keys { same(wanted[key], last.metrics[key]?.doubleValue, "\(name) remembered \(key)") }
        }
    }

    func testLessonsAndMemoryMatchTheWebModule() throws {
        var memory = memory(try XCTUnwrap(Self.fixture.list("memories").dropFirst().first))
        for entry in Self.fixture.maps("notes") {
            let ok = CallDiagnostics.rememberNote(&memory, entry.str("text"))
            XCTAssertEqual(ok, entry.bool("ok"), entry.str("text"))
            XCTAssertEqual(memory.notes, entry.strings("notes"), entry.str("text"))
        }
        var crowded = CallDiagnosticsMemory()
        for index in 0..<25 {
            let samples = (0..<3).map { CallQualitySample(at: Double(index * 10 + $0 * 1000), rttMs: 400) }
            CallDiagnostics.rememberReport(&crowded, CallDiagnostics.diagnose(samples))
        }
        for index in 0..<10 { CallDiagnostics.rememberNote(&crowded, "note \(index)") }
        let expected = Self.fixture.obj("crowded")
        XCTAssertEqual(crowded.reports.count, expected.int("reports"))
        XCTAssertEqual(crowded.reports.first?.at, expected.double("firstAt"))
        XCTAssertEqual(crowded.notes, expected.strings("notes"))
        let stored = CallDiagnosticsMemory(json: Self.fixture.str("stored"))
        XCTAssertEqual(stored.notes.count, 2)
        XCTAssertEqual(stored.record("latency", "vpn"), CallFixRecord(worked: 2, failed: 0))
        XCTAssertEqual(stored.reports.map(\.at), [1, 2, 27000])
        XCTAssertEqual(CallDiagnosticsMemory(json: stored.json), stored)
        XCTAssertEqual(CallDiagnosticsMemory(json: "not json"), CallDiagnosticsMemory())
        XCTAssertEqual(CallDiagnosticsMemory(json: nil), CallDiagnosticsMemory())
    }

    func testNumbersFormatLikeJavaScript() {
        XCTAssertEqual(CallDiagnostics.fixed(3.25, 1), "3.3")
        XCTAssertEqual(CallDiagnostics.fixed(0.125, 2), "0.13")
        XCTAssertEqual(CallDiagnostics.fixed(1.005, 2), "1.00")
        XCTAssertEqual(CallDiagnostics.fixed(9.96, 1), "10.0")
        XCTAssertEqual(CallDiagnostics.fixed(99.5, 0), "100")
        XCTAssertEqual(CallDiagnostics.fixed(0, 1), "0.0")
        XCTAssertEqual(CallDiagnostics.round(300.5), 301)
        XCTAssertEqual(CallDiagnostics.round(2.4999), 2)
        XCTAssertEqual(CallDiagnostics.text(150), "150")
        XCTAssertEqual(CallDiagnostics.text(5.5), "5.5")
        XCTAssertEqual(CallDiagnostics.formatKbps(1250), "1.3 Mb/s")
        XCTAssertEqual(CallDiagnostics.formatKbps(999.6), "1000 kb/s")
        XCTAssertEqual(CallQuality.formatLoss(3.25), "3.3% loss")
    }

    func testFixesThatClearAProblemAreRememberedAsWorking() {
        let bad = (0..<4).map { CallQualitySample(at: Double($0), rttMs: 600, lossPct: 0, jitterMs: 4) }
        let diagnosis = CallDiagnostics.diagnose(bad)
        XCTAssertEqual(diagnosis.issues.map(\.id), ["latency"])
        XCTAssertTrue(diagnosis.problem)
        XCTAssertEqual(diagnosis.summary, "1 problem found: high latency")
        XCTAssertEqual(diagnosis.heading, "1 problem found")
        var memory = CallDiagnosticsMemory()
        var tracker = CallDiagnosticsTracker()
        tracker.opened(diagnosis, memory: &memory)
        tracker.opened(diagnosis, memory: &memory)
        XCTAssertEqual(memory.reports.count, 1)
        XCTAssertEqual(tracker.pending.map(\.fix), ["wired"])
        XCTAssertFalse(tracker.refreshed(CallDiagnostics.diagnose([]), memory: &memory))
        XCTAssertFalse(tracker.refreshed(diagnosis, memory: &memory))
        let good = (0..<4).map { CallQualitySample(at: Double(10 + $0), rttMs: 40, lossPct: 0, jitterMs: 4) }
        let cleared = CallDiagnostics.diagnose(good)
        XCTAssertEqual(cleared.summary, "No problems found")
        XCTAssertTrue(tracker.refreshed(cleared, memory: &memory))
        XCTAssertEqual(memory.record("latency", "wired"), CallFixRecord(worked: 1, failed: 0))
        XCTAssertTrue(tracker.pending.isEmpty)
        tracker.opened(diagnosis, memory: &memory)
        XCTAssertEqual(tracker.pending.map(\.fix), ["wired"])
        XCTAssertTrue(tracker.finished(memory: &memory))
        XCTAssertEqual(memory.record("latency", "wired"), CallFixRecord(worked: 1, failed: 1))
        XCTAssertFalse(tracker.finished(memory: &memory))
        XCTAssertEqual(CallDiagnostics.fixLabel(CallDiagnostics.rankFixes(diagnosis.issues[0], memory)[0]),
            "Use a wired connection, or move closer to the Wi-Fi router. (worked here before)")
        XCTAssertEqual(CallDiagnosis.empty.summary, "Checking the call")
        XCTAssertEqual(CallDiagnosis.empty.heading, "Not enough measurements yet")
        XCTAssertEqual(CallDiagnostics.promptLabel(diagnosis, memory), "Prompt for an AI assistant · v1, learned from 0 earlier reports")
    }
}
