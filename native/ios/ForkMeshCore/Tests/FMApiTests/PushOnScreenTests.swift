@testable import FMApi
import XCTest

final class PushOnScreenTests: XCTestCase {
    private let finished = PushOpen(arguments: [
        "alertId": "a1", "title": "Agent #7", "category": "agent-finished", "sessionId": "7", "kind": "desktop_ping",
    ])

    func testAnAgentDonePushIsHiddenOnTheAgentsList() {
        XCTAssertTrue(finished.isShown(on: PushScreen(tab: .agents)))
        XCTAssertTrue(finished.isShown(on: PushScreen(tab: .agents, route: .allAgents)))
    }

    func testAnAgentDonePushIsHiddenOnThatAgentOnly() {
        XCTAssertTrue(finished.isShown(on: PushScreen(tab: .agents, route: .agent(id: "desktop:7"))))
        XCTAssertFalse(finished.isShown(on: PushScreen(tab: .agents, route: .agent(id: "desktop:8"))))
    }

    func testAnAgentDonePushShowsElsewhereOrWhenCovered() {
        XCTAssertFalse(finished.isShown(on: PushScreen(tab: .code)))
        XCTAssertFalse(finished.isShown(on: PushScreen(tab: .agents, covered: true)))
    }

    func testAPingIsHiddenOnTheInboxList() {
        let ping = PushOpen(arguments: ["alertId": "p1", "title": "Release published"])
        XCTAssertTrue(ping.isShown(on: PushScreen(tab: .inbox)))
        XCTAssertFalse(ping.isShown(on: PushScreen(tab: .inbox, route: .pingSettings)))
        XCTAssertFalse(ping.isShown(on: PushScreen(tab: .chat)))
    }

    func testAPingIsHiddenOnTheRecordItLinksTo() {
        let ping = PushOpen(arguments: ["alertId": "p2", "title": "New comment", "href": "/acme/site/pulls/12"])
        let repo = RepoRef(owner: "acme", name: "site")
        XCTAssertTrue(ping.isShown(on: PushScreen(tab: .code, route: .pull(repo, number: 12, tab: .conversation))))
        XCTAssertFalse(ping.isShown(on: PushScreen(tab: .code, route: .pull(repo, number: 13, tab: .conversation))))
    }

    func testATaskPingIsHiddenOnThatTask() {
        let ping = PushOpen(arguments: ["alertId": "p4", "title": "Task done", "href": "/dashboard/tasks/?task=t9"])
        XCTAssertTrue(ping.isShown(on: PushScreen(tab: .inbox, route: .task(id: "t9"))))
        XCTAssertFalse(ping.isShown(on: PushScreen(tab: .inbox, route: .task(id: "t8"))))
    }

    func testADirectMessageIsHiddenInItsOpenConversation() {
        let message = PushOpen(arguments: ["topic": "direct-messages", "conversation": "direct:abc"])
        XCTAssertTrue(message.isShown(on: PushScreen(tab: .chat, conversation: "direct:abc")))
        XCTAssertFalse(message.isShown(on: PushScreen(tab: .chat, conversation: "direct:xyz")))
        XCTAssertFalse(message.isShown(on: PushScreen(tab: .chat, route: .people, conversation: "direct:abc")))
        XCTAssertFalse(message.isShown(on: PushScreen(tab: .inbox, conversation: "direct:abc")))
    }

    func testAnAlarmAlwaysShows() {
        let alarm = PushOpen(arguments: ["alertId": "p3", "title": "tower lost power", "alarm": "power"])
        XCTAssertFalse(alarm.isShown(on: PushScreen(tab: .inbox)))
    }
}
