import FMAccount
import Foundation
import XCTest
@testable import FMTools

final class PerformanceAlertTests: XCTestCase {
    private let now: Date = Date(timeIntervalSince1970: 1_800_000_000)

    private func sample(_ kind: PerformanceEventKind, _ ms: Int, ago: TimeInterval) -> PerformanceAlert.Sample {
        PerformanceAlert.Sample(kind: kind, durationMs: ms, at: now.addingTimeInterval(-ago))
    }

    func testQuietAppIsNeutral() {
        let alert = PerformanceAlert.evaluate(
            samples: [sample(.eventLoop, 249, ago: 10), sample(.frame, 80, ago: 5), sample(.operation, 3000, ago: 5)],
            diagnostics: [], lastSent: nil, now: now)
        XCTAssertFalse(alert.detected)
        XCTAssertEqual(alert.label, "Send diagnostics")
    }

    func testAMainThreadHangOfAQuarterSecondTurnsItRed() {
        let alert = PerformanceAlert.evaluate(
            samples: [sample(.eventLoop, 420, ago: 30), sample(.sync, 300, ago: 60)], diagnostics: [], lastSent: nil, now: now)
        XCTAssertTrue(alert.detected)
        XCTAssertEqual(alert.reason, "The app froze for 0.4 s (2 freezes)")
        XCTAssertEqual(alert.label, "Slow, send report")
    }

    func testOldHangsAndHangsBeforeTheLastSendDoNotCount() {
        XCTAssertFalse(PerformanceAlert.evaluate(
            samples: [sample(.eventLoop, 900, ago: 601)], diagnostics: [], lastSent: nil, now: now).detected)
        XCTAssertFalse(PerformanceAlert.evaluate(
            samples: [sample(.eventLoop, 900, ago: 60)], diagnostics: [], lastSent: now.addingTimeInterval(-30), now: now).detected)
    }

    func testSustainedFrameHitchesTurnItRed() {
        let four = (1...4).map { sample(.frame, 60, ago: TimeInterval($0 * 10)) }
        XCTAssertFalse(PerformanceAlert.evaluate(samples: four, diagnostics: [], lastSent: nil, now: now).detected)
        let five = four + [sample(.frame, 60, ago: 100)]
        XCTAssertEqual(
            PerformanceAlert.evaluate(samples: five, diagnostics: [], lastSent: nil, now: now).reason,
            "5 dropped-frame hitches in 10 min")
    }

    func testAMetricKitDiagnosticSinceTheLastSendWins() {
        let hang = PerformanceAlert.Diagnostic(summary: "The main thread hung for 2.0 s", at: now.addingTimeInterval(-3600))
        XCTAssertEqual(
            PerformanceAlert.evaluate(samples: [], diagnostics: [hang], lastSent: nil, now: now).reason,
            "iOS reported: The main thread hung for 2.0 s")
        XCTAssertFalse(PerformanceAlert.evaluate(
            samples: [], diagnostics: [hang], lastSent: now.addingTimeInterval(-60), now: now).detected)
        let stale = PerformanceAlert.Diagnostic(summary: "old", at: now.addingTimeInterval(-90_000))
        XCTAssertFalse(PerformanceAlert.evaluate(samples: [], diagnostics: [stale], lastSent: nil, now: now).detected)
    }
}

