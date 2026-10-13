import FMApi
import FMBase
import FMChat
import Foundation
import XCTest

@testable import FMControl

final class DesktopPingFixesTests: XCTestCase {
    private func desktopPing(pingId: String = "1790000000000-7") -> InboxItem {
        InboxItem(ping: ForkNotification(
            id: "n1", kind: "desktop_ping", title: "Action failed",
            body: "Native iOS ad hoc build on the local Mac \u{00B7} forkmesh/forkmesh\nStep: Find the build Mac",
            ts: 1_790_000_000_500, meta: ["node": "studio", "pingId": .string(pingId)]))
    }

    private static let reply: JSONObject = [
        "found": true, "event": "action-failed", "cause": "host-offline",
        "fixes": .array([
            .object(["id": "workflow.disable", "label": "Turn off this action", "detail": "Stops it.", "icon": "pause",
                     "score": .double(0.85), "trusted": false, "primary": true]),
            .object(["id": "run.rerun", "label": "Run it again", "icon": "rerun", "score": .double(0.35)]),
        ]),
    ]

    func testInboxItemKeepsTheDesktopPingIdentity() {
        let item = desktopPing()
        XCTAssertTrue(item.fromDesktop)
        XCTAssertEqual(item.desktopPingId, "1790000000000-7")
        XCTAssertEqual(item.desktopNode, "studio")
        let web = InboxItem(ping: ForkNotification(id: "n2", kind: "pull_submitted", title: "PR"))
        XCTAssertFalse(web.fromDesktop)
        XCTAssertNil(DesktopPingFixes.params(for: web))
    }

    func testParamsFallBackToTextWhenThePingIdIsMissing() {
        let params = DesktopPingFixes.params(for: desktopPing(pingId: ""))
        XCTAssertEqual(params?["title"], .string("Action failed"))
        XCTAssertEqual(params?["ts"], .int(1_790_000_000_500))
        XCTAssertNil(params?["pingId"])
    }

    @MainActor func testFixesAskThePingsFamilyAndKeepTheOrder() async throws {
        let channel = FakeControlChannel()
        channel.reply = DesktopPingFixesTests.reply
        let fixes = try await DesktopPingFixes(channel: channel).fixes(for: desktopPing())
        XCTAssertEqual(channel.requests.first?.0, "pings.fixes")
        XCTAssertEqual(channel.requests.first?.1["pingId"], .string("1790000000000-7"))
        XCTAssertTrue(fixes.found)
        XCTAssertEqual(fixes.fixes.map(\.id), ["workflow.disable", "run.rerun"])
        XCTAssertEqual(fixes.primary?.label, "Turn off this action")
        XCTAssertEqual(fixes.primary?.systemImage, "pause.circle")
        XCTAssertEqual(fixes.fixes.first?.score ?? 0, 0.85, accuracy: 0.0001)
    }

    @MainActor func testApplySendsTheFixAndReturnsTheMessage() async throws {
        let channel = FakeControlChannel()
        var reply = DesktopPingFixesTests.reply
        reply["ok"] = true
        reply["message"] = "Turned off it."
        channel.reply = reply
        let client = DesktopPingFixes(channel: channel)
        let fix = PingQuickFix(id: "workflow.disable", label: "Turn off this action", primary: true)
        let result = try await client.apply(fix, to: desktopPing())
        XCTAssertEqual(channel.requests.first?.0, "pings.fix")
        XCTAssertEqual(channel.requests.first?.1["fix"], .string("workflow.disable"))
        XCTAssertEqual(result.message, "Turned off it.")
    }

    @MainActor func testAnOlderDesktopExplainsItself() async {
        let channel = FakeControlChannel()
        channel.failure = LocalControlException("unsupported")
        do {
            _ = try await DesktopPingFixes(channel: channel).fixes(for: desktopPing())
            XCTFail("expected a failure")
        } catch {
            XCTAssertEqual(DesktopPingFixes.explain(error), "Update the desktop to use quick fixes.")
        }
    }

    func testANotFoundReplyHasNoFixes() {
        let fixes = PingQuickFixes(json: ["found": false, "fixes": .array([])])
        XCTAssertFalse(fixes.found)
        XCTAssertNil(fixes.primary)
    }
}