final class DiagnosticsBundleTests: XCTestCase {
    func testScrubsSecretsEmailsAndMessageContents() {
        let raw = """
            GET https://app.forkmesh.com/api/chat?token=abc123&room=7 200
            Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJqZXR0In0.c2lnbmF0dXJlLXZhbHVl
            api_key=sk-live-1234 password: hunter2
            from jett@forkmesh.com {"text":"meet at 5pm \\"secret\\"","id":4}
            node 3f9a0c1d2e4b5a6978877665544332211aabbccdd done
            call +1 (415) 555-0100 at 2026-10-13 19:20:15 took 420 ms
            /var/mobile/Containers/Data/Application/1234-ABCD/Library/stalls.log
            """
        let clean = DiagnosticsBundle.scrub(raw)
        for leaked in ["abc123", "eyJhbGci", "sk-live-1234", "hunter2", "jett@forkmesh.com", "meet at 5pm",
                       "3f9a0c1d2e4b5a6978877665544332211aabbccdd", "555-0100", "1234-ABCD"] {
            XCTAssertFalse(clean.contains(leaked), leaked)
        }
        XCTAssertTrue(clean.contains("https://app.forkmesh.com/api/chat?[query] 200"))
        XCTAssertTrue(clean.contains("[email]"))
        XCTAssertTrue(clean.contains(#""text":"[redacted]","id":4"#))
        XCTAssertTrue(clean.contains("2026-10-13 19:20:15 took 420 ms"))
    }

    func testScrubsConversationAndRoomNames() {
        let raw = """
            relay.send.edit {"type":"edit","conversation":"#layoffs-plan","to":"node-9"}
            stall conversation=#layoffs-plan room=secret-room channel=@pat took 300 ms
            """
        let clean = DiagnosticsBundle.scrub(raw)
        for leaked in ["layoffs-plan", "secret-room", "@pat", "node-9"] {
            XCTAssertFalse(clean.contains(leaked), leaked)
        }
        XCTAssertTrue(clean.contains("took 300 ms"))
    }

    @MainActor func testThePerformanceReportIsScrubbed() async {
        let telemetry = AppTelemetry(log: AppLog(), rssMegabytes: { 1 })
        let text = PerformanceReport.text(
            telemetry: telemetry, context: [("screen", "chat conversation=#layoffs-plan")],
            stallLog: "stall token=abc123def from pat@example.com")
        for leaked in ["layoffs-plan", "abc123def", "pat@example.com"] {
            XCTAssertFalse(text.contains(leaked), leaked)
        }
    }

    func testLogsCanBeLeftOut() {
        let report = "# report\nheader\n\n## Metrics (5m | 1h)\nfps 60\n\n## App log, last 10 lines\nsecret line\n\n## stalls.log tail\nstall\n\n## Log volume\n12"
        let without = DiagnosticsBundle.prompt(reason: "The app froze for 0.4 s", report: report, includeLogs: false, limit: 8000)
        XCTAssertFalse(without.contains("secret line"))
        XCTAssertFalse(without.contains("stall\n"))
        XCTAssertTrue(without.contains("fps 60"))
        XCTAssertTrue(without.contains("## Log volume"))
        XCTAssertTrue(without.hasPrefix("The app detected a performance issue: The app froze for 0.4 s."))
        XCTAssertTrue(without.contains(DiagnosticsBundle.repository))
        let with = DiagnosticsBundle.prompt(reason: "", report: report, includeLogs: true, limit: 8000)
        XCTAssertTrue(with.contains("secret line"))
    }

    func testThePromptSendsTheAgentToThePlaybookAndAsksItToAddThisCheck() throws {
        let lead = DiagnosticsBundle.lead(reason: "")
        XCTAssertTrue(lead.contains("First read \(DiagnosticsBundle.playbook)"))
        XCTAssertTrue(lead.contains("add this check to it"))
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0..<6 { root.deleteLastPathComponent() }
        let page = try String(contentsOf: root.appendingPathComponent(DiagnosticsBundle.playbook), encoding: .utf8)
        for heading in ["## Reading the bundle", "## Checks so far", "## Adding a check"] {
            XCTAssertTrue(page.contains(heading), heading)
        }
    }

    func testAnotherLeadKeepsTheScrubAndTheLimit() {
        let prompt = DiagnosticsBundle.prompt(lead: "Fix the DJ studio.", report: "token=abc123\n" + String(repeating: "x\n", count: 5000),
                                              includeLogs: true, limit: 2000)
        XCTAssertTrue(prompt.hasPrefix("Fix the DJ studio.\n\n# Diagnostics bundle\n"))
        XCTAssertFalse(prompt.contains("abc123"))
        XCTAssertLessThanOrEqual(prompt.count, 2000)
    }

    func testThePromptFitsTheLimit() {
        let prompt = DiagnosticsBundle.prompt(reason: "x", report: String(repeating: "line of report\n", count: 2000),
                                              includeLogs: true, limit: 8000)
        XCTAssertLessThanOrEqual(prompt.count, 8000)
        XCTAssertTrue(prompt.hasSuffix("[cut to fit the prompt]"))
    }
}
